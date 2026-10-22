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

#include "syntax/ast_dump.hpp"

#include <charconv>

#include "support/utf8.hpp"

namespace phs {

using namespace ast;

namespace {

std::string quote(std::string_view s) {
    std::string out = "\"";
    for (usize i = 0; i < s.size();) {
        char c = s[i];
        if (static_cast<u8>(c) >= 0x80) {
            // Valid UTF-8 is copied through; invalid bytes are escaped so dumps
            // are always valid UTF-8.
            u32 cp;
            u32 len = utf8_decode(s.data() + i, s.size() - i, &cp);
            if (len) {
                out.append(s.data() + i, len);
                i += len;
            } else {
                char buf[16];
                std::snprintf(buf, sizeof buf, "\\x{%02X}", static_cast<unsigned>(static_cast<u8>(c)));
                out += buf;
                ++i;
            }
            continue;
        }
        ++i;
        switch (c) {
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '\0': out += "\\0"; break;
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            default:
                if (static_cast<u8>(c) < 0x20 || c == 0x7F) {
                    char buf[16];
                    std::snprintf(buf, sizeof buf, "\\u{%x}", static_cast<unsigned>(c));
                    out += buf;
                } else {
                    out.push_back(c);
                }
        }
    }
    out.push_back('"');
    return out;
}

std::string fmt_float(double v) {
    char buf[64];
    auto r = std::to_chars(buf, buf + sizeof buf, v);
    std::string s(buf, r.ptr);
    if (s.find_first_of(".en") == std::string::npos) s += ".0";
    return s;
}

std::string fmt_char(u32 c) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "U+%04X", c);
    return buf;
}

const char* unop_name(UnOp op) {
    switch (op) {
        case UnOp::Neg: return "Neg";
        case UnOp::Not: return "Not";
        case UnOp::BitNot: return "BitNot";
        case UnOp::Try: return "Try";
        case UnOp::TryOpt: return "TryOpt";
        case UnOp::Await: return "Await";
        case UnOp::Share: return "Share";
    }
    PHS_UNREACHABLE();
}

const char* binop_name(BinOp op) {
    switch (op) {
        case BinOp::Coalesce: return "Coalesce";
        case BinOp::Or: return "Or";
        case BinOp::And: return "And";
        case BinOp::Eq: return "Eq";
        case BinOp::Ne: return "Ne";
        case BinOp::Lt: return "Lt";
        case BinOp::Le: return "Le";
        case BinOp::Gt: return "Gt";
        case BinOp::Ge: return "Ge";
        case BinOp::Range: return "Range";
        case BinOp::RangeExcl: return "RangeExcl";
        case BinOp::Add: return "Add";
        case BinOp::Sub: return "Sub";
        case BinOp::WrapAdd: return "WrapAdd";
        case BinOp::WrapSub: return "WrapSub";
        case BinOp::BitOr: return "BitOr";
        case BinOp::BitXor: return "BitXor";
        case BinOp::Mul: return "Mul";
        case BinOp::Div: return "Div";
        case BinOp::Rem: return "Rem";
        case BinOp::WrapMul: return "WrapMul";
        case BinOp::BitAnd: return "BitAnd";
        case BinOp::Shl: return "Shl";
        case BinOp::Shr: return "Shr";
    }
    PHS_UNREACHABLE();
}

const char* assign_name(AssignOp op) {
    switch (op) {
        case AssignOp::Assign: return "=";
        case AssignOp::Add: return "+=";
        case AssignOp::Sub: return "-=";
        case AssignOp::Mul: return "*=";
        case AssignOp::Div: return "/=";
        case AssignOp::Rem: return "%=";
        case AssignOp::BitAnd: return "&=";
        case AssignOp::BitOr: return "|=";
        case AssignOp::BitXor: return "^=";
        case AssignOp::Shl: return "<<=";
        case AssignOp::Shr: return ">>=";
    }
    PHS_UNREACHABLE();
}

