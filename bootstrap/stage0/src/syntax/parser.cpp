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

#include "syntax/parser.hpp"

#include <algorithm>
#include <optional>
#include <string>
#include <utility>

namespace phs {

using namespace ast;

namespace {

enum class Ctx : u8 { Top, Member, Extern };

class Parser {
public:
    Parser(FileId file, std::vector<Token> toks, Arena& arena, Interner& in, DiagEngine& diag)
        : file_(file), toks_(std::move(toks)), arena_(arena), in_(in), diag_(diag) {
        PHS_ASSERT(!toks_.empty() && toks_.back().kind == Tok::Eof);
        for (const Diagnostic& d : diag_.all()) {
            std::string_view code = d.code;
            if (code == "E0010" || code == "E0003") lexer_reported_unclosed_ = true;
            // Errors for characters the lexer dropped without producing a token.
            bool dropped = code == "E0001" || code == "E0008" || code == "E0011";
            if (dropped && d.span.file == file_) lexer_error_offsets_.push_back(d.span.lo);
        }
        std::sort(lexer_error_offsets_.begin(), lexer_error_offsets_.end());
    }

    File* run() {
        std::vector<Import> imports;
        std::vector<Item*> items;
        skip_terms();
        while (!at(Tok::Eof)) {
            if (at(Tok::RBrace)) {  // stray '}' was already reported by the lexer
                advance();
                skip_terms();
                continue;
            }
            usize start = pos_;
            u32 errs = diag_.error_count();
            if (at(Tok::KwImport)) {
                Import imp = parse_import();
                if (!items.empty()) error("E0110", imp.span, "imports must come before all declarations");
                imports.push_back(imp);
            } else if (Item* it = parse_item(Ctx::Top)) {
                items.push_back(it);
            }
            finish_entry(errs, start, "declaration", false);
        }
        File* f = arena_.make<File>();
        f->file = file_;
        f->imports = arena_.copy(imports);
        f->items = arena_.copy(items);
        return f;
    }

private:
    // ------------------------------------------------------------ token access

    const Token& cur() const { return toks_[pos_]; }
    const Token& peek(usize n = 1) const { return pos_ + n < toks_.size() ? toks_[pos_ + n] : toks_.back(); }
    bool at(Tok k) const { return cur().kind == k; }
    void advance() {
        if (cur().kind != Tok::Eof) ++pos_;
    }
    bool eat(Tok k) {
        if (!at(k)) return false;
        advance();
        return true;
    }
    Span prev_span() const { return pos_ > 0 ? toks_[pos_ - 1].span : cur().span; }
    Span from(Span start) const { return Span::join(start, prev_span()); }
    void skip_newlines() {
        while (at(Tok::Newline)) advance();
    }
    void skip_terms() {
        while (at(Tok::Newline) || at(Tok::Semi)) advance();
    }
    bool at_terminator() const {
        Tok k = cur().kind;
        return k == Tok::Newline || k == Tok::Semi || k == Tok::RBrace || k == Tok::Eof;
    }

    template <typename T>
    T* mk(Span s) {
        T* n = arena_.make<T>();
        n->span = s;
        return n;
    }

    // ------------------------------------------------------------ diagnostics

    std::string describe(const Token& t) const {
        if (t.kind == Tok::Ident) return "identifier '" + std::string(in_.str(t.symbol())) + "'";
        if (tok_is_keyword(t.kind)) return "keyword '" + std::string(in_.str(t.symbol())) + "'";
        return tok_spelling(t.kind);
    }

    void error(const char* code, Span span, std::string msg) {
        if (spec_depth_ > 0) {
            spec_failed_ = true;
            return;
        }
        // At most one error per token position avoids cascades.
        if (pos_ == last_error_pos_) return;
        // An unexpected end of file is already explained by the lexer's
        // "unclosed delimiter" / "unterminated string" error.
        if (at(Tok::Eof) && lexer_reported_unclosed_) return;
        // Likewise when the lexer dropped an invalid character just before
        // this token (e.g. `let x = $`): its error already explains this one.
        if (lexer_error_in_gap()) return;
        last_error_pos_ = pos_;
        diag_.error(code, span, std::move(msg));
    }

    void error_expected(const char* code, const std::string& what) {
        error(code, cur().span, "expected " + what + ", found " + describe(cur()));
    }

    // True if a lexer error starts between the last non-newline token and the
    // current token, i.e. the lexer skipped something right here.
    bool lexer_error_in_gap() const {
        if (lexer_error_offsets_.empty()) return false;
        usize k = pos_;
        while (k > 0 && toks_[k - 1].kind == Tok::Newline) --k;
        u32 lo = k > 0 ? toks_[k - 1].span.hi : 0;
        u32 hi = cur().span.lo;
        auto it = std::lower_bound(lexer_error_offsets_.begin(), lexer_error_offsets_.end(), lo);
        return it != lexer_error_offsets_.end() && *it < hi;
    }

    bool expect(Tok k) {
        if (eat(k)) return true;
        error_expected("E0100", tok_spelling(k));
        return false;
    }

    Name expect_name(const char* what) {
        if (at(Tok::Ident)) {
            Name n{cur().symbol(), cur().span};
            advance();
            return n;
        }
        error_expected("E0100", what);
        return Name{{}, cur().span};
    }

    // Skips to the next terminator at the current brace depth.
    void sync() {
        int depth = 0;
        while (!at(Tok::Eof)) {
            Tok k = cur().kind;
            if (depth == 0 && (k == Tok::Newline || k == Tok::Semi || k == Tok::RBrace)) return;
            if (k == Tok::LBrace) ++depth;
            if (k == Tok::RBrace) --depth;
            advance();
        }
    }

    // Skips to (and consumes) the delimiter that closes an already-opened one.
    void skip_to_close(Tok open, Tok close) {
        int depth = 0;
        while (!at(Tok::Eof)) {
            if (at(open)) ++depth;
            if (at(close)) {
                if (depth == 0) {
                    advance();
                    return;
                }
                --depth;
            }
            if (at(Tok::RBrace) && open != Tok::LBrace && depth == 0) return;
            advance();
        }
    }

