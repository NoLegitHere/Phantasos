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

#include <algorithm>
#include <format>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "codegen/mir.hpp"

namespace phs::mir {
namespace {
using namespace ast;

Type scalar_type(ScalarType type) {
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
        case ScalarType::String: return Type::Str;
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
        std::unordered_map<std::string, u32> seen;
        // Every struct in the file passed the gate; a struct's layout follows
        // those of the structs it holds.
        for (const Item* item : file.items)
            if (item->kind == ItemKind::Struct) (void)structure(items_.at(item));
        const auto add = [&](const FunItem& fun, std::string name) {
            functions_.emplace(&fun, static_cast<u32>(funs.size()));
            funs.push_back(&fun);
            // Overloads get a numeric suffix so that every name is unique.
            const u32 index = seen[name]++;
            auto& out = program_.functions.emplace_back();
            out.name = index ? std::format("{}.{}", name, index) : name;
            out.span = fun.name.span;
        };
        for (const Item* item : file.items) {
            if (item->kind == ItemKind::Struct) {
                // Methods are named after their struct and follow it.
                const auto& type = *as_type_decl(item);
                for (const Item* member : type.members)
                    if (const auto* method = as<FunItem>(member)) {
                        owners_.emplace(method, items_.at(item));
                        add(*method, std::format("{}.{}", names_.str(type.name.sym), names_.str(method->name.sym)));
                    }
                continue;
            }
            const auto* fun = as<FunItem>(item);
            if (!fun) continue;
            if (names_.str(fun->name.sym) == "main") program_.main = static_cast<u32>(funs.size());
            add(*fun, std::string(names_.str(fun->name.sym)));
        }
        for (usize i = 0; i < funs.size(); ++i) function(*funs[i], program_.functions[i]);
        return std::move(program_);
    }

private:
    struct Loop {
        u32 head, exit;
    };
    // A local or the receiver, and the fields below it from the outside in.
    struct Place {
        Reg root;
        std::vector<u32> fields;
        std::vector<Type> types;  // the type of each field
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

    Type lower_type(ScalarType type) {
        if (type.kind != ScalarType::Nominal) return scalar_type(type);
        PHS_ASSERT(type.optional_depth == 0);
        return structure(types_.descriptor(type)->declaration);
    }

    // A struct's layout, built once; the gate rejects recursive structs.
    Type structure(BindingId declaration) {
        if (const auto it = layouts_.find(declaration.value); it != layouts_.end()) return aggregate(it->second);
        const auto& item = *as_type_decl(resolution_.binding(declaration).item);
        Layout layout;
        layout.name = names_.str(item.name.sym);
        for (usize i = 0; i < item.ctor_params.size(); ++i) {
            const auto& param = item.ctor_params[i];
            layout.fields.push_back(lower_type(types_.binding(values_.at(param.name.span.lo))));
            layout.names.emplace_back(names_.str(param.name.sym));
            fields_.emplace(param.name.span.lo, static_cast<u32>(i));
        }
        place_fields(program_, layout);
        const auto index = static_cast<u32>(program_.layouts.size());
        program_.layouts.push_back(std::move(layout));
        layouts_.emplace(declaration.value, index);
        return aggregate(index);
    }
    // The declaration index of the field a member expression reads.
    u32 field(const MemberExpr& member) {
        (void)lower_type(scalar(member.base));
        const auto* record = types_.member(&member);
        PHS_ASSERT(record && record->candidates.size() == 1);
        return fields_.at(resolution_.binding(record->candidates.front()).name.span.lo);
    }

