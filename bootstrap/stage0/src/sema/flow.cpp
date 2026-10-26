// Copyright 2026 The Phantasos Authors
// SPDX-License-Identifier: Apache-2.0
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "sema/flow.hpp"

#include <initializer_list>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace phs {
namespace {
using namespace ast;

// One bit per local that is declared without an initializer in the current callable.
class Bits {
public:
    Bits(usize size, bool value) : words_((size + 63) / 64, value ? ~u64{0} : u64{0}) {}
    bool test(usize i) const { return (words_[i / 64] >> (i % 64)) & 1; }
    void set(usize i, bool value) {
        const u64 mask = u64{1} << (i % 64);
        words_[i / 64] = value ? words_[i / 64] | mask : words_[i / 64] & ~mask;
    }
    void intersect(const Bits& other) {
        for (usize w = 0; w < words_.size(); ++w) words_[w] &= other.words_[w];
    }
    void unite(const Bits& other) {
        for (usize w = 0; w < words_.size(); ++w) words_[w] |= other.words_[w];
    }

private:
    std::vector<u64> words_;
};

// An unreachable state assigns everything and nothing, so dead code reports
// no initialization errors and merges as the identity.
struct State {
    Bits assigned;  // definitely assigned on every path reaching this point
    Bits maybe;     // assigned on at least one path
    bool live = true;
};

void merge(State& into, const State& from) {
    if (!from.live) return;
    if (!into.live) { into = from; return; }
    into.assigned.intersect(from.assigned);
    into.maybe.unite(from.maybe);
}

const Expr* unparen(const Expr* expr) {
    while (const auto* paren = as<ParenExpr>(expr)) expr = paren->inner;
    return expr;
}

bool control(const Expr& expr) {
    return expr.kind == ExprKind::If || expr.kind == ExprKind::Match || expr.kind == ExprKind::Block ||
           expr.kind == ExprKind::Unsafe;
}

bool conditional(const Expr& expr) {
    const auto* binary = as<BinaryExpr>(&expr);
    return binary && (binary->op == BinOp::And || binary->op == BinOp::Or || binary->op == BinOp::Coalesce);
}

// Direct expression operands in evaluation order (left to right). Statements
// inside blocks and the branches of control expressions are not included.
void operands(const Expr& expr, std::vector<const Expr*>& out) {
    switch (expr.kind) {
        case ExprKind::Paren: out.push_back(cast<ParenExpr>(&expr)->inner); break;
        case ExprKind::Array: for (const Expr* e : cast<ArrayExpr>(&expr)->elems) out.push_back(e); break;
        case ExprKind::Interp: for (const Expr* e : cast<InterpExpr>(&expr)->exprs) out.push_back(e); break;
        case ExprKind::Unary: out.push_back(cast<UnaryExpr>(&expr)->operand); break;
        case ExprKind::Binary:
            out.push_back(cast<BinaryExpr>(&expr)->lhs);
            out.push_back(cast<BinaryExpr>(&expr)->rhs);
            break;
        case ExprKind::Cast: out.push_back(cast<CastExpr>(&expr)->operand); break;
        case ExprKind::Call:
            out.push_back(cast<CallExpr>(&expr)->callee);
            for (const auto& arg : cast<CallExpr>(&expr)->args) out.push_back(arg.value);
            break;
        case ExprKind::Member: out.push_back(cast<MemberExpr>(&expr)->base); break;
        case ExprKind::OptMember: out.push_back(cast<OptMemberExpr>(&expr)->base); break;
        case ExprKind::Index:
            out.push_back(cast<IndexExpr>(&expr)->base);
            out.push_back(cast<IndexExpr>(&expr)->index);
            break;
        case ExprKind::Unwrap: out.push_back(cast<UnwrapExpr>(&expr)->operand); break;
        case ExprKind::GenericInst: out.push_back(cast<GenericInstExpr>(&expr)->base); break;
        case ExprKind::If: {
            const auto& branch = *cast<IfExpr>(&expr);
            out.push_back(branch.cond.expr);
            out.push_back(branch.then_block);
            out.push_back(branch.else_branch);
            break;
        }
        case ExprKind::Match:
            out.push_back(cast<MatchExpr>(&expr)->scrutinee);
            for (const auto& arm : cast<MatchExpr>(&expr)->arms) {
                out.push_back(arm.guard);
                out.push_back(arm.body);
            }
            break;
        case ExprKind::Unsafe: out.push_back(cast<UnsafeExpr>(&expr)->block); break;
        default: break;
    }
}

// Visits every statement nested anywhere inside root, without the C++ stack.
template <typename F>
void each_stmt(const Expr* root, F&& visit) {
    std::vector<std::variant<const Expr*, const Stmt*>> work{root};
    std::vector<const Expr*> exprs;
    while (!work.empty()) {
        const auto next = work.back();
        work.pop_back();
        exprs.clear();
        if (const auto* stmt_ptr = std::get_if<const Stmt*>(&next)) {
            const Stmt& stmt = **stmt_ptr;
            visit(stmt);
            switch (stmt.kind) {
                case StmtKind::Var: exprs.push_back(cast<VarStmt>(&stmt)->init); break;
                case StmtKind::Return: exprs.push_back(cast<ReturnStmt>(&stmt)->value); break;
                case StmtKind::Throw: exprs.push_back(cast<ThrowStmt>(&stmt)->value); break;
                case StmtKind::While:
                    exprs.push_back(cast<WhileStmt>(&stmt)->cond.expr);
                    exprs.push_back(cast<WhileStmt>(&stmt)->body);
                    break;
                case StmtKind::For:
                    exprs.push_back(cast<ForStmt>(&stmt)->iter);
                    exprs.push_back(cast<ForStmt>(&stmt)->body);
                    break;
                case StmtKind::Loop: exprs.push_back(cast<LoopStmt>(&stmt)->body); break;
                case StmtKind::Defer: exprs.push_back(cast<DeferStmt>(&stmt)->body); break;
                case StmtKind::Expr: exprs.push_back(cast<ExprStmt>(&stmt)->expr); break;
                case StmtKind::Assign:
                    exprs.push_back(cast<AssignStmt>(&stmt)->target);
                    exprs.push_back(cast<AssignStmt>(&stmt)->value);
                    break;
                case StmtKind::Break: case StmtKind::Continue: break;
            }
        } else if (const Expr* expr = std::get<const Expr*>(next)) {
            if (const auto* block = as<BlockExpr>(expr)) {
                for (const Stmt* stmt : block->stmts) work.push_back(stmt);
                continue;
            }
            operands(*expr, exprs);
        }
        for (const Expr* e : exprs)
            if (e) work.push_back(e);
    }
}

// Analyzes one callable at a time. Statement and control-expression recursion
// follows syntactic nesting, which the parser bounds (E0112); flat expression
// chains and `else if` chains are iterative.
class FlowChecker {
public:
    FlowChecker(const Resolution& resolution, const ScalarTypes& types, const Interner& names, DiagEngine& diag)
        : resolution_(resolution), types_(types), names_(names), diag_(diag) {
        for (usize id = 0; id < resolution.bindings().size(); ++id)
            if (const Item* item = resolution.bindings()[id].item) items_.emplace(item, BindingId{id});
    }

