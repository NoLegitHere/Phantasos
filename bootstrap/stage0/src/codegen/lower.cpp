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

#include <format>
#include <string>
#include <unordered_map>
#include <vector>

#include "codegen/mir.hpp"

namespace phs::mir {
namespace {
using namespace ast;

Type lower_type(ScalarType type) {
    PHS_ASSERT(type.optional_depth == 0);
    switch (type.kind) {
        case ScalarType::Int8: return Type::I8;
        case ScalarType::Int16: return Type::I16;
        case ScalarType::Int32: return Type::I32;
        case ScalarType::Int64: case ScalarType::Int: return Type::I64;
        case ScalarType::UInt8: return Type::U8;
        case ScalarType::UInt16: return Type::U16;
        case ScalarType::UInt32: return Type::U32;
        case ScalarType::UInt64: case ScalarType::UInt: return Type::U64;
        case ScalarType::Bool: return Type::Bool;
        case ScalarType::Unit: return Type::Unit;
        case ScalarType::Never: return Type::Never;
        default: PHS_ASSERT(false && "type outside the code generation subset"); unreachable();
    }
}

bool has_value(Type type) { return type != Type::Unit && type != Type::Never; }

const Expr* unparen(const Expr* expr) {
    while (const auto* paren = as<ParenExpr>(expr)) expr = paren->inner;
    return expr;
}

bool control(const Expr* expr) {
    expr = unparen(expr);
    return expr->kind == ExprKind::If || expr->kind == ExprKind::Block;
}

Op binary_op(BinOp op) {
    switch (op) {
        case BinOp::Add: return Op::Add;
        case BinOp::Sub: return Op::Sub;
        case BinOp::Mul: return Op::Mul;
        case BinOp::Div: return Op::Div;
        case BinOp::Rem: return Op::Rem;
        case BinOp::WrapAdd: return Op::WrapAdd;
        case BinOp::WrapSub: return Op::WrapSub;
        case BinOp::WrapMul: return Op::WrapMul;
        case BinOp::BitAnd: return Op::And;
        case BinOp::BitOr: return Op::Or;
        case BinOp::BitXor: return Op::Xor;
        case BinOp::Eq: return Op::Eq;
        case BinOp::Ne: return Op::Ne;
        case BinOp::Lt: return Op::Lt;
        case BinOp::Le: return Op::Le;
        case BinOp::Gt: return Op::Gt;
        case BinOp::Ge: return Op::Ge;
        default: PHS_ASSERT(false && "operator outside the code generation subset"); unreachable();
    }
}

Op assign_op(AssignOp op) {
    switch (op) {
        case AssignOp::Add: return Op::Add;
        case AssignOp::Sub: return Op::Sub;
        case AssignOp::Mul: return Op::Mul;
        case AssignOp::Div: return Op::Div;
        case AssignOp::Rem: return Op::Rem;
        case AssignOp::BitAnd: return Op::And;
        case AssignOp::BitOr: return Op::Or;
        case AssignOp::BitXor: return Op::Xor;
        default: PHS_ASSERT(false && "assignment outside the code generation subset"); unreachable();
    }
}

class Lowerer {
public:
    Lowerer(const Resolution& resolution, const ScalarTypes& types, const Interner& names)
        : resolution_(resolution), types_(types), names_(names) {
        for (usize id = 0; id < resolution.bindings().size(); ++id) {
            const auto& binding = resolution.bindings()[id];
            if (binding.item) items_.emplace(binding.item, BindingId{id});
            if (binding.kind == BindingKind::Local || binding.kind == BindingKind::Parameter)
                values_.emplace(binding.name.span.lo, BindingId{id});
        }
    }

