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

#include "sema/types.hpp"

#include <limits>
#include <optional>
#include <unordered_set>
#include <variant>

namespace phs {
namespace {
using namespace ast;
using Type = ScalarType;

bool integer(Type t) { return !t.optional_depth && t.kind >= Type::Int8 && t.kind <= Type::UInt; }
bool floating(Type t) { return t == Type::Float32 || t == Type::Float64; }
bool numeric(Type t) { return integer(t) || floating(t); }
bool unsigned_integer(Type t) { return !t.optional_depth && ((t.kind >= Type::UInt8 && t.kind <= Type::UInt64) || t == Type::UInt); }
bool known(Type t) { return t.kind != Type::Deferred && t.kind != Type::Error; }
bool promotes(Type from, Type to) { return known(from) && known(to) && from.scalar() == to.scalar() && from.optional_depth <= to.optional_depth; }
unsigned width(Type t) {
    switch (t.kind) {
        case Type::Int8: case Type::UInt8: return 8;
        case Type::Int16: case Type::UInt16: return 16;
        case Type::Int32: case Type::UInt32: return 32;
        default: return 64;
    }
}
bool widening(Type from, Type to) {
    if (from == Type::Never || from == to) return true;
    if (integer(from) && integer(to)) {
        if (unsigned_integer(from) == unsigned_integer(to)) return width(to) >= width(from);
        return unsigned_integer(from) && !unsigned_integer(to) && width(to) > width(from);
    }
    return from == Type::Float32 && to == Type::Float64;
}
u64 integer_limit(Type type, bool negative) {
    const unsigned bits = width(type);
    if (unsigned_integer(type)) return std::numeric_limits<u64>::max() >> (64 - bits);
    return (u64{1} << (bits - 1)) - 1 + static_cast<u64>(negative);
}

enum class Literal : u8 { None, Integer, Float, Array };
struct Value {
    Type type = Type::Deferred;
    Literal literal = Literal::None;
    bool context_pending = false; // a literal array still has an unknown concrete element anchor
};
bool numeric(Value v) { return v.literal == Literal::Integer || v.literal == Literal::Float || numeric(v.type); }
bool integer(Value v) { return v.literal == Literal::Integer || integer(v.type); }
bool unavailable(Value v) { return v.literal == Literal::None && !known(v.type); }
enum class OperandDomain : u8 { Numeric, Integer, Ordered };
bool in_domain(Value operand, OperandDomain domain) {
    // A pending or non-returning operand cannot disprove a domain requirement.
    if (unavailable(operand) || operand.type == Type::Never) return true;
    switch (domain) {
        case OperandDomain::Numeric: return numeric(operand);
        case OperandDomain::Integer: return integer(operand);
        case OperandDomain::Ordered: return numeric(operand) || operand.type == Type::Char;
    }
    unreachable();
}
Type default_type(Value v) {
    if (v.literal == Literal::Integer) return Type::Int;
    if (v.literal == Literal::Float) return Type::Float64;
    return v.type;
}

struct Node {
    const Expr* expr = nullptr; // null for a binding
    const FunItem* function = nullptr; // return-type inference node, not a value binding
    const TypeExpr* annotation = nullptr;
    const Expr* init = nullptr;
    std::vector<usize> deps;
    Value value;
    u8 state = 0; // unvisited, active, finished
    bool call_prepared = false;
};
struct Requirement {
    const Expr* value;
    Type expected;
    Span span;
    const Expr* target = nullptr; // assignment: type obtained after inference
    AssignOp assignment = AssignOp::Assign;
};
struct Walk {
    std::variant<const Item*, const Expr*, const Stmt*, const Pattern*> node;
    Type returns = Type::Deferred;
    bool discarded = false;
};
}  // namespace

std::string scalar_type_name(ScalarType type) {
    const auto base = [&]() -> std::string_view {
    switch (type.kind) {
        case Type::Deferred: return "<deferred>";
        case Type::Error: return "<error>";
        case Type::Int8: return "Int8";
        case Type::Int16: return "Int16";
        case Type::Int32: return "Int32";
        case Type::Int64: return "Int64";
        case Type::UInt8: return "UInt8";
        case Type::UInt16: return "UInt16";
        case Type::UInt32: return "UInt32";
        case Type::UInt64: return "UInt64";
        case Type::Int: return "Int";
        case Type::UInt: return "UInt";
        case Type::Float32: return "Float32";
        case Type::Float64: return "Float64";
        case Type::Bool: return "Bool";
        case Type::Char: return "Char";
        case Type::String: return "String";
        case Type::Unit: return "Unit";
        case Type::Never: return "Never";
        case Type::Nominal: return "<nominal>";
        case Type::Array: return "<array>";
        case Type::Pointer: return "<pointer>";
        case Type::Shared: return "<shared>";
        case Type::Weak: return "<weak>";
        case Type::Function: return "<function>";
    }
    unreachable();
    }();
    std::string result(base);
    result.append(type.optional_depth, '?');
    return result;
}

class ScalarChecker {
public:
    ScalarChecker(const Resolution& resolution, const Interner& names, DiagEngine& diag)
        : resolution_(resolution), names_(names), diag_(diag) {}