    void run(const File& file) {
        struct Work { const Item* item; const TypeDeclItem* owner; };
        std::vector<Work> work;
        for (const Item* item : file.items) work.push_back({item, nullptr});
        while (!work.empty()) {
            const auto [item, owner] = work.back();
            work.pop_back();
            if (const auto* type = as_type_decl(item)) {
                for (const Item* member : type->members) work.push_back({member, type});
            } else if (const auto* ext = as<ExternItem>(item)) {
                for (const Item* nested : ext->items) work.push_back({nested, owner});
            } else if (const auto* fun = as<FunItem>(item)) {
                if (fun->body) callable(fun->sig.params, fun->body, fun);
            } else if (const auto* init = as<InitItem>(item)) {
                if (init->body) callable(init->has_params || !owner ? init->params : owner->ctor_params, init->body, nullptr);
            } else if (const auto* deinit = as<DeinitItem>(item)) {
                if (deinit->body) callable({}, deinit->body, nullptr);
            }
        }
    }

private:
    struct Local {
        const VarStmt* decl;
        usize tracked;  // bit index, or kUntracked for locals with an initializer
    };
    static constexpr usize kUntracked = static_cast<usize>(-1);

    State dead() const { return {Bits(tracked_, true), Bits(tracked_, false), false}; }

