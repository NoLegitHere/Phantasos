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

#include "codegen/gate.hpp"

#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

namespace phs {
namespace {
using namespace ast;

std::string_view expr_name(ExprKind kind) {
    switch (kind) {
        case ExprKind::Float: return "floating-point values";
        case ExprKind::Char: return "character values";
        case ExprKind::String: return "string literals";
        case ExprKind::Interp: return "string interpolation";
        case ExprKind::None: return "'none'";
        case ExprKind::SelfValue: return "'self'";
        case ExprKind::Super: return "'super'";
        case ExprKind::SelfType: return "'Self'";
        case ExprKind::ImplicitMember: return "implicit member expressions";
        case ExprKind::Array: return "array literals";
        case ExprKind::Cast: return "casts";
        case ExprKind::Member: return "member access";
        case ExprKind::OptMember: return "optional member access";
        case ExprKind::Index: return "indexing";
        case ExprKind::Unwrap: return "optional unwrapping";
        case ExprKind::GenericInst: return "explicit generic arguments";
        case ExprKind::Match: return "'match'";
        case ExprKind::Unsafe: return "'unsafe' blocks";
        default: return "this expression";
    }
}

std::string_view unary_name(UnOp op) {
    switch (op) {
        case UnOp::Try: return "'try'";
        case UnOp::TryOpt: return "'try?'";
        case UnOp::Await: return "'await'";
        case UnOp::Share: return "sharing";
        default: return "this operator";
    }
}

std::string_view binary_name(BinOp op) {
    switch (op) {
        case BinOp::Coalesce: return "'?\?'";
        case BinOp::Range: case BinOp::RangeExcl: return "ranges";
        case BinOp::Shl: case BinOp::Shr: return "shifts";
        default: return "this operator";
    }
}

bool supported_binary(BinOp op) {
    return op != BinOp::Coalesce && op != BinOp::Range && op != BinOp::RangeExcl && op != BinOp::Shl &&
           op != BinOp::Shr;
}

bool supported_unary(UnOp op) { return op == UnOp::Neg || op == UnOp::Not || op == UnOp::BitNot; }

bool supported_type(ScalarType type) {
    if (type.optional_depth != 0) return false;
    switch (type.kind) {
        case ScalarType::Int8: case ScalarType::Int16: case ScalarType::Int32: case ScalarType::Int64:
        case ScalarType::UInt8: case ScalarType::UInt16: case ScalarType::UInt32: case ScalarType::UInt64:
        case ScalarType::Int: case ScalarType::UInt: case ScalarType::Bool: case ScalarType::Unit:
        case ScalarType::Never:
            return true;
        default: return false;
    }
}

// Iterative over expressions and statements. Control expressions in discarded
// positions have no value, so only their parts are checked.
class Gate {
public:
    Gate(const Resolution& resolution, const ScalarTypes& types, const Interner& names, DiagEngine& diag)
        : resolution_(resolution), types_(types), names_(names), diag_(diag) {
        for (usize id = 0; id < resolution.bindings().size(); ++id) {
            const auto& binding = resolution.bindings()[id];
            if (binding.item) items_.emplace(binding.item, BindingId{id});
            if (binding.kind == BindingKind::Local || binding.kind == BindingKind::Parameter)
                values_.emplace(binding.name.span.lo, BindingId{id});
        }
    }

    bool run(const File& file) {
        for (const auto& import : file.imports) unsupported(import.span, "imports");
        std::vector<const FunItem*> mains;
        for (const Item* item : file.items) {
            const auto* fun = as<FunItem>(item);
            if (!fun) {
                unsupported(item->kind == ItemKind::Extern ? item->span : item->name.span, item_name(item->kind));
                continue;
            }
            functions_.insert(fun);
            if (names_.str(fun->name.sym) == "main") mains.push_back(fun);
        }
        for (const Item* item : file.items)
            if (const auto* fun = as<FunItem>(item)) function(*fun);
        entry(file, mains);
        return ok_;
    }

private:
    struct Work {
        std::variant<const Expr*, const Stmt*> node;
        bool used;
    };

    static std::string_view item_name(ItemKind kind) {
        switch (kind) {
            case ItemKind::Class: return "classes";
            case ItemKind::Struct: return "structs";
            case ItemKind::Actor: return "actors";
            case ItemKind::Interface: return "interfaces";
            case ItemKind::Enum: return "enums";
            case ItemKind::Var: return "global variables";
            case ItemKind::Extern: return "extern blocks";
            case ItemKind::TypeAlias: return "type aliases";
            default: return "this declaration";
        }
    }

    void unsupported(Span span, std::string_view what) {
        ok_ = false;
        diag_.error(DiagCode::UnsupportedCodegen, span, "stage0 code generation does not support {} yet", what);
    }