    ScalarTypes run(const File& file) {
        aliases_ = expand_aliases(resolution_, names_, diag_);
        if (diag_.has_errors(file.file)) {
            ScalarTypes result;
            result.bindings_.resize(resolution_.bindings().size());
            result.aliases_ = std::move(aliases_);
            return result;
        }
        for (auto& [id, fact] : aliases_.aliases_) {
            (void)id;
            if (!fact.syntax) continue;
            const Type type = annotation(fact.syntax);
            if (known(type)) {
                fact.state = ExpansionState::Resolved;
                fact.scalar = type;
                fact.optional_depth = type.optional_depth;
            }
        }
        // Binding nodes have the same indices as the resolver's BindingIds.
        nodes_.resize(resolution_.bindings().size());
        for (usize i = 0; i < resolution_.bindings().size(); ++i) {
            const auto& b = resolution_.bindings()[i];
            if (b.kind != BindingKind::BuiltinType && b.kind != BindingKind::BuiltinValue)
                declarations_.emplace(b.name.span.lo, i);
            if (b.kind == BindingKind::Function) {
                const auto* fun = cast<FunItem>(b.item);
                function_nodes_.emplace(i, nodes_.size());
                nodes_.push_back({.function = fun, .init = fun->expr_body && !fun->sig.ret ? fun->body : nullptr});
                ScalarSignature sig;
                parameter_indices_.try_emplace(fun);
                for (usize p = 0; p < fun->sig.params.size(); ++p) {
                    sig.parameters.push_back(annotation(fun->sig.params[p].type));
                    parameter_indices_[fun].emplace(fun->sig.params[p].name.sym.id, p);
                }
                sig.generic = !fun->sig.generics.empty();
                signatures_.emplace(i, std::move(sig));
            }
        }
        for (const Item* item : file.items) push(item);
        while (!walk_.empty()) {
            const auto next = walk_.back();
            walk_.pop_back();
            std::visit([&](auto node) { collect(*node, next.returns, next.discarded); }, next.node);
        }
        for (usize i = 0; i < nodes_.size(); ++i) {
            if (!nodes_[i].expr) dependency(i, nodes_[i].init);
        }
        // Explicit DFS over the dependency graph handles long expression trees
        // and forward references without using the native call stack.
        struct Frame { usize node, next; };
        std::vector<Frame> stack;
        for (usize root = 0; root < nodes_.size(); ++root) {
            if (nodes_[root].state != 0) continue;
            nodes_[root].state = 1;
            stack.push_back({root, 0});
            while (!stack.empty()) {
                auto& frame = stack.back();
                auto& node = nodes_[frame.node];
                if (frame.next < node.deps.size()) {
                    const auto dep = node.deps[frame.next++];
                    // An active dependency is a cycle. Its provisional value
                    // stays Deferred; initialization cycles need a later pass.
                    if (nodes_[dep].state == 0) {
                        nodes_[dep].state = 1;
                        stack.push_back({dep, 0});
                    }
                } else {
                    if (const auto* call = as<CallExpr>(node.expr); call && !node.call_prepared) {
                        // Shape and argument types select a target without its
                        // return type. Rejected overloads must not create edges
                        // (and artificial cycles) in the inference graph.
                        node.call_prepared = true;
                        node.value = prepare_call(*call);
                        const auto& record = calls_.at(call);
                        if (record.state == CallState::Resolved && record.kind == CallKind::Function) {
                            node.deps.push_back(function_nodes_.at(record.target.value));
                            continue;
                        }
                    } else evaluate(frame.node);
                    node.state = 2;
                    stack.pop_back();
                }
            }
        }
        for (const auto& requirement : requirements_) {
            const Type expected = requirement.target ? value(requirement.target).type : requirement.expected;
            if (requirement.target && requirement.assignment != AssignOp::Assign) {
                const bool ints = requirement.assignment >= AssignOp::BitAnd || requirement.assignment == AssignOp::Rem;
                const auto domain = ints ? OperandDomain::Integer : OperandDomain::Numeric;
                if (!in_domain(value(requirement.target), domain) || !in_domain(value(requirement.value), domain)) {
                    diag_.error(DiagCode::InvalidScalarOperand, requirement.span,
                                "compound assignment requires {} operands", ints ? "integer" : "numeric");
                    continue;
                }
            }
            (void)require(requirement.value, expected, requirement.span);
        }
        // Only context-free roots default. A literal inside a deferred call,
        // annotation or composite expression may still need a contextual type
        // (for example a UInt64 argument beyond the default Int range).
        for (const Expr* expr : defaults_) {
            const auto index = expressions_.at(expr);
            if (nodes_[index].value.literal != Literal::None) materialize(index, default_type(nodes_[index].value));
        }
        ScalarTypes result;
        for (const auto& [expr, index] : expressions_) result.expressions_.emplace(expr, nodes_[index].value.type);
        for (usize i = 0; i < resolution_.bindings().size(); ++i) result.bindings_.push_back(nodes_[i].value.type);
        for (auto& [id, sig] : signatures_) sig.result = nodes_[function_nodes_.at(id)].value.type;
        result.signatures_ = std::move(signatures_);
        result.calls_ = std::move(calls_);
        result.conversions_ = std::move(conversions_);
        result.aliases_ = std::move(aliases_);
        result.types_ = std::move(types_);
        return result;
    }

private:
    std::string scalar_type_name(Type type) const { return types_.name(type, resolution_, names_); }

    enum class Relation : u8 { Yes, No, Deferred };
    bool protocol(Type type) const {
        if (type.kind != Type::Nominal) return false;
        const auto* d = types_.descriptor(type);
        const auto& b = resolution_.binding(d->declaration);
        if (b.kind == BindingKind::BuiltinType) return true;
        const auto* item = cast_type_decl(b.item);
        return item->kind == ItemKind::Interface;
    }
    bool inherited(Type type) const {
        if (type.kind != Type::Nominal) return false;
        const auto& b = resolution_.binding(types_.descriptor(type)->declaration);
        return b.kind == BindingKind::Type && !cast_type_decl(b.item)->supers.empty();
    }
    Relation relation(Type from, Type to) const {
        if (promotes(from, to) || from == Type::Never) return Relation::Yes;
        // Shared descriptors form a DAG. Keep the pair directed, since function
        // parameters compare in the opposite direction from their results.
        auto hash = [](const std::pair<Type, Type>& pair) {
            return TypeDescriptorHash{}({.element = pair.first, .result = pair.second});
        };
        std::unordered_set<std::pair<Type, Type>, decltype(hash)> compared(0, hash);
        std::vector<std::pair<Type, Type>> pending{{from, to}};
        Relation result = Relation::Yes;
        while (!pending.empty()) {
            const auto [source, target] = pending.back(); pending.pop_back();
            if (promotes(source, target) || source == Type::Never) continue;
            if (!compared.emplace(source, target).second) continue;
            if (!known(source) || !known(target)) { result = Relation::Deferred; continue; }
            if (protocol(target)) { result = Relation::Deferred; continue; }
            if (source.optional_depth > target.optional_depth) return Relation::No;
            if (source.kind == Type::Nominal && target.kind == Type::Nominal && (protocol(source) || inherited(source) || inherited(target))) {
                result = Relation::Deferred; continue;
            }
            if (source.kind == Type::Function && target.kind == Type::Function) {
                const auto* a = types_.descriptor(source);
                const auto* b = types_.descriptor(target);
                if (a->parameters.size() != b->parameters.size()) return Relation::No;
                // Function variance/effect coercions are not established by
                // this pass, even when their component types are compatible.
                result = Relation::Deferred;
                for (usize p = 0; p < a->parameters.size(); ++p) pending.push_back({b->parameters[p], a->parameters[p]});
                pending.push_back({a->result, b->result});
                if (a->throws && b->throws) pending.push_back({a->error, b->error});
                continue;
            }
            if (source.kind == target.kind && (source.kind == Type::Shared || source.kind == Type::Weak)) {
                result = Relation::Deferred;
                pending.push_back({types_.descriptor(source)->element, types_.descriptor(target)->element});
                continue;
            }
            return Relation::No;
        }
        return result;
    }

    Type nominal(BindingId id) {
        const auto& b = resolution_.binding(id);
        if (b.kind != BindingKind::Type && b.kind != BindingKind::BuiltinType) return Type::Deferred;
        if (b.kind == BindingKind::Type && !cast_type_decl(b.item)->generics.empty()) return Type::Deferred;
        for (auto scope = b.scope; scope.valid(); scope = resolution_.scopes()[scope.value].parent) {
            if (resolution_.scopes()[scope.value].kind != ScopeKind::Generics) continue;
            for (auto binding : resolution_.scopes()[scope.value].bindings)
                if (resolution_.binding(binding).kind == BindingKind::GenericParameter) return Type::Deferred;
        }
        return types_.intern({.kind = Type::Nominal, .declaration = id});
    }