    Program run(const File& file) {
        std::vector<const FunItem*> funs;
        std::unordered_map<std::string_view, u32> seen;
        for (const Item* item : file.items) {
            const auto* fun = cast<FunItem>(item);
            functions_.emplace(fun, static_cast<u32>(funs.size()));
            funs.push_back(fun);
            const auto name = names_.str(fun->name.sym);
            if (name == "main") program_.main = static_cast<u32>(funs.size() - 1);
            // Overloads get a numeric suffix so that every name is unique.
            const u32 index = seen[name]++;
            auto& out = program_.functions.emplace_back();
            out.name = index ? std::format("{}.{}", name, index) : std::string(name);
            out.span = fun->name.span;
        }
        for (usize i = 0; i < funs.size(); ++i) function(*funs[i], program_.functions[i]);
        return std::move(program_);
    }

private:
    struct Loop {
        u32 head, exit;
    };

    // ------------------------------------------------------------ building

    u32 block() {
        fun_->blocks.emplace_back();
        return static_cast<u32>(fun_->blocks.size() - 1);
    }
    Reg reg(Type type) {
        PHS_ASSERT(has_value(type));
        fun_->regs.push_back(type);
        return {static_cast<u32>(fun_->regs.size() - 1)};
    }
    void emit(Inst inst) { fun_->blocks[current_].insts.push_back(std::move(inst)); }
    void copy(Reg dst, Reg src) {
        if (dst.valid() && src.valid() && dst != src) emit({.op = Op::Copy, .dst = dst, .a = src});
    }
    Reg constant(Type type, u64 value) {
        const Reg dst = reg(type);
        emit({.op = Op::Const, .dst = dst, .imm = value});
        return dst;
    }
    // Ending a block starts an unreachable one; code after a jump lands there
    // and is pruned at the end.
    void end(Term term, Reg value = {}, u32 target = 0, u32 other = 0) {
        auto& b = fun_->blocks[current_];
        b.term = term;
        b.value = value;
        b.target = target;
        b.other = other;
        current_ = block();
    }

    // ------------------------------------------------------------ facts

    ScalarType scalar(const Expr* expr) const { return types_.expression(expr); }
    BindingId target(const NameExpr& name) const {
        const auto* ref = resolution_.reference(name.name.span);
        PHS_ASSERT(ref && ref->state == ResolutionState::Resolved);
        const auto ids = resolution_.candidates(ref->target);
        PHS_ASSERT(ids.size() == 1);
        return ids.front();
    }
    Reg local(const NameExpr& name) const { return locals_.at(target(name).value); }
    // A used branch's type is that of its first tail with a value.
    Type branch_type(const Expr* root) const {
        std::vector<const Expr*> work{root};
        while (!work.empty()) {
            const Expr* expr = unparen(work.back());
            work.pop_back();
            if (const auto* block = as<BlockExpr>(expr)) {
                if (const auto* tail = block->stmts.empty() ? nullptr : as<ExprStmt>(block->stmts.back()))
                    work.push_back(tail->expr);
            } else if (const auto* branch = as<IfExpr>(expr)) {
                if (branch->else_branch) work.push_back(branch->else_branch);
                work.push_back(branch->then_block);
            } else if (const auto type = lower_type(scalar(expr)); has_value(type)) return type;
        }
        return Type::Unit;
    }

    // ------------------------------------------------------------ items

    void function(const FunItem& item, Function& out) {
        fun_ = &out;
        locals_.clear();
        const ScalarSignature* sig = types_.signature(items_.at(&item));
        for (const auto& param : item.sig.params)
            locals_[values_.at(param.name.span.lo).value] = reg(lower_type(types_.binding(values_.at(param.name.span.lo))));
        out.params = static_cast<u32>(out.regs.size());
        out.result = lower_type(sig->result);
        current_ = block();
        if (item.expr_body) {
            const Reg result = any(item.body);
            end(Term::Ret, has_value(out.result) ? result : Reg{});
        } else {
            (void)branch(item.body, false);
            // Flow checks guarantee that a value-returning body cannot fall off its end.
            end(out.result == Type::Unit ? Term::Ret : Term::Unreachable);
        }
        prune();
    }