    void unsupported_type(Span span, ScalarType type) {
        if (type.kind == ScalarType::Deferred) {
            ok_ = false;
            diag_.error(DiagCode::UnsupportedCodegen, span,
                        "stage0 code generation needs a known type here; this type is not inferred yet");
            return;
        }
        ok_ = false;
        diag_.error(DiagCode::UnsupportedCodegen, span, "stage0 code generation does not support values of type '{}' yet",
                    types_.type_name(type, resolution_, names_, 256));
    }

    bool type(Span span, ScalarType type) {
        if (supported_type(type)) return true;
        unsupported_type(span, type);
        return false;
    }

    // Parameters and locals hold a value, so 'Unit' and 'Never' are not stored.
    bool stored(Span span, ScalarType value, std::string_view what) {
        if (!type(span, value)) return false;
        if (value.kind != ScalarType::Unit && value.kind != ScalarType::Never) return true;
        ok_ = false;
        diag_.error(DiagCode::UnsupportedCodegen, span, "stage0 code generation does not support {} of type '{}' yet", what,
                    value.kind == ScalarType::Unit ? "Unit" : "Never");
        return false;
    }

    BindingId target(const NameExpr& name) const {
        const auto* ref = resolution_.reference(name.name.span);
        if (!ref || ref->state != ResolutionState::Resolved) return {};
        const auto ids = resolution_.candidates(ref->target);
        return ids.size() == 1 ? ids.front() : BindingId{};
    }

    void function(const FunItem& fun) {
        const bool before = ok_;
        ok_ = true;
        if (!fun.attrs.empty()) unsupported(fun.attrs.front().span, "attributes");
        if (has_modifier(fun.mods, Modifier::Async)) unsupported(fun.name.span, "'async' functions");
        if (has_modifier(fun.mods, Modifier::Unsafe)) unsupported(fun.name.span, "'unsafe' functions");
        if (!fun.sig.generics.empty()) unsupported(fun.name.span, "generic functions");
        if (fun.sig.throws) unsupported(fun.name.span, "throwing functions");
        if (!fun.body) unsupported(fun.name.span, "functions without a body");
        if (ok_) {
            for (const auto& param : fun.sig.params) {
                if (param.conv != Convention::Default) unsupported(param.name.span, "parameter conventions");
                if (param.default_value) unsupported(param.default_value->span, "parameter defaults");
                const auto it = values_.find(param.name.span.lo);
                if (it != values_.end()) stored(param.name.span, types_.binding(it->second), "parameters");
            }
            const auto id = items_.find(&fun);
            const ScalarSignature* sig = id == items_.end() ? nullptr : types_.signature(id->second);
            const Span result_span = fun.sig.ret ? fun.sig.ret->span : fun.name.span;
            if (!sig) unsupported_type(result_span, ScalarType{});
            else type(result_span, sig->result);
        }
        // Report a body only once its signature is supported, to avoid repeating
        // the same unsupported type. A block body's tail is not the result.
        if (ok_) body(fun.body, fun.expr_body);
        ok_ = ok_ && before;
    }

    void body(const Expr* root, bool used) {
        std::vector<Work> work{{root, used}};
        while (!work.empty()) {
            const auto [node, value] = work.back();
            work.pop_back();
            if (const auto* stmt = std::get_if<const Stmt*>(&node)) statement(**stmt, work);
            else expression(*std::get<const Expr*>(node), value, work);
        }
    }

    void condition(const Cond& cond, std::vector<Work>& work) {
        if (cond.is_let) { unsupported(cond.span, "'let' conditions"); return; }
        work.push_back({cond.expr, true});
    }