const char* binding_name(Binding b) {
    switch (b) {
        case Binding::None: return "";
        case Binding::Let: return "let";
        case Binding::Var: return "var";
        case Binding::Const: return "const";
    }
    PHS_UNREACHABLE();
}

class Dumper {
public:
    explicit Dumper(const Interner& in) : in_(in) {}

    std::string run(const File& f) {
        line(0, "File");
        for (const Import& imp : f.imports) {
            std::string s = "Import " + path(imp.path);
            if (!imp.alias.sym.empty()) s += " as " + name(imp.alias);
            line(1, s);
        }
        for (Item* it : f.items) item(1, it);
        return std::move(out_);
    }

private:
    void line(int depth, const std::string& text) {
        out_.append(static_cast<usize>(depth) * 2, ' ');
        out_ += text;
        out_.push_back('\n');
    }

    std::string name(Name n) const { return n.sym.empty() ? "<missing>" : std::string(in_.str(n.sym)); }

    std::string path(const Path& p) const {
        std::string s;
        for (usize i = 0; i < p.parts.size(); ++i) {
            if (i) s += ".";
            s += name(p.parts[i]);
        }
        return s;
    }

    // ---------------------------------------------------------- inline forms

    std::string type(const TypeExpr* t) const {
        if (!t) return "<none>";
        switch (t->kind) {
            case TypeKind::Path: {
                std::string s = path(t->path);
                if (!t->args.empty()) s += "<" + type_list(t->args) + ">";
                return s;
            }
            case TypeKind::Optional: return type(t->inner) + "?";
            case TypeKind::Shared: return "shared " + type(t->inner);
            case TypeKind::Weak: return "weak " + type(t->inner);
            case TypeKind::Pointer: return std::string(t->is_mut ? "*mut " : "*") + type(t->inner);
            case TypeKind::Array: return "[" + type(t->inner) + "]";
            case TypeKind::Unit: return "()";
            case TypeKind::SelfType: return "Self";
            case TypeKind::Fun: {
                std::string s = "fun(" + type_list(t->args) + ")";
                if (t->inner) s += " -> " + type(t->inner);
                if (t->throws) s += t->throws_type ? " throws(" + type(t->throws_type) + ")" : " throws";
                return s;
            }
            case TypeKind::Error: return "<error>";
        }
        PHS_UNREACHABLE();
    }

    std::string type_list(List<TypeExpr*> ts) const {
        std::string s;
        for (usize i = 0; i < ts.size(); ++i) {
            if (i) s += ", ";
            s += type(ts[i]);
        }
        return s;
    }

    std::string literal(const Expr* e) const {
        switch (e->kind) {
            case ExprKind::Int: return std::to_string(static_cast<const IntExpr*>(e)->value);
            case ExprKind::Float: return fmt_float(static_cast<const FloatExpr*>(e)->value);
            case ExprKind::Char: return fmt_char(static_cast<const CharExpr*>(e)->value);
            case ExprKind::String: return quote(in_.str(static_cast<const StringExpr*>(e)->value));
            case ExprKind::Bool: return static_cast<const BoolExpr*>(e)->value ? "true" : "false";
            case ExprKind::None: return "none";
            case ExprKind::Unary: {
                auto* u = static_cast<const UnaryExpr*>(e);
                return "-" + literal(u->operand);
            }
            default: return "<error>";
        }
    }

    std::string pattern(const Pattern* p) const {
        switch (p->kind) {
            case PatKind::Wildcard: return "_";
            case PatKind::Literal: return literal(static_cast<const LiteralPat*>(p)->literal);
            case PatKind::Binding: return name(static_cast<const BindingPat*>(p)->name);
            case PatKind::Case: {
                auto* c = static_cast<const CasePat*>(p);
                std::string s = c->leading_dot ? "." : "";
                for (usize i = 0; i < c->path.size(); ++i) {
                    if (i) s += ".";
                    s += name(c->path[i]);
                }
                if (c->has_args) {
                    s += "(";
                    for (usize i = 0; i < c->args.size(); ++i) {
                        if (i) s += ", ";
                        s += pattern(c->args[i]);
                    }
                    s += ")";
                }
                return s;
            }
            case PatKind::Is: {
                auto* i = static_cast<const IsPat*>(p);
                std::string s = "is " + type(i->type);
                if (!i->binding.sym.empty()) s += " as " + name(i->binding);
                return s;
            }
            case PatKind::Alt: {
                auto* a = static_cast<const AltPat*>(p);
                std::string s;
                for (usize i = 0; i < a->alts.size(); ++i) {
                    if (i) s += " | ";
                    s += pattern(a->alts[i]);
                }
                return s;
            }
            case PatKind::Error: return "<error>";
        }
        PHS_UNREACHABLE();
    }