    // Threads jumps through empty blocks, then keeps the blocks reachable from
    // the entry in depth-first order, with a branch's true side first.
    void prune() {
        auto& blocks = fun_->blocks;
        constexpr u32 none = std::numeric_limits<u32>::max();
        const auto thread = [&](u32 target) {
            for (usize steps = 0; steps < blocks.size(); ++steps) {
                const auto& b = blocks[target];
                if (!b.insts.empty() || b.term != Term::Br || b.target == target) break;
                target = b.target;
            }
            return target;
        };
        for (auto& b : blocks) {
            if (b.term == Term::Br || b.term == Term::CondBr) b.target = thread(b.target);
            if (b.term == Term::CondBr) b.other = thread(b.other);
        }
        std::vector<u32> index(blocks.size(), none), order;
        std::vector<u32> work{0};
        while (!work.empty()) {
            const u32 next = work.back();
            work.pop_back();
            if (index[next] != none) continue;
            index[next] = static_cast<u32>(order.size());
            order.push_back(next);
            const auto& b = blocks[next];
            if (b.term == Term::CondBr) work.push_back(b.other);
            if (b.term == Term::Br || b.term == Term::CondBr) work.push_back(b.target);
        }
        std::vector<Block> kept;
        for (const u32 old : order) kept.push_back(std::move(blocks[old]));
        for (auto& b : kept) {
            b.target = b.term == Term::Br || b.term == Term::CondBr ? index[b.target] : 0;
            b.other = b.term == Term::CondBr ? index[b.other] : 0;
        }
        blocks = std::move(kept);
    }

    // ------------------------------------------------------------ statements

    void statement(const Stmt& stmt) {
        switch (stmt.kind) {
            case StmtKind::Var: {
                const auto& var = *cast<VarStmt>(&stmt);
                const Reg init = var.init ? any(var.init) : Reg{};
                const auto id = values_.at(var.name.span.lo);
                const Reg dst = reg(lower_type(types_.binding(id)));
                locals_[id.value] = dst;
                copy(dst, init);
                return;
            }
            case StmtKind::Assign: {
                const auto& assign = *cast<AssignStmt>(&stmt);
                const Reg dst = local(*cast<NameExpr>(assign.target));
                const Reg src = any(assign.value);
                if (assign.op == AssignOp::Assign) copy(dst, src);
                else if (src.valid()) emit({.op = assign_op(assign.op), .dst = dst, .a = dst, .b = src});
                return;
            }
            case StmtKind::Return: {
                const auto* ret = cast<ReturnStmt>(&stmt);
                const Reg result = ret->value ? any(ret->value) : Reg{};
                end(Term::Ret, has_value(fun_->result) ? result : Reg{});
                return;
            }
            case StmtKind::Break: end(Term::Br, {}, loops_.back().exit); return;
            case StmtKind::Continue: end(Term::Br, {}, loops_.back().head); return;
            case StmtKind::While: {
                const auto& loop = *cast<WhileStmt>(&stmt);
                const u32 head = block(), body = block(), exit = block();
                end(Term::Br, {}, head);
                current_ = head;
                const Reg cond = any(loop.cond.expr);
                end(Term::CondBr, cond, body, exit);
                current_ = body;
                loops_.push_back({head, exit});
                (void)branch(loop.body, false);
                loops_.pop_back();
                end(Term::Br, {}, head);
                current_ = exit;
                return;
            }
            case StmtKind::Loop: {
                const u32 body = block(), exit = block();
                end(Term::Br, {}, body);
                current_ = body;
                loops_.push_back({body, exit});
                (void)branch(cast<LoopStmt>(&stmt)->body, false);
                loops_.pop_back();
                end(Term::Br, {}, body);
                current_ = exit;
                return;
            }
            case StmtKind::Expr: {
                const Expr* expr = cast<ExprStmt>(&stmt)->expr;
                if (control(expr)) (void)branch(expr, false);
                else (void)value(expr);
                return;
            }
            default: PHS_ASSERT(false && "statement outside the code generation subset");
        }
    }

    // ------------------------------------------------------------ expressions

    Reg any(const Expr* expr) { return control(expr) ? branch(expr, true) : value(expr); }