    // After a declaration/statement/arm: require a terminator, recover from
    // errors, and guarantee forward progress.
    void finish_entry(u32 errs_before, usize start, const char* what, bool allow_comma) {
        if (diag_.error_count() > errs_before) {
            sync();
        } else if (!at_terminator() && !(allow_comma && at(Tok::Comma))) {
            error("E0106", cur().span, std::string("expected newline or ';' after ") + what + ", found " + describe(cur()));
            sync();
        }
        while (at(Tok::Newline) || at(Tok::Semi) || (allow_comma && at(Tok::Comma))) advance();
        if (pos_ == start && !at(Tok::Eof) && !at(Tok::RBrace)) advance();
    }

    // ------------------------------------------------------------ speculation

    struct Snapshot {
        usize pos;
        usize log;
    };
    Snapshot snapshot() const { return {pos_, split_log_.size()}; }
    void restore(Snapshot s) {
        while (split_log_.size() > s.log) {
            toks_[split_log_.back().first] = split_log_.back().second;
            split_log_.pop_back();
        }
        pos_ = s.pos;
    }

    // True if the current token begins with '>' (eat_gt can consume it).
    bool at_gt() const {
        Tok k = cur().kind;
        return k == Tok::Gt || k == Tok::Shr || k == Tok::GtEq || k == Tok::ShrEq;
    }

    // Consumes one '>' — splitting '>>', '>=' and '>>=' when necessary.
    bool eat_gt() {
        Tok k = cur().kind;
        if (k == Tok::Gt) {
            advance();
            return true;
        }
        if (k == Tok::Shr || k == Tok::GtEq || k == Tok::ShrEq) {
            split_log_.push_back({pos_, toks_[pos_]});
            Token& t = toks_[pos_];
            t.kind = k == Tok::Shr ? Tok::Gt : k == Tok::GtEq ? Tok::Eq : Tok::GtEq;
            t.span.lo += 1;
            t.flags = 0;
            return true;
        }
        return false;
    }

    // ------------------------------------------------------------ items

    Import parse_import() {
        Span start = cur().span;
        advance();  // import
        Import imp{};
        imp.path = parse_path("module path");
        if (eat(Tok::KwAs)) imp.alias = expect_name("alias name");
        imp.span = from(start);
        return imp;
    }

    // True if the current token is '.' followed by an identifier, possibly on
    // a later line (spec 1.3: newlines after '.' continue the line).
    bool at_dot_then_ident() const {
        if (!at(Tok::Dot)) return false;
        usize k = 1;
        while (peek(k).kind == Tok::Newline) ++k;
        return peek(k).kind == Tok::Ident;
    }

    Path parse_path(const char* what) {
        Span start = cur().span;
        std::vector<Name> parts;
        parts.push_back(expect_name(what));
        while (at_dot_then_ident()) {
            advance();
            skip_newlines();
            parts.push_back(expect_name(what));
        }
        return Path{arena_.copy(parts), from(start)};
    }

    Attribute parse_attribute() {
        Span start = cur().span;
        advance();  // @
        Attribute a{};
        // Attribute names may be keywords: @extern("C"), @inline.
        if (tok_is_keyword(cur().kind)) {
            a.name = Name{cur().symbol(), cur().span};
            advance();
        } else {
            a.name = expect_name("attribute name");
        }
        std::vector<AttrArg> args;
        if (at(Tok::LParen) && !cur().space_before()) {
            advance();
            a.has_parens = true;
            u32 errs = diag_.error_count();
            while (!at(Tok::RParen) && !at(Tok::Eof)) {
                AttrArg arg{};
                if (at(Tok::Ident) && peek().kind == Tok::Eq) {
                    arg.key = expect_name("attribute key");
                    advance();
                }
                arg.value = parse_expr();
                args.push_back(arg);
                if (!eat(Tok::Comma)) break;
            }
            close_list(errs, Tok::LParen, Tok::RParen);
        }
        a.args = arena_.copy(args);
        a.span = from(start);
        return a;
    }

    void close_list(u32 errs_before, Tok open, Tok close) {
        if (diag_.error_count() > errs_before) {
            skip_to_close(open, close);
        } else {
            expect(close);
        }
    }

    Visibility parse_visibility() {
        switch (cur().kind) {
            case Tok::KwPublic: advance(); return Visibility::Public;
            case Tok::KwInternal: advance(); return Visibility::Internal;
            case Tok::KwFile: advance(); return Visibility::File;
            case Tok::KwPrivate: advance(); return Visibility::Private;
            default: return Visibility::Default;
        }
    }

    u16 parse_modifiers() {
        u16 mods = 0;
        while (true) {
            u16 m = 0;
            switch (cur().kind) {
                case Tok::KwOpen: m = kModOpen; break;
                case Tok::KwFinal: m = kModFinal; break;
                case Tok::KwAbstract: m = kModAbstract; break;
                case Tok::KwSealed: m = kModSealed; break;
                case Tok::KwOverride: m = kModOverride; break;
                case Tok::KwStatic: m = kModStatic; break;
                case Tok::KwMut: m = kModMut; break;
                case Tok::KwAsync: m = kModAsync; break;
                case Tok::KwUnsafe: m = kModUnsafe; break;
                default: return mods;
            }
            if (mods & m) error("E0111", cur().span, "duplicate modifier " + describe(cur()));
            mods |= m;
            advance();
        }
    }

    Item* parse_item(Ctx ctx) {
        Span start = cur().span;
        std::vector<Attribute> attrs;
        while (at(Tok::At)) {
            attrs.push_back(parse_attribute());
            skip_newlines();
        }
        Visibility vis = parse_visibility();
        u16 mods = parse_modifiers();
        Item* item = nullptr;
        switch (cur().kind) {
            case Tok::KwFun: item = parse_fun(); break;
            case Tok::KwClass: item = parse_type_decl(ItemKind::Class); break;
            case Tok::KwStruct: item = parse_type_decl(ItemKind::Struct); break;
            case Tok::KwActor: item = parse_type_decl(ItemKind::Actor); break;
            case Tok::KwInterface: item = parse_type_decl(ItemKind::Interface); break;
            case Tok::KwEnum: item = parse_enum(); break;
            case Tok::KwLet:
            case Tok::KwVar:
            case Tok::KwConst: item = parse_var_item(); break;
            case Tok::KwExtern: item = parse_extern(); break;
            case Tok::KwType: item = parse_type_alias(); break;
            case Tok::KwInit: item = parse_init(); break;
            case Tok::KwDeinit: item = parse_deinit(); break;
            default:
                error_expected("E0104", ctx == Ctx::Extern ? "function declaration" : "declaration");
                return nullptr;
        }
        item->attrs = arena_.copy(attrs);
        item->vis = vis;
        item->mods = mods;
        item->span = from(start);
        if (ctx == Ctx::Extern && item->kind != ItemKind::Fun)
            error("E0109", item->span, "only function declarations are allowed in extern blocks");
        return item;
    }