    Type annotation(const TypeExpr* type) {
        if (!type) return Type::Deferred;
        if (const auto it = annotations_.find(type); it != annotations_.end()) return it->second;
        struct Frame { const TypeExpr* syntax; bool ready = false; const TypeExpr* redirect = nullptr; };
        std::vector<Frame> work{{type}};
        std::unordered_map<const TypeExpr*, bool> active;
        const auto cached = [&](const TypeExpr* t) -> Type {
            if (!t) return Type::Unit;
            const auto it = annotations_.find(t);
            return it == annotations_.end() ? Type{} : it->second;
        };
        while (!work.empty()) {
            auto& frame = work.back();
            const auto* syntax = frame.syntax;
            if (annotations_.contains(syntax)) { work.pop_back(); continue; }
            if (!frame.ready) {
                frame.ready = true;
                active[syntax] = true;
                if (syntax->kind == TypeKind::Path || syntax->kind == TypeKind::Unit || syntax->kind == TypeKind::SelfType) {
                    const auto expanded = aliases_.type(syntax, resolution_);
                    if (!expanded.unknown_member.sym.empty())
                        diag_.error(DiagCode::UnknownType, expanded.unknown_member.span, "unknown type '{}' in '{}'",
                                    names_.str(expanded.unknown_member.sym), names_.str(resolution_.binding(expanded.member_owner).name.sym));
                    Type value = expanded.type.scalar;
                    if (!known(value) && expanded.type.state == ExpansionState::Resolved && expanded.type.target.valid()) {
                        value = nominal(expanded.type.target);
                        if (known(value)) value.optional_depth = expanded.type.optional_depth;
                    }
                    if (known(value) || value == Type::Error) { annotations_[syntax] = value; continue; }
                    if (expanded.type.syntax && expanded.type.syntax != syntax) {
                        frame.redirect = expanded.type.syntax;
                        if (!active.contains(frame.redirect)) work.push_back({frame.redirect});
                    } else annotations_[syntax] = Type::Deferred;
                    continue;
                }
                std::vector<const TypeExpr*> children;
                if (syntax->inner) children.push_back(syntax->inner);
                if (syntax->throws_type) children.push_back(syntax->throws_type);
                for (auto* child : syntax->args) children.push_back(child);
                for (auto* child : children)
                    if (!annotations_.contains(child) && !active.contains(child)) work.push_back({child});
                continue;
            }
            Type value;
            if (frame.redirect) value = cached(frame.redirect);
            else if (syntax->kind == TypeKind::Optional) value = cached(syntax->inner).optional();
            else if (syntax->kind == TypeKind::Fun) {
                TypeDescriptor descriptor{.kind = Type::Function, .result = cached(syntax->inner), .throws = syntax->throws};
                bool complete = known(descriptor.result);
                for (auto* param : syntax->args) { descriptor.parameters.push_back(cached(param)); complete &= known(cached(param)); }
                if (syntax->throws) {
                    if (syntax->throws_type) descriptor.error = cached(syntax->throws_type);
                    else for (usize i = 0; i < resolution_.bindings().size(); ++i) {
                        const auto& b = resolution_.bindings()[i];
                        if (b.kind == BindingKind::BuiltinType && names_.str(b.name.sym) == "Error") descriptor.error = nominal(BindingId{i});
                    }
                    complete &= known(descriptor.error);
                }
                if (complete) value = types_.intern(std::move(descriptor));
            } else if (known(cached(syntax->inner))) {
                Type::Kind kind;
                switch (syntax->kind) {
                    case TypeKind::Array: kind = Type::Array; break;
                    case TypeKind::Pointer: kind = Type::Pointer; break;
                    case TypeKind::Shared: kind = Type::Shared; break;
                    case TypeKind::Weak: kind = Type::Weak; break;
                    default: annotations_[syntax] = Type::Deferred; continue;
                }
                value = types_.intern({.kind = kind, .element = cached(syntax->inner), .mutable_pointer = syntax->is_mut});
            }
            annotations_[syntax] = value;
        }
        return annotations_.at(type);
    }

    std::span<const BindingId> callees(const Expr* expr) const {
        while (const auto* paren = as<ParenExpr>(expr)) expr = paren->inner;
        const auto* name = as<NameExpr>(expr);
        if (!name) return {};
        const auto* ref = resolution_.reference(name->name.span);
        return ref && ref->state == ResolutionState::Resolved ? resolution_.candidates(ref->target)
                                                             : std::span<const BindingId>{};
    }

    template <typename T>
    void push(const T* node, Type returns = Type::Deferred, bool discarded = false) {
        if (node) walk_.push_back({node, returns, discarded});
    }
    usize expression(const Expr* expr) {
        const auto [it, inserted] = expressions_.try_emplace(expr, nodes_.size());
        if (inserted) nodes_.push_back({.expr = expr});
        return it->second;
    }
    void dependency(usize parent, const Expr* expr) {
        if (!expr) return;
        const auto child = expression(expr);
        nodes_[parent].deps.push_back(child);
    }
    void child(usize parent, const Expr* expr, Type returns, bool discarded = false) {
        if (!expr) return;
        dependency(parent, expr);
        push(expr, returns, discarded);
    }
    Value value(const Expr* expr) const {
        return expr ? nodes_[expressions_.at(expr)].value : Value{Type::Unit};
    }
    void declare(Name name, const TypeExpr* type, const Expr* init) {
        const auto [first, last] = declarations_.equal_range(name.span.lo);
        for (auto it = first; it != last; ++it) {
            auto& node = nodes_[it->second];
            node.annotation = type;
            node.init = init;
        }
    }
    void parameters(List<Param> params) {
        for (const auto& p : params) {
            declare(p.name, p.type, nullptr);
            push(p.default_value);
            if (p.default_value) requirements_.push_back({p.default_value, annotation(p.type), p.default_value->span});
        }
    }
    void condition(const Cond& cond, Type returns) {
        push(cond.expr, returns);
        if (!cond.is_let) requirements_.push_back({cond.expr, Type::Bool, cond.span});
        // Conditional optional bindings are deferred along with optional types.
    }

    void collect(const Item& item, Type, bool) {
        for (const auto& attr : item.attrs)
            for (const auto& arg : attr.args) push(arg.value);
        if (const auto* type = as_type_decl(&item)) {
            parameters(type->ctor_params);
            for (const auto& base : type->supers)
                for (const auto& arg : base.args) push(arg.value);
            for (const Item* member : type->members) push(member);
        } else if (const auto* fun = as<FunItem>(&item)) {
            parameters(fun->sig.params);
            const Type returns = fun->sig.ret ? annotation(fun->sig.ret) : fun->expr_body ? Type::Deferred : Type::Unit;
            push(fun->body, returns, !fun->expr_body);
            if (fun->body && fun->expr_body && fun->sig.ret)
                requirements_.push_back({fun->body, returns, fun->body->span});
        } else if (const auto* var = as<VarItem>(&item)) {
            declare(var->name, var->type, var->init);
            push(var->init);
        } else if (const auto* ext = as<ExternItem>(&item)) {
            for (const Item* nested : ext->items) push(nested);
        } else if (const auto* init = as<InitItem>(&item)) {
            parameters(init->params);
            push(init->body, Type::Unit, true);
        } else if (const auto* deinit = as<DeinitItem>(&item)) push(deinit->body, Type::Unit, true);
    }

    void collect(const Stmt& stmt, Type returns, bool discarded) {
        switch (stmt.kind) {
            case StmtKind::Var: {
                const auto& var = *cast<VarStmt>(&stmt);
                declare(var.name, var.type, var.init);
                push(var.init, returns);
                break;
            }
            case StmtKind::Return: {
                const auto* ret = cast<ReturnStmt>(&stmt);
                push(ret->value, returns);
                requirements_.push_back({ret->value, returns, ret->value ? ret->value->span : ret->span});
                break;
            }
            case StmtKind::Throw: push(cast<ThrowStmt>(&stmt)->value, returns); break;
            case StmtKind::Expr: push(cast<ExprStmt>(&stmt)->expr, returns, discarded); break;
            case StmtKind::Assign: {
                const auto& assign = *cast<AssignStmt>(&stmt);
                push(assign.target, returns);
                push(assign.value, returns);
                requirements_.push_back({assign.value, Type::Deferred, assign.span, assign.target, assign.op});
                break;
            }
            case StmtKind::While: {
                const auto& loop = *cast<WhileStmt>(&stmt);
                condition(loop.cond, returns);
                push(loop.body, returns, true);
                break;
            }
            case StmtKind::For: {
                const auto& loop = *cast<ForStmt>(&stmt);
                push(loop.iter, returns);
                push(loop.body, returns, true);
                break;
            }
            case StmtKind::Loop: push(cast<LoopStmt>(&stmt)->body, returns, true); break;
            case StmtKind::Defer: push(cast<DeferStmt>(&stmt)->body, returns, true); break;
            case StmtKind::Break: case StmtKind::Continue: break;
        }
    }