    // Blocks and `if` chains. Recursion follows syntactic nesting; `else if`
    // chains are iterative.
    Reg branch(const Expr* root, bool used) {
        root = unparen(root);
        if (const auto* block = as<BlockExpr>(root)) {
            const usize count = block->stmts.size();
            for (usize i = 0; i < count; ++i) {
                const auto* tail = used && i + 1 == count ? as<ExprStmt>(block->stmts[i]) : nullptr;
                if (tail) return any(tail->expr);
                statement(*block->stmts[i]);
            }
            return {};
        }
        const auto type = used ? branch_type(root) : Type::Unit;
        const Reg result = has_value(type) ? reg(type) : Reg{};
        const u32 join = block();
        for (const auto* next = cast<IfExpr>(root); next;) {
            const Reg cond = any(next->cond.expr);
            const u32 then = block(), other = block();
            end(Term::CondBr, cond, then, other);
            current_ = then;
            copy(result, branch(next->then_block, used));
            end(Term::Br, {}, join);
            current_ = other;
            const Expr* rest = next->else_branch;
            next = rest ? as<IfExpr>(rest) : nullptr;
            if (!next) {
                if (rest) copy(result, branch(rest, used));
                end(Term::Br, {}, join);
            }
        }
        current_ = join;
        return result;
    }

    // Operators, names, literals and calls, with an explicit stack so that
    // long operator chains do not recurse.
    Reg value(const Expr* root) {
        struct Task {
            const Expr* expr;
            u32 stage = 0;
            Reg result;
            u32 join = 0;
        };
        std::vector<Task> tasks{{root}};
        std::vector<Reg> values;
        while (!tasks.empty()) {
            Task& task = tasks.back();
            const Expr* expr = task.expr;
            const u32 stage = task.stage++;
            switch (expr->kind) {
                case ExprKind::Int: {
                    tasks.pop_back();
                    values.push_back(constant(lower_type(scalar(expr)), cast<IntExpr>(expr)->value));
                    break;
                }
                case ExprKind::Bool:
                    tasks.pop_back();
                    values.push_back(constant(Type::Bool, cast<BoolExpr>(expr)->value));
                    break;
                case ExprKind::Unit:
                    tasks.pop_back();
                    values.push_back({});
                    break;
                case ExprKind::Name: {
                    tasks.pop_back();
                    values.push_back(local(*cast<NameExpr>(expr)));
                    break;
                }
                case ExprKind::If: case ExprKind::Block:
                    tasks.pop_back();
                    values.push_back(branch(expr, true));
                    break;
                case ExprKind::Paren:
                    if (stage == 0) tasks.push_back({cast<ParenExpr>(expr)->inner});
                    else tasks.pop_back();
                    break;
                case ExprKind::Unary: {
                    const auto& unary = *cast<UnaryExpr>(expr);
                    const auto* literal = as<IntExpr>(unparen(unary.operand));
                    if (unary.op == UnOp::Neg && literal) {
                        // Fold a negated literal, which may be the signed minimum.
                        tasks.pop_back();
                        values.push_back(constant(lower_type(scalar(expr)), u64{0} - literal->value));
                    } else if (stage == 0) tasks.push_back({unary.operand});
                    else {
                        tasks.pop_back();
                        const Reg operand = values.back();
                        values.pop_back();
                        const Op op = unary.op == UnOp::Neg ? Op::Neg : unary.op == UnOp::Not ? Op::Not : Op::BitNot;
                        const Reg dst = reg(lower_type(scalar(expr)));
                        emit({.op = op, .dst = dst, .a = operand});
                        values.push_back(dst);
                    }
                    break;
                }
                case ExprKind::Binary: {
                    const auto& binary = *cast<BinaryExpr>(expr);
                    if (binary.op == BinOp::And || binary.op == BinOp::Or) logic(task, stage, binary, tasks, values);
                    else if (stage == 0) tasks.push_back({binary.lhs});
                    else if (stage == 1) tasks.push_back({binary.rhs});
                    else {
                        tasks.pop_back();
                        const Reg rhs = values.back();
                        values.pop_back();
                        const Reg lhs = values.back();
                        values.pop_back();
                        values.push_back(arithmetic(binary, lhs, rhs));
                    }
                    break;
                }
                case ExprKind::Call: {
                    const auto& call = *cast<CallExpr>(expr);
                    if (stage < call.args.size()) {
                        tasks.push_back({call.args[stage].value});
                        break;
                    }
                    tasks.pop_back();
                    std::vector<Reg> args(values.end() - static_cast<std::ptrdiff_t>(call.args.size()), values.end());
                    values.resize(values.size() - call.args.size());
                    values.push_back(this->call(call, std::move(args)));
                    break;
                }
                default: PHS_ASSERT(false && "expression outside the code generation subset");
            }
        }
        PHS_ASSERT(values.size() == 1);
        return values.back();
    }

