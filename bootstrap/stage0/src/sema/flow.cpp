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

#include <format>
#include <algorithm>
#include <initializer_list>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include "support/flat_map.hpp"
#include "support/small_vec.hpp"

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
using ExprList = SmallVec<const Expr*, 8>;

void operands(const Expr& expr, ExprList& out) {
    switch (expr.kind) {
        case ExprKind::Paren: out.push_back(cast<ParenExpr>(&expr)->inner); break;
        case ExprKind::Array: for (const Expr* e : cast<ArrayExpr>(&expr)->elems) out.push_back(e); break;
        case ExprKind::Tuple: for (const Expr* e : cast<TupleExpr>(&expr)->elems) out.push_back(e); break;
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
        case ExprKind::TupleField: out.push_back(cast<TupleFieldExpr>(&expr)->base); break;
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

// Visits every statement and expression nested anywhere inside root, without
// the C++ stack. Closure bodies are separate callables, entered only on request.
template <typename S, typename E>
void each_node(const Expr* root, bool closures, S&& visit, E&& visit_expr) {
    SmallVec<std::variant<const Expr*, const Stmt*>, 16> work{root};
    ExprList exprs;
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
            visit_expr(*expr);
            if (const auto* block = as<BlockExpr>(expr)) {
                for (const Stmt* stmt : block->stmts) work.push_back(stmt);
                continue;
            }
            if (const auto* closure = as<ClosureExpr>(expr); closure && closures) exprs.push_back(closure->body);
            operands(*expr, exprs);
        }
        for (const Expr* e : exprs)
            if (e) work.push_back(e);
    }
}

template <typename F>
void each_stmt(const Expr* root, F&& visit) {
    each_node(root, false, visit, [](const Expr&) {});
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
        fields(work);
        while (!work.empty()) {
            const auto [item, owner] = work.back();
            work.pop_back();
            if (const auto* type = as_type_decl(item)) {
                defaults(type->ctor_params);
                for (const auto& base : type->supers)
                    for (const auto& arg : base.args) callable(type->ctor_params, arg.value, nullptr);
                for (const Item* member : type->members) work.push_back({member, type});
            } else if (const auto* ext = as<ExternItem>(item)) {
                for (const Item* nested : ext->items) work.push_back({nested, owner});
            } else if (const auto* fun = as<FunItem>(item)) {
                defaults(fun->sig.params);
                if (fun->body) {
                    receiver(owner, fun);
                    callable(fun->sig.params, fun->body, fun);
                    receiver(nullptr, nullptr);
                }
            } else if (const auto* var = as<VarItem>(item)) {
                // Field initializers see the owner's primary-constructor parameters.
                if (var->init) callable(owner ? owner->ctor_params : List<Param>{}, var->init, nullptr);
            } else if (const auto* init = as<InitItem>(item)) {
                defaults(init->params);
                // Construction rules for `self` are separate, but closures still capture it.
                capturing_receiver(owner);
                if (init->body) callable(init->has_params || !owner ? init->params : owner->ctor_params, init->body, nullptr);
                receiver(nullptr, nullptr);
            } else if (const auto* deinit = as<DeinitItem>(item)) {
                capturing_receiver(owner);
                if (deinit->body) callable({}, deinit->body, nullptr);
                receiver(nullptr, nullptr);
            }
        }
    }