    void collect(const Pattern& pat, Type returns, bool) {
        if (const auto* literal = as<LiteralPat>(&pat)) push(literal->literal, returns);
        else if (const auto* alt = as<AltPat>(&pat)) {
            for (const Pattern* p : alt->alts) push(p, returns);
        } else if (const auto* c = as<CasePat>(&pat)) {
            for (const Pattern* p : c->args) push(p, returns);
        }
    }

    void collect(const Expr& expr, Type returns, bool discarded) {
        const usize index = expression(&expr);
        if (discarded) defaults_.push_back(&expr);
        switch (expr.kind) {
            case ExprKind::Name: {
                const auto* ref = resolution_.reference(cast<NameExpr>(&expr)->name.span);
                if (ref && ref->state == ResolutionState::Resolved) {
                    const auto candidates = resolution_.candidates(ref->target);
                    if (candidates.size() == 1) nodes_[index].deps.push_back(candidates.front().value);
                }
                break;
            }
            case ExprKind::Paren: child(index, cast<ParenExpr>(&expr)->inner, returns, discarded); break;
            case ExprKind::Unary: child(index, cast<UnaryExpr>(&expr)->operand, returns); break;
            case ExprKind::Binary: {
                const auto& b = *cast<BinaryExpr>(&expr);
                child(index, b.lhs, returns);
                child(index, b.rhs, returns);
                break;
            }
            case ExprKind::Block: {
                const auto stmts = cast<BlockExpr>(&expr)->stmts;
                // Only the last statement can supply a block's deferred result.
                // Function, loop and cleanup bodies discard every expression.
                for (usize i = 0; i < stmts.size(); ++i) push(stmts[i], returns, discarded || i + 1 < stmts.size());
                break;
            }
            case ExprKind::If: {
                const auto& branch = *cast<IfExpr>(&expr);
                condition(branch.cond, returns);
                child(index, branch.then_block, returns, discarded);
                child(index, branch.else_branch, returns, discarded);
                break;
            }
            case ExprKind::Match: {
                const auto& match = *cast<MatchExpr>(&expr);
                child(index, match.scrutinee, returns);
                for (const auto& arm : match.arms) {
                    push(arm.pattern, returns);
                    child(index, arm.guard, returns);
                    child(index, arm.body, returns, discarded);
                    if (arm.guard) requirements_.push_back({arm.guard, Type::Bool, arm.guard->span});
                }
                break;
            }
            case ExprKind::Interp:
                for (const Expr* part : cast<InterpExpr>(&expr)->exprs) child(index, part, returns);
                break;
            case ExprKind::Array:
                for (const Expr* elem : cast<ArrayExpr>(&expr)->elems) child(index, elem, returns);
                break;
            case ExprKind::Call: {
                const auto& call = *cast<CallExpr>(&expr);
                child(index, call.callee, returns);
                for (const auto& arg : call.args) child(index, arg.value, returns);
                break;
            }
            case ExprKind::Member: child(index, cast<MemberExpr>(&expr)->base, returns); break;
            case ExprKind::OptMember: child(index, cast<OptMemberExpr>(&expr)->base, returns); break;
            case ExprKind::Index: {
                const auto& subscript = *cast<IndexExpr>(&expr);
                child(index, subscript.base, returns);
                child(index, subscript.index, returns);
                break;
            }
            case ExprKind::Cast: child(index, cast<CastExpr>(&expr)->operand, returns); break;
            case ExprKind::Unwrap: child(index, cast<UnwrapExpr>(&expr)->operand, returns); break;
            case ExprKind::GenericInst: child(index, cast<GenericInstExpr>(&expr)->base, returns); break;
            case ExprKind::Unsafe: child(index, cast<UnsafeExpr>(&expr)->block, returns, discarded); break;
            default: break;
        }
    }

    Value invalid(Span span, std::string_view domain) {
        diag_.error(DiagCode::InvalidScalarOperand, span, "operator requires {} operands", domain);
        return {Type::Error};
    }
    Value compatible(usize index, Type expected, Span span) {
        Value actual = nodes_[index].value;
        if (!known(expected) || unavailable(actual)) return actual;
        if (actual.literal == Literal::Array && expected.kind == Type::Array) {
            const auto array = materialize(index, expected.scalar());
            return !known(array.type) ? array : Value{expected};
        }
        if ((actual.literal == Literal::Integer && integer(expected.scalar())) ||
            (actual.literal == Literal::Float && floating(expected.scalar()))) {
            const auto literal = materialize(index, expected.scalar());
            return literal.type == Type::Error ? literal : Value{expected};
        }
        if (relation(default_type(actual), expected) == Relation::Deferred &&
            !(actual.literal == Literal::Array && expected.kind != Type::Array && !protocol(expected))) return {};
        if (actual.literal == Literal::None && promotes(actual.type, expected)) return {expected};
        if (actual.type == Type::Never) return actual;
        diag_.error(DiagCode::ScalarTypeMismatch, span, "expected '{}', found '{}'",
                    scalar_type_name(expected), scalar_type_name(default_type(actual)));
        if (actual.literal != Literal::None) materialize(index, default_type(actual));
        // Preserve the expression's actual type; the declaration/check fails.
        return {Type::Error};
    }
    Value require(const Expr* expr, Type expected, Span span) {
        if (expr) return compatible(expressions_.at(expr), expected, span);
        if (known(expected) && !promotes(Type::Unit, expected)) {
            diag_.error(DiagCode::ScalarTypeMismatch, span, "expected '{}', found 'Unit'", scalar_type_name(expected));
            return {Type::Error};
        }
        return {known(expected) ? expected : Type{Type::Unit}};
    }

    Value materialize(usize root, Type type) {
        if (nodes_[root].value.literal == Literal::Array) return materialize_array(root, type);
        struct Pending { usize index; bool negative; };
        std::vector<Pending> pending{{root, false}};
        std::vector<usize> visited;
        while (!pending.empty()) {
            const auto [index, negative] = pending.back();
            pending.pop_back();
            auto& node = nodes_[index];
            if (node.value.literal == Literal::None) continue;
            node.value = {type};
            visited.push_back(index);
            const Expr* expr = node.expr;
            if (const auto* number = as<IntExpr>(expr)) {
                if (number->value > integer_limit(type, negative)) {
                    diag_.error(DiagCode::ScalarLiteralRange, expr->span,
                                "integer literal is out of range for '{}'", scalar_type_name(type));
                    node.value = {Type::Error};
                }
            } else if (const auto* float_literal = as<FloatExpr>(expr)) {
                if (type == Type::Float32 && float_literal->value > std::numeric_limits<float>::max()) {
                    diag_.error(DiagCode::ScalarLiteralRange, expr->span, "float literal is out of range for 'Float32'");
                    node.value = {Type::Error};
                }
            } else if (const auto* paren = as<ParenExpr>(expr)) {
                pending.push_back({expressions_.at(paren->inner), negative});
            } else if (const auto* unary = as<UnaryExpr>(expr)) {
                if (unary->op == UnOp::Neg && unsigned_integer(type))
                    node.value = invalid(expr->span, "signed numeric");
                pending.push_back({expressions_.at(unary->operand), unary->op == UnOp::Neg});
            } else if (const auto* binary = as<BinaryExpr>(expr)) {
                pending.push_back({expressions_.at(binary->lhs), false});
                pending.push_back({expressions_.at(binary->rhs), false});
            }
        }
        for (auto it = visited.rbegin(); it != visited.rend(); ++it) {
            auto& node = nodes_[*it];
            for (const auto dep : node.deps)
                if (nodes_[dep].value.type == Type::Error) node.value = {Type::Error};
        }
        return nodes_[root].value;
    }