    std::vector<GenericParam> parse_generic_params() {
        std::vector<GenericParam> out;
        if (!at(Tok::Lt)) return out;
        advance();
        skip_newlines();
        if (at_gt()) error_expected("E0100", "generic parameter name");  // `<>` is not a valid list
        while (!at_gt() && !at(Tok::Eof)) {
            GenericParam g{};
            g.name = expect_name("generic parameter name");
            if (eat(Tok::Colon)) g.bounds = parse_bounds();
            out.push_back(g);
            skip_newlines();
            if (!eat(Tok::Comma)) break;
            skip_newlines();
        }
        if (!eat_gt()) error_expected("E0100", "'>'");
        return out;
    }

    List<TypeExpr*> parse_bounds() {
        std::vector<TypeExpr*> out;
        out.push_back(parse_type());
        while (eat(Tok::Amp)) out.push_back(parse_type());
        return arena_.copy(out);
    }

    List<WhereClause> parse_where() {
        std::vector<WhereClause> out;
        if (!eat(Tok::KwWhere)) return {};
        do {
            WhereClause w{};
            w.type = parse_type();
            expect(Tok::Colon);
            w.bounds = parse_bounds();
            out.push_back(w);
        } while (eat(Tok::Comma));
        return arena_.copy(out);
    }

    // Called after '('; consumes the closing ')'.
    List<Param> parse_params() {
        std::vector<Param> out;
        u32 errs = diag_.error_count();
        while (!at(Tok::RParen) && !at(Tok::Eof)) {
            Span start = cur().span;
            Param p{};
            if (eat(Tok::KwLet)) p.binding = Binding::Let;
            else if (eat(Tok::KwVar)) p.binding = Binding::Var;
            p.name = expect_name("parameter name");
            expect(Tok::Colon);
            if (eat(Tok::KwBorrow)) p.conv = Convention::Borrow;
            else if (eat(Tok::KwInout)) p.conv = Convention::Inout;
            else if (eat(Tok::KwOwn)) p.conv = Convention::Own;
            p.type = parse_type();
            if (eat(Tok::Eq)) p.default_value = parse_expr();
            p.span = from(start);
            out.push_back(p);
            if (diag_.error_count() > errs || !eat(Tok::Comma)) break;
        }
        close_list(errs, Tok::LParen, Tok::RParen);
        return arena_.copy(out);
    }

    void parse_throws(bool& throws, TypeExpr*& type) {
        if (!eat(Tok::KwThrows)) return;
        throws = true;
        if (at(Tok::LParen)) {
            advance();
            type = parse_type();
            expect(Tok::RParen);
        }
    }

    FunItem* parse_fun() {
        advance();  // fun
        FunItem* f = arena_.make<FunItem>();
        f->name = expect_name("function name");
        f->sig.generics = arena_.copy(parse_generic_params());
        if (expect(Tok::LParen)) f->sig.params = parse_params();
        if (eat(Tok::Colon)) f->sig.ret = parse_type();
        parse_throws(f->sig.throws, f->sig.throws_type);
        f->sig.where = parse_where();
        if (at(Tok::LBrace)) {
            f->body = parse_block();
        } else if (eat(Tok::Eq)) {
            skip_newlines();
            f->body = parse_expr();
            f->expr_body = true;
        }
        return f;
    }

    List<SuperEntry> parse_supers() {
        std::vector<SuperEntry> out;
        do {
            SuperEntry e{};
            e.type = parse_type();
            if (at(Tok::LParen)) {
                advance();
                e.has_args = true;
                e.args = parse_args();
            }
            out.push_back(e);
        } while (eat(Tok::Comma));
        return arena_.copy(out);
    }

    TypeDeclItem* parse_type_decl(ItemKind kind) {
        advance();  // class / struct / actor / interface
        TypeDeclItem* d = arena_.make<TypeDeclItem>();
        d->kind = kind;
        d->name = expect_name("type name");
        d->generics = arena_.copy(parse_generic_params());
        if (kind != ItemKind::Interface && at(Tok::LParen)) {
            advance();
            d->has_ctor = true;
            d->ctor_params = parse_params();
        }
        if (eat(Tok::Colon)) d->supers = parse_supers();
        d->where = parse_where();
        if (at(Tok::LBrace)) {
            d->has_body = true;
            d->members = parse_member_block();
        } else if (kind == ItemKind::Interface) {
            error_expected("E0100", "'{'");
        }
        return d;
    }

    List<Item*> parse_member_block() {
        std::vector<Item*> out;
        expect(Tok::LBrace);
        bool saved = no_leading_dot_;
        no_leading_dot_ = false;
        skip_terms();
        while (!at(Tok::RBrace) && !at(Tok::Eof)) {
            usize start = pos_;
            u32 errs = diag_.error_count();
            if (Item* it = parse_item(Ctx::Member)) out.push_back(it);
            finish_entry(errs, start, "declaration", false);
        }
        no_leading_dot_ = saved;
        expect(Tok::RBrace);
        return arena_.copy(out);
    }

    TypeDeclItem* parse_enum() {
        advance();  // enum
        TypeDeclItem* d = arena_.make<TypeDeclItem>();
        d->kind = ItemKind::Enum;
        d->name = expect_name("enum name");
        d->generics = arena_.copy(parse_generic_params());
        if (eat(Tok::Colon)) d->supers = parse_supers();
        d->where = parse_where();
        d->has_body = true;
        std::vector<Item*> members;
        expect(Tok::LBrace);
        bool saved = no_leading_dot_;
        no_leading_dot_ = false;
        while (at(Tok::Newline) || at(Tok::Semi) || at(Tok::Comma)) advance();
        while (!at(Tok::RBrace) && !at(Tok::Eof)) {
            usize start = pos_;
            u32 errs = diag_.error_count();
            if (at(Tok::Ident)) {
                members.push_back(parse_enum_case());
            } else if (Item* it = parse_item(Ctx::Member)) {
                members.push_back(it);
            }
            finish_entry(errs, start, "enum case", true);
        }
        no_leading_dot_ = saved;
        expect(Tok::RBrace);
        d->members = arena_.copy(members);
        return d;
    }

