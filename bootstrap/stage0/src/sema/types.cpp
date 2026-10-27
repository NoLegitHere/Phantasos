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
#include "support/scoped_value.hpp"

#include <limits>
#include <memory>
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
    bool concrete_anchor = false; // a literal array's type came from concrete elements, not numeric defaults
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
    bool reference_prepared = false;
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
        case Type::TypeParameter: return "<type-parameter>";
        case Type::Projection: return "<projection>";
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
        // Binding nodes have the same indices as the resolver's BindingIds.
        for (usize i = 0; i < resolution_.bindings().size(); ++i) {
            const auto& b = resolution_.bindings()[i];
            if (b.kind == BindingKind::GenericParameter) generic_bindings_[b.name.span.lo] = BindingId{i};
            if (b.members.valid()) member_owners_.emplace(b.members.value, BindingId{i});
            if (resolution_.scopes()[b.scope.value].kind == ScopeKind::Members &&
                b.kind != BindingKind::Type && b.kind != BindingKind::TypeAlias)
                member_values_[b.scope.value][b.name.sym.id].push_back(BindingId{i});
        }
        normalize_aliases();
        build_hierarchies();
        hierarchy_ready_ = true;
        collect_formal_bounds();
        complete_hierarchy_types();
        collect_formal_bounds();
        collect_bound_uses();
        clear_deferred_annotations();
        nodes_.resize(resolution_.bindings().size());
        for (usize i = 0; i < resolution_.bindings().size(); ++i) {
            const auto& b = resolution_.bindings()[i];
            if (b.kind != BindingKind::BuiltinType && b.kind != BindingKind::BuiltinValue)
                declarations_.emplace(b.name.span.lo, i);
            if (b.kind == BindingKind::Function) {
                const auto* fun = cast<FunItem>(b.item);
                function_nodes_.emplace(i, nodes_.size());
                nodes_.push_back({.function = fun, .init = fun->expr_body && !fun->sig.ret ? fun->body : nullptr,
                                  .value = {fun->sig.ret ? annotation(fun->sig.ret) : fun->expr_body ? Type{Type::Deferred} : Type{Type::Unit}}});
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
                    if (node.expr && !node.reference_prepared &&
                        (node.expr->kind == ExprKind::Member || node.expr->kind == ExprKind::Name)) {
                        node.reference_prepared = true;
                        if (const auto field = prepare_reference(node.expr)) {
                            node.deps.push_back(field->value);
                            continue;
                        }
                    }
                    if (const auto* call = as<CallExpr>(node.expr); call && !node.call_prepared) {
                        // Shape and argument types select a target without its
                        // return type. Rejected overloads must not create edges
                        // (and artificial cycles) in the inference graph.
                        node.call_prepared = true;
                        node.value = prepare_call(*call);
                        const auto& record = calls_.at(call);
                        if (record.state == CallState::Deferred && !pending_dependencies_.empty()) {
                            // Resume after consulted defaults or constraint methods
                            // have available facts. Active cycles remain deferred.
                            node.deps.insert(node.deps.end(), pending_dependencies_.begin(), pending_dependencies_.end());
                            node.call_prepared = false;
                            continue;
                        }
                        if (record.state == CallState::Resolved && (record.kind == CallKind::Function || record.kind == CallKind::Method)) {
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
        check_super_arguments();
        check_hierarchy_contracts();
        validate_constraints();
        // Only context-free roots default. A literal inside a deferred call,
        // annotation or composite expression may still need a contextual type
        // (for example a UInt64 argument beyond the default Int range).
        for (const Expr* expr : defaults_) {
            const auto index = expressions_.at(expr);
            if (nodes_[index].value.literal != Literal::None) materialize(index, default_type(nodes_[index].value));
        }
        retain_bodies();
        ScalarTypes result;
        for (const auto& [expr, index] : expressions_) result.expressions_.emplace(expr, nodes_[index].value.type);
        for (usize i = 0; i < resolution_.bindings().size(); ++i) result.bindings_.push_back(nodes_[i].value.type);
        for (auto& [id, sig] : signatures_) sig.result = nodes_[function_nodes_.at(id)].value.type;
        result.signatures_ = std::move(signatures_);
        result.calls_ = std::move(calls_);
        result.conversions_ = std::move(conversions_);
        result.members_ = std::move(members_);
        result.hierarchies_ = std::move(hierarchies_);
        result.aliases_ = std::move(aliases_);
        result.types_ = std::move(types_);
        result.constraints_ = std::move(constraints_);
        result.bodies_ = std::move(bodies_);
        return result;
    }

private:
    static constexpr usize kDependentDepth = 64;
    std::string scalar_type_name(Type type) const { return types_.name(type, resolution_, names_, 1024); }

    enum class Relation : u8 { Yes, No, Deferred };
    static bool symbolic(Type type) { return type.kind == Type::TypeParameter || type.kind == Type::Projection; }
    Relation formal_relation(Type source, Type target) const {
        std::vector<Type> work{source.scalar()};
        std::unordered_set<usize> seen;
        Relation result = Relation::No;
        while (!work.empty()) {
            const auto current = work.back(); work.pop_back();
            if (current == target.scalar()) return Relation::Yes;
            if (!seen.insert(current.detail).second) continue;
            if (uncertain_bounds_.contains(current.detail)) result = Relation::Deferred;
            const auto append = [&](const auto& table) {
                if (const auto it = table.find(current.detail); it != table.end())
                    for (auto bound : it->second) work.push_back(bound);
            };
            append(formal_bounds_); append(projection_bounds_);
            if (symbolic(current)) continue;
            if (current.kind != Type::Nominal || target.kind != Type::Nominal) continue;
            const auto* a = types_.descriptor(current);
            const auto* b = types_.descriptor(target);
            if (resolution_.binding(a->declaration).kind == BindingKind::BuiltinType) { result = Relation::Deferred; continue; }
            for (const auto& clause : cast_type_decl(resolution_.binding(a->declaration).item)->where)
                if (clause.type->kind == TypeKind::SelfType) result = Relation::Deferred;
            if (!a->arguments.empty() || !b->arguments.empty()) {
                if (a->declaration != b->declaration) result = Relation::Deferred;
                continue;
            }
            const auto match = ancestry(a->declaration, b->declaration);
            if (match == Relation::Yes) return match;
            if (match == Relation::Deferred) result = match;
        }
        return result;
    }
    bool protocol(Type type) const {
        if (type.kind != Type::Nominal) return false;
        const auto* d = types_.descriptor(type);
        const auto& b = resolution_.binding(d->declaration);
        if (b.kind == BindingKind::BuiltinType) return true;
        const auto* item = cast_type_decl(b.item);
        return item->kind == ItemKind::Interface;
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
            if (protocol(target) && resolution_.binding(types_.descriptor(target)->declaration).kind == BindingKind::BuiltinType) {
                if (symbolic(source) && !source.optional_depth) {
                    const auto match = formal_relation(source, target);
                    if (match == Relation::No) return match;
                    if (match == Relation::Deferred) result = match;
                } else result = Relation::Deferred;
                continue;
            }
            if (source.optional_depth > target.optional_depth) return Relation::No;
            if (symbolic(source) || formal_bounds_.contains(source.detail) || uncertain_bounds_.contains(source.detail)) {
                const auto match = formal_relation(source, target);
                if (match == Relation::No) return match;
                if (match == Relation::Deferred) result = match;
                continue;
            }
            if (symbolic(target)) return Relation::No;
            if (source.kind == Type::Nominal && target.kind == Type::Nominal) {
                const auto* a = types_.descriptor(source);
                const auto* b = types_.descriptor(target);
                if (a->declaration == b->declaration) return Relation::No; // different invariant arguments
                const auto match = ancestry(types_.descriptor(source)->declaration, types_.descriptor(target)->declaration);
                if (match == Relation::No) return Relation::No;
                if (!b->arguments.empty()) { result = Relation::Deferred; continue; }
                if (match == Relation::Deferred) result = Relation::Deferred;
                continue;
            }
            if (protocol(target)) { result = Relation::Deferred; continue; }
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
        for (auto scope = b.scope; scope.valid(); scope = resolution_.scopes()[scope.value].parent) {
            if (resolution_.scopes()[scope.value].kind != ScopeKind::Generics) continue;
            for (auto binding : resolution_.scopes()[scope.value].bindings)
                if (resolution_.binding(binding).kind == BindingKind::GenericParameter) return Type::Deferred;
        }
        std::vector<Type> args;
        for (const auto& param : generic_parameters(id)) args.push_back(symbol(generic_bindings_.at(param.name.span.lo)));
        return types_.intern({.kind = Type::Nominal, .declaration = id, .arguments = std::move(args)});
    }

    Type symbol(BindingId id) { return types_.intern({.kind = Type::TypeParameter, .declaration = id}); }
    Type formal_argument(BindingId id, usize context) {
        const auto value = substituted(id, context);
        return known(value) ? value : symbol(id);
    }

    List<GenericParam> generic_parameters(BindingId id) const {
        const auto* item = resolution_.binding(id).item;
        if (const auto* type = as_type_decl(item)) return type->generics;
        if (const auto* fun = as<FunItem>(item)) return fun->sig.generics;
        if (const auto* alias = as<TypeAliasItem>(item)) return alias->generics;
        return {};
    }
    bool constrained(BindingId id) const {
        for (const auto& param : generic_parameters(id)) if (!param.bounds.empty()) return true;
        const auto* item = resolution_.binding(id).item;
        if (const auto* type = as_type_decl(item)) return !type->where.empty();
        if (const auto* fun = as<FunItem>(item)) return !fun->sig.where.empty();
        return false;
    }
    struct Environment {
        BindingId declaration;
        std::vector<Type> arguments;
        usize parent = 0;
        Type self;
    };
    Type substituted(BindingId parameter, usize context) const {
        while (context) {
            const auto& env = environments_[context];
            const auto params = generic_parameters(env.declaration);
            for (usize p = 0; p < params.size(); ++p)
                if (generic_bindings_.at(params[p].name.span.lo) == parameter) return env.arguments[p];
            context = env.parent;
        }
        return Type::Deferred;
    }
    struct Application { ExpansionState state = ExpansionState::Deferred; usize context = 0; };
    Application application(BindingId id, std::vector<Type> args, usize parent, Span span, Type self = {}) {
        if (args.size() != generic_parameters(id).size()) {
            if (reported_applications_.insert(span.lo).second)
                diag_.error(DiagCode::GenericArguments, span, "type argument count does not match '{}'", names_.str(resolution_.binding(id).name.sym));
            return {ExpansionState::Error};
        }
        for (auto arg : args) if (arg == Type::Error) return {ExpansionState::Error};
        for (auto arg : args) if (!known(arg)) return {};
        if (resolution_.scopes()[resolution_.binding(id).scope.value].kind == ScopeKind::File) parent = 0;
        if (args.empty() && !known(self)) return {ExpansionState::Resolved, parent};
        const TypeDescriptor key{.kind = Type::Nominal, .element = self, .declaration = id, .arguments = args};
        auto& cache = environment_ids_[parent];
        if (const auto it = cache.find(key); it != cache.end()) return {ExpansionState::Resolved, it->second};
        const auto context = environments_.size();
        cache.emplace(key, context);
        environments_.push_back({id, std::move(args), parent, self});
        substituted_annotations_.emplace_back();
        speculative_annotations_.emplace_back();
        return {ExpansionState::Resolved, context};
    }
    Type context_self(usize context, BindingId owner = {}) {
        while (context) {
            if (known(environments_[context].self) &&
                (!owner.valid() || resolution_.binding(owner).item->kind == ItemKind::Interface || environments_[context].declaration == owner))
                return environments_[context].self;
            if (owner.valid() && environments_[context].declaration == owner && resolution_.binding(owner).item->kind != ItemKind::Interface)
                return instance(owner, context);
            context = environments_[context].parent;
        }
        return Type::Deferred;
    }
    Type schema_self(BindingId owner) {
        return resolution_.binding(owner).item->kind == ItemKind::Interface ? symbol(owner) : nominal(owner);
    }

    bool owned_formal(Type source, BindingId owner) const {
        std::unordered_set<usize> parameters;
        for (const auto& param : generic_parameters(owner)) parameters.insert(generic_bindings_.at(param.name.span.lo).value);
        const auto& binding = resolution_.binding(owner);
        if (binding.kind == BindingKind::Type && binding.item->kind == ItemKind::Interface) parameters.insert(owner.value);
        if (parameters.empty()) return false;
        std::vector<Type> work{source}; std::unordered_set<usize> seen;
        while (!work.empty()) {
            const auto current = work.back(); work.pop_back();
            const auto* descriptor = types_.descriptor(current);
            if (!descriptor || !seen.insert(current.detail).second) continue;
            if (current.kind == Type::TypeParameter) { if (parameters.contains(descriptor->declaration.value)) return true; continue; }
            if (current.kind == Type::Nominal) for (auto arg : descriptor->arguments) work.push_back(arg);
            else if (current.kind == Type::Function) {
                for (auto param : descriptor->parameters) work.push_back(param);
                work.push_back(descriptor->result); if (descriptor->throws) work.push_back(descriptor->error);
            } else work.push_back(descriptor->element);
        }
        return false;
    }
    void collect_formal_bounds() {
        const ScopedValue assumptions(probing_constraints_, true); // declared bounds are assumptions, not applications
        bounds_ready_ = false;
        formal_bounds_.clear(); projection_bounds_.clear(); uncertain_bounds_.clear();
        for (usize i = 0; i < resolution_.bindings().size(); ++i) {
            const auto& binding = resolution_.bindings()[i];
            if (binding.kind == BindingKind::Type && binding.item->kind == ItemKind::Interface)
                formal_bounds_[symbol(BindingId{i}).detail].push_back(nominal(BindingId{i}));
        }
        std::unordered_set<usize> added_uncertain;
        const auto mark_uncertain = [&](usize detail) {
            if (uncertain_bounds_.insert(detail).second) added_uncertain.insert(detail);
        };
        const auto add = [&](BindingId owner, Type source, const TypeExpr* syntax, usize context = 0, bool premise = true) {
            const auto bound = annotation(syntax, context);
            if (known(bound) && !symbolic(bound) && (bound.optional_depth || !protocol(bound))) {
                constraint_error(owner, syntax->span); invalid_constraints_.insert(owner.value); return true;
            }
            if (!known(source) || concrete(source)) return known(source) && known(bound);
            if (source.optional_depth) { mark_uncertain(source.detail); return known(bound); }
            if (!premise) { mark_uncertain(source.detail); return known(bound); }
            if (!owned_formal(source, owner)) { mark_uncertain(source.detail); return known(bound); }
            if (!known(bound) || symbolic(bound)) { mark_uncertain(source.detail); return known(bound); }
            auto& bounds = formal_bounds_[source.detail];
            if (std::ranges::find(bounds, bound) == bounds.end()) bounds.push_back(bound);
            return true;
        };
        for (usize i = 0; i < resolution_.bindings().size(); ++i) {
            const auto id = BindingId{i};
            const auto& binding = resolution_.binding(id);
            if (binding.kind != BindingKind::Function && binding.kind != BindingKind::Type && binding.kind != BindingKind::TypeAlias) continue;
            List<WhereClause> where;
            List<Param> incoming_params;
            if (const auto* fun = as<FunItem>(binding.item)) { where = fun->sig.where; incoming_params = fun->sig.params; }
            else if (const auto* type = as_type_decl(binding.item)) { where = type->where; incoming_params = type->ctor_params; }
            if (!generic_parameters(id).empty() || !where.empty()) {
                // Incoming parameter bounds or nested projections may be needed to
                // resolve later parameters and multi-step where-clause subjects.
                added_uncertain.clear();
                for (usize count = std::numeric_limits<usize>::max(), pass = 0;; ++pass) {
                    usize before = 0;
                    for (const auto& [_, bounds] : formal_bounds_) before += bounds.size();
                    for (const auto& [_, bounds] : projection_bounds_) before += bounds.size();
                    if (before == count) break;
                    count = before;
                    for (auto detail : added_uncertain) uncertain_bounds_.erase(detail);
                    added_uncertain.clear();
                    if (pass) clear_deferred_annotations();
                    bool deferred = false;
                    for (const auto& param : generic_parameters(id)) {
                        const auto source = symbol(generic_bindings_.at(param.name.span.lo));
                        for (const auto* bound : param.bounds) deferred |= !add(id, source, bound);
                    }
                    for (const auto& clause : where) {
                        const auto source = annotation(clause.type);
                        deferred |= !known(source);
                        const bool self_clause = binding.kind == BindingKind::Type &&
                            binding.item->kind == ItemKind::Interface && clause.type->kind == TypeKind::SelfType;
                        for (const auto* bound : clause.bounds) deferred |= !add(id, source, bound, 0, !self_clause);
                    }
                    if (!generic_parameters(id).empty() && !incoming_params.empty()) {
                        std::unordered_set<usize> visited;
                        std::vector<Type> work;
                        for (const auto& param : incoming_params) {
                            const auto type = annotation(param.type);
                            deferred |= !known(type);
                            work.push_back(type);
                        }
                        while (!work.empty()) {
                            const auto type = work.back(); work.pop_back();
                            const auto* pointer = types_.descriptor(type);
                            if (!pointer || !visited.insert(type.detail).second) continue;
                            const auto descriptor = *pointer;
                            if (type.kind == Type::Nominal) {
                                const auto params = generic_parameters(descriptor.declaration);
                                if (params.size() == descriptor.arguments.size()) {
                                    const auto context = application(descriptor.declaration, descriptor.arguments, 0, binding.name.span).context;
                                    for (usize p = 0; p < params.size(); ++p)
                                        for (const auto* bound : params[p].bounds)
                                            deferred |= !add(id, descriptor.arguments[p], bound, context);
                                    if (const auto* declaration = as_type_decl(resolution_.binding(descriptor.declaration).item))
                                        for (const auto& clause : declaration->where) {
                                            const auto subject = annotation(clause.type, context);
                                            deferred |= !known(subject);
                                            for (const auto* bound : clause.bounds)
                                                deferred |= !add(id, subject, bound, context);
                                        }
                                }
                                for (auto arg : descriptor.arguments) work.push_back(arg);
                            } else if (type.kind == Type::Function) {
                                for (auto param : descriptor.parameters) work.push_back(param);
                                work.push_back(descriptor.result); if (descriptor.throws) work.push_back(descriptor.error);
                            } else if (type.kind != Type::TypeParameter) work.push_back(descriptor.element);
                        }
                    }
                    const ScopedValue ready(bounds_ready_, true);
                    projection_bounds_.clear();
                    for (usize p = 0; p < projection_types_.size(); ++p) {
                        const auto descriptor = *types_.descriptor(projection_types_[p]);
                        const bool was_uncertain = uncertain_bounds_.contains(projection_types_[p].detail);
                        (void)project(descriptor.element, descriptor.declaration);
                        if (!was_uncertain && uncertain_bounds_.contains(projection_types_[p].detail))
                            added_uncertain.insert(projection_types_[p].detail);
                    }
                    if (!deferred) break;
                }
            }
        }
        bounds_ready_ = true;
        projection_bounds_.clear();
        clear_deferred_annotations();
        for (usize i = 0; i < projection_types_.size(); ++i) {
            const auto descriptor = *types_.descriptor(projection_types_[i]);
            (void)project(descriptor.element, descriptor.declaration);
        }
    }

    Type bound_view(Type receiver, BindingId owner) {
        std::vector<Type> work{receiver.scalar()};
        std::unordered_set<usize> seen;
        while (!work.empty()) {
            const auto current = work.back(); work.pop_back();
            if (!known(current) || !seen.insert(current.detail).second) continue;
            if (symbolic(current)) {
                for (const auto* table : {&formal_bounds_, &projection_bounds_})
                    if (const auto it = table->find(current.detail); it != table->end())
                        for (auto bound : it->second) work.push_back(bound);
            } else if (current.kind == Type::Nominal) {
                const auto declaration = types_.descriptor(current)->declaration;
                if (declaration == owner) return current;
                const auto graph = hierarchies_.find(declaration.value);
                if (graph != hierarchies_.end() && graph->second.state == ExpansionState::Resolved)
                    for (auto base : graph->second.bases) work.push_back(nominal(base));
            }
        }
        return Type::Deferred;
    }
    void collect_bound_uses() {
        for (usize i = 0; i < resolution_.bindings().size(); ++i) {
            const auto id = BindingId{i};
            const auto& binding = resolution_.binding(id);
            if (binding.kind != BindingKind::Function && binding.kind != BindingKind::Type && binding.kind != BindingKind::TypeAlias) continue;
            for (const auto& param : generic_parameters(id)) for (const auto* bound : param.bounds) (void)annotation(bound);
            if (const auto* alias = as<TypeAliasItem>(binding.item)) for (const auto* bound : alias->bounds) (void)annotation(bound);
            List<WhereClause> where;
            if (const auto* fun = as<FunItem>(binding.item)) where = fun->sig.where;
            else if (const auto* type = as_type_decl(binding.item)) where = type->where;
            for (const auto& clause : where) for (const auto* bound : clause.bounds) (void)annotation(bound);
        }
    }
    usize owner_context(BindingId owner, Type receiver) {
        if (!owner.valid() || !known(receiver)) return 0;
        std::vector<Type> args;
        if (!generic_parameters(owner).empty()) {
            const auto view = bound_view(receiver, owner);
            if (view.kind != Type::Nominal) return 0;
            args = types_.descriptor(view)->arguments;
        }
        auto self = receiver;
        if (resolution_.binding(owner).item->kind != ItemKind::Interface &&
            (receiver.kind != Type::Nominal || types_.descriptor(receiver)->declaration != owner)) self = nominal(owner);
        return application(owner, std::move(args), 0, resolution_.binding(owner).name.span, self).context;
    }

    Type substitute_type(Type root, usize context) {
        if (!context || !known(root)) return root;
        auto hash = [](Type type) { return TypeDescriptorHash{}({.element = type}); };
        std::unordered_map<Type, Type, decltype(hash)> cache(0, hash);
        struct Frame { Type type; bool ready = false; };
        std::vector<Frame> work{{root}};
        while (!work.empty()) {
            auto& frame = work.back();
            const auto type = frame.type;
            if (cache.contains(type)) { work.pop_back(); continue; }
            const auto* pointer = types_.descriptor(type);
            if (!pointer) { cache[type] = type; continue; }
            const auto descriptor = *pointer;
            if (type.kind == Type::TypeParameter) {
                auto value = resolution_.binding(descriptor.declaration).kind == BindingKind::Type
                    ? context_self(context, descriptor.declaration) : substituted(descriptor.declaration, context);
                if (!known(value)) value = type.scalar();
                value.optional_depth += type.optional_depth;
                cache[type] = value; continue;
            }
            if (!frame.ready) {
                frame.ready = true;
                if (type.kind == Type::Nominal) { for (auto arg : descriptor.arguments) work.push_back({arg}); }
                else if (type.kind == Type::Function) {
                    for (auto param : descriptor.parameters) work.push_back({param});
                    work.push_back({descriptor.result});
                    if (descriptor.throws) work.push_back({descriptor.error});
                } else work.push_back({descriptor.element});
                continue;
            }
            auto result = descriptor;
            if (type.kind == Type::Projection) {
                auto value = project(cache.at(descriptor.element), descriptor.declaration);
                if (known(value)) value.optional_depth += type.optional_depth;
                cache[type] = value; continue;
            }
            if (type.kind == Type::Nominal) for (auto& arg : result.arguments) arg = cache.at(arg);
            else if (type.kind == Type::Function) {
                for (auto& param : result.parameters) param = cache.at(param);
                result.result = cache.at(result.result);
                if (result.throws) result.error = cache.at(result.error);
            } else result.element = cache.at(result.element);
            auto value = types_.intern(std::move(result));
            value.optional_depth = type.optional_depth;
            cache[type] = value;
        }
        return cache.at(root);
    }
    bool associated_alias(BindingId id) const {
        const auto owner = member_owner(id);
        return owner.valid() && resolution_.binding(owner).item->kind == ItemKind::Interface;
    }
    Type instance(BindingId id, usize context) {
        const auto params = generic_parameters(id);
        if (params.empty()) return nominal(id);
        // Captured nominal owners need an enclosing-instance identity as well.
        for (auto scope = resolution_.binding(id).scope; scope.valid(); scope = resolution_.scopes()[scope.value].parent)
            if (resolution_.scopes()[scope.value].kind == ScopeKind::Generics && !resolution_.scopes()[scope.value].bindings.empty()) return Type::Deferred;
        std::vector<Type> args;
        for (const auto& param : params) {
            const auto type = substituted(generic_bindings_.at(param.name.span.lo), context);
            if (!known(type)) return type;
            args.push_back(type);
        }
        return types_.intern({.kind = Type::Nominal, .declaration = id, .arguments = std::move(args)});
    }

    bool concrete(Type root) const {
        std::vector<Type> work{root};
        std::unordered_set<usize> seen;
        while (!work.empty()) {
            const auto type = work.back(); work.pop_back();
            if (!known(type) || type.kind == Type::TypeParameter || type.kind == Type::Projection) return false;
            const auto* descriptor = types_.descriptor(type);
            if (!descriptor || !seen.insert(type.detail).second) continue;
            if (type.kind == Type::Nominal) for (auto arg : descriptor->arguments) work.push_back(arg);
            else if (type.kind == Type::Function) {
                for (auto param : descriptor->parameters) work.push_back(param);
                work.push_back(descriptor->result);
                if (descriptor->throws) work.push_back(descriptor->error);
            } else work.push_back(descriptor->element);
        }
        return true;
    }

    struct Inference {
        Relation state = Relation::Deferred;
        usize context = 0;
        std::vector<Type> arguments;
    };
    Inference infer_arguments(BindingId id, List<Param> params, const CallExpr& call,
                             std::span<const usize> mapping, usize parent) {
        const auto generics = generic_parameters(id);
        Inference result{.context = parent, .arguments = std::vector<Type>(generics.size())};
        std::unordered_map<usize, usize> positions;
        std::vector<Type> symbols;
        for (usize p = 0; p < generics.size(); ++p) {
            const auto binding = generic_bindings_.at(generics[p].name.span.lo);
            positions.emplace(binding.value, p);
            symbols.push_back(types_.intern({.kind = Type::TypeParameter, .declaration = binding}));
        }
        const auto pattern_context = application(id, std::move(symbols), parent, call.span).context;
        std::vector<Type> patterns;
        for (const auto& param : params) patterns.push_back(annotation(param.type, pattern_context));
        bool pending = false;
        std::vector<std::pair<usize, Type>> pending_nominals;
        // Concrete arguments anchor inference before numeric literal defaults.
        // Probes later contextualize literals against the complete substitution.
        for (bool literals : {false, true}) for (usize a = 0; a < call.args.size(); ++a) {
            const auto actual = value(call.args[a].value);
            const bool literal = actual.literal != Literal::None &&
                                 (actual.literal != Literal::Array || !actual.concrete_anchor);
            if (literal != literals) continue;
            if (!known(default_type(actual))) continue;
            auto hash = [](const std::pair<Type, Type>& pair) {
                return TypeDescriptorHash{}({.element = pair.first, .result = pair.second});
            };
            std::unordered_set<std::pair<Type, Type>, decltype(hash)> seen(0, hash);
            std::vector<std::pair<Type, Type>> work{{patterns[mapping[a]], default_type(actual)}};
            while (!work.empty()) {
                const auto [pattern, argument] = work.back(); work.pop_back();
                if (!known(pattern) || !known(argument) || argument == Type::Never) continue;
                if (!seen.emplace(pattern, argument).second) continue;
                if (pattern.kind == Type::TypeParameter) {
                    const auto position = positions.find(types_.descriptor(pattern)->declaration.value);
                    if (position == positions.end()) continue;
                    auto inferred = argument;
                    inferred.optional_depth -= std::min(inferred.optional_depth, pattern.optional_depth);
                    auto& anchor = result.arguments[position->second];
                    if (!known(anchor)) anchor = inferred;
                    else if (!literal && anchor != inferred) {
                        if (anchor.scalar() == inferred.scalar()) anchor.optional_depth = std::max(anchor.optional_depth, inferred.optional_depth);
                        else if (anchor.kind == Type::Nominal && inferred.kind == Type::Nominal) {
                            const auto depth = std::max(anchor.optional_depth, inferred.optional_depth);
                            if (relation(inferred.scalar(), anchor.scalar()) == Relation::Yes) anchor.optional_depth = depth;
                            else if (relation(anchor.scalar(), inferred.scalar()) == Relation::Yes) { anchor = inferred; anchor.optional_depth = depth; }
                            else {
                                pending_nominals.push_back({position->second, inferred});
                                anchor.optional_depth = depth;
                            }
                        }
                        else if (relation(inferred, anchor) == Relation::Yes) {}
                        else if (relation(anchor, inferred) == Relation::Yes) anchor = inferred;
                        else if (anchor.kind == Type::TypeParameter || inferred.kind == Type::TypeParameter ||
                                 anchor.kind == Type::Projection || inferred.kind == Type::Projection) pending_nominals.push_back({position->second, inferred});
                        else if (relation(anchor, inferred) == Relation::Deferred || relation(inferred, anchor) == Relation::Deferred) pending = true;
                        else { result.state = Relation::No; return result; }
                    }
                    continue;
                }
                if (pattern.kind == Type::Projection) continue;
                if (pattern.kind != argument.kind) {
                    // These positions supply no inference evidence. Preserve
                    // the ordinary probe's deferred protocol/other conversions.
                    if (types_.descriptor(pattern) && !protocol(pattern) && !concrete(pattern)) { result.state = Relation::No; return result; }
                    continue;
                }
                const auto* x = types_.descriptor(pattern);
                const auto* y = types_.descriptor(argument);
                if (!x || !y) continue;
                if (pattern.kind == Type::Nominal) {
                    if (x->declaration != y->declaration) continue; // generic base substitution remains pending
                    if (x->arguments.size() != y->arguments.size()) continue;
                    for (usize p = 0; p < x->arguments.size(); ++p) work.push_back({x->arguments[p], y->arguments[p]});
                } else if (pattern.kind == Type::Function) {
                    if (x->parameters.size() != y->parameters.size()) { result.state = Relation::No; return result; }
                    for (usize p = 0; p < x->parameters.size(); ++p) work.push_back({x->parameters[p], y->parameters[p]});
                    work.push_back({x->result, y->result});
                    if (x->throws && y->throws) work.push_back({x->error, y->error});
                } else work.push_back({x->element, y->element});
            }
        }
        // A later supplied base/interface may resolve earlier sibling anchors.
        // No common-ancestor search is needed: check against the final anchor.
        for (const auto& [position, candidate] : pending_nominals)
            pending |= relation(candidate, result.arguments[position]) != Relation::Yes;
        if (pending) return result; // no general common-ancestor or variance solver yet
        for (auto arg : result.arguments) if (!known(arg)) return result;
        result.context = application(id, result.arguments, parent, call.span).context;
        result.state = Relation::Yes;
        return result;
    }

    Type annotation(const TypeExpr* type, usize context = 0) {
        if (!type) return Type::Deferred;
        const auto cache = [&](usize env) -> auto& {
            // A probed substitution must not suppress later validation through
            // a real cache hit if another call selects this same signature.
            if (probing_constraints_) return speculative_annotations_[env];
            return env ? substituted_annotations_[env] : annotations_;
        };
        const auto cached = [&](const TypeExpr* syntax, usize env) -> Type {
            if (!syntax) return Type::Unit;
            const auto it = cache(env).find(syntax);
            return it == cache(env).end() ? Type{} : it->second;
        };
        struct Frame {
            const TypeExpr* syntax;
            usize context;
            bool ready = false;
            const TypeExpr* redirect = nullptr;
            usize redirect_context = 0;
            BindingId alias;
        };
        std::vector<Frame> work{{type, context}};
        std::unordered_set<usize> active_aliases;
        while (!work.empty()) {
            auto& frame = work.back();
            const auto* syntax = frame.syntax;
            const auto env = frame.context;
            if (cache(env).contains(syntax)) {
                if (frame.alias.valid()) active_aliases.erase(frame.alias.value);
                work.pop_back(); continue;
            }
            if (!frame.ready) {
                frame.ready = true;
                if (syntax->inner) work.push_back({syntax->inner, env});
                if (syntax->throws_type) work.push_back({syntax->throws_type, env});
                for (auto* arg : syntax->args) work.push_back({arg, env});
                continue;
            }
            Type value;
            if (frame.redirect) value = cached(frame.redirect, frame.redirect_context);
            else if (syntax->kind == TypeKind::Path || syntax->kind == TypeKind::Unit || syntax->kind == TypeKind::SelfType) {
                BindingId id;
                const auto* ref = syntax->kind == TypeKind::Path && !syntax->path.parts.empty()
                    ? resolution_.reference(syntax->path.parts.back().span) : resolution_.reference(syntax->span);
                if (ref && ref->state == ResolutionState::Resolved) {
                    const auto ids = resolution_.candidates(ref->target);
                    if (ids.size() == 1) id = ids.front();
                }
                if (!id.valid() && syntax->kind == TypeKind::Path && (!syntax->args.empty() || hierarchy_ready_)) {
                    const auto direct = aliases_.type(syntax, resolution_);
                    if (direct.type.state == ExpansionState::Error) {
                        if (!direct.unknown_member.sym.empty())
                            diag_.error(DiagCode::UnknownType, direct.unknown_member.span, "unknown type '{}' in '{}'",
                                        names_.str(direct.unknown_member.sym), names_.str(resolution_.binding(direct.member_owner).name.sym));
                        cache(env)[syntax] = Type::Error; continue;
                    }
                    const auto qualified = inherited_type(syntax, &id);
                    if (qualified.state == ExpansionState::Error) { cache(env)[syntax] = Type::Error; continue; }
                }
                if (id.valid() && resolution_.binding(id).kind == BindingKind::GenericParameter) {
                    if (!syntax->args.empty()) {
                        (void)application(id, std::vector<Type>(syntax->args.size()), env, syntax->span);
                        cache(env)[syntax] = Type::Error;
                    } else cache(env)[syntax] = formal_argument(id, env);
                    continue;
                }
                if (syntax->kind == TypeKind::SelfType && id.valid() && resolution_.binding(id).kind == BindingKind::Type) {
                    const auto self = context_self(env, id);
                    cache(env)[syntax] = known(self) ? self : schema_self(id);
                    continue;
                }
                if (id.valid() && syntax->kind == TypeKind::Path && syntax->path.parts.size() > 1) {
                    const auto owner = member_owner(id);
                    if (owner.valid() && !generic_parameters(owner).empty()) {
                        const auto prefix = aliases_.path(syntax->path.parts.first(syntax->path.parts.size() - 1), NameSpace::Type, resolution_).type;
                        Type receiver;
                        if (prefix.state == ExpansionState::Resolved && !prefix.optional_depth && prefix.scalar.kind == Type::Nominal &&
                            types_.descriptor(prefix.scalar)->declaration == owner) receiver = prefix.scalar;
                        else if (ref && enclosing_owner(ref->scope) == owner) {
                            receiver = context_self(env, owner);
                            if (!known(receiver)) receiver = schema_self(owner);
                        }
                        cache(env)[syntax] = known(receiver) ? dependent_path(syntax, receiver, syntax->path.parts.size() - 1) : Type{};
                        continue; // a bare generic qualifier outside its owner has no enclosing instance
                    }
                }
                if (id.valid() && resolution_.binding(id).kind == BindingKind::TypeAlias && associated_alias(id) && syntax->args.empty()) {
                    const auto self = context_self(env);
                    auto owner = member_owner(id);
                    if (ref && syntax->path.parts.size() == 1) {
                        const auto lexical = enclosing_owner(ref->scope);
                        if (lexical.valid() && resolution_.binding(lexical).item->kind == ItemKind::Interface && ancestry(lexical, owner) == Relation::Yes) owner = lexical;
                    }
                    const bool applicable = known(self) && !self.optional_depth &&
                        (known(bound_view(self, owner)) || (self.kind == Type::Nominal && ancestry(types_.descriptor(self)->declaration, owner) == Relation::Yes));
                    if (applicable || syntax->path.parts.size() == 1)
                        cache(env)[syntax] = project(applicable ? self : schema_self(owner), id);
                    else cache(env)[syntax] = Type::Deferred;
                    continue;
                }
                if (syntax->kind == TypeKind::Path && syntax->path.parts.size() > 1) {
                    const auto* first = resolution_.reference(syntax->path.parts.front().span);
                    const auto ids = first && first->state == ResolutionState::Resolved ? resolution_.candidates(first->target) : std::span<const BindingId>{};
                    if (ids.size() == 1 && resolution_.binding(ids.front()).kind == BindingKind::GenericParameter) {
                        cache(env)[syntax] = dependent_path(syntax, formal_argument(ids.front(), env));
                        continue;
                    }
                }
                const auto kind = id.valid() ? resolution_.binding(id).kind : BindingKind::Import;
                if (id.valid() && (kind == BindingKind::Type || kind == BindingKind::TypeAlias || kind == BindingKind::BuiltinType) &&
                    (!syntax->args.empty() || env)) {
                    std::vector<Type> args;
                    for (auto* arg : syntax->args) args.push_back(cached(arg, env));
                    if (syntax->args.empty() && !generic_parameters(id).empty()) {
                        cache(env)[syntax] = syntax->kind == TypeKind::SelfType ? instance(id, env) : Type{};
                        continue;
                    }
                    const auto applied = application(id, std::move(args), env, syntax->span);
                    if (applied.state != ExpansionState::Resolved) {
                        cache(env)[syntax] = applied.state == ExpansionState::Error ? Type::Error : Type::Deferred; continue;
                    }
                    remember_constraints(id, applied.context, syntax->span);
                    if (kind != BindingKind::TypeAlias) {
                        cache(env)[syntax] = kind == BindingKind::BuiltinType ? aliases_.binding(id, resolution_).type.scalar : instance(id, applied.context);
                        continue;
                    }
                    const auto* alias = cast<TypeAliasItem>(resolution_.binding(id).item);
                    if (!alias->type || associated_alias(id) || constrained(id)) { cache(env)[syntax] = Type::Deferred; continue; }
                    if (!active_aliases.insert(id.value).second) {
                        diag_.error(DiagCode::AliasCycle, syntax->span, "cyclic generic type alias expansion");
                        for (const auto& entry : work) cache(entry.context)[entry.syntax] = Type::Error;
                        continue;
                    }
                    frame.alias = id;
                    frame.redirect = alias->type;
                    frame.redirect_context = applied.context;
                    work.push_back({alias->type, applied.context});
                    continue;
                }
                auto expanded = aliases_.type(syntax, resolution_);
                if (hierarchy_ready_ && expanded.type.state == ExpansionState::Deferred && syntax->kind == TypeKind::Path && syntax->args.empty())
                    expanded.type = inherited_type(syntax);
                if (!expanded.unknown_member.sym.empty())
                    diag_.error(DiagCode::UnknownType, expanded.unknown_member.span, "unknown type '{}' in '{}'",
                                names_.str(expanded.unknown_member.sym), names_.str(resolution_.binding(expanded.member_owner).name.sym));
                if (expanded.type.state == ExpansionState::Resolved && expanded.type.target.valid() &&
                    resolution_.binding(expanded.type.target).kind == BindingKind::Type && generic_parameters(expanded.type.target).empty())
                    remember_constraints(expanded.type.target, env, syntax->span);
                value = expanded.type.scalar;
                if (!known(value) && expanded.type.state == ExpansionState::Resolved && expanded.type.target.valid()) {
                    value = nominal(expanded.type.target);
                    if (known(value)) value.optional_depth = expanded.type.optional_depth;
                }
                if (!known(value) && value != Type::Error && expanded.type.syntax && expanded.type.syntax != syntax) {
                    frame.redirect = expanded.type.syntax;
                    frame.redirect_context = env;
                    const bool active = std::ranges::any_of(work, [&](const Frame& entry) {
                        return entry.syntax == frame.redirect && entry.context == env;
                    });
                    if (active) {
                        diag_.error(DiagCode::AliasCycle, syntax->span, "cyclic type alias expansion through inherited lookup");
                        for (const auto& entry : work) cache(entry.context)[entry.syntax] = Type::Error;
                    } else work.push_back({frame.redirect, env});
                    continue;
                }
            } else if (syntax->kind == TypeKind::Optional) value = cached(syntax->inner, env).optional();
            else if (syntax->kind == TypeKind::Fun) {
                TypeDescriptor descriptor{.kind = Type::Function, .result = cached(syntax->inner, env), .throws = syntax->throws};
                bool complete = known(descriptor.result);
                for (auto* param : syntax->args) { descriptor.parameters.push_back(cached(param, env)); complete &= known(cached(param, env)); }
                if (syntax->throws) {
                    if (syntax->throws_type) descriptor.error = cached(syntax->throws_type, env);
                    else for (usize i = 0; i < resolution_.bindings().size(); ++i) {
                        const auto& b = resolution_.bindings()[i];
                        if (b.kind == BindingKind::BuiltinType && names_.str(b.name.sym) == "Error") descriptor.error = nominal(BindingId{i});
                    }
                    complete &= known(descriptor.error);
                }
                if (complete) value = types_.intern(std::move(descriptor));
            } else if (known(cached(syntax->inner, env))) {
                Type::Kind kind;
                switch (syntax->kind) {
                    case TypeKind::Array: kind = Type::Array; break;
                    case TypeKind::Pointer: kind = Type::Pointer; break;
                    case TypeKind::Shared: kind = Type::Shared; break;
                    case TypeKind::Weak: kind = Type::Weak; break;
                    default: cache(env)[syntax] = Type::Deferred; continue;
                }
                value = types_.intern({.kind = kind, .element = cached(syntax->inner, env), .mutable_pointer = syntax->is_mut});
            }
            // Errors in nested applications are already diagnosed; retain them.
            if (syntax->inner && cached(syntax->inner, env) == Type::Error) value = Type::Error;
            if (syntax->throws_type && cached(syntax->throws_type, env) == Type::Error) value = Type::Error;
            for (auto* arg : syntax->args) if (cached(arg, env) == Type::Error) value = Type::Error;
            cache(env)[syntax] = value;
        }
        return cached(type, context);
    }

    ExpandedType entity(const Expr* expr) const {
        const auto it = entities_.find(expr);
        return it == entities_.end() ? ExpandedType{} : it->second;
    }

    ExpandedType type_entity(Type type) const {
        if (!known(type)) return {};
        BindingId target;
        if (type.kind == Type::Nominal) target = types_.descriptor(type)->declaration;
        else for (const auto& [builtin, scalar] : aliases_.builtins_) if (scalar == type.scalar()) { target = BindingId{builtin}; break; }
        return {ExpansionState::Resolved, target, type, type.optional_depth};
    }

    void entity(const Expr* expr, BindingId id, usize context = 0) {
        if (resolution_.binding(id).kind == BindingKind::GenericParameter) { entities_[expr] = type_entity(formal_argument(id, context)); return; }
        if (context && resolution_.binding(id).kind == BindingKind::TypeAlias) {
            const auto* alias = cast<TypeAliasItem>(resolution_.binding(id).item);
            entities_.erase(expr);
            if (associated_alias(id) && known(context_self(context))) entities_[expr] = type_entity(project(context_self(context), id));
            else if (alias->type && alias->generics.empty() && !constrained(id))
                entities_[expr] = type_entity(annotation(alias->type, context));
            return;
        }
        const auto type = aliases_.binding(id, resolution_).type;
        if (type.state == ExpansionState::Resolved) entities_[expr] = type;
    }

    void normalize_aliases() {
        for (auto& [id, fact] : aliases_.aliases_) {
            (void)id;
            if (!fact.syntax) continue;
            const auto value = annotation(fact.syntax);
            if (value == Type::Error) { fact.state = ExpansionState::Error; fact.scalar = value; continue; }
            if (!known(value)) continue;
            fact.state = ExpansionState::Resolved;
            fact.scalar = value;
            fact.optional_depth = value.optional_depth;
            if (value.kind == Type::Nominal) fact.target = types_.descriptor(value)->declaration;
            else for (const auto& [builtin, type] : aliases_.builtins_) if (type == value.scalar()) { fact.target = BindingId{builtin}; break; }
        }
    }

    void clear_deferred_annotations() {
        const auto deferred = [](const auto& entry) { return entry.second == Type::Deferred; };
        std::erase_if(annotations_, deferred);
        for (auto& cache : substituted_annotations_) std::erase_if(cache, deferred);
        for (auto& cache : speculative_annotations_) std::erase_if(cache, deferred);
    }

    void complete_hierarchy_types() {
        const auto progress = [&]() {
            std::pair<usize, usize> counts{};
            for (const auto& [id, fact] : aliases_.aliases_) { (void)id; counts.first += fact.state != ExpansionState::Deferred; }
            for (const auto& b : resolution_.bindings()) if (b.kind == BindingKind::Type)
                for (const auto& base : cast_type_decl(b.item)->supers)
                    if (const auto it = annotations_.find(base.type); it != annotations_.end()) counts.second += it->second != Type::Deferred;
            return counts;
        };
        for (;;) {
            const auto before = progress();
            clear_deferred_annotations();
            normalize_aliases();
            clear_deferred_annotations();
            // Normalize every base against the previous complete graph before
            // rebuilding, so forward declarations cannot change lookup order.
            for (const auto& b : resolution_.bindings()) if (b.kind == BindingKind::Type)
                for (const auto& base : cast_type_decl(b.item)->supers) (void)annotation(base.type);
            const auto after = progress();
            if (after.second > before.second) {
                hierarchies_.clear(); hierarchy_order_.clear(); ancestry_cache_.clear(); inherited_members_.clear();
                build_hierarchies();
            }
            if (after == before) break; // only Deferred -> known/error facts advance
        }
    }

    void build_hierarchies() {
        for (usize id = 0; id < resolution_.bindings().size(); ++id) {
            const auto& b = resolution_.bindings()[id];
            if (b.kind != BindingKind::Type) continue;
            auto& fact = hierarchies_[id];
            const auto* item = cast_type_decl(b.item);
            if (!known(nominal(BindingId{id}))) continue;
            fact.state = ExpansionState::Resolved; // declared structure is separate from conditions
            for (const auto& base : item->supers) {
                const auto type = annotation(base.type);
                if (type == Type::Error) { fact.state = ExpansionState::Error; continue; }
                if (!known(type)) { if (fact.state != ExpansionState::Error) fact.state = ExpansionState::Deferred; continue; }
                if (symbolic(type)) { if (fact.state != ExpansionState::Error) fact.state = ExpansionState::Deferred; continue; }
                if (type.kind == Type::Nominal && !types_.descriptor(type)->arguments.empty() && fact.state != ExpansionState::Error)
                    fact.state = ExpansionState::Deferred; // generic base argument substitution remains pending
                bool valid = type.kind == Type::Nominal && !type.optional_depth;
                BindingId parent;
                if (valid) {
                    parent = types_.descriptor(type)->declaration;
                    const auto& target = resolution_.binding(parent);
                    if (target.kind == BindingKind::BuiltinType) {
                        if (std::ranges::find(fact.bases, parent) != fact.bases.end()) {
                            if (reported_bases_.insert(base.type).second) diag_.error(DiagCode::InvalidBase, base.type->span, "repeated base type");
                            fact.state = ExpansionState::Error; continue;
                        }
                        fact.bases.push_back(parent);
                        if (fact.state != ExpansionState::Error) fact.state = ExpansionState::Deferred;
                        continue; // built-in trait/error contracts need later checking
                    }
                    const auto* declaration = cast_type_decl(target.item);
                    valid = declaration->kind == ItemKind::Interface ? !base.has_args :
                        declaration->kind == ItemKind::Class && item->kind == ItemKind::Class && !fact.superclass.valid() &&
                        !has_modifier(declaration->mods, Modifier::Final) &&
                        (has_modifier(declaration->mods, Modifier::Open) || has_modifier(declaration->mods, Modifier::Abstract) ||
                         has_modifier(declaration->mods, Modifier::Sealed));
                    if (std::ranges::find(fact.bases, parent) != fact.bases.end()) valid = false;
                    if (valid && declaration->kind == ItemKind::Class) fact.superclass = parent;
                }
                if (!valid) {
                    if (reported_bases_.insert(base.type).second) {
                        auto& d = diag_.error(DiagCode::InvalidBase, base.type->span, "invalid, repeated or final base type for '{}'", names_.str(b.name.sym));
                        if (parent.valid()) d.notes.push_back({resolution_.binding(parent).name.span, "base declared here"});
                    }
                    fact.state = ExpansionState::Error;
                } else fact.bases.push_back(parent);
            }
        }
        struct Frame { usize id; usize next = 0; };
        std::vector<u8> color(resolution_.bindings().size());
        std::vector<Frame> stack;
        // Explicit DFS validates cycles and propagates incomplete/error bases.
        for (usize root = 0; root < color.size(); ++root) {
            if (!hierarchies_.contains(root) || color[root]) continue;
            stack.push_back({root}); color[root] = 1;
            while (!stack.empty()) {
                auto& frame = stack.back();
                auto& fact = hierarchies_.at(frame.id);
                if (frame.next < fact.bases.size()) {
                    const auto base = fact.bases[frame.next++];
                    if (!hierarchies_.contains(base.value)) continue;
                    if (!color[base.value]) { color[base.value] = 1; stack.push_back({base.value}); continue; }
                    if (color[base.value] == 1 && fact.state != ExpansionState::Error) {
                        Diagnostic* diagnostic = nullptr;
                        if (!reported_cycles_.contains(base.value)) diagnostic = &diag_.error(DiagCode::InheritanceCycle, resolution_.bindings()[frame.id].name.span,
                                              "cyclic inheritance involving '{}'", names_.str(resolution_.binding(base).name.sym));
                        bool cycle = false;
                        for (const auto& entry : stack) {
                            cycle |= entry.id == base.value;
                            if (!cycle) continue;
                            hierarchies_.at(entry.id).state = ExpansionState::Error;
                            reported_cycles_.insert(entry.id);
                            if (diagnostic) diagnostic->notes.push_back({resolution_.bindings()[entry.id].name.span, "type in inheritance cycle"});
                        }
                    }
                    continue;
                }
                for (const auto base : fact.bases) {
                    const auto it = hierarchies_.find(base.value);
                    if (it == hierarchies_.end()) continue;
                    if (it->second.state == ExpansionState::Error) fact.state = ExpansionState::Error;
                    else if (it->second.state == ExpansionState::Deferred && fact.state != ExpansionState::Error) fact.state = ExpansionState::Deferred;
                }
                color[frame.id] = 2;
                hierarchy_order_.push_back(BindingId{frame.id});
                stack.pop_back();
            }
        }
    }

    Relation ancestry(BindingId source, BindingId target) const {
        if (source == target) return Relation::Yes;
        auto& cache = ancestry_cache_[source.value];
        if (const auto it = cache.find(target.value); it != cache.end()) return it->second;
        std::vector<BindingId> work{source};
        std::unordered_set<usize> visited;
        Relation result = Relation::No;
        while (!work.empty()) {
            const auto next = work.back(); work.pop_back();
            if (next == target) { result = Relation::Yes; break; }
            if (!visited.insert(next.value).second) continue;
            const auto it = hierarchies_.find(next.value);
            if (it == hierarchies_.end() || it->second.state != ExpansionState::Resolved) { result = Relation::Deferred; continue; }
            for (const auto base : it->second.bases) work.push_back(base);
        }
        cache[target.value] = result;
        return result;
    }

    bool static_member(BindingId id) const {
        const auto& b = resolution_.binding(id);
        return b.kind == BindingKind::Type || b.kind == BindingKind::TypeAlias || b.kind == BindingKind::EnumCase ||
               (b.item && has_modifier(b.item->mods, Modifier::Static));
    }
    BindingId member_owner(BindingId id) const {
        const auto it = member_owners_.find(resolution_.binding(id).scope.value);
        return it == member_owners_.end() ? BindingId{} : it->second;
    }
    bool interface_owner(BindingId id) const {
        const auto owner = member_owner(id);
        return owner.valid() && resolution_.binding(owner).item->kind == ItemKind::Interface;
    }
    bool contains_self_type(const TypeExpr* root) const {
        if (!root) return false;
        std::vector<const TypeExpr*> work{root};
        while (!work.empty()) {
            const auto* type = work.back(); work.pop_back();
            if (type->kind == TypeKind::SelfType) return true;
            if (type->inner) work.push_back(type->inner);
            if (type->throws_type) work.push_back(type->throws_type);
            for (const auto* arg : type->args) work.push_back(arg);
        }
        return false;
    }
    bool method_inputs_known(BindingId id) const {
        const auto* fun = cast<FunItem>(resolution_.binding(id).item);
        const auto& sig = signatures_.at(id.value);
        if (sig.generic || !fun->sig.where.empty()) return false;
        for (const auto param : sig.parameters) if (!known(param)) return false;
        for (const auto& param : fun->sig.params) {
            if (param.conv != Convention::Default) return false;
        }
        return true;
    }
    bool same_method_shape(BindingId a, BindingId b) const {
        if (static_member(a) != static_member(b)) return false;
        const auto x = cast<FunItem>(resolution_.binding(a).item)->sig.params;
        const auto y = cast<FunItem>(resolution_.binding(b).item)->sig.params;
        if (x.size() != y.size()) return false;
        for (usize p = 0; p < x.size(); ++p) if (x[p].name.sym != y[p].name.sym) return false;
        return true;
    }
    // Compare inputs as seen from `receiver` (by default, the first method's owner). A lookup
    // receiver may supply associated types that make inherited and required profiles agree.
    bool same_method_inputs(BindingId a, BindingId b, Type receiver = {}) {
        if (!same_method_shape(a, b) || !method_inputs_known(a) || !method_inputs_known(b)) return false;
        if (cast<FunItem>(resolution_.binding(a).item)->sig.params.empty()) return true;
        if (!known(receiver)) receiver = schema_self(member_owner(a));
        const auto x = owner_context(member_owner(a), receiver), y = owner_context(member_owner(b), receiver);
        const auto px = cast<FunItem>(resolution_.binding(a).item)->sig.params;
        const auto py = cast<FunItem>(resolution_.binding(b).item)->sig.params;
        for (usize p = 0; p < px.size(); ++p) {
            const auto from = annotation(px[p].type, x), to = annotation(py[p].type, y);
            if (!known(from) || !known(to) || from != to) return false;
        }
        return true;
    }
    struct MemberLookup {
        ExpansionState state = ExpansionState::Resolved;
        // Keep reabstracting declarations even when a peer default supplies
        // the visible implementation. Descendants still need their blockers.
        std::vector<BindingId> declarations;
        std::vector<BindingId> ids;
    };
    std::vector<BindingId> direct_members(BindingId owner, Symbol name, bool types_only) const {
        const auto scope = resolution_.binding(owner).members;
        if (!scope.valid()) return {};
        if (!types_only) if (const auto table = member_values_.find(scope.value); table != member_values_.end())
            if (const auto it = table->second.find(name.id); it != table->second.end()) return it->second;
        const auto& types = aliases_.member_types_.at(scope.value);
        if (const auto it = types.find(name.id); it != types.end()) return {it->second};
        return {};
    }
    void merge_members(std::vector<BindingId>& result, std::span<const BindingId> incoming, Type receiver = {}) {
        const auto hides = [&](BindingId a, BindingId b) {
            if (a == b) return true;
            const auto& x = resolution_.binding(a);
            const auto& y = resolution_.binding(b);
            if (x.kind != BindingKind::Function || y.kind != BindingKind::Function)
                return ancestry(member_owner(a), member_owner(b)) == Relation::Yes;
            if (!same_method_inputs(a, b, receiver)) return false;
            return ancestry(member_owner(a), member_owner(b)) == Relation::Yes;
        };
        for (const auto candidate : incoming) {
            if (std::ranges::any_of(result, [&](BindingId id) { return hides(id, candidate); })) continue;
            std::erase_if(result, [&](BindingId id) { return hides(candidate, id); });
            result.push_back(candidate);
        }
    }
    std::vector<BindingId> preferred_members(std::span<const BindingId> declarations, Type receiver = {}) {
        std::vector<BindingId> result;
        for (const auto candidate : declarations) {
            const bool hidden = std::ranges::any_of(declarations, [&](BindingId other) {
                if (candidate == other || resolution_.binding(candidate).kind != BindingKind::Function ||
                    resolution_.binding(other).kind != BindingKind::Function || !same_method_inputs(other, candidate, receiver)) return false;
                const bool candidate_interface = interface_owner(candidate), other_interface = interface_owner(other);
                if (!other_interface && candidate_interface) return true;
                return other_interface && candidate_interface && cast<FunItem>(resolution_.binding(other).item)->body &&
                       !cast<FunItem>(resolution_.binding(candidate).item)->body;
            });
            if (!hidden) result.push_back(candidate);
        }
        return result;
    }
    std::shared_ptr<const MemberLookup> lookup_member(BindingId owner, Symbol name, bool types_only = false) {
        const u64 key = (static_cast<u64>(name.id) << 1) | types_only;
        auto& cache = inherited_members_[key];
        if (const auto it = cache.find(owner.value); it != cache.end()) return it->second;
        struct Frame { BindingId id; usize next = 0; };
        std::vector<Frame> stack{{owner}};
        while (!stack.empty()) {
            auto& frame = stack.back();
            const auto hierarchy = hierarchies_.find(frame.id.value);
            const auto own = direct_members(frame.id, name, types_only);
            if (hierarchy == hierarchies_.end() || hierarchy->second.state == ExpansionState::Error ||
                (!own.empty() && resolution_.binding(own.front()).kind != BindingKind::Function)) {
                auto result = std::make_shared<MemberLookup>();
                result->state = hierarchy == hierarchies_.end() ? ExpansionState::Deferred : hierarchy->second.state;
                if (result->state != ExpansionState::Error && !own.empty()) { result->state = ExpansionState::Resolved; result->ids = own; result->declarations = own; }
                cache[frame.id.value] = result; stack.pop_back(); continue;
            }
            const auto& fact = hierarchy->second;
            if (frame.next < fact.bases.size()) {
                const auto base = fact.bases[frame.next++];
                if (!cache.contains(base.value)) stack.push_back({base});
                continue;
            }
            if (own.empty() && fact.bases.size() == 1 && fact.state == ExpansionState::Resolved) {
                const auto parent = cache.at(fact.bases.front().value);
                cache[frame.id.value] = parent;
                // A descendant may supply associated types that let a class implementation
                // satisfy an interface requirement which the parent could not compare.
                if (parent->ids.size() > 1 && std::ranges::any_of(parent->ids, [&](BindingId id) {
                        return resolution_.binding(id).kind == BindingKind::Function && interface_owner(id);
                    })) {
                    auto ids = preferred_members(parent->declarations, schema_self(frame.id));
                    if (ids != parent->ids) {
                        auto result = std::make_shared<MemberLookup>(*parent);
                        result->ids = std::move(ids);
                        cache[frame.id.value] = std::move(result);
                    }
                }
            } else {
                auto result = std::make_shared<MemberLookup>();
                result->state = fact.state;
                result->declarations = own;
                const auto receiver = schema_self(frame.id);
                for (const auto base : fact.bases) {
                    const auto& parent = *cache.at(base.value);
                    if (parent.state == ExpansionState::Error) result->state = ExpansionState::Error;
                    else if (parent.state == ExpansionState::Deferred && result->state != ExpansionState::Error) result->state = ExpansionState::Deferred;
                    merge_members(result->declarations, parent.declarations, receiver);
                }
                result->ids = preferred_members(result->declarations, receiver);
                cache[frame.id.value] = result;
            }
            stack.pop_back();
        }
        return cache.at(owner.value);
    }

    MemberLookup symbolic_members(Type receiver, Symbol name, bool types_only = false) {
        MemberLookup result;
        std::vector<Type> work{receiver.scalar()};
        std::unordered_set<usize> seen;
        while (!work.empty()) {
            const auto current = work.back(); work.pop_back();
            if (!known(current)) { result.state = ExpansionState::Deferred; continue; }
            if (!seen.insert(current.detail).second) continue;
            if (symbolic(current)) {
                if (uncertain_bounds_.contains(current.detail)) result.state = ExpansionState::Deferred;
                for (const auto* table : {&formal_bounds_, &projection_bounds_})
                    if (const auto it = table->find(current.detail); it != table->end())
                        for (auto bound : it->second) work.push_back(bound);
                continue;
            }
            if (current.kind != Type::Nominal || resolution_.binding(types_.descriptor(current)->declaration).kind == BindingKind::BuiltinType) {
                result.state = ExpansionState::Deferred; continue;
            }
            const auto found = lookup_member(types_.descriptor(current)->declaration, name, types_only);
            for (const auto& clause : cast_type_decl(resolution_.binding(types_.descriptor(current)->declaration).item)->where)
                if (clause.type->kind == TypeKind::SelfType) result.state = ExpansionState::Deferred;
            if (found->state != ExpansionState::Resolved) result.state = found->state;
            merge_members(result.declarations, found->declarations, receiver.scalar());
        }
        result.ids = preferred_members(result.declarations, receiver.scalar());
        return result;
    }

    Type project(Type owner, BindingId requirement) {
        if (!known(owner) || owner.optional_depth) return Type::Deferred;
        if (projection_depth_ == kDependentDepth) return Type::Deferred;
        const ScopedValue depth(projection_depth_, projection_depth_ + 1);
        if (symbolic(owner)) {
            const auto found = symbolic_members(owner, resolution_.binding(requirement).name.sym, true);
            if (found.state == ExpansionState::Resolved && found.ids.size() == 1 &&
                resolution_.binding(found.ids.front()).kind == BindingKind::TypeAlias && associated_alias(found.ids.front()))
                requirement = found.ids.front();
            const auto type = types_.intern({.kind = Type::Projection, .element = owner, .declaration = requirement});
            if (projection_ids_.insert(type.detail).second) projection_types_.push_back(type);
            if (bounds_ready_ && !projection_bounds_.contains(type.detail)) {
                projection_bounds_.emplace(type.detail, std::vector<Type>{}); // recursive bound guard
                const bool uncertain = uncertain_bounds_.contains(type.detail);
                uncertain_bounds_.insert(type.detail);
                bool pending = found.state != ExpansionState::Resolved;
                const auto add = [&](const TypeExpr* syntax, usize context) {
                    const auto bound = annotation(syntax, context);
                    if (known(bound) && protocol(bound)) projection_bounds_[type.detail].push_back(bound);
                    else pending = true;
                };
                const auto name = resolution_.binding(requirement).name.sym;
                std::vector<Type> work{owner.scalar(), nominal(member_owner(requirement))};
                std::unordered_set<usize> seen_owners;
                while (!work.empty()) {
                    const auto current = work.back(); work.pop_back();
                    if (!known(current) || !seen_owners.insert(current.detail).second) continue;
                    if (symbolic(current)) {
                        pending |= uncertain_bounds_.contains(current.detail);
                        for (const auto* table : {&formal_bounds_, &projection_bounds_})
                            if (const auto it = table->find(current.detail); it != table->end())
                                for (auto bound : it->second) work.push_back(bound);
                        continue;
                    }
                    if (current.kind != Type::Nominal) { pending = true; continue; }
                    const auto interface = types_.descriptor(current)->declaration;
                    if (resolution_.binding(interface).kind != BindingKind::Type) { pending = true; continue; }
                    const auto graph = hierarchies_.find(interface.value);
                    if (graph == hierarchies_.end() || graph->second.state != ExpansionState::Resolved) pending = true;
                    else for (auto base : graph->second.bases) work.push_back(nominal(base));
                    const auto context = owner_context(interface, owner);
                    for (const auto declaration : direct_members(interface, name, true))
                        if (resolution_.binding(declaration).kind == BindingKind::TypeAlias && associated_alias(declaration))
                            for (const auto* bound : cast<TypeAliasItem>(resolution_.binding(declaration).item)->bounds) add(bound, context);
                    for (const auto& clause : cast_type_decl(resolution_.binding(interface).item)->where) {
                        if (clause.type->kind == TypeKind::SelfType) pending = true;
                        if (annotation(clause.type, context) == type) for (const auto* bound : clause.bounds) add(bound, context);
                    }
                }
                if (!pending && !uncertain) uncertain_bounds_.erase(type.detail);
            }
            return type;
        }
        if (owner.kind != Type::Nominal) return Type::Deferred;
        const auto declaration = types_.descriptor(owner)->declaration;
        if (resolution_.binding(declaration).kind != BindingKind::Type || resolution_.binding(declaration).item->kind == ItemKind::Interface) return Type::Deferred;
        if (ancestry(declaration, member_owner(requirement)) != Relation::Yes) return Type::Deferred;
        const TypeDescriptor key{.kind = Type::Projection, .element = owner, .declaration = requirement};
        if (!active_projections_.insert(key).second) return Type::Deferred;
        const auto found = lookup_member(declaration, resolution_.binding(requirement).name.sym, true);
        Type result;
        if (found->state == ExpansionState::Resolved && found->ids.size() == 1) {
            const auto id = found->ids.front();
            const auto* alias = as<TypeAliasItem>(resolution_.binding(id).item);
            if (alias && alias->type && alias->generics.empty() && !associated_alias(id))
                result = annotation(alias->type, owner_context(member_owner(id), owner));
        }
        active_projections_.erase(key);
        return result;
    }

    Type dependent_path(const TypeExpr* syntax, Type receiver, usize first = 1) {
        if (!syntax->args.empty()) return Type::Deferred;
        for (usize p = first; p < syntax->path.parts.size(); ++p) {
            if (!known(receiver) || receiver.optional_depth) return Type::Deferred;
            const auto name = syntax->path.parts[p];
            MemberLookup found;
            if (symbolic(receiver)) found = symbolic_members(receiver, name.sym, true);
            else if (receiver.kind == Type::Nominal && resolution_.binding(types_.descriptor(receiver)->declaration).members.valid())
                found = *lookup_member(types_.descriptor(receiver)->declaration, name.sym, true);
            else return Type::Deferred;
            if (found.state != ExpansionState::Resolved) return Type::Deferred;
            if (found.ids.size() != 1) {
                if (!bounds_ready_) return Type::Deferred;
                diag_.error(found.ids.empty() ? DiagCode::UnknownType : DiagCode::AmbiguousMember, name.span,
                            "{} associated type '{}'", found.ids.empty() ? "unknown" : "ambiguous", names_.str(name.sym));
                return Type::Error;
            }
            const auto id = found.ids.front();
            // Interfaces may also own nested nominal declarations; only aliases are projected.
            if (resolution_.binding(id).kind == BindingKind::TypeAlias && associated_alias(id)) receiver = project(receiver, id);
            else if (const auto* alias = as<TypeAliasItem>(resolution_.binding(id).item); alias && alias->type && alias->generics.empty())
                receiver = annotation(alias->type, owner_context(member_owner(id), receiver));
            else receiver = nominal(id);
        }
        return receiver;
    }

    bool mutable_field(BindingId id) const {
        const auto& binding = resolution_.binding(id);
        if (const auto* field = as<VarItem>(binding.item)) return field->binding == Binding::Var;
        const auto owner = member_owner(id);
        if (owner.valid()) for (const auto& param : cast_type_decl(resolution_.binding(owner).item)->ctor_params)
            if (param.name.span.lo == binding.name.span.lo) return param.binding == Binding::Var;
        return false;
    }
    Relation dependent_contract(Type receiver, BindingId requirement, BindingId& implementation, Type& type) {
        const auto& binding = resolution_.binding(requirement);
        if (receiver.kind != Type::Nominal) return Relation::Deferred;
        const auto source = types_.descriptor(receiver)->declaration;
        const auto* source_item = resolution_.binding(source).item;
        const bool abstract = source_item->kind == ItemKind::Interface || has_modifier(source_item->mods, Modifier::Abstract);
        const auto found = lookup_member(source, binding.name.sym, binding.kind == BindingKind::TypeAlias);
        if (found->state != ExpansionState::Resolved) return Relation::Deferred;
        if (found->ids.size() != 1) return Relation::No;
        const auto candidate = found->ids.front();
        if (binding.kind == BindingKind::TypeAlias) {
            if (associated_alias(candidate)) return cast<TypeAliasItem>(binding.item)->type || abstract ? Relation::Deferred : Relation::No;
            type = project(receiver, requirement);
            if (!known(type)) return Relation::Deferred;
            Relation result = Relation::Yes;
            const auto context = owner_context(member_owner(requirement), receiver);
            for (const auto* bound : cast<TypeAliasItem>(binding.item)->bounds) {
                const auto match = satisfies(type, annotation(bound, context));
                if (match == Relation::No) return match;
                if (match == Relation::Deferred) result = match;
            }
            if (result == Relation::Yes) implementation = candidate;
            return result;
        }
        if (resolution_.binding(candidate).kind != BindingKind::Field || static_member(candidate) != static_member(requirement) ||
            (mutable_field(requirement) && !mutable_field(candidate))) return Relation::No;
        if (interface_owner(candidate)) {
            const auto* field = cast<VarItem>(resolution_.binding(candidate).item);
            if (!field->init && !abstract) return Relation::No;
            return Relation::Deferred;
        }
        const auto from_context = owner_context(member_owner(candidate), receiver);
        const auto to_context = owner_context(member_owner(requirement), receiver);
        for (const auto field : {candidate, requirement})
            if (!nodes_[field.value].annotation && !known(nodes_[field.value].value.type) && nodes_[field.value].state == 0)
                pending_dependencies_.push_back(field.value);
        const auto from = nodes_[candidate.value].annotation ? annotation(nodes_[candidate.value].annotation, from_context)
            : substitute_type(nodes_[candidate.value].value.type, from_context);
        const auto to = nodes_[requirement.value].annotation ? annotation(nodes_[requirement.value].annotation, to_context)
            : substitute_type(nodes_[requirement.value].value.type, to_context);
        if (!known(from) || !known(to)) return Relation::Deferred;
        if (from != to) return Relation::No;
        type = from; implementation = candidate; return Relation::Yes;
    }

    Relation method_contract(BindingId implementation, BindingId requirement, Type receiver = {}) {
        if (!same_method_shape(implementation, requirement)) return Relation::No;
        if (!method_inputs_known(implementation) || !method_inputs_known(requirement)) return Relation::Deferred;
        if (!known(receiver)) receiver = schema_self(member_owner(implementation));
        const auto from_context = owner_context(member_owner(implementation), receiver);
        const auto to_context = owner_context(member_owner(requirement), receiver);
        const auto* a = cast<FunItem>(resolution_.binding(implementation).item);
        const auto* b = cast<FunItem>(resolution_.binding(requirement).item);
        for (usize p = 0; p < a->sig.params.size(); ++p) {
            const auto from = annotation(a->sig.params[p].type, from_context), to = annotation(b->sig.params[p].type, to_context);
            if (!known(from) || !known(to)) return Relation::Deferred;
            if (from != to) return Relation::No;
        }
        if (has_modifier(a->mods, Modifier::Mut) && !has_modifier(b->mods, Modifier::Mut)) return Relation::No;
        if (a->sig.throws && !b->sig.throws) return Relation::No;
        if (has_modifier(a->mods, Modifier::Async) != has_modifier(b->mods, Modifier::Async) ||
            has_modifier(a->mods, Modifier::Unsafe) != has_modifier(b->mods, Modifier::Unsafe)) return Relation::Deferred;
        if (a->sig.throws && b->sig.throws && (a->sig.throws_type || b->sig.throws_type)) {
            if (!a->sig.throws_type || !b->sig.throws_type) return Relation::Deferred;
            const auto errors = relation(annotation(a->sig.throws_type, from_context), annotation(b->sig.throws_type, to_context));
            if (errors != Relation::Yes) return errors;
        }
        const auto from = a->sig.ret ? annotation(a->sig.ret, from_context) : substitute_type(nodes_[function_nodes_.at(implementation.value)].value.type, from_context);
        const auto to = b->sig.ret ? annotation(b->sig.ret, to_context) : substitute_type(nodes_[function_nodes_.at(requirement.value)].value.type, to_context);
        return known(from) && known(to) ? relation(from, to) : Relation::Deferred;
    }

    bool inherited_self_contract(BindingId implementation, BindingId requirement, Type receiver) const {
        if (receiver.kind != Type::Nominal || interface_owner(implementation) ||
            member_owner(implementation) == types_.descriptor(receiver)->declaration) return false;
        const auto& sig = cast<FunItem>(resolution_.binding(requirement).item)->sig;
        if (contains_self_type(sig.ret) || contains_self_type(sig.throws_type)) return true;
        return std::ranges::any_of(sig.params, [&](const Param& param) { return contains_self_type(param.type); });
    }

    // Declared ancestry alone does not prove a constraint: the supported
    // requirement signatures must have compatible implementations as well.
    Relation satisfies(Type source, Type bound) {
        if (!known(source) || !known(bound)) return Relation::Deferred;
        if (bound.optional_depth) return Relation::No;
        if (symbolic(bound)) return Relation::Deferred;
        if (bound.kind != Type::Nominal) return Relation::No;
        const auto target = types_.descriptor(bound)->declaration;
        if (resolution_.binding(target).kind != BindingKind::BuiltinType && resolution_.binding(target).item->kind != ItemKind::Interface) return Relation::No;
        if (resolution_.binding(target).kind == BindingKind::BuiltinType && source.optional_depth) return Relation::Deferred;
        if (symbolic(source) || formal_bounds_.contains(source.detail) || uncertain_bounds_.contains(source.detail))
            return source.optional_depth ? Relation::No : formal_relation(source, bound);
        if (resolution_.binding(target).kind == BindingKind::BuiltinType) return Relation::Deferred;
        if (resolution_.binding(target).item->kind != ItemKind::Interface) return Relation::No;
        if (source == Type::Never) return Relation::Deferred;
        if (source.kind != Type::Nominal || source.optional_depth) return Relation::No;
        const auto owner = types_.descriptor(source)->declaration;
        if (!types_.descriptor(bound)->arguments.empty()) return Relation::Deferred;
        const auto declared = ancestry(owner, target);
        if (declared != Relation::Yes) return declared;
        if (active_conformances_.size() >= kDependentDepth || !active_conformances_.insert(target.value).second) return Relation::Deferred;
        struct Active { std::unordered_set<usize>& set; usize key; ~Active() { set.erase(key); } } active{active_conformances_, target.value};
        const auto* item = resolution_.binding(owner).item;
        const bool abstract = item->kind == ItemKind::Interface || has_modifier(item->mods, Modifier::Abstract);
        Relation result = Relation::Yes;
        std::unordered_set<usize> seen;
        std::vector<BindingId> work{target};
        while (!work.empty()) {
            const auto next = work.back(); work.pop_back();
            if (!seen.insert(next.value).second) continue;
            const auto graph = hierarchies_.find(next.value);
            if (graph == hierarchies_.end() || graph->second.state != ExpansionState::Resolved) { result = Relation::Deferred; continue; }
            for (const auto base : graph->second.bases) work.push_back(base);
            for (const auto requirement : resolution_.scopes()[resolution_.binding(next).members.value].bindings) {
                const auto& b = resolution_.binding(requirement);
                if (b.kind == BindingKind::TypeAlias || b.kind == BindingKind::Field) {
                    if (b.kind == BindingKind::Field && static_member(requirement) && cast<VarItem>(b.item)->init) continue;
                    BindingId implementation; Type type;
                    const auto contract = dependent_contract(source, requirement, implementation, type);
                    if (contract == Relation::No) return contract;
                    if (contract == Relation::Deferred) result = contract;
                    continue;
                }
                if (b.kind != BindingKind::Function) continue;
                if (!method_inputs_known(requirement)) { result = Relation::Deferred; continue; }
                const auto available = lookup_member(owner, b.name.sym);
                bool pending = available->state != ExpansionState::Resolved;
                bool inherited_self = false;
                usize matches = 0;
                for (const auto candidate : available->ids) {
                    if (resolution_.binding(candidate).kind != BindingKind::Function ||
                        (!abstract && !cast<FunItem>(resolution_.binding(candidate).item)->body)) continue;
                    if (!same_method_shape(candidate, requirement)) continue;
                    for (const auto function : {candidate, requirement}) {
                        const auto node = function_nodes_.at(function.value);
                        if (!known(nodes_[node].value.type) && nodes_[node].state == 0) pending_dependencies_.push_back(node);
                    }
                    const auto contract = method_contract(candidate, requirement, source);
                    inherited_self |= contract == Relation::No && inherited_self_contract(candidate, requirement, source);
                    pending |= contract == Relation::Deferred;
                    matches += contract == Relation::Yes;
                }
                if (!matches && inherited_self) pending = true;
                if (pending) result = Relation::Deferred;
                else if (!matches || (!abstract && matches != 1)) return Relation::No;
            }
            if (constrained(next)) {
                const auto conditions = constraint_status(next, owner_context(next, source));
                if (conditions == Relation::No) return conditions;
                if (conditions == Relation::Deferred) result = conditions;
            }
        }
        return result;
    }

    Relation constraint_status(BindingId id, usize context) {
        if (invalid_constraints_.contains(id.value)) return Relation::No;
        if (!constrained(id)) return Relation::Yes;
        if (active_conditions_.size() >= kDependentDepth || !active_conditions_.insert(id.value).second) return Relation::Deferred;
        struct Active { std::unordered_set<usize>& set; usize key; ~Active() { set.erase(key); } } active{active_conditions_, id.value};
        Relation result = Relation::Yes;
        const auto check = [&](Type source, const TypeExpr* syntax) {
            const auto match = satisfies(source, annotation(syntax, context));
            if (match == Relation::No) result = Relation::No;
            else if (match == Relation::Deferred && result != Relation::No) result = Relation::Deferred;
        };
        for (const auto& param : generic_parameters(id)) {
            const auto source = formal_argument(generic_bindings_.at(param.name.span.lo), context);
            for (const auto* bound : param.bounds) check(source, bound);
        }
        List<WhereClause> where;
        const auto* item = resolution_.binding(id).item;
        if (const auto* fun = as<FunItem>(item)) where = fun->sig.where;
        else if (const auto* type = as_type_decl(item)) where = type->where;
        for (const auto& clause : where) {
            const auto source = annotation(clause.type, context);
            for (const auto* bound : clause.bounds) check(source, bound);
        }
        return result;
    }

    void remember_constraints(BindingId id, usize context, Span span) {
        if (probing_constraints_ || !constrained(id)) return;
        if (active_constraint_ && constraint_uses_[*active_constraint_].recursive) return;
        for (usize next = context; next; next = environments_[next].parent)
            for (const auto arg : environments_[next].arguments) if (!known(arg)) return;
        for (const auto& param : generic_parameters(id))
            if (!known(substituted(generic_bindings_.at(param.name.span.lo), context))) return;
        if (!constraint_spans_[id.value][context].insert(span.lo).second) return;
        bool recursive = false;
        for (auto parent = active_constraint_; parent; parent = constraint_uses_[*parent].parent)
            if (constraint_uses_[*parent].declaration == id) { recursive = true; break; }
        constraint_uses_.push_back({id, context, span, active_constraint_, recursive});
    }
    void constraint_error(BindingId id, Span span) {
        if (invalid_constraints_.contains(id.value)) return; // definition diagnostic already explains every application
        if (!reported_constraints_.insert(span.lo).second) return;
        auto& diagnostic = diag_.error(DiagCode::GenericConstraint, span, "type arguments do not satisfy interface constraints of '{}'", names_.str(resolution_.binding(id).name.sym));
        diagnostic.notes.push_back({resolution_.binding(id).name.span, "constrained declaration here"});
    }
    void validate_constraints() {
        for (usize i = 0; i < constraint_uses_.size(); ++i) {
            const auto use = constraint_uses_[i];
            const ScopedValue active(active_constraint_, std::optional<usize>{i});
            auto state = constraint_status(use.declaration, use.context);
            // A declaration repeated along this dependency path may grow its
            // arguments forever. Check its immediate facts, but do not enqueue
            // further applications or claim that recursive validity is proven.
            if (use.recursive && state != Relation::No) state = Relation::Deferred;
            CheckedConstraint record{.span = use.span, .declaration = use.declaration,
                                     .state = state == Relation::Yes ? ExpansionState::Resolved :
                                              state == Relation::No ? ExpansionState::Error : ExpansionState::Deferred};
            for (const auto& param : generic_parameters(use.declaration))
                record.arguments.push_back(substituted(generic_bindings_.at(param.name.span.lo), use.context));
            constraints_.push_back(std::move(record));
            if (state == Relation::No) constraint_error(use.declaration, use.span);
        }
    }

    void check_super_arguments() {
        for (const auto owner : hierarchy_order_) {
            const auto& fact = hierarchies_.at(owner.value);
            if (fact.state != ExpansionState::Resolved || !fact.superclass.valid()) continue;
            const auto* parent = cast_type_decl(resolution_.binding(fact.superclass).item);
            if (std::ranges::any_of(parent->members, [](const Item* item) { const auto* init = as<InitItem>(item); return init && init->has_params; })) continue;
            std::unordered_map<u32, usize> labels;
            for (usize p = 0; p < parent->ctor_params.size(); ++p) labels.emplace(parent->ctor_params[p].name.sym.id, p);
            for (const auto& base : cast_type_decl(resolution_.binding(owner).item)->supers) {
                if (!base.has_args) continue; // an explicit super(...) body can delegate later
                const auto type = annotation(base.type);
                if (type.kind != Type::Nominal || types_.descriptor(type)->declaration != fact.superclass) continue;
                if (std::ranges::any_of(base.args, [&](const Arg& arg) { return value(arg.value).type == Type::Error; })) continue;
                CallExpr arguments;
                arguments.args = base.args;
                std::vector<usize> mapping;
                bool matches = argument_mapping(arguments, parent->ctor_params, labels, mapping);
                if (matches) for (usize a = 0; a < base.args.size(); ++a)
                    matches &= probe(expressions_.at(base.args[a].value), annotation(parent->ctor_params[mapping[a]].type)).matches;
                if (!matches) diag_.error(DiagCode::NoMatchingCall, base.type->span, "arguments do not match superclass constructor");
                else for (usize a = 0; a < base.args.size(); ++a)
                    (void)require(base.args[a].value, annotation(parent->ctor_params[mapping[a]].type), base.args[a].value->span);
            }
        }
    }

    void check_hierarchy_contracts() {
        std::unordered_map<usize, std::shared_ptr<const std::vector<BindingId>>> obligations;
        for (const auto owner : hierarchy_order_) {
            auto& fact = hierarchies_.at(owner.value);
            if (fact.state == ExpansionState::Error) { fact.conformance = ExpansionState::Error; obligations[owner.value] = std::make_shared<std::vector<BindingId>>(); continue; }
            const auto* item = cast_type_decl(resolution_.binding(owner).item);
            fact.conformance = fact.state;
            auto required = std::make_shared<std::vector<BindingId>>();
            for (const auto base : fact.bases) {
                if (const auto it = obligations.find(base.value); it != obligations.end())
                    for (const auto id : *it->second) if (std::ranges::find(*required, id) == required->end()) required->push_back(id);
                if (const auto it = hierarchies_.find(base.value); it != hierarchies_.end()) {
                    fact.dependent |= it->second.dependent;
                    if (it->second.conformance == ExpansionState::Error) fact.conformance = ExpansionState::Error;
                }
            }
            if (fact.conformance == ExpansionState::Error) { obligations[owner.value] = required; continue; }
            if (fact.dependent) fact.conformance = ExpansionState::Deferred;
            bool added = false;
            if (item->kind == ItemKind::Interface && !item->where.empty()) { required->push_back(owner); added = true; }
            for (const auto id : resolution_.scopes()[resolution_.binding(owner).members.value].bindings) {
                const auto& b = resolution_.binding(id);
                if (item->kind == ItemKind::Interface && (b.kind == BindingKind::Field || b.kind == BindingKind::TypeAlias)) {
                    if (b.kind == BindingKind::TypeAlias || !static_member(id) || !cast<VarItem>(b.item)->init) {
                        required->push_back(id); added = true;
                    }
                }
                if (b.kind != BindingKind::Function) continue;
                const auto* fun = cast<FunItem>(b.item);
                if (item->kind == ItemKind::Interface || (!static_member(id) && !fun->body)) {
                    required->push_back(id); added = true;
                    if (!method_inputs_known(id) || !known(nodes_[function_nodes_.at(id.value)].value.type)) {
                        fact.conformance = ExpansionState::Deferred; fact.dependent = true;
                    }
                }
                bool found = false, pending = fact.state != ExpansionState::Resolved ||
                    (has_modifier(fun->mods, Modifier::Override) && !method_inputs_known(id)), invalid = false;
                for (const auto base : fact.bases) {
                    const auto inherited = lookup_member(base, b.name.sym);
                    pending |= inherited->state != ExpansionState::Resolved;
                    for (const auto parent : inherited->ids) {
                        if (resolution_.binding(parent).kind != BindingKind::Function || !same_method_shape(id, parent)) continue;
                        pending |= !method_inputs_known(id) || !method_inputs_known(parent);
                        if (!same_method_inputs(id, parent)) continue;
                        found = true;
                        const auto* declaration = cast<FunItem>(resolution_.binding(parent).item);
                        const auto* parent_owner = cast_type_decl(resolution_.binding(member_owner(parent)).item);
                        const bool virtual_method = interface_owner(parent) || has_modifier(declaration->mods, Modifier::Open) ||
                            has_modifier(declaration->mods, Modifier::Abstract) || (has_modifier(parent_owner->mods, Modifier::Abstract) && !declaration->body);
                        const auto contract = method_contract(id, parent);
                        pending |= contract == Relation::Deferred;
                        if (has_modifier(fun->mods, Modifier::Override) && static_member(id)) invalid = true;
                        else if (!static_member(id) && (!interface_owner(parent) || has_modifier(fun->mods, Modifier::Override) || item->kind == ItemKind::Interface) &&
                                 (!virtual_method || has_modifier(declaration->mods, Modifier::Final) ||
                                 (!interface_owner(parent) && !has_modifier(fun->mods, Modifier::Override)) || contract == Relation::No)) invalid = true;
                    }
                }
                if (has_modifier(fun->mods, Modifier::Override) && !found && !pending) invalid = true;
                if (invalid) {
                    diag_.error(DiagCode::InvalidOverride, b.name.span, "invalid override of method '{}'", names_.str(b.name.sym));
                    fact.conformance = ExpansionState::Error;
                } else if (pending && fact.conformance != ExpansionState::Error) { fact.conformance = ExpansionState::Deferred; fact.dependent = true; }
            }
            // Reuse obligation lists along chains with no new requirements.
            if (!added && fact.bases.size() == 1 && obligations.contains(fact.bases.front().value)) obligations[owner.value] = obligations.at(fact.bases.front().value);
            else obligations[owner.value] = required;
            if (fact.conformance == ExpansionState::Error) continue;
            if (item->kind == ItemKind::Interface) continue;
            for (const auto requirement : *obligations.at(owner.value)) {
                ConformanceWitness witness{.requirement = requirement};
                const auto& binding = resolution_.binding(requirement);
                bool pending = fact.state != ExpansionState::Resolved;
                usize matches = 0;
                if (binding.kind == BindingKind::Type) {
                    const auto contract = constraint_status(requirement, owner_context(requirement, nominal(owner)));
                    pending |= contract == Relation::Deferred;
                    if (contract == Relation::Yes) { witness.implementation = owner; matches = 1; }
                } else if (binding.kind != BindingKind::Function) {
                    BindingId implementation; Type type;
                    const auto contract = dependent_contract(nominal(owner), requirement, implementation, type);
                    pending |= contract == Relation::Deferred;
                    if (contract == Relation::Yes) { witness.implementation = implementation; witness.type = type; matches = 1; }
                } else if (!method_inputs_known(requirement)) pending = true;
                else {
                    const auto available = lookup_member(owner, binding.name.sym);
                    pending |= available->state != ExpansionState::Resolved;
                    bool inherited_self = false;
                    for (const auto id : available->ids) {
                        if (resolution_.binding(id).kind != BindingKind::Function || !cast<FunItem>(resolution_.binding(id).item)->body) continue;
                        const auto contract = method_contract(id, requirement, nominal(owner));
                        inherited_self |= contract == Relation::No && inherited_self_contract(id, requirement, nominal(owner));
                        pending |= contract == Relation::Deferred;
                        if (contract == Relation::Yes) { witness.implementation = id; ++matches; }
                    }
                    if (!matches && inherited_self) pending = true;
                }
                if (matches == 1 && !pending) witness.state = ExpansionState::Resolved;
                else if (!pending && !has_modifier(item->mods, Modifier::Abstract)) {
                    auto& d = diag_.error(DiagCode::MissingImplementation, item->name.span,
                                          "type '{}' has {} implementation of '{}'", names_.str(item->name.sym),
                                          matches ? "ambiguous" : "no compatible", names_.str(binding.name.sym));
                    d.notes.push_back({binding.name.span, "requirement declared here"});
                    witness.state = ExpansionState::Error;
                    fact.conformance = ExpansionState::Error;
                } else if (fact.conformance != ExpansionState::Error) fact.conformance = ExpansionState::Deferred;
                fact.dependent |= pending;
                if (witness.state != ExpansionState::Resolved) witness.implementation = {};
                fact.witnesses.push_back(witness);
            }
        }
    }

    // Receiver facts select a declaration before adding its dependency. Method
    // overloads do not depend on their bodies until a call selects one target.
    BindingId enclosing_owner(ScopeId scope) const {
        while (scope.valid()) {
            if (const auto it = member_owners_.find(scope.value); it != member_owners_.end()) return it->second;
            scope = resolution_.scopes()[scope.value].parent;
        }
        return {};
    }
    MemberLookup outer_lookup(BindingId owner, Symbol name, bool types_only = false) {
        for (auto scope = resolution_.binding(owner).scope; scope.valid(); scope = resolution_.scopes()[scope.value].parent) {
            if (const auto it = member_owners_.find(scope.value); it != member_owners_.end()) {
                const auto found = lookup_member(it->second, name, types_only);
                if (found->state != ExpansionState::Resolved || !found->ids.empty()) return *found;
            } else {
                MemberLookup result;
                std::vector<BindingId> types;
                for (const auto id : resolution_.scopes()[scope.value].bindings) {
                    const auto& b = resolution_.binding(id);
                    if (b.name.sym != name) continue;
                    if (b.kind == BindingKind::Type || b.kind == BindingKind::TypeAlias || b.kind == BindingKind::GenericParameter || b.kind == BindingKind::BuiltinType) types.push_back(id);
                    else if (!types_only) result.ids.push_back(id);
                }
                if (result.ids.empty()) result.ids = std::move(types);
                if (!result.ids.empty()) return result;
            }
        }
        return {};
    }
    ExpandedType inherited_type(const TypeExpr* syntax, BindingId* declaration = nullptr) {
        ExpandedType current;
        for (usize p = 0; p < syntax->path.parts.size(); ++p) {
            if (p && current.optional_depth) return {};
            const auto name = syntax->path.parts[p];
            const auto* reference = resolution_.reference(name.span);
            std::vector<BindingId> ids;
            if (reference && reference->state == ResolutionState::Resolved) {
                const auto candidates = resolution_.candidates(reference->target);
                ids.assign(candidates.begin(), candidates.end());
            } else {
                const auto owner = p == 0 && reference ? enclosing_owner(reference->scope) : current.target;
                if (!owner.valid() || !resolution_.binding(owner).members.valid() || !known(nominal(owner))) return {};
                MemberLookup found;
                if (hierarchy_ready_) found = *lookup_member(owner, name.sym, true);
                else found.ids = direct_members(owner, name.sym, true);
                if (hierarchy_ready_ && p == 0 && found.state == ExpansionState::Resolved && found.ids.empty()) found = outer_lookup(owner, name.sym, true);
                if (found.state != ExpansionState::Resolved) return {found.state, {}, found.state == ExpansionState::Error ? Type{Type::Error} : Type{}};
                ids = std::move(found.ids);
                if (ids.empty()) {
                    if (!hierarchy_ready_) return {}; // inherited candidates may become available later
                    diag_.error(DiagCode::UnknownType, name.span, "unknown inherited type '{}'", names_.str(name.sym));
                    return {ExpansionState::Error, {}, Type::Error};
                }
                if (ids.size() > 1) {
                    diag_.error(DiagCode::AmbiguousMember, name.span, "ambiguous inherited type '{}'", names_.str(name.sym));
                    return {ExpansionState::Error, {}, Type::Error};
                }
            }
            if (ids.size() != 1) return {};
            if (declaration && p + 1 == syntax->path.parts.size()) *declaration = ids.front();
            current = aliases_.binding(ids.front(), resolution_).type;
            if (current.state == ExpansionState::Deferred && current.syntax == syntax) {
                diag_.error(DiagCode::AliasCycle, syntax->span, "cyclic type alias expansion through inherited lookup");
                return {ExpansionState::Error, {}, Type::Error};
            }
            if (p + 1 < syntax->path.parts.size() && current.state != ExpansionState::Resolved) return {};
        }
        return current;
    }
    bool value_binding(BindingId id) const {
        const auto kind = resolution_.binding(id).kind;
        return kind == BindingKind::Field || kind == BindingKind::Variable || kind == BindingKind::Local ||
               kind == BindingKind::Parameter || kind == BindingKind::Pattern || kind == BindingKind::BuiltinValue;
    }
    std::optional<BindingId> prepare_reference(const Expr* expr) {
        const auto* name_expr = as<NameExpr>(expr);
        const auto* member = as<MemberExpr>(expr);
        const Name name = name_expr ? name_expr->name : member->name;
        CheckedMember record;
        std::vector<BindingId> candidates;
        const auto* reference = resolution_.reference(name.span);
        if (name_expr) {
            if (!reference) return std::nullopt;
            if (reference->state == ResolutionState::Resolved) {
                const auto ids = resolution_.candidates(reference->target);
                if (ids.size() == 1) entity(expr, ids.front());
                if (ids.empty()) return std::nullopt;
                record.owner = member_owner(ids.front());
                if (!record.owner.valid()) return std::nullopt;
            } else record.owner = enclosing_owner(reference->scope);
            if (!record.owner.valid()) return std::nullopt;
            if (!known(nominal(record.owner))) { members_[expr] = std::move(record); return std::nullopt; }
            record.receiver_type = schema_self(record.owner);
        } else {
            const auto base = value(member->base);
            const auto type = entity(member->base);
            if (base.type == Type::Error) record.state = ResolutionState::Unresolved;
            else if (type.state == ExpansionState::Resolved && type.target.valid() && !type.optional_depth &&
                     resolution_.binding(type.target).kind == BindingKind::Type && (known(type.scalar) || known(nominal(type.target)))) {
                record.owner = type.target;
                record.type_receiver = true;
            } else if (base.type.kind == Type::Nominal && !base.type.optional_depth)
                record.owner = types_.descriptor(base.type)->declaration;
            if (base.type.kind == Type::Nominal && !base.type.optional_depth) record.receiver_type = base.type;
            else if (record.type_receiver) record.receiver_type = type.scalar;
            else if (symbolic(base.type) && !base.type.optional_depth) record.receiver_type = base.type;
            else if (type.state == ExpansionState::Resolved && symbolic(type.scalar) && !type.optional_depth) {
                record.receiver_type = type.scalar; record.type_receiver = true;
            }
            if (!symbolic(record.receiver_type)) {
                if (!record.owner.valid() || !resolution_.binding(record.owner).members.valid()) { members_[expr] = std::move(record); return std::nullopt; }
            }
        }
        auto found = symbolic(record.receiver_type) ? symbolic_members(record.receiver_type, name.sym) : *lookup_member(record.owner, name.sym);
        if (symbolic(record.receiver_type) && !found.ids.empty()) record.owner = member_owner(found.ids.front());
        if (record.receiver_type.kind == Type::Nominal &&
            (!types_.descriptor(record.receiver_type)->arguments.empty() || constrained(record.owner))) {
            const auto* owner = cast_type_decl(resolution_.binding(record.owner).item);
            if (owner->supers.empty()) {
                found.state = ExpansionState::Resolved;
                found.ids = direct_members(record.owner, name.sym, false);
                const auto args = types_.descriptor(record.receiver_type)->arguments;
                reference_contexts_[expr] = application(record.owner, args, 0, expr->span).context;
            }
        }
        if (uncertain_bounds_.contains(record.receiver_type.detail) ||
            (!symbolic(record.receiver_type) && formal_bounds_.contains(record.receiver_type.detail))) found.state = ExpansionState::Deferred;
        if (name_expr && found.state == ExpansionState::Resolved && found.ids.empty()) {
            found = outer_lookup(record.owner, name.sym);
            if (!found.ids.empty()) record.owner = member_owner(found.ids.front());
        }
        if (found.state != ExpansionState::Resolved) {
            record.state = found.state == ExpansionState::Error ? ResolutionState::Unresolved : ResolutionState::Deferred;
            record.candidates = std::move(found.ids);
            members_[expr] = std::move(record);
            return std::nullopt;
        }
        candidates = std::move(found.ids);
        if (candidates.empty()) {
            if (name_expr) diag_.error(DiagCode::UnknownValue, name.span, "unknown name '{}'", names_.str(name.sym));
            else diag_.error(DiagCode::UnknownMember, name.span, "type '{}' has no member '{}'",
                             scalar_type_name(record.receiver_type), names_.str(name.sym));
            record.state = ResolutionState::Unresolved;
            members_[expr] = std::move(record);
            return std::nullopt;
        }
        for (const auto id : candidates) {
            const bool is_static = static_member(id);
            const bool allowed = name_expr ? !record.owner.valid() || is_static ||
                (reference && reference->receiver.valid() && ancestry(reference->receiver, member_owner(id)) == Relation::Yes)
                                           : is_static == record.type_receiver;
            if (allowed) record.candidates.push_back(id);
        }
        if (record.candidates.empty()) {
            diag_.error(DiagCode::InvalidMemberReceiver, name.span, "member '{}' is unavailable through this {} receiver",
                        names_.str(name.sym), name_expr ? "implicit" : record.type_receiver ? "type" : "instance");
            record.state = ResolutionState::Unresolved;
        } else if (record.candidates.size() > 1 && std::ranges::any_of(record.candidates, [&](BindingId id) { return resolution_.binding(id).kind != BindingKind::Function; })) {
            diag_.error(DiagCode::AmbiguousMember, name.span, "ambiguous inherited member '{}'", names_.str(name.sym));
            record.state = ResolutionState::Unresolved;
        } else record.state = ResolutionState::Resolved;
        std::optional<BindingId> field;
        if (record.candidates.size() == 1) {
            const auto id = record.candidates.front();
            if (record.owner.valid() && known(record.receiver_type)) reference_contexts_[expr] = owner_context(member_owner(id), record.receiver_type);
            entity(expr, id, reference_contexts_.contains(expr) ? reference_contexts_.at(expr) : 0);
            if (value_binding(id)) field = id;
        }
        members_[expr] = std::move(record);
        return field;
    }

    std::span<const BindingId> callees(const Expr* expr) const {
        for (;;) {
            if (const auto* paren = as<ParenExpr>(expr)) expr = paren->inner;
            else if (const auto* generic = as<GenericInstExpr>(expr)) expr = generic->base;
            else break;
        }
        if (const auto it = members_.find(expr); it != members_.end())
            return it->second.state == ResolutionState::Resolved ? std::span<const BindingId>(it->second.candidates) : std::span<const BindingId>{};
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
                // A used result is final only after its tail expression.
                if (!discarded && !stmts.empty())
                    if (const auto* tail = as<ExprStmt>(stmts.back())) dependency(index, tail->expr);
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
    // `none` stays deferred until optional contexts are checked, but it can
    // never fit a concrete non-optional type.
    bool none_mismatch(usize index, Type expected) const {
        const Expr* expr = nodes_[index].expr;
        while (const auto* paren = as<ParenExpr>(expr)) expr = paren->inner;
        return expr->kind == ExprKind::None && known(expected) && !expected.optional_depth &&
               expected.kind != Type::TypeParameter && expected.kind != Type::Projection;
    }
    Value compatible(usize index, Type expected, Span span) {
        Value actual = nodes_[index].value;
        if (none_mismatch(index, expected)) {
            diag_.error(DiagCode::ScalarTypeMismatch, span, "expected '{}', found 'none'", scalar_type_name(expected));
            return {Type::Error};
        }
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
        const auto match = relation(default_type(actual), expected);
        if (match == Relation::Deferred &&
            !(actual.literal == Literal::Array && expected.kind != Type::Array && !protocol(expected))) return {};
        if (actual.type == Type::Never) return actual;
        if (actual.literal == Literal::None && match == Relation::Yes) return {expected};
        diag_.error(DiagCode::ScalarTypeMismatch, span, "expected '{}', found '{}'",
                    scalar_type_name(expected), scalar_type_name(default_type(actual)));
        if (actual.literal != Literal::None) materialize(index, default_type(actual));
        // Preserve the expression's actual type; the declaration/check fails.
        return {Type::Error};
    }
    // A used block's final expression and each `if`/`match` branch supply the
    // value. Diverging tails produce Never. Without `else`, the false path
    // produces `none` when an optional is expected and no value otherwise.
    struct Tail {
        enum Kind : u8 { Value, Unit, MissingElse } kind;
        const Expr* expr;
    };
    static const Expr* branching(const Expr* expr) {
        while (const auto* paren = as<ParenExpr>(expr)) expr = paren->inner;
        return expr && (expr->kind == ExprKind::If || expr->kind == ExprKind::Block || expr->kind == ExprKind::Unsafe ||
                        expr->kind == ExprKind::Match) ? expr : nullptr;
    }
    // Whether a `break` leaves this loop. Nested loop bodies and cleanups
    // have their own targets; nested conditions and iterables do not.
    static bool breaks_out(const LoopStmt& loop) {
        std::vector<const Stmt*> stmts;
        std::vector<const Expr*> exprs{loop.body};
        while (!exprs.empty() || !stmts.empty()) {
            if (!stmts.empty()) {
                const Stmt* stmt = stmts.back();
                stmts.pop_back();
                switch (stmt->kind) {
                    case StmtKind::Break: return true;
                    case StmtKind::Var: exprs.push_back(cast<VarStmt>(stmt)->init); break;
                    case StmtKind::Return: exprs.push_back(cast<ReturnStmt>(stmt)->value); break;
                    case StmtKind::Throw: exprs.push_back(cast<ThrowStmt>(stmt)->value); break;
                    case StmtKind::Expr: exprs.push_back(cast<ExprStmt>(stmt)->expr); break;
                    case StmtKind::Assign:
                        exprs.push_back(cast<AssignStmt>(stmt)->target);
                        exprs.push_back(cast<AssignStmt>(stmt)->value);
                        break;
                    case StmtKind::While: exprs.push_back(cast<WhileStmt>(stmt)->cond.expr); break;
                    case StmtKind::For: exprs.push_back(cast<ForStmt>(stmt)->iter); break;
                    case StmtKind::Loop: case StmtKind::Defer: case StmtKind::Continue: break;
                }
                continue;
            }
            const Expr* expr = exprs.back();
            exprs.pop_back();
            if (!expr) continue;
            switch (expr->kind) {
                case ExprKind::Block:
                    // Statements after an unconditional jump are unreachable.
                    for (const Stmt* stmt : cast<BlockExpr>(expr)->stmts) {
                        stmts.push_back(stmt);
                        if (stmt->kind == StmtKind::Return || stmt->kind == StmtKind::Throw ||
                            stmt->kind == StmtKind::Break || stmt->kind == StmtKind::Continue) break;
                    }
                    break;
                case ExprKind::Unsafe: exprs.push_back(cast<UnsafeExpr>(expr)->block); break;
                case ExprKind::Paren: exprs.push_back(cast<ParenExpr>(expr)->inner); break;
                case ExprKind::Unary: exprs.push_back(cast<UnaryExpr>(expr)->operand); break;
                case ExprKind::Binary:
                    exprs.push_back(cast<BinaryExpr>(expr)->lhs);
                    exprs.push_back(cast<BinaryExpr>(expr)->rhs);
                    break;
                case ExprKind::Call:
                    exprs.push_back(cast<CallExpr>(expr)->callee);
                    for (const auto& arg : cast<CallExpr>(expr)->args) exprs.push_back(arg.value);
                    break;
                case ExprKind::If: {
                    const auto& branch = *cast<IfExpr>(expr);
                    exprs.push_back(branch.cond.expr);
                    exprs.push_back(branch.then_block);
                    exprs.push_back(branch.else_branch);
                    break;
                }
                case ExprKind::Match:
                    exprs.push_back(cast<MatchExpr>(expr)->scrutinee);
                    for (const auto& arm : cast<MatchExpr>(expr)->arms) {
                        exprs.push_back(arm.guard);
                        exprs.push_back(arm.body);
                    }
                    break;
                case ExprKind::Member: exprs.push_back(cast<MemberExpr>(expr)->base); break;
                case ExprKind::OptMember: exprs.push_back(cast<OptMemberExpr>(expr)->base); break;
                case ExprKind::Index:
                    exprs.push_back(cast<IndexExpr>(expr)->base);
                    exprs.push_back(cast<IndexExpr>(expr)->index);
                    break;
                case ExprKind::Cast: exprs.push_back(cast<CastExpr>(expr)->operand); break;
                case ExprKind::Unwrap: exprs.push_back(cast<UnwrapExpr>(expr)->operand); break;
                case ExprKind::GenericInst: exprs.push_back(cast<GenericInstExpr>(expr)->base); break;
                case ExprKind::Array: for (const Expr* e : cast<ArrayExpr>(expr)->elems) exprs.push_back(e); break;
                case ExprKind::Interp: for (const Expr* e : cast<InterpExpr>(expr)->exprs) exprs.push_back(e); break;
                default: break;
            }
        }
        return false;
    }
    static void tails(const Expr* root, std::vector<Tail>& out) {
        std::vector<const Expr*> work{root};
        while (!work.empty()) {
            const Expr* expr = work.back();
            work.pop_back();
            while (const auto* paren = as<ParenExpr>(expr)) expr = paren->inner;
            if (const auto* block = as<BlockExpr>(expr)) {
                const Stmt* last = block->stmts.empty() ? nullptr : block->stmts.back();
                if (const auto* tail = last ? as<ExprStmt>(last) : nullptr) work.push_back(tail->expr);
                else if (!last || (last->kind != StmtKind::Return && last->kind != StmtKind::Throw &&
                                   last->kind != StmtKind::Break && last->kind != StmtKind::Continue &&
                                   !(last->kind == StmtKind::Loop && !breaks_out(*cast<LoopStmt>(last)))))
                    out.push_back({Tail::Unit, block});
            } else if (const auto* unsafe = as<UnsafeExpr>(expr)) work.push_back(unsafe->block);
            else if (const auto* branch = as<IfExpr>(expr)) {
                if (branch->else_branch) work.push_back(branch->else_branch);
                else out.push_back({Tail::MissingElse, branch});
                work.push_back(branch->then_block);
            } else if (const auto* match = as<MatchExpr>(expr)) {
                for (auto it = match->arms.rbegin(); it != match->arms.rend(); ++it) work.push_back(it->body);
            } else out.push_back({Tail::Value, expr});
        }
    }
    Value require_branches(const Expr* root, Type expected) {
        std::vector<Tail> found;
        tails(root, found);
        Value result{expected};
        for (const auto& tail : found) {
            if (tail.kind == Tail::Value) {
                const auto checked = compatible(expressions_.at(tail.expr), expected, tail.expr->span);
                if (checked.type == Type::Error) result = checked;
                else if (unavailable(checked) && result.type != Type::Error) result = {};
            } else if (tail.kind == Tail::MissingElse) {
                if (expected.optional_depth || promotes(Type::Unit, expected)) continue;
                diag_.error(DiagCode::IfWithoutElse, tail.expr->span,
                            "'if' without 'else' has no '{}' value when its condition is false; add an 'else' or expect '{}?'",
                            scalar_type_name(expected), scalar_type_name(expected));
                result = {Type::Error};
            } else if (!promotes(Type::Unit, expected)) {
                diag_.error(DiagCode::ScalarTypeMismatch, tail.expr->span, "expected '{}', found 'Unit'", scalar_type_name(expected));
                result = {Type::Error};
            }
        }
        return result;
    }
    Value require(const Expr* expr, Type expected, Span span) {
        if (expr && known(expected) && branching(expr)) return require_branches(branching(expr), expected);
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
                    const auto checked = branching(element) ? require_branches(branching(element), element_type)
                                                            : compatible(child, element_type, element->span);
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
        const bool equality = expr.op == BinOp::Eq || expr.op == BinOp::Ne;
        if (equality) {
            // Validate independently inferred array elements before deferring
            // equality. Check both operands even when one literal fails.
            if (lhs.literal == Literal::Array && known(lhs.type)) lhs = materialize(expressions_.at(expr.lhs), lhs.type);
            if (rhs.literal == Literal::Array && known(rhs.type)) rhs = materialize(expressions_.at(expr.rhs), rhs.type);
        }
        if (lhs.type == Type::Error || rhs.type == Type::Error) return {Type::Error};
        if (expr.op == BinOp::Coalesce || expr.op == BinOp::Range || expr.op == BinOp::RangeExcl) return {};
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
            if (known(target) && branching(nodes_[index].expr)) {
                std::vector<Tail> found;
                tails(branching(nodes_[index].expr), found);
                for (const auto& tail : found) {
                    if (tail.kind == Tail::Value) work.push_back({expressions_.at(tail.expr), target});
                    else if (!promotes(Type::Unit, target) && !(tail.kind == Tail::MissingElse && target.optional_depth))
                        result.matches = false;
                }
                continue;
            }
            if (none_mismatch(index, target)) { result.matches = false; continue; }
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
                else {
                    result.score += actual.type.scalar() != target.scalar();
                    result.lifts += target.optional_depth - actual.type.optional_depth;
                }
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

    bool defaults_known(List<Param> params, std::span<const usize> supplied, std::span<const Type> types) {
        (void)types; // defaults are checked once against the declaration's symbolic parameter types
        bool complete = true;
        for (usize p = 0; p < params.size(); ++p) {
            const auto* argument = params[p].default_value;
            if (!argument || std::ranges::find(supplied, p) != supplied.end()) continue;
            if (!known(annotation(params[p].type))) { complete = false; continue; }
            const auto index = expressions_.at(argument);
            if (nodes_[index].state == 0) pending_dependencies_.push_back(index);
            const auto fact = probe(index, annotation(params[p].type));
            complete &= fact.matches && !fact.deferred && fact.fits;
        }
        return complete;
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
        auto target = entity(callee);
        if (target.state != ExpansionState::Resolved || target.optional_depth || !numeric(target.scalar)) {
            const auto* member = as<MemberExpr>(callee);
            if (!member) return std::nullopt;
            const auto name = names_.str(member->name.sym);
            if (name == "checked") kind = ConversionKind::Checked;
            else if (name == "truncating") kind = ConversionKind::Truncating;
            else return std::nullopt;
            target = entity(member->base);
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

    // print(value:) takes one String, Int, UInt or Bool, positionally or with
    // its label, and returns Unit (3.27). Integer literals select Int.
    std::optional<Value> prepare_builtin(const CallExpr& expr) {
        const Expr* callee = expr.callee;
        while (const auto* paren = as<ParenExpr>(callee)) callee = paren->inner;
        if (callee->kind != ExprKind::Name) return std::nullopt;
        const auto ids = callees(callee);
        if (ids.size() != 1) return std::nullopt;
        const auto& binding = resolution_.binding(ids.front());
        if (binding.kind != BindingKind::BuiltinValue || names_.str(binding.name.sym) != "print") return std::nullopt;
        auto& record = calls_[&expr];
        record.kind = CallKind::Builtin;
        record.target = ids.front();
        const auto reject = [&](std::string_view found) -> Value {
            diag_.error(DiagCode::NoMatchingCall, expr.span,
                        "print takes one String, Int, UInt or Bool argument{}", found);
            record.state = CallState::Error;
            return Value{Type::Error};
        };
        if (expr.args.size() != 1 || (!expr.args[0].label.sym.empty() && names_.str(expr.args[0].label.sym) != "value"))
            return reject("");
        record.parameters = {0};
        const Expr* arg = expr.args[0].value;
        const Value source = value(arg);
        if (source.type == Type::Error) {
            record.state = CallState::Error;
            return Value{Type::Error};
        }
        // `none` stays deferred without an optional context, which print never
        // gives, so reject it here, in the argument or any branch tail.
        std::vector<Tail> found;
        tails(arg, found);
        for (const auto& tail : found)
            if (tail.kind == Tail::Value && tail.expr->kind == ExprKind::None) return reject(", found 'none'");
        Type parameter = source.type;
        if (unavailable(source)) {
            // Branching arguments are checked through their tails, so probe each
            // overload as ordinary calls do and keep the best fitting match.
            std::optional<std::pair<Probe, Type>> best;
            for (const Type candidate : {Type{Type::String}, Type{Type::Int}, Type{Type::UInt}, Type{Type::Bool}}) {
                const auto p = probe(expressions_.at(arg), candidate);
                if (p.deferred) return Value{};
                if (p.matches && (!best || std::pair{!p.fits, p.score} < std::pair{!best->first.fits, best->first.score}))
                    best = {p, candidate};
            }
            if (!best) return reject("");
            parameter = best->second;
        } else if (source.literal == Literal::Integer) parameter = Type::Int;
        else if (source.literal != Literal::None || source.type.optional_depth ||
                 (source.type != Type::String && source.type != Type::Int && source.type != Type::UInt &&
                  source.type != Type::Bool && source.type != Type::Never))
            return reject(std::format(", found '{}'", scalar_type_name(default_type(source))));
        if (parameter == Type::Never) parameter = Type::Unit;
        record.parameter_types = {parameter};
        record.state = CallState::Resolved;
        if (parameter != Type::Unit && require(arg, parameter, arg->span).type == Type::Error) record.state = CallState::Error;
        return Value{record.state == CallState::Error ? Type{Type::Error} : Type{Type::Unit}};
    }

    std::optional<Value> prepare_constructor(const CallExpr& expr) {
        auto type = entity(expr.callee);
        bool inferred = false;
        if (type.state != ExpansionState::Resolved || !type.target.valid()) {
            const Expr* callee = expr.callee;
            while (const auto* paren = as<ParenExpr>(callee)) callee = paren->inner;
            if (callee->kind == ExprKind::GenericInst) return std::nullopt;
            const auto candidates = callees(callee);
            if (candidates.size() != 1 || resolution_.binding(candidates.front()).kind != BindingKind::Type ||
                generic_parameters(candidates.front()).empty()) return std::nullopt;
            type.target = candidates.front();
            inferred = true;
        }
        if (type.optional_depth) return std::nullopt;
        const auto& binding = resolution_.binding(type.target);
        if (binding.kind != BindingKind::Type) return std::nullopt;
        const auto* item = cast_type_decl(binding.item);
        auto& record = calls_[&expr];
        record.kind = CallKind::NominalConstructor;
        record.target = type.target;
        Type result = known(type.scalar) ? type.scalar : nominal(type.target);
        if (item->kind == ItemKind::Interface || has_modifier(item->mods, Modifier::Abstract)) {
            diag_.error(DiagCode::NotConstructible, expr.span, "type '{}' cannot be constructed directly",
                        known(result) ? scalar_type_name(result) : std::string(names_.str(binding.name.sym)));
            record.state = CallState::Error;
            return Value{Type::Error};
        }
        if (item->kind == ItemKind::Enum) return Value{};
        for (const Item* member : item->members)
            if (const auto* init = as<InitItem>(member); init && init->has_params) return Value{};
        std::unordered_map<u32, usize> labels;
        for (usize p = 0; p < item->ctor_params.size(); ++p) labels.emplace(item->ctor_params[p].name.sym.id, p);
        bool matches = argument_mapping(expr, item->ctor_params, labels, record.parameters), deferred = false, fits = true, pending_arguments = false;
        usize context = 0;
        std::vector<Type> arguments;
        if (inferred && matches) {
            const auto inference = infer_arguments(type.target, item->ctor_params, expr, record.parameters, 0);
            context = inference.context;
            arguments = inference.arguments;
            matches &= inference.state != Relation::No;
            deferred |= inference.state == Relation::Deferred;
            pending_arguments = inference.state == Relation::Deferred;
            if (inference.state == Relation::Yes) result = instance(type.target, context);
        } else if (result.kind == Type::Nominal && !types_.descriptor(result)->arguments.empty()) {
            arguments = types_.descriptor(result)->arguments;
            context = application(type.target, arguments, 0, expr.span).context;
        }
        deferred |= !known(result);
        std::vector<Type> parameters;
        for (const auto& param : item->ctor_params) {
            const auto expected = annotation(param.type, context);
            parameters.push_back(pending_arguments && owned_formal(expected, record.target) ? Type{} : expected);
            deferred |= param.conv != Convention::Default;
        }
        if (matches && context) deferred |= !defaults_known(item->ctor_params, record.parameters, parameters);
        for (const auto& arg : expr.args) if (value(arg.value).type == Type::Error) {
            record.state = CallState::Error; return Value{Type::Error};
        }
        if (matches) for (usize a = 0; a < expr.args.size(); ++a) {
            const auto p = probe(expressions_.at(expr.args[a].value), parameters[record.parameters[a]]);
            matches &= p.matches; deferred |= p.deferred; fits &= p.fits;
        }
        if (!matches) {
            auto& d = diag_.error(DiagCode::NoMatchingCall, expr.span, "arguments do not match the primary constructor of '{}'", names_.str(binding.name.sym));
            d.notes.push_back({binding.name.span, "type declared here"});
            record.state = CallState::Error;
            return Value{Type::Error};
        }
        const auto contract = constraint_status(type.target, context);
        remember_constraints(type.target, context, expr.span);
        if (contract == Relation::No && fits) {
            constraint_error(type.target, expr.span);
            record.state = CallState::Error; return Value{Type::Error};
        }
        deferred |= contract == Relation::Deferred;
        if (deferred && fits) return Value{};
        record.state = CallState::Resolved;
        record.parameter_types = parameters;
        record.type_arguments = std::move(arguments);
        record.inferred_type_arguments = inferred;
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
        pending_dependencies_.clear();
        calls_[&expr] = {}; // a deferred application may resume after pending dependencies
        if (auto conversion = prepare_conversion(expr)) return *conversion;
        if (auto builtin = prepare_builtin(expr)) return *builtin;
        if (auto constructor = prepare_constructor(expr)) return *constructor;
        auto& record = calls_[&expr];
        const Value callee = value(expr.callee);
        if (callee.type == Type::Error) { record.state = CallState::Error; return {Type::Error}; }
        for (const auto& arg : expr.args) {
            if (value(arg.value).type == Type::Error) { record.state = CallState::Error; return {Type::Error}; }
        }
        const Expr* receiver = expr.callee;
        while (const auto* paren = as<ParenExpr>(receiver)) receiver = paren->inner;
        const auto* generic = as<GenericInstExpr>(receiver);
        std::vector<Type> type_arguments;
        if (generic) {
            for (auto* arg : generic->args) type_arguments.push_back(annotation(arg));
            receiver = generic->base;
            while (const auto* paren = as<ParenExpr>(receiver)) receiver = paren->inner;
        }
        if (receiver->kind == ExprKind::Super) {
            record.kind = CallKind::NominalConstructor;
            if (callee.type.kind == Type::Nominal) record.target = types_.descriptor(callee.type)->declaration;
            return {}; // constructor delegation/initialization ordering is a later pass
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
        for (BindingId id : candidates) {
            const auto& b = resolution_.binding(id);
            if (b.kind != BindingKind::Function) return {};
            if (resolution_.scopes()[b.scope.value].kind == ScopeKind::Members) {
                record.kind = CallKind::Method;
                if (const auto* member = as<MemberExpr>(receiver); member && !members_.at(member).type_receiver)
                    record.receiver = member->base;
            } else if (resolution_.scopes()[b.scope.value].kind != ScopeKind::File) return {};
        }
        struct Match {
            BindingId id;
            std::vector<usize> parameters;
            usize score = 0;
            usize lifts = 0;
            bool deferred = false;
            usize context = 0;
            std::vector<Type> types;
            std::vector<Type> arguments;
        };
        std::vector<Match> matches, out_of_range;
        struct FailedConstraint { BindingId id; usize context; };
        std::vector<FailedConstraint> failed_constraints;
        bool generic_arity = !generic;
        for (BindingId id : candidates) {
            const auto* fun = cast<FunItem>(resolution_.binding(id).item);
            const auto& signature = signatures_.at(id.value);
            const ScopedValue probing(probing_constraints_, true);
            if (generic && fun->sig.generics.size() != type_arguments.size()) continue;
            generic_arity = true;
            usize context = reference_contexts_.contains(receiver) ? reference_contexts_.at(receiver) : 0;
            if (const auto member = members_.find(receiver); member != members_.end() && known(member->second.receiver_type))
                context = owner_context(member_owner(id), member->second.receiver_type);
            Match match{.id = id, .arguments = type_arguments};
            if (!argument_mapping(expr, fun->sig.params, parameter_indices_.at(fun), match.parameters)) continue;
            bool pending = false;
            if (generic) {
                const auto applied = application(id, type_arguments, context, generic->span);
                if (applied.state == ExpansionState::Error) { record.state = CallState::Error; return {Type::Error}; }
                pending |= applied.state != ExpansionState::Resolved;
                context = applied.context;
            } else if (signature.generic) {
                const auto inferred = infer_arguments(id, fun->sig.params, expr, match.parameters, context);
                if (inferred.state == Relation::No) continue;
                pending |= inferred.state != Relation::Yes;
                context = inferred.context;
                match.arguments = inferred.arguments;
            }
            match.context = context;
            const auto contract = constraint_status(id, context);
            match.deferred = pending || contract == Relation::Deferred;
            BindingId failed = contract == Relation::No ? id : BindingId{};
            const auto owner = member_owner(id);
            if (owner.valid()) {
                const auto owner_contract = constraint_status(owner, context);
                match.deferred |= owner_contract == Relation::Deferred;
                if (owner_contract == Relation::No) failed = owner;
            }
            for (const auto& param : fun->sig.params) {
                const auto expected = annotation(param.type, context);
                match.types.push_back(pending && owned_formal(expected, id) ? Type{} : expected);
                match.deferred |= (owner.valid() || signature.generic) && param.conv != Convention::Default;
            }
            if (interface_owner(id) && !method_inputs_known(id) && !signature.generic) match.deferred = true;
            if (context) match.deferred |= !defaults_known(fun->sig.params, match.parameters, match.types);
            bool compatible_args = true, fits = true;
            for (usize a = 0; a < expr.args.size(); ++a) {
                const Type expected = match.types[match.parameters[a]];
                const auto p = probe(expressions_.at(expr.args[a].value), expected);
                match.deferred |= p.deferred;
                match.score += p.score; match.lifts += p.lifts;
                fits &= p.fits; compatible_args &= p.matches;
            }
            if (!compatible_args) continue;
            if (fits && failed.valid()) { failed_constraints.push_back({failed, context}); continue; }
            if (fits) matches.push_back(std::move(match));
            else out_of_range.push_back(std::move(match));
        }
        if (!generic_arity) {
            diag_.error(DiagCode::GenericArguments, generic->span, "type argument count does not match any callable signature");
            record.state = CallState::Error; return {Type::Error};
        }
        if (matches.empty() && out_of_range.size() != 1) {
            if (!failed_constraints.empty()) {
                const auto failure = failed_constraints.front();
                remember_constraints(failure.id, failure.context, expr.span);
                constraint_error(failure.id, expr.span);
                record.state = CallState::Error; return {Type::Error};
            }
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
            if (matches.size() == 1 && matches.front().deferred)
                remember_constraints(matches.front().id, matches.front().context, expr.span);
            for (const auto& match : matches) if (match.deferred) return {};
            auto best = std::pair{std::numeric_limits<usize>::max(), std::numeric_limits<usize>::max()};
            std::vector<Match*> tied;
            for (auto& match : matches) {
                const auto rank = std::pair{match.score, match.lifts};
                if (rank < best) { best = rank; tied.clear(); tied.push_back(&match); }
                else if (rank == best) tied.push_back(&match);
            }
            // An otherwise identical non-generic input profile is more specific
            // than a function whose own parameters had to be inferred.
            const auto profiles = tied;
            std::erase_if(tied, [&](const Match* candidate) {
                if (!signatures_.at(candidate->id.value).generic) return false;
                return std::ranges::any_of(profiles, [&](const Match* other) {
                    if (signatures_.at(other->id.value).generic) return false;
                    for (usize a = 0; a < expr.args.size(); ++a)
                        if (candidate->types[candidate->parameters[a]] != other->types[other->parameters[a]]) return false;
                    return true;
                });
            });
            if (tied.size() == 1) selected = tied.front();
            else for (auto* candidate : tied) {
                bool dominates = true;
                for (const auto* other : tied) {
                    if (candidate == other) continue;
                    bool strict = false;
                    for (usize a = 0; a < expr.args.size(); ++a) {
                        const auto from = candidate->types[candidate->parameters[a]];
                        const auto to = other->types[other->parameters[a]];
                        if (from == to) continue;
                        strict = true;
                        if (relation(from, to) != Relation::Yes) { dominates = false; break; }
                    }
                    if (!strict) dominates = false;
                    if (!dominates) break;
                }
                if (dominates) { selected = candidate; break; }
            }
            if (!selected) {
                auto& d = diag_.error(DiagCode::AmbiguousCall, expr.span, "call is ambiguous between matching signatures");
                candidate_notes(d, candidates);
                record.state = CallState::Error;
                return {Type::Error};
            }
        }
        PHS_ASSERT(selected);
        const auto* selected_function = cast<FunItem>(resolution_.binding(selected->id).item);
        for (const auto& parameter : selected_function->sig.params)
            (void)annotation(parameter.type, selected->context); // commit only the selected signature's type uses
        record.target = selected->id;
        record.parameters = std::move(selected->parameters);
        record.state = CallState::Resolved;
        call_contexts_[&expr] = selected->context;
        record.type_arguments = selected->arguments;
        record.inferred_type_arguments = !generic && signatures_.at(selected->id.value).generic;
        record.parameter_types = selected->types;
        for (usize a = 0; a < expr.args.size(); ++a) {
            if (require(expr.args[a].value, selected->types[record.parameters[a]], expr.args[a].value->span).type == Type::Error)
                record.state = CallState::Error;
        }
        if (record.state == CallState::Error) return {Type::Error};
        remember_constraints(selected->id, selected->context, expr.span);
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
            case ExprKind::Super: {
                const auto* reference = resolution_.reference(expr->span);
                if (reference && reference->receiver.valid()) {
                    const auto it = hierarchies_.find(reference->receiver.value);
                    if (it != hierarchies_.end()) {
                        if (it->second.state == ExpansionState::Error) node.value = {Type::Error};
                        else if (it->second.state == ExpansionState::Resolved && it->second.superclass.valid()) node.value = {nominal(it->second.superclass)};
                    }
                }
                break;
            }
            case ExprKind::SelfValue: {
                const auto* reference = resolution_.reference(expr->span);
                if (reference && reference->state == ResolutionState::Resolved) {
                    const auto ids = resolution_.candidates(reference->target);
                    if (ids.size() == 1) node.value = {schema_self(ids.front())};
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
                    if (!known(element) && known(value_type.type) &&
                        (value_type.literal == Literal::None || value_type.concrete_anchor)) element = value_type.type;
                }
                const bool concrete_anchor = known(element);
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
                else node.value = {known(element) ? types_.intern({.kind = Type::Array, .element = element}) : Type{},
                                   Literal::Array, context_pending && !known(element), concrete_anchor};
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
            case ExprKind::Member: {
                const auto it = members_.find(expr);
                if (it != members_.end()) {
                    const auto& record = it->second;
                    if (record.state == ResolutionState::Unresolved) node.value = {Type::Error};
                    else if (record.state == ResolutionState::Resolved && record.candidates.size() == 1 && value_binding(record.candidates.front()))
                        node.value = nodes_[record.candidates.front().value].value;
                    if (record.state == ResolutionState::Resolved && record.candidates.size() == 1 && reference_contexts_.contains(expr) &&
                        value_binding(record.candidates.front())) {
                        const auto& field = nodes_[record.candidates.front().value];
                        const auto context = reference_contexts_.at(expr);
                        // Inferred field types are symbolic in their owner, so instantiate them through the receiver too.
                        if (field.annotation) node.value = {annotation(field.annotation, context)};
                        else node.value.type = substitute_type(node.value.type, context);
                    }
                } else if (expr->kind == ExprKind::Name && !node.deps.empty()) node.value = nodes_[node.deps.front()].value;
                break;
            }
            case ExprKind::GenericInst: {
                const auto* generic = cast<GenericInstExpr>(expr);
                if (value(generic->base).type == Type::Error) { node.value = {Type::Error}; break; }
                std::vector<Type> args;
                for (auto* arg : generic->args) args.push_back(annotation(arg));
                if (std::ranges::find(args, Type{Type::Error}) != args.end()) { node.value = {Type::Error}; break; }
                const auto ids = callees(generic->base);
                if (ids.size() == 1) {
                    const auto id = ids.front();
                    const auto kind = resolution_.binding(id).kind;
                    if (kind == BindingKind::Type || kind == BindingKind::TypeAlias || kind == BindingKind::BuiltinType) {
                        const Expr* base = generic->base;
                        while (const auto* paren = as<ParenExpr>(base)) base = paren->inner;
                        const auto context = reference_contexts_.contains(base) ? reference_contexts_.at(base) : 0;
                        const auto applied = application(id, std::move(args), context, expr->span);
                        if (applied.state == ExpansionState::Error) { node.value = {Type::Error}; break; }
                        if (applied.state != ExpansionState::Resolved) break;
                        remember_constraints(id, applied.context, expr->span);
                        Type result;
                        if (kind == BindingKind::TypeAlias) {
                            const auto* alias = cast<TypeAliasItem>(resolution_.binding(id).item);
                            if (alias->type && !associated_alias(id) && !constrained(id)) result = annotation(alias->type, applied.context);
                        } else result = instance(id, applied.context);
                        if (result == Type::Error) { node.value = {Type::Error}; break; }
                        if (known(result)) entities_[expr] = type_entity(result);
                        break;
                    }
                }
                if ((!ids.empty() && resolution_.binding(ids.front()).kind != BindingKind::Function) || known(value(generic->base).type)) {
                    diag_.error(DiagCode::GenericArguments, expr->span, "type arguments require a generic declaration");
                    node.value = {Type::Error};
                }
                break;
            }
            case ExprKind::SelfType: {
                const auto* reference = resolution_.reference(expr->span);
                if (reference && reference->state == ResolutionState::Resolved) {
                    const auto ids = resolution_.candidates(reference->target);
                    if (ids.size() == 1) entities_[expr] = type_entity(schema_self(ids.front()));
                }
                break;
            }
            case ExprKind::Paren: {
                const auto* inner = cast<ParenExpr>(expr)->inner;
                node.value = value(inner);
                if (const auto type = entity(inner); type.state == ExpansionState::Resolved) entities_[expr] = type;
                break;
            }
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
                const auto context = call_contexts_.at(cast<CallExpr>(expr));
                const auto* fun = cast<FunItem>(resolution_.binding(record.target).item);
                if (context) node.value = {fun->sig.ret ? annotation(fun->sig.ret, context) : substitute_type(node.value.type, context)};
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

    void retain_bodies() {
        std::vector<Span> errors, pending;
        for (const auto& diagnostic : diag_.all()) if (diagnostic.severity == Severity::Error) errors.push_back(diagnostic.span);
        for (const auto& [expr, call] : calls_) {
            if (call.state == CallState::Error) errors.push_back(expr->span);
            else if (call.state == CallState::Deferred) pending.push_back(expr->span);
        }
        for (const auto& [expr, member] : members_) if (member.state == ResolutionState::Deferred) pending.push_back(expr->span);
        for (const auto& contract : constraints_) {
            if (contract.state == ExpansionState::Deferred) pending.push_back(contract.span);
            else if (contract.state == ExpansionState::Error) errors.push_back(contract.span);
        }
        for (const auto& requirement : requirements_) if (requirement.value) {
            const auto expected = requirement.target ? value(requirement.target).type : requirement.expected;
            if (!known(expected) || probe(expressions_.at(requirement.value), expected).deferred) pending.push_back(requirement.span);
        }
        for (const auto* expr : defaults_)
            if (expr->kind != ExprKind::Block && expr->kind != ExprKind::If && expr->kind != ExprKind::Match && expr->kind != ExprKind::Unsafe &&
                unavailable(value(expr))) pending.push_back(expr->span);
        for (usize n = 0; n < resolution_.bindings().size(); ++n) {
            const auto& binding = resolution_.bindings()[n];
            if (binding.kind == BindingKind::Local || binding.kind == BindingKind::Field) {
                if (!known(nodes_[n].value.type)) pending.push_back(binding.name.span);
                if (nodes_[n].init && (unavailable(value(nodes_[n].init)) ||
                    (nodes_[n].annotation && probe(expressions_.at(nodes_[n].init), annotation(nodes_[n].annotation)).deferred))) pending.push_back(nodes_[n].init->span);
            }
        }
        for (usize id = 0; id < resolution_.bindings().size(); ++id) {
            const auto& binding = resolution_.bindings()[id];
            if (binding.kind != BindingKind::Function && binding.kind != BindingKind::Type) continue;
            bool schema = !generic_parameters(BindingId{id}).empty() || (binding.item && binding.item->kind == ItemKind::Interface);
            for (auto scope = binding.scope; scope.valid() && !schema; scope = resolution_.scopes()[scope.value].parent)
                if (resolution_.scopes()[scope.value].kind == ScopeKind::Generics)
                    for (auto parameter : resolution_.scopes()[scope.value].bindings)
                        schema |= resolution_.binding(parameter).kind == BindingKind::GenericParameter;
            schema |= binding.kind == BindingKind::Function && interface_owner(BindingId{id});
            if (!schema) continue;
            const auto span = binding.item->span;
            const auto inside = [&](Span use) { return use.file == span.file && use.lo >= span.lo && use.hi <= span.hi; };
            auto state = std::ranges::any_of(pending, inside) ? ExpansionState::Deferred : ExpansionState::Resolved;
            if (binding.kind == BindingKind::Function && !known(nodes_[function_nodes_.at(id)].value.type)) state = ExpansionState::Deferred;
            if (binding.kind == BindingKind::Type) {
                const auto graph = hierarchies_.find(id);
                if (graph != hierarchies_.end() && graph->second.conformance == ExpansionState::Deferred) state = ExpansionState::Deferred;
            }
            if (std::ranges::any_of(errors, inside)) state = ExpansionState::Error;
            bodies_[id] = {state};
        }
    }

    const Resolution& resolution_;
    const Interner& names_;
    DiagEngine& diag_;
    AliasExpansion aliases_;
    TypeTable types_;
    std::unordered_map<const TypeExpr*, Type> annotations_;
    std::unordered_map<usize, std::vector<Type>> formal_bounds_;
    std::unordered_map<usize, std::vector<Type>> projection_bounds_;
    std::unordered_set<usize> uncertain_bounds_;
    std::unordered_set<usize> invalid_constraints_;
    std::unordered_set<usize> projection_ids_;
    std::vector<Type> projection_types_;
    std::unordered_set<TypeDescriptor, TypeDescriptorHash> active_projections_;
    std::unordered_set<usize> active_conformances_;
    std::unordered_set<usize> active_conditions_;
    bool bounds_ready_ = false;
    usize projection_depth_ = 0;
    std::unordered_map<usize, CheckedBody> bodies_;
    std::unordered_map<u32, BindingId> generic_bindings_;
    std::vector<Environment> environments_{{}};
    std::unordered_map<usize, std::unordered_map<TypeDescriptor, usize, TypeDescriptorHash>> environment_ids_;
    std::vector<std::unordered_map<const TypeExpr*, Type>> substituted_annotations_{1};
    std::vector<std::unordered_map<const TypeExpr*, Type>> speculative_annotations_{1};
    bool probing_constraints_ = false;
    std::unordered_set<u32> reported_applications_;
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
    std::unordered_map<const Expr*, CheckedMember> members_;
    std::unordered_map<const Expr*, usize> reference_contexts_;
    std::unordered_map<const CallExpr*, usize> call_contexts_;
    std::unordered_map<const Expr*, ExpandedType> entities_;
    std::unordered_map<usize, BindingId> member_owners_;
    std::unordered_map<usize, std::unordered_map<u32, std::vector<BindingId>>> member_values_;
    std::unordered_map<usize, CheckedHierarchy> hierarchies_;
    std::vector<BindingId> hierarchy_order_;
    bool hierarchy_ready_ = false;
    std::unordered_set<const TypeExpr*> reported_bases_;
    std::unordered_set<usize> reported_cycles_;
    mutable std::unordered_map<usize, std::unordered_map<usize, Relation>> ancestry_cache_;
    std::unordered_map<u64, std::unordered_map<usize, std::shared_ptr<const MemberLookup>>> inherited_members_;
    std::vector<Walk> walk_;
    std::vector<Requirement> requirements_;
    struct ConstraintUse {
        BindingId declaration;
        usize context;
        Span span;
        std::optional<usize> parent;
        bool recursive = false;
    };
    std::vector<ConstraintUse> constraint_uses_;
    std::optional<usize> active_constraint_;
    std::unordered_map<usize, std::unordered_map<usize, std::unordered_set<u32>>> constraint_spans_;
    std::unordered_set<u32> reported_constraints_;
    std::vector<CheckedConstraint> constraints_;
    std::vector<usize> pending_dependencies_;
    std::vector<const Expr*> defaults_;
};

ScalarTypes check_scalar_types(const ast::File& file, const Resolution& resolution,
                              const Interner& names, DiagEngine& diag) {
    return ScalarChecker(resolution, names, diag).run(file);
}

}  // namespace phs