    // Short-circuit `&&` and `||`: the right operand runs only when needed.
    template <typename Task>
    void logic(Task& task, u32 stage, const BinaryExpr& binary, std::vector<Task>& tasks, std::vector<Reg>& values) {
        if (stage == 0) {
            tasks.push_back({binary.lhs});
            return;
        }
        if (stage == 1) {
            const Reg lhs = values.back();
            values.pop_back();
            task.result = reg(Type::Bool);
            copy(task.result, lhs);
            const u32 rhs = block();
            task.join = block();
            if (binary.op == BinOp::And) end(Term::CondBr, lhs, rhs, task.join);
            else end(Term::CondBr, lhs, task.join, rhs);
            current_ = rhs;
            tasks.push_back({binary.rhs});
            return;
        }
        const Reg result = task.result;
        const u32 join = task.join;
        tasks.pop_back();
        copy(result, values.back());
        values.pop_back();
        end(Term::Br, {}, join);
        current_ = join;
        values.push_back(result);
    }

    Reg arithmetic(const BinaryExpr& binary, Reg lhs, Reg rhs) {
        const Type type = lower_type(scalar(&binary));
        if (!lhs.valid() || !rhs.valid()) {
            // Unit operands: `() == ()` is true. A Never operand already ended the block.
            if (lower_type(scalar(binary.lhs)) == Type::Unit && lower_type(scalar(binary.rhs)) == Type::Unit)
                return constant(Type::Bool, binary.op == BinOp::Eq);
            return {};
        }
        const Reg dst = reg(type);
        emit({.op = binary_op(binary.op), .dst = dst, .a = lhs, .b = rhs});
        return dst;
    }

    Reg call(const CallExpr& call, std::vector<Reg> args) {
        const Type type = lower_type(scalar(&call));
        if (const CheckedConversion* conversion = types_.conversion(&call)) {
            if (!args.front().valid()) return {};
            if (type == lower_type(conversion->source)) return args.front();
            const Reg dst = reg(type);
            emit({.op = conversion->kind == ConversionKind::Truncating ? Op::Truncate : Op::Widen, .dst = dst,
                  .a = args.front()});
            return dst;
        }
        const CheckedCall& checked = *types_.call(&call);
        const auto& callee = *cast<FunItem>(resolution_.binding(checked.target).item);
        // Arguments run in source order and are passed in parameter order.
        std::vector<Reg> ordered(callee.sig.params.size());
        for (usize i = 0; i < args.size(); ++i) ordered[checked.parameters[i]] = args[i];
        const Reg dst = has_value(type) ? reg(type) : Reg{};
        emit({.op = Op::Call, .dst = dst, .callee = functions_.at(&callee), .args = std::move(ordered)});
        if (type == Type::Never) end(Term::Unreachable);
        return dst;
    }

    const Resolution& resolution_;
    const ScalarTypes& types_;
    const Interner& names_;
    std::unordered_map<const Item*, BindingId> items_;
    std::unordered_map<u32, BindingId> values_;  // locals and parameters by declaration offset
    std::unordered_map<const FunItem*, u32> functions_;
    Program program_;
    Function* fun_ = nullptr;
    u32 current_ = 0;
    std::unordered_map<usize, Reg> locals_;  // by binding ID
    std::vector<Loop> loops_;
};

}  // namespace

Program lower(const File& file, const Resolution& resolution, const ScalarTypes& types, const Interner& names) {
    return Lowerer(resolution, types, names).run(file);
}

}  // namespace phs::mir