    EnumCaseItem* parse_enum_case() {
        Span start = cur().span;
        EnumCaseItem* c = arena_.make<EnumCaseItem>();
        c->name = expect_name("enum case name");
        if (at(Tok::LParen)) {
            advance();
            c->has_fields = true;
            std::vector<CaseField> fields;
            u32 errs = diag_.error_count();
            while (!at(Tok::RParen) && !at(Tok::Eof)) {
                CaseField f{};
                if (at(Tok::Ident) && peek().kind == Tok::Colon) {
                    f.label = expect_name("field label");
                    advance();
                }
                f.type = parse_type();
                fields.push_back(f);
                if (diag_.error_count() > errs || !eat(Tok::Comma)) break;
            }
            close_list(errs, Tok::LParen, Tok::RParen);
            c->fields = arena_.copy(fields);
        }
        c->span = from(start);
        return c;
    }

    VarItem* parse_var_item() {
        VarItem* v = arena_.make<VarItem>();
        v->binding = at(Tok::KwLet) ? Binding::Let : at(Tok::KwVar) ? Binding::Var : Binding::Const;
        advance();
        v->name = expect_name("variable name");
        if (eat(Tok::Colon)) v->type = parse_type();
        if (eat(Tok::Eq)) {
            skip_newlines();
            v->init = parse_expr();
        }
        return v;
    }

    ExternItem* parse_extern() {
        advance();  // extern
        ExternItem* e = arena_.make<ExternItem>();
        if (at(Tok::StringLit)) {
            e->abi = cur().symbol();
            advance();
        } else {
            error_expected("E0100", "ABI string such as \"C\"");
        }
        std::vector<Item*> items;
        expect(Tok::LBrace);
        skip_terms();
        while (!at(Tok::RBrace) && !at(Tok::Eof)) {
            usize start = pos_;
            u32 errs = diag_.error_count();
            if (Item* it = parse_item(Ctx::Extern)) items.push_back(it);
            finish_entry(errs, start, "declaration", false);
        }
        expect(Tok::RBrace);
        e->items = arena_.copy(items);
        return e;
    }

    TypeAliasItem* parse_type_alias() {
        advance();  // type
        TypeAliasItem* t = arena_.make<TypeAliasItem>();
        t->name = expect_name("type name");
        t->generics = arena_.copy(parse_generic_params());
        if (eat(Tok::Colon)) t->bounds = parse_bounds();
        if (eat(Tok::Eq)) t->type = parse_type();
        return t;
    }

    InitItem* parse_init() {
        advance();  // init
        InitItem* i = arena_.make<InitItem>();
        if (at(Tok::LParen)) {
            advance();
            i->has_params = true;
            i->params = parse_params();
            parse_throws(i->throws, i->throws_type);
        }
        i->body = parse_block();
        return i;
    }

    DeinitItem* parse_deinit() {
        advance();  // deinit
        DeinitItem* d = arena_.make<DeinitItem>();
        d->body = parse_block();
        return d;
    }

    // ------------------------------------------------------------ types

    TypeExpr* new_type(TypeKind k, Span s) {
        TypeExpr* t = arena_.make<TypeExpr>();
        t->kind = k;
        t->span = s;
        return t;
    }

    // Every caller requires a type here, so a type may continue on the next
    // line after ':', '=', '->', '&', ',' or a keyword such as `is` (spec 1.3).
    TypeExpr* parse_type() {
        skip_newlines();
        Span start = cur().span;
        TypeExpr* t = parse_prefix_type();
        while (at(Tok::Question)) {
            advance();
            TypeExpr* o = new_type(TypeKind::Optional, from(start));
            o->inner = t;
            t = o;
        }
        return t;
    }

    TypeExpr* parse_prefix_type() {
        Span start = cur().span;
        switch (cur().kind) {
            case Tok::KwShared:
            case Tok::KwWeak: {
                TypeKind k = at(Tok::KwShared) ? TypeKind::Shared : TypeKind::Weak;
                advance();
                TypeExpr* inner = parse_prefix_type();
                TypeExpr* t = new_type(k, from(start));
                t->inner = inner;
                return t;
            }
            case Tok::Star: {
                advance();
                bool is_mut = eat(Tok::KwMut);
                TypeExpr* inner = parse_prefix_type();
                TypeExpr* t = new_type(TypeKind::Pointer, from(start));
                t->is_mut = is_mut;
                t->inner = inner;
                return t;
            }
            case Tok::LBracket: {
                advance();
                TypeExpr* inner = parse_type();
                expect(Tok::RBracket);
                TypeExpr* t = new_type(TypeKind::Array, from(start));
                t->inner = inner;
                return t;
            }
            case Tok::LParen: {
                advance();
                expect(Tok::RParen);
                return new_type(TypeKind::Unit, from(start));
            }
            case Tok::KwSelfType:
                advance();
                return new_type(TypeKind::SelfType, from(start));
            case Tok::KwFun: {
                advance();
                std::vector<TypeExpr*> params;
                if (expect(Tok::LParen)) {
                    while (!at(Tok::RParen) && !at(Tok::Eof)) {
                        params.push_back(parse_type());
                        if (!eat(Tok::Comma)) break;
                    }
                    expect(Tok::RParen);
                }
                TypeExpr* t = new_type(TypeKind::Fun, start);
                t->args = arena_.copy(params);
                if (eat(Tok::Arrow)) t->inner = parse_type();
                parse_throws(t->throws, t->throws_type);
                t->span = from(start);
                return t;
            }
            case Tok::Ident: {
                TypeExpr* t = new_type(TypeKind::Path, start);
                t->path = parse_path("type name");
                bool in_cast = cast_type_;
                cast_type_ = false;
                if (at(Tok::Lt) && in_cast) {
                    // After `is`/`as`, `<` may be a comparison: `x as Int < y`.
                    List<TypeExpr*> args;
                    if (try_generic_args(args, false)) t->args = args;
                } else if (at(Tok::Lt)) {
                    advance();
                    skip_newlines();
                    if (at_gt()) error_expected("E0102", "type");  // `List<>` is not a valid type
                    std::vector<TypeExpr*> args;
                    while (!at_gt() && !at(Tok::Eof)) {
                        args.push_back(parse_type());
                        skip_newlines();
                        if (spec_failed_ || !eat(Tok::Comma)) break;
                        skip_newlines();
                    }
                    skip_newlines();
                    if (!eat_gt()) error_expected("E0100", "'>'");
                    t->args = arena_.copy(args);
                }
                t->span = from(start);
                return t;
            }
            default:
                error_expected("E0102", "type");
                return new_type(TypeKind::Error, cur().span);
        }
    }

