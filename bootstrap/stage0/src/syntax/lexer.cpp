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

#include "syntax/lexer.hpp"

#include <charconv>
#include <string>

namespace phs {

namespace {

bool is_ident_start(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
bool is_digit(char c) { return c >= '0' && c <= '9'; }
bool is_ident_cont(char c) { return is_ident_start(c) || is_digit(c); }

int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

void append_utf8(std::string& out, u32 cp) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

class Lexer {
public:
    Lexer(const SourceManager& sm, FileId file, Interner& in, DiagEngine& diag)
        : src_(sm.file(file).text), file_(file), in_(in), diag_(diag) {}

    std::vector<Token> run() {
        while (true) {
            skip_trivia();
            if (pos_ >= src_.size()) break;
            lex_token();
        }
        for (usize i = stack_.size(); i-- > 0;) {
            const Open& o = stack_[i];
            if (o.ch == 'I')
                diag_.error("E0003", o.span, "unterminated string interpolation");
            else
                diag_.error("E0010", o.span, std::string("unclosed delimiter '") + o.ch + "'");
        }
        Token eof;
        eof.kind = Tok::Eof;
        eof.flags = kTokFlagSpaceBefore;
        eof.span = span(static_cast<u32>(src_.size()), static_cast<u32>(src_.size()));
        out_.push_back(eof);
        return std::move(out_);
    }

private:
    struct Open {
        char ch;  // '(' '[' '{' or 'I' for a string interpolation
        Span span;
    };

    Span span(u32 lo, u32 hi) const { return {file_, lo, hi}; }
    char peek(u32 off = 0) const { return pos_ + off < src_.size() ? src_[pos_ + off] : '\0'; }
    bool newline_significant() const { return stack_.empty() || stack_.back().ch == '{'; }

    Token& push(Tok kind, u32 lo) {
        Token t;
        t.kind = kind;
        t.span = span(lo, pos_);
        if (space_) t.flags |= kTokFlagSpaceBefore;
        space_ = false;
        out_.push_back(t);
        return out_.back();
    }

    void skip_trivia() {
        while (pos_ < src_.size()) {
            char c = src_[pos_];
            if (c == ' ' || c == '\t' || c == '\f' || c == '\v') {
                ++pos_;
                space_ = true;
            } else if (c == '\n') {
                if (newline_significant() && !out_.empty() && out_.back().kind != Tok::Newline) {
                    Token t;
                    t.kind = Tok::Newline;
                    t.span = span(pos_, pos_ + 1);
                    out_.push_back(t);
                }
                ++pos_;
                space_ = true;
            } else if (c == '/' && peek(1) == '/') {
                while (pos_ < src_.size() && src_[pos_] != '\n') ++pos_;
                space_ = true;
            } else if (c == '/' && peek(1) == '*') {
                skip_block_comment();
                space_ = true;
            } else {
                break;
            }
        }
    }

    void skip_block_comment() {
        u32 start = pos_;
        pos_ += 2;
        u32 depth = 1;
        while (pos_ < src_.size() && depth > 0) {
            if (src_[pos_] == '/' && peek(1) == '*') {
                ++depth;
                pos_ += 2;
            } else if (src_[pos_] == '*' && peek(1) == '/') {
                --depth;
                pos_ += 2;
            } else {
                ++pos_;
            }
        }
        if (depth > 0) diag_.error("E0002", span(start, start + 2), "unterminated block comment");
    }

    void lex_token() {
        u32 start = pos_;
        char c = src_[pos_];
        if (is_ident_start(c)) return lex_ident();
        if (is_digit(c)) return lex_number();
        if (c == '"') {
            ++pos_;
            return lex_string_segment(start, true);
        }
        if (c == '\'') return lex_char();
        if (static_cast<u8>(c) >= 0x80) {
            ++pos_;
            while (pos_ < src_.size() && (static_cast<u8>(src_[pos_]) & 0xC0) == 0x80) ++pos_;
            diag_.error("E0008", span(start, pos_), "non-ASCII character outside of a literal or comment");
            space_ = true;
            return;
        }
        if (lex_punct(start)) return;
        ++pos_;
        diag_.error("E0001", span(start, pos_), std::string("unexpected character '") + c + "'");
        space_ = true;
    }

    void lex_ident() {
        u32 start = pos_;
        while (pos_ < src_.size() && is_ident_cont(src_[pos_])) ++pos_;
        std::string_view text(src_.data() + start, pos_ - start);
        if (text == "_") {
            push(Tok::Underscore, start);
            return;
        }
        Symbol sym = in_.intern(text);
        Token& t = push(keyword_kind(sym), start);
        t.sym = sym.id;
    }

    void lex_number() {
        u32 start = pos_;
        int base = 10;
        if (src_[pos_] == '0' && (peek(1) == 'x' || peek(1) == 'o' || peek(1) == 'b')) {
            base = peek(1) == 'x' ? 16 : peek(1) == 'o' ? 8 : 2;
            pos_ += 2;
            u32 digits_start = pos_;
            while (pos_ < src_.size() && is_ident_cont(src_[pos_])) ++pos_;
            std::string_view digits(src_.data() + digits_start, pos_ - digits_start);
            u64 value = 0;
            bool ok = !digits.empty() && digits[0] != '_', overflow = false;
            if (!ok) {
                diag_.error("E0006", span(start, pos_), "missing digits after integer base prefix");
            }
            for (usize i = 0; ok && i < digits.size(); ++i) {
                char d = digits[i];
                if (d == '_') continue;
                int v = hex_value(d);
                if (v < 0 || v >= base) {
                    const char* name = base == 16 ? "hexadecimal" : base == 8 ? "octal" : "binary";
                    u32 at = digits_start + static_cast<u32>(i);
                    diag_.error("E0006", span(at, at + 1),
                                std::string("invalid digit '") + d + "' in " + name + " literal");
                    ok = false;
                    break;
                }
                if (value > (~0ull - static_cast<u64>(v)) / static_cast<u64>(base)) overflow = true;
                value = value * static_cast<u64>(base) + static_cast<u64>(v);
            }
            if (ok && overflow) diag_.error("E0005", span(start, pos_), "integer literal is too large for 64 bits");
            Token& t = push(Tok::IntLit, start);
            t.int_value = (ok && !overflow) ? value : 0;
            return;
        }

        auto scan_digits = [&] {
            while (pos_ < src_.size() && (is_digit(src_[pos_]) || src_[pos_] == '_')) ++pos_;
        };
        scan_digits();
        bool is_float = false;
        if (peek() == '.' && is_digit(peek(1))) {
            is_float = true;
            ++pos_;
            scan_digits();
        }
        if (peek() == 'e' || peek() == 'E') {
            u32 save = pos_;
            ++pos_;
            if (peek() == '+' || peek() == '-') ++pos_;
            if (is_digit(peek())) {
                is_float = true;
                scan_digits();
            } else {
                pos_ = save;
            }
        }
        u32 end = pos_;
        if (pos_ < src_.size() && is_ident_cont(src_[pos_])) {
            u32 suf = pos_;
            while (pos_ < src_.size() && is_ident_cont(src_[pos_])) ++pos_;
            diag_.error("E0006", span(suf, pos_),
                        "invalid suffix '" + std::string(src_.data() + suf, pos_ - suf) + "' on number literal");
        }
        std::string clean;
        for (u32 k = start; k < end; ++k)
            if (src_[k] != '_') clean.push_back(src_[k]);

        if (is_float) {
            double v = 0;
            std::from_chars(clean.data(), clean.data() + clean.size(), v);
            Token& t = push(Tok::FloatLit, start);
            t.float_value = v;
            return;
        }
        u64 value = 0;
        bool overflow = false;
        for (char d : clean) {
            u64 v = static_cast<u64>(d - '0');
            if (value > (~0ull - v) / 10) overflow = true;
            value = value * 10 + v;
        }
        if (overflow) diag_.error("E0005", span(start, end), "integer literal is too large for 64 bits");
        Token& t = push(Tok::IntLit, start);
        t.int_value = overflow ? 0 : value;
    }

    // Parses an escape after the backslash at pos_-1. Returns false for an
    // interpolation start "\(" (which the caller handles).
    bool lex_escape(std::string& buf, bool in_char) {
        u32 esc_start = pos_ - 1;
        char e = peek();
        if (e == '(' && !in_char) return false;
        ++pos_;
        switch (e) {
            case 'n': buf.push_back('\n'); return true;
            case 'r': buf.push_back('\r'); return true;
            case 't': buf.push_back('\t'); return true;
            case '0': buf.push_back('\0'); return true;
            case '\\': buf.push_back('\\'); return true;
            case '"': buf.push_back('"'); return true;
            case '\'': buf.push_back('\''); return true;
            case 'u': {
                if (peek() != '{') break;
                ++pos_;
                u32 cp = 0;
                int n = 0;
                while (hex_value(peek()) >= 0 && n < 7) {
                    cp = cp * 16 + static_cast<u32>(hex_value(peek()));
                    ++pos_;
                    ++n;
                }
                if (peek() != '}' || n == 0 || n > 6) break;
                ++pos_;
                if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
                    diag_.error("E0004", span(esc_start, pos_), "invalid Unicode scalar value in escape");
                    return true;
                }
                append_utf8(buf, cp);
                return true;
            }
            default:
                break;
        }
        diag_.error("E0004", span(esc_start, pos_ > esc_start + 1 ? pos_ : esc_start + 2), "invalid escape sequence");
        return true;
    }

    // pos_ is just past the opening quote (first) or the closing ')' of an
    // interpolation; seg_start is where this token's span begins.
    void lex_string_segment(u32 seg_start, bool first) {
        std::string buf;
        while (true) {
            if (pos_ >= src_.size() || src_[pos_] == '\n') {
                diag_.error("E0003", span(seg_start, pos_), "unterminated string literal");
                Token& t = push(first ? Tok::StringLit : Tok::StringTail, seg_start);
                t.sym = in_.intern(buf).id;
                return;
            }
            char c = src_[pos_];
            if (c == '"') {
                ++pos_;
                Token& t = push(first ? Tok::StringLit : Tok::StringTail, seg_start);
                t.sym = in_.intern(buf).id;
                return;
            }
            if (c == '\\') {
                ++pos_;
                if (!lex_escape(buf, false)) {
                    u32 open_lo = pos_ - 1;
                    ++pos_;  // consume '('
                    Token& t = push(first ? Tok::StringHead : Tok::StringMiddle, seg_start);
                    t.sym = in_.intern(buf).id;
                    stack_.push_back({'I', span(open_lo, pos_)});
                    return;
                }
                continue;
            }
            buf.push_back(c);
            ++pos_;
        }
    }

    void lex_char() {
        u32 start = pos_;
        ++pos_;
        std::string buf;
        if (peek() == '\'') {
            ++pos_;
            diag_.error("E0007", span(start, pos_), "empty character literal");
            push(Tok::CharLit, start).char_value = 0;
            return;
        }
        while (pos_ < src_.size() && src_[pos_] != '\'' && src_[pos_] != '\n') {
            if (src_[pos_] == '\\') {
                ++pos_;
                lex_escape(buf, true);
            } else {
                buf.push_back(src_[pos_++]);
            }
        }
        if (peek() != '\'') {
            diag_.error("E0007", span(start, pos_), "unterminated character literal");
            push(Tok::CharLit, start).char_value = 0;
            return;
        }
        ++pos_;
        // Decode exactly one UTF-8 scalar value from buf.
        u32 cp = 0;
        usize len = 0;
        if (!buf.empty()) {
            u8 b0 = static_cast<u8>(buf[0]);
            len = b0 < 0x80 ? 1 : (b0 >> 5) == 0x6 ? 2 : (b0 >> 4) == 0xE ? 3 : 4;
            cp = len == 1 ? b0 : len == 2 ? (b0 & 0x1F) : len == 3 ? (b0 & 0x0F) : (b0 & 0x07);
            for (usize i = 1; i < len && i < buf.size(); ++i) cp = (cp << 6) | (static_cast<u8>(buf[i]) & 0x3F);
        }
        if (buf.size() != len)
            diag_.error("E0007", span(start, pos_), "character literal must contain exactly one character");
        push(Tok::CharLit, start).char_value = cp;
    }

    void open(char ch, Tok kind, u32 start) {
        ++pos_;
        push(kind, start);
        stack_.push_back({ch, span(start, pos_)});
    }

    void close(char opener, Tok kind, u32 start) {
        ++pos_;
        if (opener == '(' && !stack_.empty() && stack_.back().ch == 'I') {
            stack_.pop_back();
            space_ = false;
            lex_string_segment(start, false);
            return;
        }
        if (!stack_.empty() && stack_.back().ch == opener) {
            stack_.pop_back();
        } else {
            Diagnostic& d = diag_.error("E0009", span(start, pos_),
                                        std::string("unexpected closing delimiter '") + src_[start] + "'");
            if (!stack_.empty())
                d.notes.push_back({stack_.back().span, std::string("unclosed '") +
                                                           (stack_.back().ch == 'I' ? '(' : stack_.back().ch) +
                                                           "' opened here"});
            for (usize i = stack_.size(); i-- > 0;) {
                char ch = stack_[i].ch == 'I' ? '(' : stack_[i].ch;
                if (ch == opener) {
                    stack_.resize(i);
                    break;
                }
            }
        }
        push(kind, start);
    }

    bool lex_punct(u32 start) {
        char c = src_[pos_];
        char n = peek(1), n2 = peek(2);
        auto emit = [&](Tok k, u32 len) {
            pos_ += len;
            push(k, start);
            return true;
        };
        switch (c) {
            case '(': open('(', Tok::LParen, start); return true;
            case '[': open('[', Tok::LBracket, start); return true;
            case '{': open('{', Tok::LBrace, start); return true;
            case ')': close('(', Tok::RParen, start); return true;
            case ']': close('[', Tok::RBracket, start); return true;
            case '}': close('{', Tok::RBrace, start); return true;
            case ',': return emit(Tok::Comma, 1);
            case ';': return emit(Tok::Semi, 1);
            case ':': return emit(Tok::Colon, 1);
            case '@': return emit(Tok::At, 1);
            case '~': return emit(Tok::Tilde, 1);
            case '^': return n == '=' ? emit(Tok::CaretEq, 2) : emit(Tok::Caret, 1);
            case '.':
                if (n == '.' && n2 == '<') return emit(Tok::DotDotLt, 3);
                if (n == '.') return emit(Tok::DotDot, 2);
                return emit(Tok::Dot, 1);
            case '-':
                if (n == '>') return emit(Tok::Arrow, 2);
                if (n == '=') return emit(Tok::MinusEq, 2);
                return emit(Tok::Minus, 1);
            case '=':
                if (n == '=') return emit(Tok::EqEq, 2);
                if (n == '>') return emit(Tok::FatArrow, 2);
                return emit(Tok::Eq, 1);
            case '?':
                if (n == '.') return emit(Tok::QuestionDot, 2);
                if (n == '?') return emit(Tok::QuestionQuestion, 2);
                return emit(Tok::Question, 1);
            case '!': return n == '=' ? emit(Tok::NotEq, 2) : emit(Tok::Bang, 1);
            case '+': return n == '=' ? emit(Tok::PlusEq, 2) : emit(Tok::Plus, 1);
            case '*': return n == '=' ? emit(Tok::StarEq, 2) : emit(Tok::Star, 1);
            case '/': return n == '=' ? emit(Tok::SlashEq, 2) : emit(Tok::Slash, 1);
            case '%': return n == '=' ? emit(Tok::PercentEq, 2) : emit(Tok::Percent, 1);
            case '&':
                if (n == '&') return emit(Tok::AmpAmp, 2);
                if (n == '+') return emit(Tok::AmpPlus, 2);
                if (n == '-') return emit(Tok::AmpMinus, 2);
                if (n == '*') return emit(Tok::AmpStar, 2);
                if (n == '=') return emit(Tok::AmpEq, 2);
                return emit(Tok::Amp, 1);
            case '|':
                if (n == '|') return emit(Tok::PipePipe, 2);
                if (n == '=') return emit(Tok::PipeEq, 2);
                return emit(Tok::Pipe, 1);
            case '<':
                if (n == '<' && n2 == '=') return emit(Tok::ShlEq, 3);
                if (n == '<') return emit(Tok::Shl, 2);
                if (n == '=') return emit(Tok::LtEq, 2);
                return emit(Tok::Lt, 1);
            case '>':
                if (n == '>' && n2 == '=') return emit(Tok::ShrEq, 3);
                if (n == '>') return emit(Tok::Shr, 2);
                if (n == '=') return emit(Tok::GtEq, 2);
                return emit(Tok::Gt, 1);
            default:
                return false;
        }
    }

    const std::string& src_;
    FileId file_;
    Interner& in_;
    DiagEngine& diag_;
    u32 pos_ = 0;
    bool space_ = true;
    std::vector<Token> out_;
    std::vector<Open> stack_;
};

}  // namespace

std::vector<Token> lex_file(const SourceManager& sm, FileId file, Interner& interner, DiagEngine& diag) {
    return Lexer(sm, file, interner, diag).run();
}

}  // namespace phs