    Value materialize_array(usize root, Type expected) {
        struct Work { usize index; Type type; bool finish = false; };
        std::vector<Work> work{{root, expected}};
        while (!work.empty()) {
            const auto next = work.back(); work.pop_back();
            auto& node = nodes_[next.index];
            if (const auto* paren = as<ParenExpr>(node.expr)) {
                if (next.finish) node.value = value(paren->inner);
                else {
                    work.push_back({next.index, next.type, true});
                    work.push_back({expressions_.at(paren->inner), next.type});
                }
                continue;
            }
            const auto* array = cast<ArrayExpr>(node.expr);
            if (next.finish) {
                for (auto* element : array->elems) {
                    const auto child = value(element);
                    if (child.type == Type::Error) node.value = {Type::Error};
                    else if (unavailable(child) && node.value.type != Type::Error) node.value = {};
                }
                continue;
            }
            if (!known(next.type) || next.type.kind != Type::Array) { node.value = {}; continue; }
            const auto element_type = types_.descriptor(next.type)->element;
            node.value = {next.type.scalar()};
            work.push_back({next.index, next.type, true});
            for (auto* element : array->elems) {
                const usize child = expressions_.at(element);
                if (nodes_[child].value.literal == Literal::Array && element_type.kind == Type::Array) work.push_back({child, element_type});
                else {
                    const auto checked = compatible(child, element_type, element->span);
                    if (checked.type == Type::Error) node.value = checked;
                    else if (unavailable(checked) && node.value.type != Type::Error) node.value = {};
                }
            }
        }
        return nodes_[root].value;
    }

    Value binary(const BinaryExpr& expr) {
        auto lhs = value(expr.lhs), rhs = value(expr.rhs);
        const bool logical = expr.op == BinOp::And || expr.op == BinOp::Or;
        if (logical) {
            // Each Bool requirement is independent of the other operand's
            // pending type. Keep the result deferred until both are checked.
            const auto a = require(expr.lhs, Type::Bool, expr.lhs->span);
            const auto b = require(expr.rhs, Type::Bool, expr.rhs->span);
            if (a.type == Type::Error || b.type == Type::Error) return {Type::Error};
            if (unavailable(a) || unavailable(b)) return {};
            return {Type::Bool};
        }
        if (lhs.type == Type::Error || rhs.type == Type::Error) return {Type::Error};
        if (expr.op == BinOp::Coalesce || expr.op == BinOp::Range || expr.op == BinOp::RangeExcl) return {};
        const bool equality = expr.op == BinOp::Eq || expr.op == BinOp::Ne;
        if (equality && (lhs.type.optional_depth || rhs.type.optional_depth || lhs.type.kind >= Type::Nominal || rhs.type.kind >= Type::Nominal ||
                         lhs.literal == Literal::Array || rhs.literal == Literal::Array)) return {};
        const bool ordering = expr.op >= BinOp::Lt && expr.op <= BinOp::Ge;
        const bool ints = expr.op == BinOp::Rem || expr.op == BinOp::WrapAdd || expr.op == BinOp::WrapSub ||
                          expr.op == BinOp::WrapMul || expr.op == BinOp::BitOr || expr.op == BinOp::BitXor ||
                          expr.op == BinOp::BitAnd || expr.op == BinOp::Shl || expr.op == BinOp::Shr;
        const auto domain = ints ? OperandDomain::Integer : ordering ? OperandDomain::Ordered : OperandDomain::Numeric;
        if (!equality && (!in_domain(lhs, domain) || !in_domain(rhs, domain)))
            return invalid(expr.span, ints ? "integer" : "numeric");
        // Operand domains can fail independently; matching operand types and
        // inferring the result still require both operands to be available.
        if (unavailable(lhs) || unavailable(rhs)) return {};
        // Never can stand in for a required operand type. It does not make an
        // otherwise invalid operator (such as String arithmetic) available.
        if (lhs.type == Type::Never && rhs.type == Type::Never)
            return {equality || ordering ? Type::Bool : Type::Never};
        if (lhs.type == Type::Never) lhs = rhs;
        if (rhs.type == Type::Never) rhs = lhs;
        if (ordering && ((lhs.type == Type::Char) != (rhs.type == Type::Char))) return invalid(expr.span, "numeric");
        if (lhs.literal != Literal::None && rhs.literal == Literal::None)
            lhs = require(expr.lhs, rhs.type, expr.lhs->span);
        else if (rhs.literal != Literal::None && lhs.literal == Literal::None)
            rhs = require(expr.rhs, lhs.type, expr.rhs->span);
        if (lhs.type == Type::Error || rhs.type == Type::Error) return {Type::Error};
        if (lhs.type != rhs.type || lhs.literal != rhs.literal) {
            diag_.error(DiagCode::ScalarTypeMismatch, expr.span, "operator operands have different types '{}' and '{}'",
                        scalar_type_name(default_type(lhs)), scalar_type_name(default_type(rhs)));
            return {Type::Error};
        }
        if (equality || ordering) {
            // Comparisons have Bool results, so numeric operands must default
            // here rather than taking the result's contextual type.
            if (lhs.literal != Literal::None) {
                const auto a = materialize(expressions_.at(expr.lhs), default_type(lhs));
                const auto b = materialize(expressions_.at(expr.rhs), default_type(rhs));
                if (a.type == Type::Error || b.type == Type::Error) return {Type::Error};
            }
            return {Type::Bool};
        }
        return lhs;
    }

    bool fits_literal_tree(usize root, Type type) const {
        struct Pending { usize index; bool negative; };
        std::vector<Pending> pending{{root, false}};
        while (!pending.empty()) {
            const auto [index, negative] = pending.back();
            pending.pop_back();
            const auto& node = nodes_[index];
            if (node.value.literal == Literal::None) continue;
            if (const auto* number = as<IntExpr>(node.expr)) {
                if (number->value > integer_limit(type, negative)) return false;
            } else if (const auto* f = as<FloatExpr>(node.expr)) {
                if (type == Type::Float32 && f->value > std::numeric_limits<float>::max()) return false;
            } else if (const auto* paren = as<ParenExpr>(node.expr)) {
                pending.push_back({expressions_.at(paren->inner), negative});
            } else if (const auto* unary = as<UnaryExpr>(node.expr)) {
                if (unary->op == UnOp::Neg && unsigned_integer(type)) return false;
                pending.push_back({expressions_.at(unary->operand), unary->op == UnOp::Neg});
            } else if (const auto* binary = as<BinaryExpr>(node.expr)) {
                pending.push_back({expressions_.at(binary->lhs), false});
                pending.push_back({expressions_.at(binary->rhs), false});
            }
        }
        return true;
    }