    // ------------------------------------------------------------ statements

    BlockExpr* parse_block() {
        Span start = cur().span;
        BlockExpr* b = mk<BlockExpr>(start);
        if (!expect(Tok::LBrace)) return b;
        bool saved = no_leading_dot_;
        no_leading_dot_ = false;
        std::vector<Stmt*> stmts;
        skip_terms();
        while (!at(Tok::RBrace) && !at(Tok::Eof)) {
            usize s = pos_;
            u32 errs = diag_.error_count();
            if (Stmt* st = parse_stmt()) stmts.push_back(st);
            finish_entry(errs, s, "statement", false);
        }
        no_leading_dot_ = saved;
        expect(Tok::RBrace);
        b->stmts = arena_.copy(stmts);
        b->span = from(start);
        return b;
    }

    Cond parse_cond() {
        Cond c{};
        Span start = cur().span;
        if (eat(Tok::KwLet)) {
            c.is_let = true;
            c.name = expect_name("binding name");
            if (eat(Tok::Colon)) c.type = parse_type();
            expect(Tok::Eq);
            skip_newlines();
        }
        c.expr = parse_expr();
        c.span = from(start);
        return c;
    }

    static std::optional<AssignOp> assign_op(Tok k) {
        switch (k) {
            case Tok::Eq: return AssignOp::Assign;
            case Tok::PlusEq: return AssignOp::Add;
            case Tok::MinusEq: return AssignOp::Sub;
            case Tok::StarEq: return AssignOp::Mul;
            case Tok::SlashEq: return AssignOp::Div;
            case Tok::PercentEq: return AssignOp::Rem;
            case Tok::AmpEq: return AssignOp::BitAnd;
            case Tok::PipeEq: return AssignOp::BitOr;
            case Tok::CaretEq: return AssignOp::BitXor;
            case Tok::ShlEq: return AssignOp::Shl;
            case Tok::ShrEq: return AssignOp::Shr;
            default: return std::nullopt;
        }
    }

    Stmt* parse_stmt() {
        Span start = cur().span;
        switch (cur().kind) {
            case Tok::KwLet:
            case Tok::KwVar:
            case Tok::KwConst: {
                VarStmt* v = mk<VarStmt>(start);
                v->binding = at(Tok::KwLet) ? Binding::Let : at(Tok::KwVar) ? Binding::Var : Binding::Const;
                advance();
                v->name = expect_name("variable name");
                if (eat(Tok::Colon)) v->type = parse_type();
                if (eat(Tok::Eq)) {
                    skip_newlines();
                    v->init = parse_expr();
                }
                v->span = from(start);
                return v;
            }
            case Tok::KwReturn: {
                advance();
                ReturnStmt* r = mk<ReturnStmt>(start);
                if (!at_terminator()) r->value = parse_expr();
                r->span = from(start);
                return r;
            }
            case Tok::KwBreak: advance(); return mk<BreakStmt>(start);
            case Tok::KwContinue: advance(); return mk<ContinueStmt>(start);
            case Tok::KwThrow: {
                advance();
                ThrowStmt* t = mk<ThrowStmt>(start);
                t->value = parse_expr();
                t->span = from(start);
                return t;
            }
            case Tok::KwWhile: {
                advance();
                WhileStmt* w = mk<WhileStmt>(start);
                w->cond = parse_cond();
                w->body = parse_block();
                w->span = from(start);
                return w;
            }
            case Tok::KwFor: {
                advance();
                ForStmt* f = mk<ForStmt>(start);
                f->var = expect_name("loop variable name");
                expect(Tok::KwIn);
                f->iter = parse_expr();
                f->body = parse_block();
                f->span = from(start);
                return f;
            }
            case Tok::KwLoop: {
                advance();
                LoopStmt* l = mk<LoopStmt>(start);
                l->body = parse_block();
                l->span = from(start);
                return l;
            }
            case Tok::KwDefer: {
                advance();
                DeferStmt* d = mk<DeferStmt>(start);
                d->body = parse_block();
                d->span = from(start);
                return d;
            }
            case Tok::KwFun:
            case Tok::KwClass:
            case Tok::KwStruct:
            case Tok::KwEnum:
            case Tok::KwInterface:
            case Tok::KwActor:
            case Tok::KwExtern:
            case Tok::KwImport:
            case Tok::KwType:
                error("E0108", cur().span, "declarations are not allowed inside function bodies in Phantasos-Core");
                return nullptr;
            default:
                break;
        }
        Expr* e = parse_expr();
        if (auto op = assign_op(cur().kind)) {
            advance();
            skip_newlines();
            AssignStmt* a = mk<AssignStmt>(start);
            a->op = *op;
            a->target = e;
            a->value = parse_expr();
            a->span = from(start);
            return a;
        }
        ExprStmt* s = mk<ExprStmt>(start);
        s->expr = e;
        s->span = from(start);
        return s;
    }

    // ------------------------------------------------------------ expressions

    Expr* binary(BinOp op, Expr* l, Expr* r) {
        BinaryExpr* b = mk<BinaryExpr>(Span::join(l->span, r->span));
        b->op = op;
        b->lhs = l;
        b->rhs = r;
        return b;
    }

    Expr* parse_expr() { return parse_coalesce(); }

    Expr* parse_coalesce() {
        Expr* l = parse_or();
        if (!at(Tok::QuestionQuestion)) return l;
        advance();
        skip_newlines();
        return binary(BinOp::Coalesce, l, parse_coalesce());
    }

    Expr* parse_or() {
        Expr* l = parse_and();
        while (at(Tok::PipePipe)) {
            advance();
            skip_newlines();
            l = binary(BinOp::Or, l, parse_and());
        }
        return l;
    }

    Expr* parse_and() {
        Expr* l = parse_cmp();
        while (at(Tok::AmpAmp)) {
            advance();
            skip_newlines();
            l = binary(BinOp::And, l, parse_cmp());
        }
        return l;
    }

