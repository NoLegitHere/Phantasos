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

#include <algorithm>
#include <string_view>
#include <variant>

namespace phs {
usize TypeDescriptorHash::operator()(const TypeDescriptor& d) const {
    usize hash = static_cast<usize>(d.kind);
    const auto add = [&](usize n) { hash ^= n + 0x9e3779b9U + (hash << 6) + (hash >> 2); };
    const auto type = [&](ScalarType t) { add(t.kind); add(t.optional_depth); add(t.detail); };
    type(d.element); type(d.result); type(d.error);
    add(d.declaration.value); add(d.mutable_pointer); add(d.throws);
    for (auto param : d.parameters) type(param);
    return hash;
}

ScalarType TypeTable::intern(TypeDescriptor descriptor) {
    const auto [it, added] = ids_.try_emplace(descriptor, descriptors_.size());
    if (added) descriptors_.push_back(std::move(descriptor));
    return {descriptors_[it->second].kind, 0, it->second};
}

std::string TypeTable::name(ScalarType root, const Resolution& resolution, const Interner& names, usize limit) const {
    struct Repeat { usize count; char character; };
    using Work = std::variant<ScalarType, std::string_view, Repeat>;
    std::vector<Work> work{root};
    std::string result;
    const usize work_limit = std::max<usize>(1, limit);
    usize steps = 0;
    bool elided = false;
    const auto append = [&](std::string_view text) {
        const usize available = limit - result.size();
        result.append(text.substr(0, available));
        elided |= text.size() > available;
    };
    const auto push = [&](Work next) {
        if (work.size() >= work_limit) elided = true;
        else work.push_back(next);
    };
    while (!work.empty() && !elided) {
        if (steps++ == work_limit) { elided = true; break; }
        Work next = std::move(work.back()); work.pop_back();
        if (const auto* text = std::get_if<std::string_view>(&next)) { append(*text); continue; }
        if (const auto* repeat = std::get_if<Repeat>(&next)) {
            const usize available = limit - result.size();
            result.append(std::min(repeat->count, available), repeat->character);
            elided |= repeat->count > available;
            continue;
        }
        auto type = std::get<ScalarType>(next);
        if (type.optional_depth) {
            const bool group = type.kind == ScalarType::Function || type.kind == ScalarType::Pointer ||
                               type.kind == ScalarType::Shared || type.kind == ScalarType::Weak;
            if (group) append("(");
            push(Repeat{type.optional_depth, '?'});
            if (group) push(")");
            type.optional_depth = 0;
        }
        if (elided) break;
        const auto* d = descriptor(type);
        if (!d) { append(scalar_type_name(type)); continue; }
        switch (d->kind) {
            case ScalarType::Nominal: {
                std::vector<std::string_view> parts{names.str(resolution.binding(d->declaration).name.sym)};
                auto scope = resolution.binding(d->declaration).scope;
                while (scope.valid()) {
                    if (steps++ == work_limit) { elided = true; break; }
                    if (resolution.scopes()[scope.value].kind == ScopeKind::Members) {
                        for (const auto& binding : resolution.bindings())
                            if (binding.members == scope) { parts.push_back(names.str(binding.name.sym)); break; }
                    }
                    scope = resolution.scopes()[scope.value].parent;
                }
                for (auto it = parts.rbegin(); it != parts.rend() && !elided; ++it) {
                    if (it != parts.rbegin()) append(".");
                    if (!elided) append(*it);
                }
                break;
            }
            case ScalarType::Array:
                append("["); push("]"); push(d->element); break;
            case ScalarType::Pointer:
                append(d->mutable_pointer ? "*mut " : "*"); push(d->element); break;
            case ScalarType::Shared: case ScalarType::Weak:
                append(d->kind == ScalarType::Shared ? "shared " : "weak "); push(d->element); break;
            case ScalarType::Function:
                append("fun(");
                if (d->throws) {
                    push(")"); push(d->error); push(" throws(");
                }
                push(d->result); push(") -> ");
                for (usize i = d->parameters.size(); i > 0 && !elided; --i) {
                    push(d->parameters[i - 1]);
                    if (i > 1) push(", ");
                }
                break;
            default: unreachable();
        }
    }
    if (elided) {
        const auto marker = std::string_view("...").substr(0, std::min<usize>(3, limit));
        result.resize(std::min(result.size(), limit - marker.size()));
        result += marker;
    }
    return result;
}
}  // namespace phs