    void callable(List<Param> params, const Expr* body, const FunItem* fun) {
        params_.clear();
        locals_.clear();
        tracked_ = 0;
        for (const auto& param : params) params_.emplace(param.name.span.lo, &param);
        each_stmt(body, [&](const Stmt& stmt) {
            if (const auto* var = as<VarStmt>(&stmt))
                locals_.emplace(var->name.span.lo, Local{var, var->init ? kUntracked : tracked_++});
        });
        State state{Bits(tracked_, false), Bits(tracked_, false), true};
        eval(body, state);
        if (fun && !fun->expr_body && state.live && returns_value(*fun)) {
            const Span end{body->span.file, body->span.hi - 1, body->span.hi};
            auto& d = diag_.error(DiagCode::MissingReturn, end, "function '{}' must return '{}' on every path",
                                  names_.str(fun->name.sym), result_name(*fun));
            d.notes.push_back({fun->name.span, "the end of the body is reachable without a 'return'"});
        }
    }

    ScalarType result(const FunItem& fun) const {
        const auto it = items_.find(&fun);
        const ScalarSignature* sig = it == items_.end() ? nullptr : types_.signature(it->second);
        return sig ? sig->result : ScalarType{};
    }
    bool returns_value(const FunItem& fun) const {
        if (!fun.sig.ret || fun.sig.ret->kind == TypeKind::Unit) return false;
        return result(fun) != ScalarType{ScalarType::Unit};
    }
    std::string result_name(const FunItem& fun) const {
        const auto type = result(fun);
        if (type.kind == ScalarType::Deferred || type.kind == ScalarType::Error) return "a value";
        return types_.type_name(type, resolution_, names_, 1024);
    }

    BindingId target(const NameExpr& name) const {
        const auto* ref = resolution_.reference(name.name.span);
        if (!ref || ref->state != ResolutionState::Resolved) return {};
        const auto ids = resolution_.candidates(ref->target);
        return ids.size() == 1 ? ids.front() : BindingId{};
    }
    const Local* local(BindingId id) const {
        if (!id.valid() || resolution_.binding(id).kind != BindingKind::Local) return nullptr;
        const auto it = locals_.find(resolution_.binding(id).name.span.lo);
        return it == locals_.end() ? nullptr : &it->second;
    }

    void read(const NameExpr& name, const State& state) {
        const auto* found = local(target(name));
        if (!found || found->tracked == kUntracked || !state.live || state.assigned.test(found->tracked)) return;
        auto& d = diag_.error(DiagCode::UninitializedUse, name.span, "'{}' is used before it is initialized",
                              names_.str(name.name.sym));
        d.notes.push_back({found->decl->name.span, "declared here without an initializer"});
    }

    void immutable(const NameExpr& name, Span declared, std::string_view note) {
        auto& d = diag_.error(DiagCode::ImmutableAssignment, name.span, "cannot assign to immutable '{}'",
                              names_.str(name.name.sym));
        d.notes.push_back({declared, std::string(note)});
    }

    void write(const NameExpr& name, State& state) {
        const auto id = target(name);
        if (!id.valid()) return;
        const auto& binding = resolution_.binding(id);
        switch (binding.kind) {
            case BindingKind::Local: {
                const auto* found = local(id);
                if (!found) { immutable(name, binding.name.span, "bound here; only 'var' locals can be reassigned"); return; }
                const bool mutable_local = found->decl->binding == Binding::Var;
                if (found->tracked == kUntracked) {
                    if (!mutable_local) immutable(name, found->decl->name.span, "declared with 'let' here");
                    return;
                }
                if (!mutable_local && state.live && state.maybe.test(found->tracked)) {
                    auto& d = diag_.error(DiagCode::ImmutableAssignment, name.span,
                                          "'{}' may already be initialized; a 'let' binding is assigned only once",
                                          names_.str(name.name.sym));
                    d.notes.push_back({found->decl->name.span, "declared with 'let' here"});
                }
                state.assigned.set(found->tracked, true);
                state.maybe.set(found->tracked, true);
                return;
            }
            case BindingKind::Pattern:
                immutable(name, binding.name.span, "pattern bindings cannot be reassigned");
                return;
            case BindingKind::Parameter: {
                const auto it = params_.find(binding.name.span.lo);
                // Promoted constructor parameters name fields; receiver rules are later work.
                if (it == params_.end() || it->second->conv == Convention::Inout || it->second->binding != Binding::None) return;
                immutable(name, binding.name.span, "parameters are read-only unless declared 'inout'");
                return;
            }
            case BindingKind::Variable: {
                const auto* var = as<VarItem>(binding.item);
                if (var && var->binding != Binding::Var) immutable(name, binding.name.span, "declared with 'let' here");
                return;
            }
            default: return;
        }
    }