    static std::optional<BinOp> cmp_op(Tok k) {
        switch (k) {
            case Tok::EqEq: return BinOp::Eq;
            case Tok::NotEq: return BinOp::Ne;
            case Tok::Lt: return BinOp::Lt;
            case Tok::LtEq: return BinOp::Le;
            case Tok::Gt: return BinOp::Gt;
            case Tok::GtEq: return BinOp::Ge;
            default: return std::nullopt;
        }
    }

    Expr* parse_cmp() {
        Expr* l = parse_cast();
        auto op = cmp_op(cur().kind);
        if (!op) return l;
        advance();
        skip_newlines();
        l = binary(*op, l, parse_cast());
        while (auto again = cmp_op(cur().kind)) {
            error("E0105", cur().span, "comparison operators cannot be chained; combine them with '&&'");
            advance();
            skip_newlines();
            l = binary(*again, l, parse_cast());
        }
        return l;
    }

    Expr* parse_cast() {
        Expr* l = parse_range();
        while (at(Tok::KwIs)) {
            advance();
            cast_type_ = true;
            TypeExpr* t = parse_type();
            cast_type_ = false;
            CastExpr* c = mk<CastExpr>(Span::join(l->span, t->span));
            c->op = CastOp::Is;
            c->operand = l;
            c->type = t;
            l = c;
        }
        return l;
    }

    // `as` / `as?` bind tighter than every binary operator: `x as Int64 + 1`.
    Expr* parse_as() {
        Expr* l = parse_unary();
        while (at(Tok::KwAs)) {
            advance();
            CastOp op = CastOp::As;
            if (at(Tok::Question) && !cur().space_before()) {
                advance();
                op = CastOp::AsOpt;
            }
            cast_type_ = true;
            TypeExpr* t = parse_type();
            cast_type_ = false;
            CastExpr* c = mk<CastExpr>(Span::join(l->span, t->span));
            c->op = op;
            c->operand = l;
            c->type = t;
            l = c;
        }
        return l;
    }

    Expr* parse_range() {
        Expr* l = parse_add();
        if (!at(Tok::DotDot) && !at(Tok::DotDotLt)) return l;
        BinOp op = at(Tok::DotDot) ? BinOp::Range : BinOp::RangeExcl;
        advance();
        skip_newlines();
        l = binary(op, l, parse_add());
        if (at(Tok::DotDot) || at(Tok::DotDotLt)) error("E0107", cur().span, "range operators cannot be chained");
        return l;
    }

    static std::optional<BinOp> add_op(Tok k) {
        switch (k) {
            case Tok::Plus: return BinOp::Add;
            case Tok::Minus: return BinOp::Sub;
            case Tok::AmpPlus: return BinOp::WrapAdd;
            case Tok::AmpMinus: return BinOp::WrapSub;
            case Tok::Pipe: return BinOp::BitOr;
            case Tok::Caret: return BinOp::BitXor;
            default: return std::nullopt;
        }
    }

    static std::optional<BinOp> mul_op(Tok k) {
        switch (k) {
            case Tok::Star: return BinOp::Mul;
            case Tok::Slash: return BinOp::Div;
            case Tok::Percent: return BinOp::Rem;
            case Tok::AmpStar: return BinOp::WrapMul;
            case Tok::Amp: return BinOp::BitAnd;
            default: return std::nullopt;
        }
    }

    static std::optional<BinOp> shift_op(Tok k) {
        if (k == Tok::Shl) return BinOp::Shl;
        if (k == Tok::Shr) return BinOp::Shr;
        return std::nullopt;
    }

    Expr* parse_add() {
        Expr* l = parse_mul();
        while (auto op = add_op(cur().kind)) {
            advance();
            skip_newlines();
            l = binary(*op, l, parse_mul());
        }
        return l;
    }

    Expr* parse_mul() {
        Expr* l = parse_shift();
        while (auto op = mul_op(cur().kind)) {
            advance();
            skip_newlines();
            l = binary(*op, l, parse_shift());
        }
        return l;
    }

    Expr* parse_shift() {
        Expr* l = parse_as();
        while (auto op = shift_op(cur().kind)) {
            advance();
            skip_newlines();
            l = binary(*op, l, parse_as());
        }
        return l;
    }

    Expr* parse_unary() {
        Span start = cur().span;
        UnOp op;
        switch (cur().kind) {
            case Tok::Minus: op = UnOp::Neg; break;
            case Tok::Bang: op = UnOp::Not; break;
            case Tok::Tilde: op = UnOp::BitNot; break;
            case Tok::KwAwait: op = UnOp::Await; break;
            case Tok::KwTry: op = UnOp::Try; break;
            case Tok::KwShared: op = UnOp::Share; break;
            default: return parse_postfix();
        }
        advance();
        if (op == UnOp::Try && at(Tok::Question) && !cur().space_before()) {
            advance();
            op = UnOp::TryOpt;
        }
        Expr* operand = parse_unary();
        UnaryExpr* u = mk<UnaryExpr>(Span::join(start, operand->span));
        u->op = op;
        u->operand = operand;
        return u;
    }

    // Called after '('; consumes the closing ')'.
    List<Arg> parse_args() {
        std::vector<Arg> out;
        u32 errs = diag_.error_count();
        while (!at(Tok::RParen) && !at(Tok::Eof)) {
            Arg a{};
            if (at(Tok::Ident) && peek().kind == Tok::Colon) {
                a.label = expect_name("argument label");
                advance();
            }
            a.value = parse_expr();
            out.push_back(a);
            if (diag_.error_count() > errs || !eat(Tok::Comma)) break;
        }
        close_list(errs, Tok::LParen, Tok::RParen);
        return arena_.copy(out);
    }

    static bool generic_follow(Tok k) {
        switch (k) {
            case Tok::LParen:
            case Tok::RParen:
            case Tok::RBracket:
            case Tok::RBrace:
            case Tok::Comma:
            case Tok::Semi:
            case Tok::Colon:
            case Tok::Dot:
            case Tok::QuestionDot:
            case Tok::Newline:
            case Tok::Eof:
            case Tok::StringMiddle:
            case Tok::StringTail:
                return true;
            default:
                return false;
        }
    }

