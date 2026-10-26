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

#include "test_support.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <format>
#include <fstream>
#include <span>
#include <string>
#include <vector>

#include "codegen/elf.hpp"
#include "codegen/x86_64.hpp"
#include "frontend/session.hpp"

#if defined(__linux__) && defined(__x86_64__)
#include <sys/wait.h>
#include <unistd.h>
#define PHS_EXEC 1
#endif

using namespace phs;

namespace {

struct Built {
    FrontendSession session;
    const mir::Program* program = nullptr;
    std::vector<obj::Object> objects;
    std::vector<u8> image;

    explicit Built(std::string_view source) {
        const auto id = session.sources().add("elf.phs", source);
        CHECK(id);
        program = session.lower(*id);
        if (!program) session.diagnostics().render(stderr);
        CHECK(program);
        objects.push_back(x86_64::emit(*program));
        objects.push_back(x86_64::runtime(*program));
        auto linked = elf::link(objects, x86_64::kEntrySymbol);
        if (!linked) std::println(stderr, "{}", linked.error());
        CHECK(linked);
        image = std::move(*linked);
    }
};

u64 read(const std::vector<u8>& bytes, u64 offset, int size) {
    u64 value = 0;
    for (int i = 0; i < size; ++i) value |= u64{bytes[offset + i]} << (8 * i);
    return value;
}

constexpr std::string_view kFib = R"(
fun fib(n: Int): Int {
    if n < 2 { return n }
    return fib(n - 1) + fib(n - 2)
}

fun sumTo(limit: Int): Int {
    var total = 0
    var i = 1
    while i <= limit {
        if i % 3 == 0 || i % 5 == 0 { total += i }
        i += 1
    }
    return total
}

fun main(): Int {
    return fib(10) - sumTo(9) + 10
}
)";

void headers() {
    Built built(kFib);
    const auto& image = built.image;
    CHECK(image.size() > 128);
    CHECK(std::memcmp(image.data(), "\x7F" "ELF\x02\x01\x01", 7) == 0);
    CHECK(read(image, 16, 2) == 2);   // ET_EXEC
    CHECK(read(image, 18, 2) == 62);  // EM_X86_64
    CHECK(read(image, 56, 2) == 1);   // one program header
    CHECK(read(image, 64, 4) == 1);   // PT_LOAD
    CHECK(read(image, 68, 4) == 5);   // R+X
    CHECK(read(image, 72, 8) == 0);
    CHECK(read(image, 80, 8) == elf::kBaseAddress);
    const u64 load_size = read(image, 96, 8);
    CHECK(read(image, 104, 8) == load_size);  // memsz == filesz

    // The entry is _start, which the runtime object places first.
    const u64 runtime_base = (built.objects[0].text.size() + 15) / 16 * 16;
    CHECK(read(image, 24, 8) == elf::kBaseAddress + 128 + runtime_base);
    CHECK(load_size == 128 + runtime_base + built.objects[1].text.size());

    // .symtab names every defined symbol.
    const u64 shoff = read(image, 40, 8);
    CHECK(read(image, 60, 2) == 5 && read(image, 62, 2) == 4);
    const u64 symtab = shoff + 2 * 64, strtab = shoff + 3 * 64;
    CHECK(read(image, symtab + 4, 4) == 2 && read(image, strtab + 4, 4) == 3);
    const u64 symbols = read(image, symtab + 24, 8), strings = read(image, strtab + 24, 8);
    std::vector<std::string> names;
    for (u64 at = symbols + 24; at < symbols + read(image, symtab + 32, 8); at += 24)
        names.emplace_back(reinterpret_cast<const char*>(image.data() + strings + read(image, at, 4)));
    for (const auto* expected : {"_P3fib", "_P5sumTo", "_P4main", "_start", "__phantasos_trap"})
        CHECK(std::ranges::find(names, expected) != names.end());
}

void link_errors() {
    Built built(kFib);
    // The program alone leaves the trap entry undefined.
    const auto alone = elf::link(std::span(built.objects).first(1), x86_64::kEntrySymbol);
    CHECK(!alone && alone.error() == "undefined symbol '__phantasos_trap'");
    const std::array twice{built.objects[0], built.objects[1], built.objects[1]};
    const auto duplicate = elf::link(twice, x86_64::kEntrySymbol);
    CHECK(!duplicate && duplicate.error() == "symbol '_start' is defined more than once");
    const auto no_entry = elf::link(built.objects, "_begin");
    CHECK(!no_entry && no_entry.error() == "undefined entry symbol '_begin'");
}