    struct Probe {
        bool matches = true, deferred = false, fits = true;
        usize score = 0, lifts = 0;
    };
    Probe probe(usize root, Type expected) const {
        Probe result;
        std::vector<std::pair<usize, Type>> work{{root, expected}};
        while (!work.empty()) {
            const auto [index, target] = work.back(); work.pop_back();
            const auto actual = nodes_[index].value;
            if (!known(target) || unavailable(actual)) { result.deferred = true; continue; }
            if (actual.type == Type::Never) continue;
            if (actual.literal == Literal::Array) {
                if (const auto* paren = as<ParenExpr>(nodes_[index].expr)) {
                    work.push_back({expressions_.at(paren->inner), target}); continue;
                }
            }
            if (actual.literal == Literal::Array && target.kind == Type::Array) {
                result.lifts += target.optional_depth;
                const auto element = types_.descriptor(target)->element;
                for (auto* expr : cast<ArrayExpr>(nodes_[index].expr)->elems) work.push_back({expressions_.at(expr), element});
            } else if ((actual.literal == Literal::Integer && integer(target.scalar())) ||
                       (actual.literal == Literal::Float && floating(target.scalar()))) {
                result.score += target.scalar() != default_type(actual);
                result.lifts += target.optional_depth;
                result.fits &= fits_literal_tree(index, target.scalar());
            } else {
                if (actual.literal == Literal::Array && target.kind != Type::Array && !protocol(target)) {
                    result.matches = false; continue;
                }
                const auto match = relation(default_type(actual), target);
                if (match == Relation::Deferred) result.deferred = true;
                else if (match == Relation::No || actual.literal != Literal::None) result.matches = false;
                else result.lifts += target.optional_depth - actual.type.optional_depth;
            }
        }
        return result;
    }

    bool argument_mapping(const CallExpr& expr, List<Param> params, const std::unordered_map<u32, usize>& labels,
                          std::vector<usize>& mapping) const {
        std::vector<u8> used(params.size());
        usize positional = 0;
        bool labeled = false;
        for (const auto& arg : expr.args) {
            usize index;
            if (arg.label.sym.empty()) {
                if (labeled || positional == params.size()) return false;
                index = positional++;
            } else {
                labeled = true;
                const auto it = labels.find(arg.label.sym.id);
                if (it == labels.end()) return false;
                index = it->second;
            }
            if (used[index]) return false;
            used[index] = 1;
            mapping.push_back(index);
        }
        for (usize p = 0; p < used.size(); ++p)
            if (!used[p] && !params[p].default_value) return false;
        return true;
    }

    void candidate_notes(Diagnostic& d, std::span<const BindingId> candidates) const {
        for (BindingId id : candidates) {
            const auto& b = resolution_.binding(id);
            const auto* fun = cast<FunItem>(b.item);
            std::string signature = std::format("candidate '{}(", names_.str(b.name.sym));
            const auto& sig = signatures_.at(id.value);
            for (usize p = 0; p < sig.parameters.size(); ++p) {
                if (p) signature += ", ";
                signature += std::format("{}: {}{}", names_.str(fun->sig.params[p].name.sym),
                                         scalar_type_name(sig.parameters[p]), fun->sig.params[p].default_value ? " = ..." : "");
            }
            signature += ")'";
            d.notes.push_back({b.name.span, std::move(signature)});
        }
    }

    std::optional<Value> prepare_conversion(const CallExpr& expr) {
        const Expr* callee = expr.callee;
        while (const auto* paren = as<ParenExpr>(callee)) callee = paren->inner;
        ConversionKind kind = ConversionKind::Widening;
        auto target = aliases_.entity(callee, resolution_);
        if (target.state != ExpansionState::Resolved || target.optional_depth || !numeric(target.scalar)) {
            const auto* member = as<MemberExpr>(callee);
            if (!member) return std::nullopt;
            const auto name = names_.str(member->name.sym);
            if (name == "checked") kind = ConversionKind::Checked;
            else if (name == "truncating") kind = ConversionKind::Truncating;
            else return std::nullopt;
            target = aliases_.entity(member->base, resolution_);
        }
        if (target.state != ExpansionState::Resolved || target.optional_depth || !target.target.valid()) return std::nullopt;
        const auto& binding = resolution_.binding(target.target);
        if (binding.kind != BindingKind::BuiltinType) return std::nullopt;
        const Type destination = target.scalar;
        if (!numeric(destination)) return std::nullopt;

        auto& conversion = conversions_[&expr];
        conversion.kind = kind;
        conversion.destination = destination;
        auto& record = calls_[&expr];
        record.kind = CallKind::NumericConversion;
        record.target = target.target;
        const auto fail = [&]() -> Value {
            conversion.state = record.state = CallState::Error;
            return {Type::Error};
        };
        if (expr.args.size() != 1 || !expr.args[0].label.sym.empty()) {
            diag_.error(DiagCode::ConversionArguments, expr.span, "numeric conversion requires one positional argument");
            return fail();
        }
        record.parameters = {0};
        const Expr* arg = expr.args[0].value;
        Value source = value(arg);
        if (source.literal == Literal::None) conversion.source = source.type;
        if (source.type == Type::Error) return fail();
        const Expr* syntax = arg;
        while (const auto* paren = as<ParenExpr>(syntax)) syntax = paren->inner;
        if (syntax->kind == ExprKind::None || syntax->kind == ExprKind::Array) {
            diag_.error(DiagCode::ConversionOperand, arg->span,
                        "numeric conversion requires a non-optional numeric operand, found '{}'",
                        syntax->kind == ExprKind::None ? "none" : "array");
            return fail();
        }
        if (unavailable(source)) return Value{};
        if (!numeric(source) && source.type != Type::Never) {
            diag_.error(DiagCode::ConversionOperand, arg->span, "numeric conversion requires a non-optional numeric operand, found '{}'",
                        scalar_type_name(default_type(source)));
            return fail();
        }
        // Integer/floating interchange and floating narrowing are deliberately
        // deferred until their rounding and failure policies are specified.
        if (kind != ConversionKind::Widening && floating(destination)) return Value{};
        if (source.type != Type::Never) {
            const bool source_integer = integer(source);
            if (source_integer != integer(destination)) return Value{};
        }
        if (source.literal != Literal::None) {
            const Type context = kind == ConversionKind::Widening ? destination : default_type(source);
            source = materialize(expressions_.at(arg), context);
            if (source.type == Type::Error) return fail();
        }
        conversion.source = source.type;
        if (kind == ConversionKind::Widening && !widening(source.type, destination)) {
            diag_.error(DiagCode::NarrowingConversion, expr.span,
                        "conversion from '{}' to '{}' may narrow; use an explicit narrowing operation",
                        scalar_type_name(source.type), scalar_type_name(destination));
            return fail();
        }
        conversion.state = record.state = CallState::Resolved;
        return Value{kind == ConversionKind::Checked ? destination.optional() : destination};
    }