    ScalarType scalar(const Expr* expr) const { return types_.expression(expr); }
    BindingId target(const NameExpr& name) const {
        const auto* ref = resolution_.reference(name.name.span);
        PHS_ASSERT(ref && ref->state == ResolutionState::Resolved);
        const auto ids = resolution_.candidates(ref->target);
        PHS_ASSERT(ids.size() == 1);
        return ids.front();
    }
    Reg local(const NameExpr& name) const { return locals_.at(target(name).value); }
    // The declaration index of the receiver's field that a bare name inside a method reads.
    std::optional<u32> implicit_field(const NameExpr& name) const {
        const auto& binding = resolution_.binding(target(name));
        if (binding.kind != BindingKind::Field) return {};
        return fields_.at(binding.name.span.lo);
    }
    static bool mutating(const FunItem& fun) { return has_modifier(fun.mods, Modifier::Mut); }
    bool mutating_call(const CallExpr& call) const {
        const CheckedCall* checked = types_.call(&call);
        return checked && checked->kind == CallKind::Method &&
               mutating(*cast<FunItem>(resolution_.binding(checked->target).item));
    }
    // A used branch's type is that of its first tail with a value.
    Type branch_type(const Expr* root) {
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
        self_ = {};
        const ScalarSignature* sig = types_.signature(items_.at(&item));
        // A method's receiver is its hidden first parameter (abi.md A.6),
        // inout for a `mut fun`.
        if (const auto owner = owners_.find(&item); owner != owners_.end()) {
            self_ = reg(structure(owner->second));
            if (mutating(item)) out.inout = {true};
        }
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
                const Place assigned = place(assign.target);
                if (!assigned.fields.empty()) return assign_field(assign, assigned);
                const Reg dst = assigned.root;
                if (assign.op == AssignOp::Assign) {
                    copy(dst, any(assign.value));
                    return;
                }
                // The target is read first; snapshot it if the value can assign it.
                const Reg target = writes(assign.value) ? snapshot(dst) : dst;
                const Reg src = any(assign.value);
                if (src.valid()) emit({.op = assign_op(assign.op), .dst = dst, .a = target, .b = src});
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

    // A field of a local struct or of the receiver, possibly nested: the new
    // value is built into each enclosing struct in turn, from the root's value
    // after the assigned value was evaluated. Compound assignment reads the
    // field first.
    void assign_field(const AssignStmt& assign, const Place& target) {
        Reg src;
        if (assign.op == AssignOp::Assign) src = any(assign.value);
        else {
            const Reg old = value(assign.target);
            const Reg rhs = any(assign.value);
            if (rhs.valid()) {
                src = reg(fun_->regs[old.id]);
                emit({.op = assign_op(assign.op), .dst = src, .a = old, .b = rhs});
            }
        }
        // A diverging value has already ended the block.
        if (!src.valid()) return;
        auto owners = read(target, target.fields.size() - 1);
        owners.push_back(src);
        rebuild(target, owners);
    }

    // ------------------------------------------------------------ places

    Place place(const Expr* expr) {
        Place out;
        expr = unparen(expr);
        for (const auto* member = as<MemberExpr>(expr); member; member = as<MemberExpr>(expr)) {
            out.fields.push_back(field(*member));
            out.types.push_back(lower_type(scalar(member)));
            expr = unparen(member->base);
        }
        if (const auto* name = as<NameExpr>(expr); name && implicit_field(*name)) {
            out.fields.push_back(implicit_field(*name).value());
            out.types.push_back(lower_type(scalar(name)));
            out.root = self_;
        } else out.root = name ? local(*name) : self_;
        std::reverse(out.fields.begin(), out.fields.end());
        std::reverse(out.types.begin(), out.types.end());
        return out;
    }
    // The root and the values of its first depth fields, each read from the previous.
    std::vector<Reg> read(const Place& place, usize depth) {
        std::vector<Reg> owners{place.root};
        for (usize i = 0; i < depth; ++i) {
            const Reg inner = reg(place.types[i]);
            emit({.op = Op::Field, .dst = inner, .a = owners.back(), .imm = place.fields[i]});
            owners.push_back(inner);
        }
        return owners;
    }
    // Writes owners.back() into the place's last field, rebuilding every
    // enclosing struct up to the root in place.
    void rebuild(const Place& place, const std::vector<Reg>& owners) {
        for (usize i = place.fields.size(); i-- > 0;)
            emit({.op = Op::With, .dst = owners[i], .a = owners[i], .b = owners[i + 1], .imm = place.fields[i]});
    }

    // ------------------------------------------------------------ expressions

    Reg any(const Expr* expr) { return control(expr) ? branch(expr, true) : value(expr); }

    Reg snapshot(Reg local) {
        const Reg dst = reg(fun_->regs[local.id]);
        emit({.op = Op::Copy, .dst = dst, .a = local});
        return dst;
    }

    // Only statements inside a block or `if`, and `mut fun` calls, can
    // assign a local or the receiver while an expression is evaluated.
    bool writes(const Expr* root) const {
        std::vector<const Expr*> work{root};
        while (!work.empty()) {
            const Expr* expr = work.back();
            work.pop_back();
            if (expr->kind == ExprKind::If || expr->kind == ExprKind::Block) return true;
            if (const auto* call = as<CallExpr>(expr); call && mutating_call(*call)) return true;
            if (const auto* paren = as<ParenExpr>(expr)) work.push_back(paren->inner);
            else if (const auto* unary = as<UnaryExpr>(expr)) work.push_back(unary->operand);
            else if (const auto* binary = as<BinaryExpr>(expr)) {
                work.push_back(binary->lhs);
                work.push_back(binary->rhs);
            } else if (const auto* call = as<CallExpr>(expr)) {
                for (const auto& arg : call->args) work.push_back(arg.value);
            } else if (const auto* member = as<MemberExpr>(expr)) {
                work.push_back(member->base);
            }
        }
        return false;
    }

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
        // A local read early in the expression keeps its value even if a later
        // operand assigns it.
        const bool snapshots = root->kind != ExprKind::Name && writes(root);
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
                case ExprKind::String: {
                    tasks.pop_back();
                    const auto bytes = names_.str(cast<StringExpr>(expr)->value);
                    const auto [found, added] = strings_.try_emplace(std::string(bytes), program_.strings.size());
                    if (added) program_.strings.emplace_back(bytes);
                    const Reg dst = reg(Type::Str);
                    emit({.op = Op::Str, .dst = dst, .imm = found->second});
                    values.push_back(dst);
                    break;
                }
                case ExprKind::Unit:
                    tasks.pop_back();
                    values.push_back({});
                    break;
                case ExprKind::Name: {
                    tasks.pop_back();
                    const auto& name = *cast<NameExpr>(expr);
                    if (const auto index = implicit_field(name)) {
                        // A bare field name inside a method reads the receiver's field.
                        const Reg dst = reg(lower_type(scalar(expr)));
                        emit({.op = Op::Field, .dst = dst, .a = self_, .imm = *index});
                        values.push_back(dst);
                        break;
                    }
                    const Reg local = this->local(name);
                    values.push_back(snapshots ? snapshot(local) : local);
                    break;
                }
                case ExprKind::SelfValue:
                    tasks.pop_back();
                    values.push_back(snapshots ? snapshot(self_) : self_);
                    break;
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
                        // A diverging operand has already ended the block.
                        if (!operand.valid()) {
                            values.push_back({});
                            break;
                        }
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
                case ExprKind::Member: {
                    const auto& member = *cast<MemberExpr>(expr);
                    if (stage == 0) {
                        tasks.push_back({member.base});
                        break;
                    }
                    tasks.pop_back();
                    const Reg base = values.back();
                    values.pop_back();
                    // A diverging base has already ended the block.
                    if (!base.valid()) {
                        values.push_back({});
                        break;
                    }
                    const u32 index = field(member);
                    const Reg dst = reg(lower_type(scalar(expr)));
                    emit({.op = Op::Field, .dst = dst, .a = base, .imm = index});
                    values.push_back(dst);
                    break;
                }
                case ExprKind::Call: {
                    const auto& call = *cast<CallExpr>(expr);
                    // A method's receiver value runs before the arguments; a
                    // `mut fun` receiver is a place, read at the call.
                    const CheckedCall* checked = types_.call(&call);
                    const bool method = checked && checked->kind == CallKind::Method && !mutating_call(call);
                    const usize first = method ? 1 : 0;
                    if (stage < first) {
                        if (checked->receiver) tasks.push_back({checked->receiver});
                        else values.push_back(snapshots ? snapshot(self_) : self_);
                        break;
                    }
                    if (stage - first < call.args.size()) {
                        tasks.push_back({call.args[stage - first].value});
                        break;
                    }
                    tasks.pop_back();
                    const auto count = static_cast<std::ptrdiff_t>(call.args.size() + first);
                    std::vector<Reg> args(values.end() - count, values.end());
                    values.resize(values.size() - static_cast<usize>(count));
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
        if (checked.kind == CallKind::Builtin) {
            // print: a diverging argument has already ended the block.
            if (args.front().valid()) emit({.op = Op::Print, .a = args.front()});
            return {};
        }
        if (checked.kind == CallKind::NominalConstructor) {
            // A diverging argument has already ended the block.
            for (const Reg arg : args)
                if (!arg.valid()) return {};
            std::vector<Reg> fields(args.size());
            for (usize i = 0; i < args.size(); ++i) fields[checked.parameters[i]] = args[i];
            const Reg dst = reg(type);
            emit({.op = Op::Make, .dst = dst, .args = std::move(fields)});
            return dst;
        }
        const auto& callee = *cast<FunItem>(resolution_.binding(checked.target).item);
        // Arguments run in source order and are passed in parameter order,
        // after a method's receiver.
        const bool method = checked.kind == CallKind::Method;
        const bool inout = method && mutating(callee);
        const usize first = method && !inout ? 1 : 0;
        std::vector<Reg> ordered(callee.sig.params.size() + (method ? 1 : 0));
        if (first) ordered.front() = args.front();
        for (usize i = first; i < args.size(); ++i) ordered[checked.parameters[i - first] + (method ? 1 : 0)] = args[i];
        // A diverging operand has already ended the block.
        for (const Reg arg : args)
            if (!arg.valid()) return {};
        Place receiver;
        std::vector<Reg> owners;
        if (inout) {
            // The receiver's place is read now and written back after the call.
            receiver = checked.receiver ? place(checked.receiver) : Place{self_, {}, {}};
            owners = read(receiver, receiver.fields.size());
            ordered.front() = owners.back();
        }
        const Reg dst = has_value(type) ? reg(type) : Reg{};
        emit({.op = Op::Call, .dst = dst, .callee = functions_.at(&callee), .args = std::move(ordered)});
        if (type == Type::Never) end(Term::Unreachable);
        else if (inout) rebuild(receiver, owners);
        return dst;
    }

    const Resolution& resolution_;
    const ScalarTypes& types_;
    const Interner& names_;
    std::unordered_map<const Item*, BindingId> items_;
    std::unordered_map<u32, BindingId> values_;  // locals and parameters by declaration offset
    std::unordered_map<const FunItem*, u32> functions_;
    std::unordered_map<std::string, u64> strings_;  // literal bytes to their index in program_.strings
    std::unordered_map<usize, u32> layouts_;  // struct declaration binding to its index in program_.layouts
    std::unordered_map<u32, u32> fields_;     // field declaration offset to its declaration index
    std::unordered_map<const FunItem*, BindingId> owners_;  // a method's struct
    Reg self_;  // the current method's receiver
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