    void statement(const Stmt& stmt, std::vector<Work>& work) {
        switch (stmt.kind) {
            case StmtKind::Var: {
                const auto& var = *cast<VarStmt>(&stmt);
                const auto it = values_.find(var.name.span.lo);
                const auto local = it == values_.end() ? ScalarType{ScalarType::Unit} : types_.binding(it->second);
                if (var.binding == Binding::Const) unsupported(var.name.span, "'const' locals");
                else if (local.kind == ScalarType::Deferred) {
                    ok_ = false;
                    diag_.error(DiagCode::UnsupportedCodegen, var.name.span,
                                "stage0 code generation needs the type of '{}'; add a type annotation",
                                names_.str(var.name.sym));
                } else if (stored(var.name.span, local, "locals")) {
                    if (var.init) work.push_back({var.init, true});
                    return;
                }
                // Uses of a rejected local are not reported again.
                if (it != values_.end()) rejected_.insert(it->second.value);
                return;
            }
            case StmtKind::Return:
                if (const Expr* value = cast<ReturnStmt>(&stmt)->value) work.push_back({value, true});
                return;
            case StmtKind::Break: case StmtKind::Continue: return;
            case StmtKind::Throw: unsupported(stmt.span, "'throw'"); return;
            case StmtKind::For: unsupported(stmt.span, "'for' loops"); return;
            case StmtKind::Defer: unsupported(stmt.span, "'defer'"); return;
            case StmtKind::While: {
                const auto& loop = *cast<WhileStmt>(&stmt);
                work.push_back({loop.body, false});
                condition(loop.cond, work);
                return;
            }
            case StmtKind::Loop: work.push_back({cast<LoopStmt>(&stmt)->body, false}); return;
            case StmtKind::Expr: work.push_back({cast<ExprStmt>(&stmt)->expr, false}); return;
            case StmtKind::Assign: {
                const auto& assign = *cast<AssignStmt>(&stmt);
                if (assign.op == AssignOp::Shl || assign.op == AssignOp::Shr) {
                    unsupported(stmt.span, "shifts");
                    return;
                }
                const auto* name = as<NameExpr>(assign.target);
                const auto id = name ? target(*name) : BindingId{};
                if (!id.valid() || resolution_.binding(id).kind != BindingKind::Local) {
                    unsupported(assign.target->span, "assignment to anything but a local variable");
                    return;
                }
                work.push_back({assign.value, true});
                work.push_back({assign.target, true});
                return;
            }
        }
    }

    void expression(const Expr& expr, bool used, std::vector<Work>& work) {
        if (!supported_kind(expr)) return;
        if (const auto* name = as<NameExpr>(&expr)) {
            if (const auto id = target(*name); id.valid() && rejected_.contains(id.value)) return;
        }
        // A control expression's value, even in parentheses, is that of its tails, which are checked
        // in their own right; its own type is only recorded when discarded.
        const Expr* inner = &expr;
        while (const auto* paren = as<ParenExpr>(inner)) inner = paren->inner;
        const bool control = inner->kind == ExprKind::If || inner->kind == ExprKind::Block;
        const auto actual = types_.expression(&expr);
        // A type deferred only because of an already rejected local is not reported again.
        if (!control && actual.kind == ScalarType::Deferred && mentions_rejected(expr)) return;
        if (!control && !type(expr.span, actual)) return;
        switch (expr.kind) {
            case ExprKind::Int: case ExprKind::Bool: case ExprKind::Unit: return;
            case ExprKind::Name: name(*cast<NameExpr>(&expr)); return;
            case ExprKind::Paren: work.push_back({cast<ParenExpr>(&expr)->inner, used}); return;
            case ExprKind::Unary: work.push_back({cast<UnaryExpr>(&expr)->operand, true}); return;
            case ExprKind::Binary: {
                const auto& binary = *cast<BinaryExpr>(&expr);
                work.push_back({binary.rhs, true});
                work.push_back({binary.lhs, true});
                return;
            }
            case ExprKind::Call: call(*cast<CallExpr>(&expr), work); return;
            case ExprKind::Block: {
                const auto& block = *cast<BlockExpr>(&expr);
                for (usize i = block.stmts.size(); i-- > 0;) {
                    const auto* tail = i + 1 == block.stmts.size() ? as<ExprStmt>(block.stmts[i]) : nullptr;
                    if (tail && used) work.push_back({tail->expr, true});
                    else work.push_back({block.stmts[i], false});
                }
                return;
            }
            case ExprKind::If: {
                const auto& branch = *cast<IfExpr>(&expr);
                if (branch.else_branch) work.push_back({branch.else_branch, used});
                work.push_back({branch.then_block, used});
                condition(branch.cond, work);
                return;
            }
            default: unsupported(expr.span, expr_name(expr.kind)); return;
        }
    }

    bool supported_kind(const Expr& expr) {
        if (const auto* unary = as<UnaryExpr>(&expr); unary && !supported_unary(unary->op)) {
            unsupported(expr.span, unary_name(unary->op));
            return false;
        }
        if (const auto* binary = as<BinaryExpr>(&expr); binary && !supported_binary(binary->op)) {
            unsupported(expr.span, binary_name(binary->op));
            return false;
        }
        switch (expr.kind) {
            case ExprKind::Int: case ExprKind::Bool: case ExprKind::Unit: case ExprKind::Name: case ExprKind::Paren:
            case ExprKind::Unary: case ExprKind::Binary: case ExprKind::Call: case ExprKind::Block: case ExprKind::If:
                return true;
            default:
                unsupported(expr.span, expr_name(expr.kind));
                return false;
        }
    }