    std::optional<Value> prepare_constructor(const CallExpr& expr) {
        const auto entity = aliases_.entity(expr.callee, resolution_);
        if (entity.state != ExpansionState::Resolved || !entity.target.valid() || entity.optional_depth) return std::nullopt;
        const auto& binding = resolution_.binding(entity.target);
        if (binding.kind != BindingKind::Type) return std::nullopt;
        const auto* item = cast_type_decl(binding.item);
        auto& record = calls_[&expr];
        record.kind = CallKind::NominalConstructor;
        record.target = entity.target;
        const Type result = nominal(entity.target);
        if (!known(result)) return Value{};
        if (item->kind == ItemKind::Interface || has_modifier(item->mods, Modifier::Abstract)) {
            diag_.error(DiagCode::NotConstructible, expr.span, "type '{}' cannot be constructed directly", scalar_type_name(result));
            record.state = CallState::Error;
            return Value{Type::Error};
        }
        if (item->kind == ItemKind::Enum) return Value{};
        for (const Item* member : item->members)
            if (const auto* init = as<InitItem>(member); init && init->has_params) return Value{};
        std::unordered_map<u32, usize> labels;
        std::vector<Type> parameters;
        for (usize p = 0; p < item->ctor_params.size(); ++p) {
            labels.emplace(item->ctor_params[p].name.sym.id, p);
            parameters.push_back(annotation(item->ctor_params[p].type));
        }
        bool matches = argument_mapping(expr, item->ctor_params, labels, record.parameters), deferred = !item->where.empty(), fits = true;
        for (const auto& arg : expr.args) if (value(arg.value).type == Type::Error) {
            record.state = CallState::Error; return Value{Type::Error};
        }
        if (matches) for (usize a = 0; a < expr.args.size(); ++a) {
            const auto p = probe(expressions_.at(expr.args[a].value), parameters[record.parameters[a]]);
            matches &= p.matches; deferred |= p.deferred; fits &= p.fits;
        }
        if (!matches) {
            auto& d = diag_.error(DiagCode::NoMatchingCall, expr.span, "arguments do not match the primary constructor of '{}'", scalar_type_name(result));
            d.notes.push_back({binding.name.span, "type declared here"});
            record.state = CallState::Error;
            return Value{Type::Error};
        }
        if (deferred && fits) return Value{};
        record.state = CallState::Resolved;
        for (usize a = 0; a < expr.args.size(); ++a)
            if (require(expr.args[a].value, parameters[record.parameters[a]], expr.args[a].value->span).type == Type::Error)
                record.state = CallState::Error;
        return Value{record.state == CallState::Error ? Type{Type::Error} : result};
    }

    Value prepare_indirect(const CallExpr& expr, Type function) {
        const auto descriptor = *types_.descriptor(function);
        auto& record = calls_[&expr];
        record.kind = CallKind::IndirectFunction;
        const auto ids = callees(expr.callee);
        if (ids.size() == 1) record.target = ids.front();
        bool matches = expr.args.size() == descriptor.parameters.size(), deferred = false, fits = true;
        for (const auto& arg : expr.args) if (!arg.label.sym.empty()) matches = false;
        for (const auto& arg : expr.args) if (value(arg.value).type == Type::Error) {
            record.state = CallState::Error; return {Type::Error};
        }
        if (matches) for (usize a = 0; a < expr.args.size(); ++a) {
            const auto p = probe(expressions_.at(expr.args[a].value), descriptor.parameters[a]);
            matches &= p.matches; deferred |= p.deferred; fits &= p.fits;
        }
        if (!matches) {
            diag_.error(DiagCode::NoMatchingCall, expr.span, "arguments do not match function type '{}'", scalar_type_name(function));
            record.state = CallState::Error; return {Type::Error};
        }
        if (deferred && fits) return {};
        record.state = CallState::Resolved;
        for (usize a = 0; a < expr.args.size(); ++a) {
            record.parameters.push_back(a);
            if (require(expr.args[a].value, descriptor.parameters[a], expr.args[a].value->span).type == Type::Error) record.state = CallState::Error;
        }
        return {record.state == CallState::Error ? Type{Type::Error} : descriptor.result};
    }

    Value prepare_call(const CallExpr& expr) {
        if (auto conversion = prepare_conversion(expr)) return *conversion;
        if (auto constructor = prepare_constructor(expr)) return *constructor;
        auto& record = calls_[&expr];
        const Value callee = value(expr.callee);
        if (callee.type == Type::Error) { record.state = CallState::Error; return {Type::Error}; }
        for (const auto& arg : expr.args) {
            if (value(arg.value).type == Type::Error) { record.state = CallState::Error; return {Type::Error}; }
        }
        if (callee.type == Type::Never) return {Type::Never};
        if (callee.type.kind == Type::Function && !callee.type.optional_depth) return prepare_indirect(expr, callee.type);
        if (!unavailable(callee)) {
            diag_.error(DiagCode::NotCallable, expr.callee->span, "value of type '{}' is not callable",
                        scalar_type_name(default_type(callee)));
            record.state = CallState::Error;
            return {Type::Error};
        }
        const auto candidates = callees(expr.callee);
        if (candidates.empty()) return {};
        // Receiver rules, constructors and indirect callable values need later
        // type information. Check only direct file-level function overload sets.
        for (BindingId id : candidates) {
            const auto& b = resolution_.binding(id);
            if (b.kind != BindingKind::Function || resolution_.scopes()[b.scope.value].kind != ScopeKind::File) return {};
        }
        struct Match {
            BindingId id;
            std::vector<usize> parameters;
            usize score = 0;
            usize lifts = 0;
            bool deferred = false;
        };
        std::vector<Match> matches, out_of_range;
        for (BindingId id : candidates) {
            const auto* fun = cast<FunItem>(resolution_.binding(id).item);
            const auto& signature = signatures_.at(id.value);
            Match match{.id = id, .deferred = signature.generic || !fun->sig.where.empty()};
            if (!argument_mapping(expr, fun->sig.params, parameter_indices_.at(fun), match.parameters)) continue;
            bool compatible_args = true, fits = true;
            for (usize a = 0; a < expr.args.size(); ++a) {
                const Type expected = signature.parameters[match.parameters[a]];
                const auto p = probe(expressions_.at(expr.args[a].value), expected);
                match.deferred |= p.deferred;
                match.score += p.score; match.lifts += p.lifts;
                fits &= p.fits; compatible_args &= p.matches;
            }
            if (!compatible_args) continue;
            if (fits) matches.push_back(std::move(match));
            else out_of_range.push_back(std::move(match));
        }
        if (matches.empty() && out_of_range.size() != 1) {
            auto& d = diag_.error(DiagCode::NoMatchingCall, expr.span, "arguments do not match any callable signature");
            candidate_notes(d, candidates);
            record.state = CallState::Error;
            return {Type::Error};
        }
        Match* selected = nullptr;
        if (matches.empty()) {
            // One otherwise matching signature: retain the target and emit the
            // precise E0402/operand diagnostic instead of a generic mismatch.
            selected = &out_of_range.front();
        } else {
            for (const auto& match : matches) if (match.deferred) return {};
            auto best = std::pair{std::numeric_limits<usize>::max(), std::numeric_limits<usize>::max()};
            usize ties = 0;
            for (auto& match : matches) {
                const auto rank = std::pair{match.score, match.lifts};
                if (rank < best) { best = rank; selected = &match; ties = 1; }
                else if (rank == best) ++ties;
            }
            if (ties != 1) {
                auto& d = diag_.error(DiagCode::AmbiguousCall, expr.span, "call is ambiguous between matching signatures");
                candidate_notes(d, candidates);
                record.state = CallState::Error;
                return {Type::Error};
            }
        }
        PHS_ASSERT(selected);
        record.target = selected->id;
        record.parameters = std::move(selected->parameters);
        record.state = CallState::Resolved;
        const auto& signature = signatures_.at(selected->id.value);
        for (usize a = 0; a < expr.args.size(); ++a) {
            if (require(expr.args[a].value, signature.parameters[record.parameters[a]], expr.args[a].value->span).type == Type::Error)
                record.state = CallState::Error;
        }
        if (record.state == CallState::Error) return {Type::Error};
        // The traversal schedules the selected return dependency next.
        return {};
    }