    // ---------------------------------------------------------- items

    std::string header(const char* kind, const Item* it) const {
        std::string s = kind;
        if (!it->name.sym.empty()) s += " " + name(it->name);
        switch (it->vis) {
            case Visibility::Default: break;
            case Visibility::Public: s += " vis=public"; break;
            case Visibility::Internal: s += " vis=internal"; break;
            case Visibility::File: s += " vis=file"; break;
            case Visibility::Private: s += " vis=private"; break;
        }
        static const char* const kMods[] = {"open",     "final",  "abstract", "sealed", "override",
                                            "static",   "mut",    "async",    "unsafe"};
        std::string mods;
        for (int i = 0; i < 9; ++i) {
            if (it->mods & (1u << i)) {
                if (!mods.empty()) mods += ",";
                mods += kMods[i];
            }
        }
        if (!mods.empty()) s += " mods=" + mods;
        return s;
    }

    void attrs(int d, const Item* it) {
        for (const Attribute& a : it->attrs) {
            line(d, "Attr " + name(a.name) + (a.has_parens ? "()" : ""));
            for (const AttrArg& arg : a.args) {
                if (!arg.key.sym.empty()) {
                    line(d + 1, "Key " + name(arg.key));
                    expr(d + 2, arg.value);
                } else {
                    expr(d + 1, arg.value);
                }
            }
        }
    }

    void generics(int d, List<GenericParam> gs) {
        for (const GenericParam& g : gs) {
            std::string s = "Generic " + name(g.name);
            if (!g.bounds.empty()) {
                s += ": ";
                for (usize i = 0; i < g.bounds.size(); ++i) {
                    if (i) s += " & ";
                    s += type(g.bounds[i]);
                }
            }
            line(d, s);
        }
    }

    void where(int d, List<WhereClause> ws) {
        for (const WhereClause& w : ws) {
            std::string s = "Where " + type(w.type) + ": ";
            for (usize i = 0; i < w.bounds.size(); ++i) {
                if (i) s += " & ";
                s += type(w.bounds[i]);
            }
            line(d, s);
        }
    }

    void params(int d, List<Param> ps) {
        for (const Param& p : ps) {
            std::string s = "Param ";
            if (p.binding != Binding::None) s += std::string(binding_name(p.binding)) + " ";
            s += name(p.name) + ": ";
            if (p.conv == Convention::Borrow) s += "borrow ";
            if (p.conv == Convention::Inout) s += "inout ";
            if (p.conv == Convention::Own) s += "own ";
            s += type(p.type);
            line(d, s);
            if (p.default_value) {
                line(d + 1, "Default");
                expr(d + 2, p.default_value);
            }
        }
    }

    void throws_line(int d, bool throws, const TypeExpr* t) {
        if (!throws) return;
        line(d, t ? "Throws " + type(t) : "Throws");
    }