    bool mentions_rejected(const Expr& root) const {
        if (rejected_.empty()) return false;
        std::vector<const Expr*> work{&root};
        while (!work.empty()) {
            const Expr* expr = work.back();
            work.pop_back();
            if (const auto* name = as<NameExpr>(expr)) {
                if (const auto id = target(*name); id.valid() && rejected_.contains(id.value)) return true;
            } else if (const auto* paren = as<ParenExpr>(expr)) work.push_back(paren->inner);
            else if (const auto* unary = as<UnaryExpr>(expr)) work.push_back(unary->operand);
            else if (const auto* binary = as<BinaryExpr>(expr)) {
                work.push_back(binary->lhs);
                work.push_back(binary->rhs);
            } else if (const auto* call = as<CallExpr>(expr)) {
                for (const auto& arg : call->args) work.push_back(arg.value);
            }
        }
        return false;
    }

    void name(const NameExpr& name) {
        const auto id = target(name);
        const auto kind = id.valid() ? resolution_.binding(id).kind : BindingKind::Import;
        if (kind == BindingKind::Local || kind == BindingKind::Parameter) return;
        if (kind == BindingKind::Function) unsupported(name.span, "function values");
        else unsupported(name.span, "this name");
    }

    void call(const CallExpr& call, std::vector<Work>& work) {
        if (const CheckedConversion* conversion = types_.conversion(&call)) {
            // Conversions between integer types: widening, checked or truncating.
            if (conversion->state != CallState::Resolved || !type(call.span, conversion->source) || call.args.size() != 1) {
                if (conversion->state != CallState::Resolved) unsupported_type(call.span, ScalarType{});
                return;
            }
            work.push_back({call.args.front().value, true});
            return;
        }
        const CheckedCall* checked = types_.call(&call);
        if (checked && checked->kind == CallKind::Builtin) {
            unsupported(call.callee->span, "output with 'print'");
            return;
        }
        const auto* callee = as<NameExpr>(call.callee);
        const bool direct = checked && checked->state == CallState::Resolved && checked->kind == CallKind::Function &&
                            callee && checked->target.valid() && checked->type_arguments.empty();
        const auto* fun = direct ? as<FunItem>(resolution_.binding(checked->target).item) : nullptr;
        if (!fun || !functions_.contains(fun)) {
            unsupported(call.callee->span, "calls to anything but a top-level function");
            return;
        }
        for (usize i = call.args.size(); i-- > 0;) work.push_back({call.args[i].value, true});
    }

    void entry(const File& file, const std::vector<const FunItem*>& mains) {
        if (mains.empty()) {
            ok_ = false;
            diag_.error(DiagCode::InvalidEntryPoint, Span{file.file, 0, 0},
                        "a program needs an entry point 'fun main()'");
            return;
        }
        for (usize i = 1; i < mains.size(); ++i) {
            ok_ = false;
            auto& d = diag_.error(DiagCode::InvalidEntryPoint, mains[i]->name.span, "'main' cannot be overloaded");
            d.notes.push_back({mains.front()->name.span, "first 'main' declared here"});
        }
        const FunItem& main = *mains.front();
        if (!main.sig.params.empty()) {
            ok_ = false;
            diag_.error(DiagCode::InvalidEntryPoint, main.sig.params.front().name.span, "'main' takes no parameters");
        }
        const auto id = items_.find(&main);
        const ScalarSignature* sig = id == items_.end() ? nullptr : types_.signature(id->second);
        const auto result = sig ? sig->result : ScalarType{};
        const bool exits = result == ScalarType{ScalarType::Int} || result == ScalarType{ScalarType::Unit} ||
                           result == ScalarType{ScalarType::Never};
        if (!exits && supported_type(result)) {
            ok_ = false;
            diag_.error(DiagCode::InvalidEntryPoint, main.sig.ret ? main.sig.ret->span : main.name.span,
                        "'main' must return 'Int' or nothing, not '{}'", types_.type_name(result, resolution_, names_, 256));
        }
    }

    const Resolution& resolution_;
    const ScalarTypes& types_;
    const Interner& names_;
    DiagEngine& diag_;
    std::unordered_map<const Item*, BindingId> items_;
    std::unordered_map<u32, BindingId> values_;  // locals and parameters by declaration offset
    std::unordered_set<const FunItem*> functions_;
    std::unordered_set<usize> rejected_;  // locals whose declaration was reported
    bool ok_ = true;
};

}  // namespace

bool check_codegen_subset(const File& file, const Resolution& resolution, const ScalarTypes& types,
                          const Interner& names, DiagEngine& diag) {
    return Gate(resolution, types, names, diag).run(file);
}

}  // namespace phs