    // Spec 2.6 rule 1: speculatively parse `<Types>` after a name. With
    // check_follow, the token after '>' must also be in the follow set.
    bool try_generic_args(List<TypeExpr*>& out, bool check_follow) {
        Snapshot snap = snapshot();
        bool saved_failed = spec_failed_;
        spec_failed_ = false;
        ++spec_depth_;
        advance();  // '<'
        skip_newlines();
        std::vector<TypeExpr*> args;
        while (!spec_failed_ && !at_gt()) {
            args.push_back(parse_type());
            skip_newlines();
            if (spec_failed_ || !eat(Tok::Comma)) break;
            skip_newlines();
        }
        bool ok = !spec_failed_ && !args.empty() && eat_gt() && (!check_follow || generic_follow(cur().kind));
        --spec_depth_;
        spec_failed_ = saved_failed;
        if (!ok) {
            restore(snap);
            return false;
        }
        out = arena_.copy(args);
        return true;
    }

    Expr* parse_postfix() {
        Expr* e = parse_primary();
        while (true) {
            Span start = e->span;
            switch (cur().kind) {
                case Tok::LParen: {
                    advance();
                    CallExpr* c = mk<CallExpr>(start);
                    c->callee = e;
                    c->args = parse_args();
                    c->span = from(start);
                    e = c;
                    continue;
                }
                case Tok::Dot:
                case Tok::QuestionDot: {
                    bool opt = at(Tok::QuestionDot);
                    advance();
                    skip_newlines();
                    Name n = expect_name("member name");
                    if (opt) {
                        OptMemberExpr* m = mk<OptMemberExpr>(from(start));
                        m->base = e;
                        m->name = n;
                        e = m;
                    } else {
                        MemberExpr* m = mk<MemberExpr>(from(start));
                        m->base = e;
                        m->name = n;
                        e = m;
                    }
                    if (n.sym.empty()) return e;
                    continue;
                }
                case Tok::LBracket: {
                    advance();
                    IndexExpr* ix = mk<IndexExpr>(start);
                    ix->base = e;
                    u32 errs = diag_.error_count();
                    ix->index = parse_expr();
                    close_list(errs, Tok::LBracket, Tok::RBracket);
                    ix->span = from(start);
                    e = ix;
                    continue;
                }
                case Tok::Bang:
                    if (cur().space_before()) return e;
                    advance();
                    {
                        UnwrapExpr* u = mk<UnwrapExpr>(from(start));
                        u->operand = e;
                        e = u;
                    }
                    continue;
                case Tok::Lt: {
                    if (e->kind != ExprKind::Name && e->kind != ExprKind::Member) return e;
                    List<TypeExpr*> args;
                    if (!try_generic_args(args, true)) return e;
                    GenericInstExpr* g = mk<GenericInstExpr>(from(start));
                    g->base = e;
                    g->args = args;
                    e = g;
                    continue;
                }
                case Tok::Newline:
                    if (!no_leading_dot_ && (peek().kind == Tok::Dot || peek().kind == Tok::QuestionDot)) {
                        advance();
                        continue;
                    }
                    return e;
                default:
                    return e;
            }
        }
    }

    Expr* parse_primary() {
        const Token& t = cur();
        Span start = t.span;
        switch (t.kind) {
            case Tok::IntLit: {
                IntExpr* e = mk<IntExpr>(start);
                e->value = t.int_value;
                advance();
                return e;
            }
            case Tok::FloatLit: {
                FloatExpr* e = mk<FloatExpr>(start);
                e->value = t.float_value;
                advance();
                return e;
            }
            case Tok::CharLit: {
                CharExpr* e = mk<CharExpr>(start);
                e->value = t.char_value;
                advance();
                return e;
            }
            case Tok::StringLit: {
                StringExpr* e = mk<StringExpr>(start);
                e->value = t.symbol();
                advance();
                return e;
            }
            case Tok::StringHead: return parse_interp();
            case Tok::KwTrue:
            case Tok::KwFalse: {
                BoolExpr* e = mk<BoolExpr>(start);
                e->value = t.kind == Tok::KwTrue;
                advance();
                return e;
            }
            case Tok::KwNone: advance(); return mk<NoneExpr>(start);
            case Tok::KwSelfValue: advance(); return mk<SelfValueExpr>(start);
            case Tok::KwSuper: advance(); return mk<SuperExpr>(start);
            case Tok::KwSelfType: advance(); return mk<SelfTypeExpr>(start);
            case Tok::Ident: {
                NameExpr* e = mk<NameExpr>(start);
                e->name = Name{t.symbol(), start};
                advance();
                return e;
            }
            case Tok::Dot: {
                advance();
                ImplicitMemberExpr* e = mk<ImplicitMemberExpr>(start);
                e->name = expect_name("member name");
                e->span = from(start);
                return e;
            }
            case Tok::LParen: {
                advance();
                if (eat(Tok::RParen)) return mk<UnitExpr>(from(start));
                ParenExpr* p = mk<ParenExpr>(start);
                u32 errs = diag_.error_count();
                p->inner = parse_expr();
                close_list(errs, Tok::LParen, Tok::RParen);
                p->span = from(start);
                return p;
            }
            case Tok::LBracket: {
                advance();
                ArrayExpr* a = mk<ArrayExpr>(start);
                std::vector<Expr*> elems;
                u32 errs = diag_.error_count();
                while (!at(Tok::RBracket) && !at(Tok::Eof)) {
                    elems.push_back(parse_expr());
                    if (diag_.error_count() > errs || !eat(Tok::Comma)) break;
                }
                close_list(errs, Tok::LBracket, Tok::RBracket);
                a->elems = arena_.copy(elems);
                a->span = from(start);
                return a;
            }
            case Tok::KwIf: return parse_if();
            case Tok::KwMatch: return parse_match();
            case Tok::KwUnsafe: {
                advance();
                UnsafeExpr* u = mk<UnsafeExpr>(start);
                u->block = parse_block();
                u->span = from(start);
                return u;
            }
            default:
                error_expected("E0101", "expression");
                return mk<ErrorExpr>(start);
        }
    }

    Expr* parse_interp() {
        Span start = cur().span;
        InterpExpr* e = mk<InterpExpr>(start);
        std::vector<Symbol> texts;
        std::vector<Expr*> exprs;
        texts.push_back(cur().symbol());
        advance();
        while (true) {
            exprs.push_back(parse_expr());
            if (at(Tok::StringMiddle)) {
                texts.push_back(cur().symbol());
                advance();
                continue;
            }
            if (at(Tok::StringTail)) {
                texts.push_back(cur().symbol());
                advance();
                break;
            }
            error_expected("E0100", "')' to close the string interpolation");
            texts.push_back({});
            break;
        }
        e->texts = arena_.copy(texts);
        e->exprs = arena_.copy(exprs);
        e->span = from(start);
        return e;
    }