void runtime_listing() {
    Built built(kFib);
    std::string listing;
    (void)x86_64::runtime(*built.program, &listing);
    CHECK(listing.contains("_start:  ; process entry"));
    CHECK(listing.contains("call _P4main"));
    CHECK(listing.contains("mov edi, eax"));
    CHECK(listing.contains("mov edi, 101"));
    CHECK(listing.contains("db \"panic: division by zero\\n\""));

    Built unit("fun main() {}\n");
    (void)x86_64::runtime(*unit.program, &listing);
    CHECK(listing.contains("xor edi, edi") && !listing.contains("mov edi, eax"));
}

#ifdef PHS_EXEC
struct Exit {
    int status = -1;
    std::string err;
};

// Writes the image to a file, runs it and collects its exit status and stderr.
Exit run(const std::vector<u8>& image) {
    test::TempDirectory dir;
    const auto path = dir.path / "program";
    {
        std::ofstream out(path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(image.data()), static_cast<std::streamsize>(image.size()));
        CHECK(out.good());
    }
    std::filesystem::permissions(path, std::filesystem::perms::owner_all);
    int pipe_fds[2];
    CHECK(pipe(pipe_fds) == 0);
    const pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        dup2(pipe_fds[1], 2);
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        const std::string file = path.string();
        char* argv[] = {const_cast<char*>(file.c_str()), nullptr};
        char* envp[] = {nullptr};
        execve(file.c_str(), argv, envp);
        _exit(127);
    }
    close(pipe_fds[1]);
    Exit result;
    char buffer[256];
    for (ssize_t n; (n = ::read(pipe_fds[0], buffer, sizeof buffer)) > 0;) result.err.append(buffer, static_cast<usize>(n));
    close(pipe_fds[0]);
    int status = 0;
    CHECK(waitpid(pid, &status, 0) == pid);
    CHECK(WIFEXITED(status));
    result.status = WEXITSTATUS(status);
    return result;
}

void expect(std::string_view source, int status, std::string_view err = "") {
    Built built(source);
    const auto result = run(built.image);
    if (result.status != status || result.err != err) {
        std::println(stderr, "{}\nexpected exit {} '{}', got {} '{}'", source, status, err, result.status, result.err);
        CHECK(false);
    }
}

void executables() {
    expect(kFib, 42);
    expect("fun main() {}\n", 0);
    expect("fun main(): Int = 0\n", 0);
    // The kernel keeps the low 8 bits of the status.
    expect("fun main(): Int = 300\n", 44);
    expect("fun main(): Int = -1\n", 255);
    expect(R"(
fun add(a: Int, b: Int, c: Int, d: Int, e: Int, f: Int, g: Int, h: Int): Int = a + b + c + d + e + f + g * h
fun main(): Int = add(1, 2, 3, 4, 5, 6, 7, 8)
)", 77);
    expect(R"(
fun depth(n: Int): Int {
    if n == 0 { return 0 }
    return depth(n - 1) + 1
}
fun main(): Int = depth(10000) - 9990
)", 10);
    expect(R"(
fun main(): Int {
    var x: Int8 = 120
    var steps = 0
    while true {
        x = x &+ 3
        steps += 1
        if x < 0 { break }
    }
    return steps
}
)", 3);
    expect("fun zero(): Int = 0\nfun main(): Int = 7 / zero()\n", 101, "panic: division by zero\n");
    expect("fun zero(): UInt8 = 0\nfun main(): Int = Int(UInt8(7) % zero())\n", 101, "panic: division by zero\n");
    expect("fun big(): Int = 9223372036854775807\nfun main(): Int = big() + 1\n", 101, "panic: arithmetic overflow\n");
    expect("fun minus(): Int = -1\nfun main(): Int = (-9223372036854775807 - 1) / minus()\n", 101,
           "panic: arithmetic overflow\n");
    expect("fun main() {\n    let x: UInt = 0\n    let y = x - 1\n}\n", 101, "panic: arithmetic overflow\n");
    // A trap inside a Unit main still panics; short-circuiting skips one.
    expect("fun boom(): Bool = 1 / 0 == 1\nfun main(): Int {\n    let a = false && boom()\n    return 5\n}\n", 5);
}
#endif

}  // namespace

int main() {
    headers();
    link_errors();
    runtime_listing();
#ifdef PHS_EXEC
    executables();
#endif
    std::println("elf tests passed");
}