    // A set of `let` locals that some statement inside a loop's condition or body assigns.
    Bits assigned_lets(std::initializer_list<const Expr*> parts) const {
        Bits result(tracked_, false);
        for (const Expr* part : parts) {
            if (!part) continue;
            each_stmt(part, [&](const Stmt& stmt) {
                const auto* assign = as<AssignStmt>(&stmt);
                const auto* name = assign ? as<NameExpr>(unparen(assign->target)) : nullptr;
                const auto* found = name ? local(target(*name)) : nullptr;
                if (found && found->tracked != kUntracked && found->decl->binding != Binding::Var) result.set(found->tracked, true);
            });
        }
        return result;
    }

    bool panics(const CallExpr& call) const {
        const auto* callee = as<NameExpr>(unparen(call.callee));
        if (!callee) return false;
        const auto id = target(*callee);
        return id.valid() && resolution_.binding(id).kind == BindingKind::BuiltinValue &&
               names_.str(resolution_.binding(id).name.sym) == "panic";
    }
    bool diverges(const CallExpr& call) const {
        return types_.expression(&call) == ScalarType{ScalarType::Never} || panics(call);
    }

    void eval(const Expr* root, State& state) {
        if (!root) return;
        struct Frame { const Expr* expr; bool done; };
        std::vector<Frame> stack{{root, false}};
        std::vector<const Expr*> children;
        while (!stack.empty()) {
            const auto [expr, done] = stack.back();
            stack.pop_back();
            if (!done) {
                if (control(*expr)) { control_flow(*expr, state); continue; }
                if (const auto* unary = as<UnaryExpr>(expr); unary && unary->op == UnOp::TryOpt) {
                    // A caught error resumes after the `try?` expression.
                    caught_.push_back(dead());
                    eval(unary->operand, state);
                    merge(state, caught_.back());
                    caught_.pop_back();
                    continue;
                }
                stack.push_back({expr, true});
                if (conditional(*expr)) { stack.push_back({cast<BinaryExpr>(expr)->lhs, false}); continue; }
                children.clear();
                operands(*expr, children);
                for (auto it = children.rbegin(); it != children.rend(); ++it)
                    if (*it) stack.push_back({*it, false});
                continue;
            }
            if (const auto* name = as<NameExpr>(expr)) read(*name, state);
            else if (const auto* call = as<CallExpr>(expr)) {
                // Any call inside `try?` may throw after its operands have run;
                // a panic aborts instead (3.7).
                if (!caught_.empty() && !panics(*call)) merge(caught_.back(), state);
                if (diverges(*call)) state = dead();
            } else if (conditional(*expr)) {
                // The right operand runs on only some paths.
                State rhs = state;
                eval(cast<BinaryExpr>(expr)->rhs, rhs);
                merge(state, rhs);
            }
        }
    }

    void control_flow(const Expr& expr, State& state) {
        if (const auto* block = as<BlockExpr>(&expr)) {
            for (const Stmt* stmt : block->stmts) statement(*stmt, state);
        } else if (const auto* unsafe = as<UnsafeExpr>(&expr)) {
            control_flow(*unsafe->block, state);
        } else if (const auto* match = as<MatchExpr>(&expr)) {
            eval(match->scrutinee, state);
            if (match->arms.empty()) return;
            // Exhaustiveness is checked separately; some arm always runs. A
            // failed pattern or guard continues to the next arm with its effects.
            State out = dead();
            for (const auto& arm : match->arms) {
                State path = state;
                eval(arm.guard, path);
                // An irrefutable pattern always runs its guard before the next arm.
                const bool irrefutable = arm.pattern->kind == PatKind::Wildcard || arm.pattern->kind == PatKind::Binding;
                if (arm.guard && irrefutable) state = path;
                else if (arm.guard) merge(state, path);
                eval(arm.body, path);
                merge(out, path);
            }
            state = std::move(out);
        } else {
            State out = dead();
            for (const auto* branch = cast<IfExpr>(&expr); branch;) {
                eval(branch->cond.expr, state);
                State then = state;
                control_flow(*branch->then_block, then);
                merge(out, then);
                const Expr* otherwise = branch->else_branch;
                branch = as<IfExpr>(otherwise);
                if (!branch) {
                    if (otherwise) eval(otherwise, state);
                    merge(out, state);
                }
            }
            state = std::move(out);
        }
    }