    Expr* parse_if() {
        Span start = cur().span;
        advance();  // if
        IfExpr* e = mk<IfExpr>(start);
        e->cond = parse_cond();
        e->then_block = parse_block();
        if (at(Tok::Newline) && peek().kind == Tok::KwElse) advance();
        if (eat(Tok::KwElse)) {
            if (at(Tok::KwIf))
                e->else_branch = parse_if();
            else
                e->else_branch = parse_block();
        }
        e->span = from(start);
        return e;
    }

    Expr* parse_match() {
        Span start = cur().span;
        advance();  // match
        MatchExpr* m = mk<MatchExpr>(start);
        m->scrutinee = parse_expr();
        std::vector<MatchArm> arms;
        if (expect(Tok::LBrace)) {
            while (at(Tok::Newline) || at(Tok::Semi) || at(Tok::Comma)) advance();
            while (!at(Tok::RBrace) && !at(Tok::Eof)) {
                usize s = pos_;
                u32 errs = diag_.error_count();
                Span arm_start = cur().span;
                MatchArm arm{};
                arm.pattern = parse_pattern();
                if (eat(Tok::KwIf)) arm.guard = parse_expr();
                if (expect(Tok::Arrow)) {
                    skip_newlines();
                    if (at(Tok::LBrace)) {
                        arm.body = parse_block();
                    } else {
                        bool saved = no_leading_dot_;
                        no_leading_dot_ = true;
                        arm.body = parse_expr();
                        no_leading_dot_ = saved;
                    }
                } else {
                    arm.body = mk<ErrorExpr>(cur().span);
                }
                arm.span = from(arm_start);
                arms.push_back(arm);
                finish_entry(errs, s, "match arm", true);
            }
            expect(Tok::RBrace);
        }
        m->arms = arena_.copy(arms);
        m->span = from(start);
        return m;
    }

    // ------------------------------------------------------------ patterns

    Pattern* parse_pattern() {
        Span start = cur().span;
        Pattern* first = parse_simple_pattern();
        if (!at(Tok::Pipe)) return first;
        std::vector<Pattern*> alts{first};
        while (eat(Tok::Pipe)) alts.push_back(parse_simple_pattern());
        AltPat* a = mk<AltPat>(from(start));
        a->alts = arena_.copy(alts);
        return a;
    }

    List<Pattern*> parse_pattern_args() {
        std::vector<Pattern*> out;
        advance();  // '('
        u32 errs = diag_.error_count();
        while (!at(Tok::RParen) && !at(Tok::Eof)) {
            out.push_back(parse_pattern());
            if (diag_.error_count() > errs || !eat(Tok::Comma)) break;
        }
        close_list(errs, Tok::LParen, Tok::RParen);
        return arena_.copy(out);
    }

    Pattern* parse_simple_pattern() {
        Span start = cur().span;
        switch (cur().kind) {
            case Tok::Underscore: advance(); return mk<WildcardPat>(start);
            case Tok::Minus:
                if (peek().kind == Tok::IntLit || peek().kind == Tok::FloatLit) {
                    // Only a numeric literal may follow '-' in a pattern; anything
                    // after it (calls, members, ...) is left for the caller to reject.
                    advance();  // '-'
                    Expr* lit = parse_primary();
                    UnaryExpr* neg = mk<UnaryExpr>(Span::join(start, lit->span));
                    neg->op = UnOp::Neg;
                    neg->operand = lit;
                    LiteralPat* p = mk<LiteralPat>(neg->span);
                    p->literal = neg;
                    return p;
                }
                break;
            case Tok::IntLit:
            case Tok::FloatLit:
            case Tok::CharLit:
            case Tok::StringLit:
            case Tok::KwTrue:
            case Tok::KwFalse:
            case Tok::KwNone: {
                LiteralPat* p = mk<LiteralPat>(start);
                p->literal = parse_primary();
                p->span = from(start);
                return p;
            }
            case Tok::Dot: {
                advance();
                CasePat* p = mk<CasePat>(start);
                p->leading_dot = true;
                std::vector<Name> path{expect_name("enum case name")};
                p->path = arena_.copy(path);
                if (at(Tok::LParen)) {
                    p->has_args = true;
                    p->args = parse_pattern_args();
                }
                p->span = from(start);
                return p;
            }
            case Tok::KwIs: {
                advance();
                IsPat* p = mk<IsPat>(start);
                p->type = parse_type();
                if (eat(Tok::KwAs)) p->binding = expect_name("binding name");
                p->span = from(start);
                return p;
            }
            case Tok::Ident: {
                if (peek().kind != Tok::Dot && peek().kind != Tok::LParen) {
                    BindingPat* b = mk<BindingPat>(start);
                    b->name = expect_name("binding name");
                    return b;
                }
                CasePat* p = mk<CasePat>(start);
                std::vector<Name> path{expect_name("enum case name")};
                while (at(Tok::Dot)) {
                    advance();
                    skip_newlines();
                    path.push_back(expect_name("enum case name"));
                }
                p->path = arena_.copy(path);
                if (at(Tok::LParen)) {
                    p->has_args = true;
                    p->args = parse_pattern_args();
                }
                p->span = from(start);
                return p;
            }
            default:
                break;
        }
        error_expected("E0103", "pattern");
        return mk<ErrorPat>(start);
    }

    FileId file_;
    std::vector<Token> toks_;
    Arena& arena_;
    Interner& in_;
    DiagEngine& diag_;
    usize pos_ = 0;
    usize last_error_pos_ = ~usize{0};
    u32 spec_depth_ = 0;
    bool spec_failed_ = false;
    bool no_leading_dot_ = false;
    bool cast_type_ = false;
    bool lexer_reported_unclosed_ = false;
    std::vector<u32> lexer_error_offsets_;
    std::vector<std::pair<usize, Token>> split_log_;
};

}  // namespace

File* parse_file(FileId file, std::vector<Token> tokens, Arena& arena, Interner& interner, DiagEngine& diag) {
    return Parser(file, std::move(tokens), arena, interner, diag).run();
}

}  // namespace phs