private:
    struct Local {
        const VarStmt* decl;
        usize tracked;  // bit index, or kUntracked for locals with an initializer
        Span declared;  // the name, which is one of decl->names when destructuring
    };
    static constexpr usize kUntracked = static_cast<usize>(-1);

    State dead() const { return {Bits(tracked_, true), Bits(tracked_, false), false}; }

    // How a place may be modified (3.3). Unknown places, including `self`
    // during construction, are not checked.
    enum class Access : u8 { Unknown, Mutable, Immutable, ReadOnly, Temporary };
    enum class Storage : u8 { Unknown, Value, Reference };
    struct Place {
        Access access = Access::Unknown;
        std::string subject;  // what restricts the place, e.g. 'p' or field 'y'
        Span declared;
        std::string note;
    };
    struct Field {
        bool var = false;
        bool is_static = false;
        Span declared;
        std::string_view note;  // why a non-'var' field is immutable
    };

    // Registers every field before any body is checked, since members can be declared in any order.
    template <typename Work>
    void fields(const std::vector<Work>& roots) {
        std::vector<const Item*> work;
        for (const auto& root : roots) work.push_back(root.item);
        while (!work.empty()) {
            const Item* item = work.back();
            work.pop_back();
            if (const auto* type = as_type_decl(item)) {
                for (const auto& param : type->ctor_params)
                    if (type->kind == ItemKind::Struct || param.binding != Binding::None)
                        fields_.emplace(param.name.span.lo,
                                        Field{param.binding == Binding::Var, false, param.name.span,
                                              param.binding == Binding::Let ? "declared with 'let' here"
                                                                            : "declared without 'var' here"});
                for (const Item* member : type->members) {
                    if (const auto* var = as<VarItem>(member))
                        fields_.emplace(var->name.span.lo, Field{var->binding == Binding::Var,
                                                                 has_modifier(var->mods, Modifier::Static),
                                                                 var->name.span, "declared with 'let' here"});
                    work.push_back(member);
                }
            } else if (const auto* ext = as<ExternItem>(item)) {
                for (const Item* nested : ext->items) work.push_back(nested);
            }
        }
    }

    static Storage storage(ItemKind kind) {
        if (kind == ItemKind::Struct || kind == ItemKind::Enum) return Storage::Value;
        if (kind == ItemKind::Class || kind == ItemKind::Actor) return Storage::Reference;
        return Storage::Unknown;
    }
    Storage storage(ScalarType type) const {
        if (type.optional_depth) return Storage::Unknown;
        if (type.kind == ScalarType::Shared) return Storage::Reference;
        if (type.kind == ScalarType::Tuple) return Storage::Value;
        const auto* descriptor = type.kind == ScalarType::Nominal ? types_.descriptor(type) : nullptr;
        if (!descriptor || !descriptor->declaration.valid()) return Storage::Unknown;
        const Item* item = resolution_.binding(descriptor->declaration).item;
        return item ? storage(item->kind) : Storage::Unknown;
    }

    // `self` in an instance method is mutable only inside a `mut fun`.
    void receiver(const TypeDeclItem* owner, const FunItem* fun) {
        self_ = {};
        self_storage_ = Storage::Unknown;
        self_owner_ = nullptr;
        if (!owner || !fun || has_modifier(fun->mods, Modifier::Static)) return;
        self_storage_ = storage(owner->kind);
        self_owner_ = owner;
        if (has_modifier(fun->mods, Modifier::Mut)) self_ = {Access::Mutable, "'self'", {}, {}};
        else self_ = {Access::ReadOnly, "'self'", fun->name.span, "'self' is read-only in a method without 'mut'"};
    }

    // In an `init` or `deinit`, `self` stays unchecked for assignments, but a
    // closure that uses it captures it.
    void capturing_receiver(const TypeDeclItem* owner) {
        receiver(nullptr, nullptr);
        if (!owner) return;
        self_storage_ = storage(owner->kind);
        self_owner_ = owner;
    }

    Place field_place(const ResolvedBinding& binding, const Place& base, Storage storage) const {
        const auto it = fields_.find(binding.name.span.lo);
        if (it == fields_.end()) return {};
        const Field& field = it->second;
        const auto subject = "field '" + std::string(names_.str(binding.name.sym)) + "'";
        const Place fixed{Access::Immutable, subject, field.declared, std::string(field.note)};
        if (field.is_static) return field.var ? Place{Access::Mutable} : fixed;
        if (base.access == Access::Unknown) return {};
        // Nothing reached through a borrow becomes writable, whatever its type.
        if (base.access == Access::ReadOnly) return base;
        if (!field.var) return fixed;
        // Value types are immutable all the way down; a class object can be
        // changed through any reference that is not a borrow.
        if (storage == Storage::Value) return base;
        if (storage == Storage::Reference) return {Access::Mutable};
        return {};
    }

    // A bare field name inside a method. Inherited fields are found by the
    // type pass, so their lexical reference stays deferred.
    const ResolvedBinding* implicit_field(const NameExpr& name) const {
        const auto id = target(name);
        if (id.valid()) return resolution_.binding(id).kind == BindingKind::Field ? &resolution_.binding(id) : nullptr;
        const auto* record = types_.member(&name);
        if (!record || record->state != ResolutionState::Resolved || record->candidates.size() != 1) return nullptr;
        const auto& binding = resolution_.binding(record->candidates.front());
        return binding.kind == BindingKind::Field ? &binding : nullptr;
    }

    Place name_place(const NameExpr& name) const {
        const auto id = target(name);
        if (!id.valid()) {
            const auto* field = implicit_field(name);
            return field ? field_place(*field, self_, self_storage_) : Place{};
        }
        const auto& binding = resolution_.binding(id);
        const auto subject = "'" + std::string(names_.str(name.name.sym)) + "'";
        if (captured_.contains(id.value))
            return {Access::Immutable, "captured " + subject, binding.name.span, "a closure captures a read-only copy of it"};
        switch (binding.kind) {
            case BindingKind::Local: {
                const auto* found = local(id);
                if (!found) return {Access::Immutable, subject, binding.name.span, "bound here"};
                if (found->decl->binding == Binding::Var) return {Access::Mutable};
                return {Access::Immutable, subject, found->declared, "declared with 'let' here"};
            }
            case BindingKind::Pattern:
                return {Access::Immutable, subject, binding.name.span, "pattern bindings are immutable"};
            case BindingKind::Parameter: {
                if (closure_params_.contains(binding.name.span.lo))
                    return {Access::ReadOnly, subject, binding.name.span, "closure parameters are read-only"};
                const auto it = params_.find(binding.name.span.lo);
                if (it == params_.end() || it->second->binding != Binding::None) return {};
                if (it->second->conv == Convention::Inout) return {Access::Mutable};
                return {it->second->conv == Convention::Own ? Access::Immutable : Access::ReadOnly, subject,
                        binding.name.span, "parameters are read-only unless declared 'inout'"};
            }
            case BindingKind::Variable: {
                const auto* var = as<VarItem>(binding.item);
                if (!var) return {};
                if (var->binding == Binding::Var) return {Access::Mutable};
                return {Access::Immutable, subject, binding.name.span, "declared with 'let' here"};
            }
            case BindingKind::Field: return field_place(binding, self_, self_storage_);
            default: return {};
        }
    }

    static const Expr* projected(const Expr& expr) {
        if (const auto* member = as<MemberExpr>(&expr)) return member->base;
        if (const auto* index = as<IndexExpr>(&expr)) return index->base;
        if (const auto* unwrap = as<UnwrapExpr>(&expr)) return unwrap->operand;
        if (const auto* element = as<TupleFieldExpr>(&expr)) return element->base;
        return nullptr;
    }

    // Member, index, tuple element and unwrap chains are walked iteratively from
    // their root. Elements and unwrapped optionals keep the access of their container.
    Place place(const Expr* expr) const {
        ExprList chain;
        expr = unparen(expr);
        while (const Expr* base = projected(*expr)) {
            chain.push_back(expr);
            expr = unparen(base);
        }
        // Any root other than a name or `self` is a value computed on the spot.
        Place result{Access::Temporary, "a temporary value"};
        if (expr->kind == ExprKind::SelfValue || expr->kind == ExprKind::Super) result = self_;
        else if (const auto* name = as<NameExpr>(expr)) result = name_place(*name);
        for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
            const auto* member = as<MemberExpr>(*it);
            if (!member) continue;
            const auto* record = types_.member(member);
            if (!record || record->state != ResolutionState::Resolved || record->candidates.size() != 1 ||
                resolution_.binding(record->candidates.front()).kind != BindingKind::Field)
                return {};
            // A type receiver names a static field, which ignores the base.
            if (record->type_receiver) result = {};
            result = field_place(resolution_.binding(record->candidates.front()), result,
                                 storage(types_.expression(member->base)));
        }
        return result;
    }

    static std::string_view describe(Access access) {
        return access == Access::ReadOnly ? "read-only" : access == Access::Temporary ? "temporary" : "immutable";
    }
    void restricted(Span span, std::string message, const Place& place) {
        auto& d = diag_.error(DiagCode::ImmutableAssignment, span, "{}", message);
        if (!place.note.empty()) d.notes.push_back({place.declared, place.note});
    }

    // An assignment to a field or to `self`; plain names go through write().
    void write_place(const Expr& target) {
        const Expr* inner = unparen(&target);
        const auto* member = as<MemberExpr>(inner);
        const auto* tuple = as<TupleFieldExpr>(inner);
        const bool element = inner->kind == ExprKind::Index, unwrapped = inner->kind == ExprKind::Unwrap;
        if (!member && !element && !unwrapped && !tuple && inner->kind != ExprKind::SelfValue) return;
        const Place found = place(inner);
        if (found.access == Access::Unknown || found.access == Access::Mutable) return;
        if (element || unwrapped || tuple) {
            const auto what = tuple ? std::format("element {}", tuple->index)
                                    : std::string(element ? "an element" : "an unwrapped optional");
            if (found.access == Access::Temporary)
                restricted(target.span, std::format("cannot assign to {} of a temporary value", what), found);
            else
                restricted(target.span, std::format("cannot assign to {} because {} is {}", what, found.subject,
                                                    describe(found.access)), found);
            return;
        }
        if (!member) {
            restricted(target.span, std::format("cannot assign to {} 'self'", describe(found.access)), found);
            return;
        }
        const auto field = names_.str(member->name.sym);
        // place() resolved the member, so its record exists.
        const auto& binding = resolution_.binding(types_.member(member)->candidates.front());
        if (found.access == Access::Immutable && found.declared.lo == binding.name.span.lo)
            restricted(target.span, std::format("cannot assign to immutable field '{}'", field), found);
        else if (found.access == Access::Temporary)
            restricted(target.span, std::format("cannot assign to field '{}' of a temporary value", field), found);
        else
            restricted(target.span, std::format("cannot assign to field '{}' because {} is {}", field, found.subject,
                                                describe(found.access)), found);
    }

    // An `inout` argument must be a mutable place, whatever its type, since
    // the callee can replace the whole value.
    void inout_arguments(const CallExpr& call, const CheckedCall& checked, const FunItem& fun) {
        for (usize i = 0; i < checked.parameters.size() && i < call.args.size(); ++i) {
            const usize index = checked.parameters[i];
            if (index >= fun.sig.params.size() || fun.sig.params[index].conv != Convention::Inout) continue;
            const Place found = place(call.args[i].value);
            if (found.access == Access::Unknown || found.access == Access::Mutable) continue;
            const auto name = names_.str(fun.sig.params[index].name.sym);
            if (found.access == Access::Temporary)
                restricted(call.args[i].value->span,
                           std::format("cannot pass a temporary value to 'inout' parameter '{}'", name), found);
            else
                restricted(call.args[i].value->span,
                           std::format("the argument for 'inout' parameter '{}' must be mutable, but {} is {}", name,
                                       found.subject, describe(found.access)), found);
        }
    }

    // A `mut fun` needs a mutable receiver: a mutable place for value types,
    // and anything but a borrow for class instances.
    void mutating_call(const CallExpr& call) {
        const auto* checked = types_.call(&call);
        if (!checked || checked->state != CallState::Resolved ||
            (checked->kind != CallKind::Method && checked->kind != CallKind::Function))
            return;
        const auto* fun = as<FunItem>(resolution_.binding(checked->target).item);
        if (!fun) return;
        inout_arguments(call, *checked, *fun);
        if (checked->kind != CallKind::Method || !has_modifier(fun->mods, Modifier::Mut) ||
            has_modifier(fun->mods, Modifier::Static))
            return;
        Place found;
        Storage kind = self_storage_;
        if (checked->receiver) {
            found = place(checked->receiver);
            kind = storage(types_.expression(checked->receiver));
        } else {
            const Expr* callee = unparen(call.callee);
            if (const auto* generic = as<GenericInstExpr>(callee)) callee = unparen(generic->base);
            if (callee->kind == ExprKind::Name) found = self_;
        }
        const bool rejected = found.access == Access::ReadOnly ||
                              (kind == Storage::Value && (found.access == Access::Immutable || found.access == Access::Temporary));
        if (!rejected) return;
        const auto name = names_.str(fun->name.sym);
        if (found.access == Access::Temporary)
            restricted(call.callee->span, std::format("cannot call 'mut fun {}' on a temporary value", name), found);
        else
            restricted(call.callee->span, std::format("cannot call 'mut fun {}' because {} is {}", name, found.subject,
                                                      describe(found.access)), found);
    }

    // Defaults and initializers are separate roots with their own locals. Defaults keep the parameter
    // list so that writes to earlier parameters are still checked.
    void defaults(List<Param> params) {
        for (const auto& param : params)
            if (param.default_value) callable(params, param.default_value, nullptr);
    }

    void callable(List<Param> params, const Expr* body, const FunItem* fun) {
        start(body);
        for (const auto& param : params) params_.emplace(param.name.span.lo, &param);
        State state{Bits(tracked_, false), Bits(tracked_, false), true};
        eval(body, state);
        if (fun && !fun->expr_body && state.live && returns_value(*fun)) {
            const Span end{body->span.file, body->span.hi - 1, body->span.hi};
            auto& d = diag_.error(DiagCode::MissingReturn, end, "function '{}' must return '{}' on every path",
                                  names_.str(fun->name.sym), result_name(*fun));
            d.notes.push_back({fun->name.span, "the end of the body is reachable without a 'return'"});
        }
        // Each closure body is its own callable, checked after the body that
        // creates it. Its captures are read-only copies.
        while (!closures_.empty()) {
            const auto next = closures_.back();
            closures_.pop_back();
            start(next.closure->body);
            for (const auto& param : next.closure->params) closure_params_.emplace(param.name.span.lo, &param);
            for (const auto& capture : resolution_.captures(next.closure)) captured_.insert(capture.binding.value);
            self_ = next.self;
            self_storage_ = next.storage;
            self_owner_ = next.owner;
            State closure_state{Bits(tracked_, false), Bits(tracked_, false), true};
            eval(next.closure->body, closure_state);
        }
    }

    // Resets the per-callable tables and registers the locals of body.
    void start(const Expr* body) {
        params_.clear();
        closure_params_.clear();
        captured_.clear();
        locals_.clear();
        tracked_ = 0;
        each_stmt(body, [&](const Stmt& stmt) {
            if (const auto* var = as<VarStmt>(&stmt); var && var->names.empty())
                locals_.emplace(var->name.span.lo, Local{var, var->init ? kUntracked : tracked_++, var->name.span});
            else if (var)
                for (const Name& name : var->names) locals_.emplace(name.span.lo, Local{var, kUntracked, name.span});
        });
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
        d.notes.push_back({found->declared, "declared here without an initializer"});
    }

    // Creating a closure reads what it captures and copies it (3.29). Its body
    // is checked once the enclosing body is done.
    void create(const ClosureExpr& closure, const State& state) {
        for (const auto& capture : resolution_.captures(&closure)) {
            const auto& binding = resolution_.binding(capture.binding);
            const auto name = names_.str(binding.name.sym);
            if (const auto* found = local(capture.binding)) {
                if (found->tracked != kUntracked && state.live && !state.assigned.test(found->tracked)) {
                    auto& d = diag_.error(DiagCode::UninitializedUse, capture.use, "'{}' is captured before it is initialized", name);
                    d.notes.push_back({found->declared, "declared here without an initializer"});
                }
                continue;
            }
            if (binding.kind != BindingKind::Parameter) continue;
            // Parameters of an enclosing callable were checked where that callable's closures captured them.
            Convention convention = Convention::Default;
            // A promoted constructor parameter is still the parameter wherever it resolves
            // as one: in defaults, field initializers and the primary `init`.
            if (const auto it = params_.find(binding.name.span.lo); it != params_.end()) convention = it->second->conv; else if (!closure_params_.contains(binding.name.span.lo)) continue;
            if (convention == Convention::Inout) {
                auto& d = diag_.error(DiagCode::InvalidCapture, capture.use,
                                      "a closure cannot capture 'inout' parameter '{}', which is only borrowed for the call", name);
                d.notes.push_back({binding.name.span, "declared 'inout' here"});
            } else if (convention != Convention::Own && !copyable(types_.binding(capture.binding))) {
                auto& d = diag_.error(DiagCode::InvalidCapture, capture.use,
                                      "a closure cannot capture borrowed parameter '{}' of type '{}', which is not Copy; declare it 'own' to move it into the closure",
                                      name, type_name(types_.binding(capture.binding)));
                d.notes.push_back({binding.name.span, "parameters are borrowed unless declared 'own'"});
            }
        }
        PendingClosure pending{&closure, self_, self_storage_, self_owner_};
        if (self_storage_ != Storage::Unknown && self_owner_) {
            if (const auto use = uses_self(closure)) {
                // `self` is a parameter too: borrowed, or `inout` in a `mut fun`.
                const auto type = names_.str(self_owner_->name.sym);
                if (self_.access == Access::Mutable) {
                    diag_.error(DiagCode::InvalidCapture, *use,
                                "a closure cannot capture 'self' in a 'mut fun', where it is only borrowed for the call");
                } else if (!copyable(*self_owner_)) {
                    auto& d = diag_.error(DiagCode::InvalidCapture, *use,
                                          "a closure cannot capture 'self' of type '{}', which is borrowed and not Copy", type);
                    d.notes.push_back({self_owner_->name.span, "declared here"});
                }
                if (self_.access == Access::Mutable || !copyable(*self_owner_)) {
                    // Nested closures do not report it again.
                    pending.self = {};
                    pending.storage = Storage::Unknown;
                } else if (pending.self.access == Access::Unknown) {
                    // In an `init` or `deinit` too, the closure holds a read-only copy.
                    pending.self = {Access::Immutable, "the captured 'self'", {}, {}};
                }
            }
        }
        closures_.push_back(pending);
    }

    // Where a closure, or a closure inside it, uses `self`: explicitly, or
    // through a bare instance field or method name.
    std::optional<Span> uses_self(const ClosureExpr& closure) const {
        std::optional<Span> found;
        each_node(closure.body, true, [](const Stmt&) {}, [&](const Expr& expr) {
            if (found) return;
            if (expr.kind == ExprKind::SelfValue || expr.kind == ExprKind::Super) found = expr.span;
            else if (const auto* name = as<NameExpr>(&expr); name && instance_member(*name)) found = expr.span;
        });
        return found;
    }
    bool instance_member(const NameExpr& name) const {
        const auto instance = [&](BindingId id) {
            const auto& binding = resolution_.binding(id);
            if (binding.kind != BindingKind::Field && binding.kind != BindingKind::Function) return false;
            if (resolution_.scopes()[binding.scope.value].kind != ScopeKind::Members) return false;
            return !binding.item || !has_modifier(binding.item->mods, Modifier::Static);
        };
        if (const auto id = target(name); id.valid()) return instance(id);
        const auto* record = types_.member(&name);
        return record && record->state == ResolutionState::Resolved && std::ranges::any_of(record->candidates, instance);
    }

    // Whether values of a type are copied implicitly (3.2). Ownership checks
    // are later work, so stage0 decides it structurally: scalars, function
    // values and raw pointers are Copy, as are tuples, optionals, structs and
    // payload-free enums made of Copy parts. Unknown types do not report.
    // A generic parameter is Copy when the argument it stands for is; outside
    // an instance, such as in a declaration's own `self`, it may not be.
    bool copyable(ScalarType root) const {
        struct Part { ScalarType type; u32 env; };  // env: the instance whose arguments it sees
        std::vector<std::vector<std::pair<usize, Part>>> envs(1);
        std::vector<Part> work{{root, 0}};
        for (usize steps = 0; !work.empty(); ++steps) {
            if (steps > kCopyableSteps) return true;  // undecided; ownership checks decide later
            const auto [raw, env] = work.back();
            work.pop_back();
            const auto type = raw.scalar();
            switch (type.kind) {
                case ScalarType::String: case ScalarType::Array: case ScalarType::Shared: case ScalarType::Weak:
                case ScalarType::Projection:
                    return false;
                case ScalarType::TypeParameter: {
                    const auto declaration = types_.descriptor(type)->declaration.value;
                    const auto& bound = envs[env];
                    const auto it = std::ranges::find(bound, declaration, &std::pair<usize, Part>::first);
                    if (it == bound.end()) return false;
                    work.push_back(it->second);
                    break;
                }
                case ScalarType::Tuple:
                    for (const auto element : types_.descriptor(type)->parameters) work.push_back({element, env});
                    break;
                case ScalarType::Nominal: {
                    const auto* descriptor = types_.descriptor(type);
                    const Item* item = resolution_.binding(descriptor->declaration).item;
                    const auto* decl = item ? as_type_decl(item) : nullptr;
                    std::vector<ScalarType> fields;
                    if (!decl || !stored_fields(*decl, fields)) return false;
                    if (fields.empty()) break;
                    // The fields see this instance's arguments, as seen from where it is named.
                    std::vector<std::pair<usize, Part>> arguments;
                    const auto parameters = generic_parameters(*decl);
                    for (usize i = 0; i < parameters.size() && i < descriptor->arguments.size(); ++i)
                        arguments.push_back({parameters[i].value, {descriptor->arguments[i], env}});
                    envs.push_back(std::move(arguments));
                    for (const auto field : fields) work.push_back({field, static_cast<u32>(envs.size() - 1)});
                    break;
                }
                default: break;
            }
        }
        return true;
    }
    static constexpr usize kCopyableSteps = 4096;
    // A declaration's own `self`, whose generic fields stay symbolic.
    bool copyable(const TypeDeclItem& decl) const {
        std::vector<ScalarType> fields;
        if (!stored_fields(decl, fields)) return false;
        return std::ranges::all_of(fields, [&](ScalarType type) { return copyable(type); });
    }
    // The stored field types of a struct, or false when the declaration is
    // never Copy. Enums are Copy without payloads, and have no fields to check.
    bool stored_fields(const TypeDeclItem& decl, std::vector<ScalarType>& fields) const {
        if (decl.kind == ItemKind::Enum)
            return std::ranges::all_of(decl.members, [](const Item* member) {
                const auto* entry = as<EnumCaseItem>(member);
                return !entry || entry->fields.empty();
            });
        if (decl.kind != ItemKind::Struct) return false;
        const auto owner = items_.find(&decl);
        if (owner == items_.end()) return true;
        for (const BindingId id : resolution_.scopes()[resolution_.binding(owner->second).members.value].bindings) {
            const auto& binding = resolution_.binding(id);
            if (binding.kind != BindingKind::Field || (binding.item && has_modifier(binding.item->mods, Modifier::Static))) continue;
            fields.push_back(types_.binding(id));
        }
        return true;
    }
    // A type's generic parameters in declaration order, from the scope around its members.
    std::vector<BindingId> generic_parameters(const TypeDeclItem& decl) const {
        std::vector<BindingId> result;
        const auto owner = items_.find(&decl);
        if (owner == items_.end()) return result;
        const auto generics = resolution_.scopes()[resolution_.binding(owner->second).members.value].parent;
        for (const BindingId id : resolution_.scopes()[generics.value].bindings)
            if (resolution_.binding(id).kind == BindingKind::GenericParameter) result.push_back(id);
        return result;
    }
    std::string type_name(ScalarType type) const { return types_.type_name(type, resolution_, names_, 1024); }

    void immutable(const NameExpr& name, Span declared, std::string_view note) {
        auto& d = diag_.error(DiagCode::ImmutableAssignment, name.span, "cannot assign to immutable '{}'",
                              names_.str(name.name.sym));
        d.notes.push_back({declared, std::string(note)});
    }

    void write(const NameExpr& name, State& state) {
        const auto id = target(name);
        if (!id.valid()) {
            if (const auto* field = implicit_field(name)) write_field(name, *field);
            return;
        }
        const auto& binding = resolution_.binding(id);
        if (captured_.contains(id.value)) {
            auto& d = diag_.error(DiagCode::ImmutableAssignment, name.span,
                                  "cannot assign to captured '{}'; a closure captures a read-only copy of it",
                                  names_.str(name.name.sym));
            d.notes.push_back({binding.name.span, "declared outside the closure here"});
            return;
        }
        switch (binding.kind) {
            case BindingKind::Local: {
                const auto* found = local(id);
                if (!found) { immutable(name, binding.name.span, "bound here; only 'var' locals can be reassigned"); return; }
                const bool mutable_local = found->decl->binding == Binding::Var;
                if (found->tracked == kUntracked) {
                    if (!mutable_local) immutable(name, found->declared, "declared with 'let' here");
                    return;
                }
                if (!mutable_local && state.live && state.maybe.test(found->tracked)) {
                    auto& d = diag_.error(DiagCode::ImmutableAssignment, name.span,
                                          "'{}' may already be initialized; a 'let' binding is assigned only once",
                                          names_.str(name.name.sym));
                    d.notes.push_back({found->declared, "declared with 'let' here"});
                }
                state.assigned.set(found->tracked, true);
                state.maybe.set(found->tracked, true);
                return;
            }
            case BindingKind::Pattern:
                immutable(name, binding.name.span, "pattern bindings cannot be reassigned");
                return;
            case BindingKind::Parameter: {
                if (closure_params_.contains(binding.name.span.lo)) {
                    immutable(name, binding.name.span, "closure parameters are read-only");
                    return;
                }
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
            case BindingKind::Field: write_field(name, binding); return;
            default: return;
        }
    }

    // An assignment to a bare field name, through the implicit `self`.
    void write_field(const NameExpr& name, const ResolvedBinding& binding) {
        const Place found = field_place(binding, self_, self_storage_);
        if (found.access == Access::Unknown || found.access == Access::Mutable) return;
        const auto field = names_.str(name.name.sym);
        if (found.declared.lo != binding.name.span.lo)
            restricted(name.span, std::format("cannot assign to field '{}' because 'self' is {}", field,
                                              describe(found.access)), found);
        else
            restricted(name.span, std::format("cannot assign to immutable field '{}'", field), found);
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
        SmallVec<Frame, 16> stack{{root, false}};
        ExprList children;
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
            else if (const auto* closure = as<ClosureExpr>(expr)) create(*closure, state);
            else if (const auto* call = as<CallExpr>(expr)) {
                mutating_call(*call);
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
                // Later arms are unreachable after an unguarded catch-all.
                if (irrefutable && !arm.guard) break;
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
                if (const auto it = locals_.find(var.name.span.lo);
                    var.names.empty() && it != locals_.end() && it->second.tracked != kUntracked) {
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
                else write_place(*assign.target);
                break;
            }
        }
    }

    const Resolution& resolution_;
    const ScalarTypes& types_;
    const Interner& names_;
    DiagEngine& diag_;
    FlatMap<const Item*, BindingId> items_;
    FlatMap<u32, const Param*> params_;  // name span.lo -> parameter of the current callable
    FlatMap<u32, Local> locals_;         // filled before each callable is evaluated
    FlatMap<u32, Field> fields_;
    Place self_;
    Storage self_storage_ = Storage::Unknown;
    const TypeDeclItem* self_owner_ = nullptr;
    struct PendingClosure {
        const ClosureExpr* closure;
        Place self;  // the captured `self`, if the body uses it
        Storage storage;
        const TypeDeclItem* owner;
    };
    std::vector<PendingClosure> closures_;          // created by the current callable, checked after it
    FlatMap<u32, const ClosureParam*> closure_params_;  // name span.lo -> parameter of the current closure
    FlatSet<usize> captured_;                        // bindings the current closure captures
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
