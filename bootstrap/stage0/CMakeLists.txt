# Copyright 2026 The Phantasos Authors
# SPDX-License-Identifier: Apache-2.0
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

cmake_minimum_required(VERSION 3.28)
project(phantasos_stage0 VERSION 0.1.0 LANGUAGES CXX)

if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "stage0 must be built with clang++ (found ${CMAKE_CXX_COMPILER_ID})")
endif()
if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS 18)
    message(FATAL_ERROR "stage0 requires clang++ >= 18 (found ${CMAKE_CXX_COMPILER_VERSION})")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(PHS_WERROR "Treat warnings as errors" OFF)

add_executable(phantasos-stage0
    src/main.cpp
    src/support/arena.cpp
    src/support/interner.cpp
    src/support/source.cpp
    src/support/diag.cpp
    src/syntax/token.cpp
    src/syntax/lexer.cpp
    src/syntax/parser.cpp
    src/syntax/ast_dump.cpp
)

target_include_directories(phantasos-stage0 PRIVATE src)
target_compile_definitions(phantasos-stage0 PRIVATE
    PHS_VERSION="${PROJECT_VERSION}"
    _HAS_EXCEPTIONS=0
    _CRT_SECURE_NO_WARNINGS
)
target_compile_options(phantasos-stage0 PRIVATE
    -fno-exceptions -fno-rtti
    -Wall -Wextra -Wpedantic -Wshadow -Wno-missing-field-initializers
    $<$<BOOL:${PHS_WERROR}>:-Werror>
)