    void evaluate(usize index) {
        auto& node = nodes_[index];
        const Expr* expr = node.expr;
        if (node.function) {
            const auto* fun = node.function;
            if (fun->sig.ret) node.value = {annotation(fun->sig.ret)};
            else if (fun->expr_body && fun->body) {
                node.value = value(fun->body);
                if (node.value.literal != Literal::None)
                    node.value = materialize(expressions_.at(fun->body), default_type(node.value));
            } else node.value = {Type::Unit};
            return;
        }
        if (!expr) {
            const Type declared = annotation(node.annotation);
            Value initial = node.init ? value(node.init) : Value{};
            if (node.annotation) {
                if (node.init && known(declared)) initial = require(node.init, declared, node.init->span);
                node.value = {initial.type == Type::Error ? Type::Error : declared};
            } else if (node.init) {
                if (initial.literal != Literal::None) initial = materialize(expressions_.at(node.init), default_type(initial));
                node.value = initial;
            }
            return;
        }
        switch (expr->kind) {
            case ExprKind::Int: node.value.literal = Literal::Integer; break;
            case ExprKind::Float: node.value.literal = Literal::Float; break;
            case ExprKind::Bool: node.value = {Type::Bool}; break;
            case ExprKind::Char: node.value = {Type::Char}; break;
            case ExprKind::String: case ExprKind::Interp: node.value = {Type::String}; break;
            case ExprKind::Unit: node.value = {Type::Unit}; break;
            case ExprKind::SelfValue: {
                const auto* reference = resolution_.reference(expr->span);
                if (reference && reference->state == ResolutionState::Resolved) {
                    const auto ids = resolution_.candidates(reference->target);
                    if (ids.size() == 1) node.value = {nominal(ids.front())};
                }
                break;
            }
            case ExprKind::Array: {
                const auto* array = cast<ArrayExpr>(expr);
                Type element;
                bool context_pending = false;
                for (auto* child : array->elems) {
                    const auto value_type = value(child);
                    if (value_type.type == Type::Error) { element = Type::Error; break; }
                    if (value_type.type == Type::Never) continue;
                    context_pending |= unavailable(value_type) || value_type.context_pending;
                    if (!known(element) && value_type.literal == Literal::None && known(value_type.type)) element = value_type.type;
                }
                if (element != Type::Error) {
                    // Empty/Never-only siblings provide no anchor. An unknown
                    // concrete element, however, can still supply literal context.
                    if (!known(element) && !context_pending) for (auto* child : array->elems) {
                        const auto candidate = value(child);
                        if (candidate.type == Type::Never) continue;
                        const auto type = default_type(candidate);
                        if (known(type)) { element = type; break; }
                    }
                }
                if (element == Type::Error) node.value = {Type::Error};
                else node.value = {known(element) ? types_.intern({.kind = Type::Array, .element = element}) : Type{}, Literal::Array, context_pending && !known(element)};
                break;
            }
            case ExprKind::Index: {
                const auto& subscript = *cast<IndexExpr>(expr);
                const auto base = value(subscript.base), index_value = value(subscript.index);
                if (base.type == Type::Error || index_value.type == Type::Error) { node.value = {Type::Error}; break; }
                if (base.type.kind != Type::Array && base.literal != Literal::Array) break;
                const Expr* index_syntax = subscript.index;
                while (const auto* paren = as<ParenExpr>(index_syntax)) index_syntax = paren->inner;
                if (base.type.optional_depth || index_syntax->kind == ExprKind::None || !in_domain(index_value, OperandDomain::Integer)) {
                    diag_.error(DiagCode::InvalidArrayIndex, expr->span, "array indexing requires a non-optional array and integer index");
                    node.value = {Type::Error}; break;
                }
                if (base.type.kind != Type::Array) break;
                if (unavailable(index_value)) break;
                if (index_value.literal != Literal::None && materialize(expressions_.at(subscript.index), Type::Int).type == Type::Error) {
                    node.value = {Type::Error}; break;
                }
                if (base.literal == Literal::Array && !known(materialize(expressions_.at(subscript.base), base.type).type)) break;
                node.value = {types_.descriptor(base.type)->element};
                break;
            }
            case ExprKind::Name:
                if (!node.deps.empty()) node.value = nodes_[node.deps.front()].value;
                break;
            case ExprKind::Paren: node.value = value(cast<ParenExpr>(expr)->inner); break;
            case ExprKind::Binary: node.value = binary(*cast<BinaryExpr>(expr)); break;
            case ExprKind::Unwrap: {
                const auto operand = value(cast<UnwrapExpr>(expr)->operand);
                if (operand.type.optional_depth) node.value = {operand.type.element()};
                else if (operand.type == Type::Error) node.value = operand;
                break;
            }
            case ExprKind::Call: {
                auto& record = calls_.at(cast<CallExpr>(expr));
                PHS_ASSERT(node.call_prepared && record.state == CallState::Resolved);
                node.value = nodes_[function_nodes_.at(record.target.value)].value;
                if (node.value.type == Type::Error) record.state = CallState::Error;
                break;
            }
            case ExprKind::Unary: {
                const auto& unary = *cast<UnaryExpr>(expr);
                const auto operand = value(unary.operand);
                if (unary.op == UnOp::Not) {
                    const auto checked = require(unary.operand, Type::Bool, unary.operand->span);
                    node.value = {unavailable(checked) ? checked.type : Type::Bool};
                } else if (unary.op == UnOp::Neg || unary.op == UnOp::BitNot) {
                    node.value = operand;
                    if (!unavailable(operand) && operand.type != Type::Never &&
                        ((unary.op == UnOp::Neg && (!numeric(operand) || unsigned_integer(operand.type))) ||
                         (unary.op == UnOp::BitNot && !integer(operand))))
                        node.value = invalid(expr->span, unary.op == UnOp::Neg ? "signed numeric" : "integer");
                }
                break;
            }
            default: break;
        }
    }

    const Resolution& resolution_;
    const Interner& names_;
    DiagEngine& diag_;
    AliasExpansion aliases_;
    TypeTable types_;
    std::unordered_map<const TypeExpr*, Type> annotations_;
    std::vector<Node> nodes_;
    std::unordered_map<const Expr*, usize> expressions_;
    // A promoted constructor field and its parameter have separate binding
    // IDs at the same name span. Both inherit the declaration's annotation.
    std::unordered_multimap<u32, usize> declarations_;
    std::unordered_map<usize, usize> function_nodes_;
    std::unordered_map<usize, ScalarSignature> signatures_;
    std::unordered_map<const FunItem*, std::unordered_map<u32, usize>> parameter_indices_;
    std::unordered_map<const ast::CallExpr*, CheckedCall> calls_;
    std::unordered_map<const ast::CallExpr*, CheckedConversion> conversions_;
    std::vector<Walk> walk_;
    std::vector<Requirement> requirements_;
    std::vector<const Expr*> defaults_;
};

ScalarTypes check_scalar_types(const ast::File& file, const Resolution& resolution,
                              const Interner& names, DiagEngine& diag) {
    return ScalarChecker(resolution, names, diag).run(file);
}

}  // namespace phs