    void item(int d, Item* it) {
        attrs(d, it);
        switch (it->kind) {
            case ItemKind::Fun: {
                auto* f = static_cast<FunItem*>(it);
                line(d, header("Fun", it));
                generics(d + 1, f->sig.generics);
                params(d + 1, f->sig.params);
                if (f->sig.ret) line(d + 1, "Returns " + type(f->sig.ret));
                throws_line(d + 1, f->sig.throws, f->sig.throws_type);
                where(d + 1, f->sig.where);
                if (f->body) {
                    line(d + 1, f->expr_body ? "ExprBody" : "Body");
                    expr(d + 2, f->body);
                }
                return;
            }
            case ItemKind::Class:
            case ItemKind::Struct:
            case ItemKind::Actor:
            case ItemKind::Interface:
            case ItemKind::Enum: {
                auto* t = static_cast<TypeDeclItem*>(it);
                const char* k = it->kind == ItemKind::Class    ? "Class"
                                : it->kind == ItemKind::Struct ? "Struct"
                                : it->kind == ItemKind::Actor  ? "Actor"
                                : it->kind == ItemKind::Enum   ? "Enum"
                                                               : "Interface";
                line(d, header(k, it));
                generics(d + 1, t->generics);
                if (t->has_ctor) {
                    line(d + 1, "Ctor");
                    params(d + 2, t->ctor_params);
                }
                for (const SuperEntry& s : t->supers) {
                    line(d + 1, "Super " + type(s.type) + (s.has_args ? "()" : ""));
                    args(d + 2, s.args);
                }
                where(d + 1, t->where);
                for (Item* m : t->members) item(d + 1, m);
                return;
            }
            case ItemKind::EnumCase: {
                auto* c = static_cast<EnumCaseItem*>(it);
                std::string s = "Case " + name(c->name);
                if (c->has_fields) {
                    s += "(";
                    for (usize i = 0; i < c->fields.size(); ++i) {
                        if (i) s += ", ";
                        if (!c->fields[i].label.sym.empty()) s += name(c->fields[i].label) + ": ";
                        s += type(c->fields[i].type);
                    }
                    s += ")";
                }
                line(d, s);
                return;
            }
            case ItemKind::Var: {
                auto* v = static_cast<VarItem*>(it);
                const char* k = v->binding == Binding::Let ? "Let" : v->binding == Binding::Var ? "Var" : "Const";
                std::string s = header(k, it);
                if (v->type) s += ": " + type(v->type);
                line(d, s);
                if (v->init) expr(d + 1, v->init);
                return;
            }
            case ItemKind::Extern: {
                auto* e = static_cast<ExternItem*>(it);
                line(d, header("Extern", it) + " " + quote(in_.str(e->abi)));
                for (Item* m : e->items) item(d + 1, m);
                return;
            }
            case ItemKind::TypeAlias: {
                auto* t = static_cast<TypeAliasItem*>(it);
                std::string s = header("TypeAlias", it);
                if (!t->bounds.empty()) {
                    s += ": ";
                    for (usize i = 0; i < t->bounds.size(); ++i) {
                        if (i) s += " & ";
                        s += type(t->bounds[i]);
                    }
                }
                if (t->type) s += " = " + type(t->type);
                line(d, s);
                generics(d + 1, t->generics);
                return;
            }
            case ItemKind::Init: {
                auto* i = static_cast<InitItem*>(it);
                line(d, header(i->has_params ? "Init" : "PrimaryInit", it));
                params(d + 1, i->params);
                throws_line(d + 1, i->throws, i->throws_type);
                expr(d + 1, i->body);
                return;
            }
            case ItemKind::Deinit: {
                auto* x = static_cast<DeinitItem*>(it);
                line(d, header("Deinit", it));
                expr(d + 1, x->body);
                return;
            }
        }
    }

    // ---------------------------------------------------------- statements

    void cond(int d, const Cond& c) {
        if (c.is_let) {
            line(d, "IfLet " + name(c.name) + (c.type ? ": " + type(c.type) : ""));
            expr(d + 1, c.expr);
        } else {
            line(d, "Cond");
            expr(d + 1, c.expr);
        }
    }