    void loop(const Cond* cond, const BlockExpr* body, State& state, bool exits_normally) {
        // Assignment never removes initialization, so the loop head keeps the
        // entry's definite facts; any `let` assigned in the condition or body
        // may already be set when it runs again.
        state.maybe.unite(assigned_lets({cond ? cond->expr : nullptr, body}));
        if (cond) eval(cond->expr, state);
        breaks_.push_back(dead());
        State path = state;
        control_flow(*body, path);
        State out = std::move(breaks_.back());
        breaks_.pop_back();
        if (exits_normally) merge(out, state);
        state = std::move(out);
    }

    void statement(const Stmt& stmt, State& state) {
        switch (stmt.kind) {
            case StmtKind::Var: {
                const auto& var = *cast<VarStmt>(&stmt);
                eval(var.init, state);
                if (const auto it = locals_.find(var.name.span.lo); it != locals_.end() && it->second.tracked != kUntracked) {
                    state.assigned.set(it->second.tracked, false);
                    state.maybe.set(it->second.tracked, false);
                }
                break;
            }
            case StmtKind::Return:
                eval(cast<ReturnStmt>(&stmt)->value, state);
                state = dead();
                break;
            case StmtKind::Throw:
                eval(cast<ThrowStmt>(&stmt)->value, state);
                if (!caught_.empty()) merge(caught_.back(), state);
                state = dead();
                break;
            case StmtKind::Break:
                if (!breaks_.empty()) merge(breaks_.back(), state);
                state = dead();
                break;
            case StmtKind::Continue: state = dead(); break;
            case StmtKind::While: {
                const auto& loop_stmt = *cast<WhileStmt>(&stmt);
                loop(&loop_stmt.cond, loop_stmt.body, state, true);
                break;
            }
            case StmtKind::For: {
                const auto& loop_stmt = *cast<ForStmt>(&stmt);
                eval(loop_stmt.iter, state);
                loop(nullptr, loop_stmt.body, state, true);
                break;
            }
            case StmtKind::Loop: loop(nullptr, cast<LoopStmt>(&stmt)->body, state, false); break;
            case StmtKind::Defer: {
                // Cleanup reads see the state at registration and never initialize the scope.
                // Its errors are handled inside it, never by an enclosing `try?`.
                State cleanup = state;
                breaks_.push_back(dead());
                auto caught = std::exchange(caught_, {});
                control_flow(*cast<DeferStmt>(&stmt)->body, cleanup);
                caught_ = std::move(caught);
                breaks_.pop_back();
                // Its writes happen at scope exit, so a later write to the same
                // `let`, or another cleanup's, would assign it twice.
                state.maybe.unite(assigned_lets({cast<DeferStmt>(&stmt)->body}));
                break;
            }
            case StmtKind::Expr: eval(cast<ExprStmt>(&stmt)->expr, state); break;
            case StmtKind::Assign: {
                const auto& assign = *cast<AssignStmt>(&stmt);
                const auto* name = as<NameExpr>(unparen(assign.target));
                if (!name) eval(assign.target, state);
                else if (assign.op != AssignOp::Assign) read(*name, state);
                eval(assign.value, state);
                if (name) write(*name, state);
                break;
            }
        }
    }

    const Resolution& resolution_;
    const ScalarTypes& types_;
    const Interner& names_;
    DiagEngine& diag_;
    std::unordered_map<const Item*, BindingId> items_;
    std::unordered_map<u32, const Param*> params_;
    std::unordered_map<u32, Local> locals_;
    std::vector<State> breaks_;
    std::vector<State> caught_;  // error exits of enclosing `try?` expressions
    usize tracked_ = 0;
};

}  // namespace

void check_flow(const File& file, const Resolution& resolution, const ScalarTypes& types, const Interner& names,
                DiagEngine& diag) {
    FlowChecker(resolution, types, names, diag).run(file);
}

}  // namespace phs
