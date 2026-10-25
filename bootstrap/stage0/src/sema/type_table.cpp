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

std::string TypeTable::name(ScalarType root, const Resolution& resolution, const Interner& names) const {
    using Work = std::variant<ScalarType, std::string>;
    std::vector<Work> work{root};
    std::string result;
    while (!work.empty()) {
        Work next = std::move(work.back()); work.pop_back();
        if (const auto* text = std::get_if<std::string>(&next)) { result += *text; continue; }
        auto type = std::get<ScalarType>(next);
        if (type.optional_depth) {
            const bool group = type.kind == ScalarType::Function || type.kind == ScalarType::Pointer ||
                               type.kind == ScalarType::Shared || type.kind == ScalarType::Weak;
            if (group) result += '(';
            work.emplace_back(std::string(type.optional_depth, '?'));
            if (group) work.emplace_back(std::string(")"));
            type.optional_depth = 0;
        }
        const auto* d = descriptor(type);
        if (!d) { result += scalar_type_name(type); continue; }
        switch (d->kind) {
            case ScalarType::Nominal: {
                std::vector<std::string_view> parts{names.str(resolution.binding(d->declaration).name.sym)};
                auto scope = resolution.binding(d->declaration).scope;
                while (scope.valid()) {
                    if (resolution.scopes()[scope.value].kind == ScopeKind::Members) {
                        for (const auto& binding : resolution.bindings())
                            if (binding.members == scope) { parts.push_back(names.str(binding.name.sym)); break; }
                    }
                    scope = resolution.scopes()[scope.value].parent;
                }
                for (auto it = parts.rbegin(); it != parts.rend(); ++it) { if (it != parts.rbegin()) result += '.'; result += *it; }
                break;
            }
            case ScalarType::Array:
                result += '['; work.emplace_back(std::string("]")); work.emplace_back(d->element); break;
            case ScalarType::Pointer:
                result += d->mutable_pointer ? "*mut " : "*"; work.emplace_back(d->element); break;
            case ScalarType::Shared: case ScalarType::Weak:
                result += d->kind == ScalarType::Shared ? "shared " : "weak "; work.emplace_back(d->element); break;
            case ScalarType::Function:
                result += "fun(";
                if (d->throws) {
                    work.emplace_back(std::string(")")); work.emplace_back(d->error); work.emplace_back(std::string(" throws("));
                }
                work.emplace_back(d->result); work.emplace_back(std::string(") -> "));
                for (usize i = d->parameters.size(); i > 0; --i) {
                    work.emplace_back(d->parameters[i - 1]);
                    if (i > 1) work.emplace_back(std::string(", "));
                }
                break;
            default: unreachable();
        }
    }
    return result;
}
}  // namespace phs