    void stmt(int d, Stmt* s) {
        switch (s->kind) {
            case StmtKind::Var: {
                auto* v = static_cast<VarStmt*>(s);
                std::string t = std::string(v->binding == Binding::Let   ? "Let "
                                            : v->binding == Binding::Var ? "Var "
                                                                         : "Const ") +
                                name(v->name);
                if (v->type) t += ": " + type(v->type);
                line(d, t);
                if (v->init) expr(d + 1, v->init);
                return;
            }
            case StmtKind::Return: {
                auto* r = static_cast<ReturnStmt*>(s);
                line(d, "Return");
                if (r->value) expr(d + 1, r->value);
                return;
            }
            case StmtKind::Break: line(d, "Break"); return;
            case StmtKind::Continue: line(d, "Continue"); return;
            case StmtKind::Throw:
                line(d, "Throw");
                expr(d + 1, static_cast<ThrowStmt*>(s)->value);
                return;
            case StmtKind::While: {
                auto* w = static_cast<WhileStmt*>(s);
                line(d, "While");
                cond(d + 1, w->cond);
                expr(d + 1, w->body);
                return;
            }
            case StmtKind::For: {
                auto* f = static_cast<ForStmt*>(s);
                line(d, "For " + name(f->var));
                expr(d + 1, f->iter);
                expr(d + 1, f->body);
                return;
            }
            case StmtKind::Loop:
                line(d, "Loop");
                expr(d + 1, static_cast<LoopStmt*>(s)->body);
                return;
            case StmtKind::Defer:
                line(d, "Defer");
                expr(d + 1, static_cast<DeferStmt*>(s)->body);
                return;
            case StmtKind::Expr: expr(d, static_cast<ExprStmt*>(s)->expr); return;
            case StmtKind::Assign: {
                auto* a = static_cast<AssignStmt*>(s);
                line(d, std::string("Assign ") + assign_name(a->op));
                expr(d + 1, a->target);
                expr(d + 1, a->value);
                return;
            }
        }
    }

    // ---------------------------------------------------------- expressions

    void args(int d, List<Arg> as) {
        for (const Arg& a : as) {
            if (!a.label.sym.empty()) {
                line(d, "Label " + name(a.label));
                expr(d + 1, a.value);
            } else {
                expr(d, a.value);
            }
        }
    }

    void expr(int d, const Expr* e) {
        if (!e) {
            line(d, "<null>");
            return;
        }
        switch (e->kind) {
            case ExprKind::Int: line(d, "Int " + literal(e)); return;
            case ExprKind::Float: line(d, "Float " + literal(e)); return;
            case ExprKind::Char: line(d, "Char " + literal(e)); return;
            case ExprKind::String: line(d, "String " + literal(e)); return;
            case ExprKind::Interp: {
                auto* i = static_cast<const InterpExpr*>(e);
                line(d, "Interp");
                for (usize k = 0; k < i->texts.size(); ++k) {
                    line(d + 1, "Text " + quote(in_.str(i->texts[k])));
                    if (k < i->exprs.size()) expr(d + 1, i->exprs[k]);
                }
                return;
            }
            case ExprKind::Bool: line(d, "Bool " + literal(e)); return;
            case ExprKind::None: line(d, "None"); return;
            case ExprKind::Unit: line(d, "Unit"); return;
            case ExprKind::SelfValue: line(d, "SelfValue"); return;
            case ExprKind::Super: line(d, "Super"); return;
            case ExprKind::SelfType: line(d, "SelfType"); return;
            case ExprKind::Name: line(d, "Name " + name(static_cast<const NameExpr*>(e)->name)); return;
            case ExprKind::ImplicitMember:
                line(d, "ImplicitMember ." + name(static_cast<const ImplicitMemberExpr*>(e)->name));
                return;
            case ExprKind::Paren:
                line(d, "Paren");
                expr(d + 1, static_cast<const ParenExpr*>(e)->inner);
                return;
            case ExprKind::Array:
                line(d, "Array");
                for (const Expr* x : static_cast<const ArrayExpr*>(e)->elems) expr(d + 1, x);
                return;
            case ExprKind::Unary: {
                auto* u = static_cast<const UnaryExpr*>(e);
                line(d, std::string("Unary ") + unop_name(u->op));
                expr(d + 1, u->operand);
                return;
            }
            case ExprKind::Binary: {
                auto* b = static_cast<const BinaryExpr*>(e);
                line(d, std::string("Binary ") + binop_name(b->op));
                expr(d + 1, b->lhs);
                expr(d + 1, b->rhs);
                return;
            }
            case ExprKind::Cast: {
                auto* c = static_cast<const CastExpr*>(e);
                const char* op = c->op == CastOp::Is ? "is" : c->op == CastOp::As ? "as" : "as?";
                line(d, std::string("Cast ") + op + " " + type(c->type));
                expr(d + 1, c->operand);
                return;
            }
            case ExprKind::Call: {
                auto* c = static_cast<const CallExpr*>(e);
                line(d, "Call");
                expr(d + 1, c->callee);
                args(d + 1, c->args);
                return;
            }
            case ExprKind::Member: {
                auto* m = static_cast<const MemberExpr*>(e);
                line(d, "Member ." + name(m->name));
                expr(d + 1, m->base);
                return;
            }
            case ExprKind::OptMember: {
                auto* m = static_cast<const OptMemberExpr*>(e);
                line(d, "OptMember ?." + name(m->name));
                expr(d + 1, m->base);
                return;
            }
            case ExprKind::Index: {
                auto* i = static_cast<const IndexExpr*>(e);
                line(d, "Index");
                expr(d + 1, i->base);
                expr(d + 1, i->index);
                return;
            }
            case ExprKind::Unwrap:
                line(d, "Unwrap");
                expr(d + 1, static_cast<const UnwrapExpr*>(e)->operand);
                return;
            case ExprKind::GenericInst: {
                auto* g = static_cast<const GenericInstExpr*>(e);
                line(d, "GenericInst <" + type_list(g->args) + ">");
                expr(d + 1, g->base);
                return;
            }
            case ExprKind::If: {
                auto* i = static_cast<const IfExpr*>(e);
                line(d, "If");
                cond(d + 1, i->cond);
                expr(d + 1, i->then_block);
                if (i->else_branch) {
                    line(d + 1, "Else");
                    expr(d + 2, i->else_branch);
                }
                return;
            }
            case ExprKind::Match: {
                auto* m = static_cast<const MatchExpr*>(e);
                line(d, "Match");
                expr(d + 1, m->scrutinee);
                for (const MatchArm& a : m->arms) {
                    line(d + 1, "Arm " + pattern(a.pattern));
                    if (a.guard) {
                        line(d + 2, "Guard");
                        expr(d + 3, a.guard);
                    }
                    expr(d + 2, a.body);
                }
                return;
            }
            case ExprKind::Block:
                line(d, "Block");
                for (Stmt* s : static_cast<const BlockExpr*>(e)->stmts) stmt(d + 1, s);
                return;
            case ExprKind::Unsafe:
                line(d, "Unsafe");
                expr(d + 1, static_cast<const UnsafeExpr*>(e)->block);
                return;
            case ExprKind::Error: line(d, "<error>"); return;
        }
    }

    const Interner& in_;
    std::string out_;
};

}  // namespace

std::string dump_tokens(const std::vector<Token>& tokens, const Interner& in, const SourceManager& sm) {
    std::string out;
    for (const Token& t : tokens) {
        LineCol lc = sm.line_col(t.span.file, t.span.lo);
        char pos[32];
        std::snprintf(pos, sizeof pos, "%u:%u ", lc.line, lc.col);
        out += pos;
        out += tok_name(t.kind);
        switch (t.kind) {
            case Tok::Ident: out += " " + std::string(in.str(t.symbol())); break;
            case Tok::IntLit: out += " " + std::to_string(t.int_value); break;
            case Tok::FloatLit: out += " " + fmt_float(t.float_value); break;
            case Tok::CharLit: out += " " + fmt_char(t.char_value); break;
            case Tok::StringLit:
            case Tok::StringHead:
            case Tok::StringMiddle:
            case Tok::StringTail: out += " " + quote(in.str(t.symbol())); break;
            default: break;
        }
        if (t.space_before() && t.kind != Tok::Newline && t.kind != Tok::Eof) out += " (sp)";
        out.push_back('\n');
    }
    return out;
}

std::string dump_ast(const File& file, const Interner& in) { return Dumper(in).run(file); }

}  // namespace phs
