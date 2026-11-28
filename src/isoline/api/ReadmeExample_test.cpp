// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <expected>
#include <fstream>
#include <iterator>
#include <memory>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <isoline/binary/testing/Code.hpp>
#include <isoline/binary/testing/ModuleBuilder.hpp>

// readme-includes:begin
#include <isoline/api/Engine.hpp>
#include <isoline/api/Store.hpp>
// readme-includes:end

using isoline::api::FuncType;
using isoline::api::HeapMemoryAllocator;
using isoline::api::IHostFunction;
using isoline::api::IMemoryAllocator;
using isoline::api::InstantiateError;
using isoline::api::LoadError;

namespace
{

/// What the README's example returns when loading failed.
int report(LoadError const& /*error*/)
{
    return 1;
}

/// What the README's example returns when instantiation failed.
int report(InstantiateError const& /*error*/)
{
    return 2;
}

/// The README's example, as it compiles and runs: everything between the readme-body markers is the
/// README's code block after its includes, indented one level here.
/// @return 0 when "add" returned, else what failed: 1 load, 2 instantiation, 3 the call.
int readmeExample(std::span<std::byte const> wasmBytes,
                  IMemoryAllocator& memoryAllocator,
                  FuncType const& logType,
                  std::shared_ptr<IHostFunction> const& logFunction,
                  isoline::api::Value& sum)
{
    // readme-body:begin
    auto const engine =
        isoline::api::Engine { isoline::api::EngineConfig { /* features, engine, limits */ } };

    auto const module = engine.load(wasmBytes); // std::expected<Module, LoadError>
    if (!module)
        return report(module.error()); // invalid module, or over a configured limit

    auto store = isoline::api::Store { engine, memoryAllocator };
    auto imports = isoline::api::Imports {};
    imports.define("env", "log", logType, logFunction); // a host function: its type and implementation

    auto const instance = store.instantiate(*module, imports); // std::expected<Instance, InstantiateError>
    if (!instance)
        return report(instance.error());

    auto const args = std::array<isoline::api::Value, 2> { isoline::api::I32 { 2 }, isoline::api::I32 { 3 } };
    auto results = std::array<isoline::api::Value, 1> {};
    auto const called =
        instance->call(*instance->function("add"), args, results); // traps come back as values
    // readme-body:end
    sum = results[0];
    return called ? 0 : 3;
}

/// The lines of the file @p path.
std::vector<std::string> linesOfFile(std::string const& path)
{
    auto file = std::ifstream { path };
    REQUIRE(file.is_open());
    // Read line by line: slurped through an istreambuf_iterator instead, GCC 14's release build
    // reports a potential null dereference of the stream buffer.
    auto lines = std::vector<std::string> {};
    auto line = std::string {};
    while (std::getline(file, line))
    {
        if (line.ends_with('\r'))
            line.pop_back();
        lines.push_back(line);
    }
    return lines;
}

/// The lines strictly between @p begin and @p end, the first of each after @p from.
std::vector<std::string> between(std::vector<std::string> const& lines,
                                 std::string_view begin,
                                 std::string_view end,
                                 std::vector<std::string>::const_iterator from)
{
    auto const first = std::find(from, lines.end(), begin);
    REQUIRE(first != lines.end());
    auto const last = std::find(std::next(first), lines.end(), end);
    REQUIRE(last != lines.end());
    return { std::next(first), last };
}

} // namespace

TEST_CASE("Readme.exampleRuns")
{
    // (module (import "env" "log" (func (param i32))) (func (export "add") (param i32 i32) (result i32) ...))
    using namespace isoline::binary::testing;
    auto const i32 = isoline::api::ValueType { .kind = isoline::api::ValueKind::I32 };
    auto builder = ModuleBuilder {};
    builder.type(FuncType { .params = { i32 }, .results = {} })
        .type(FuncType { .params = { i32, i32 }, .results = { i32 } })
        .importFunction("env", "log", 0)
        .function(1, {}, code({ { 0x20, 0x00, 0x20, 0x01, 0x6A } })) // local.get 0 local.get 1 i32.add
        .exportFunction("add", 1);
    auto const bytes = builder.build();
    auto allocator = HeapMemoryAllocator {};
    auto const log = isoline::api::makeHostFunction(
        [](isoline::api::HostContext&,
           std::span<isoline::api::Value const>,
           std::span<isoline::api::Value>) noexcept -> std::expected<void, isoline::api::Abrupt> {
            return {};
        });
    auto sum = isoline::api::Value {};
    CHECK(readmeExample(bytes, allocator, FuncType { .params = { i32 }, .results = {} }, log, sum) == 0);
    CHECK(sum == isoline::api::Value { isoline::api::I32 { 5 } });
}

TEST_CASE("Readme.exampleIsTheReadmesCode")
{
    // The README's code block is this file's includes, a blank line, and its body, one level less indented.
    auto const self = linesOfFile(ISOLINE_SOURCE_DIR "/src/isoline/api/ReadmeExample_test.cpp");
    auto expected = between(self, "// readme-includes:begin", "// readme-includes:end", self.begin());
    expected.emplace_back();
    for (auto const& line: between(self, "    // readme-body:begin", "    // readme-body:end", self.begin()))
        expected.push_back(line.starts_with("    ") ? line.substr(4) : line);
    auto const readme = linesOfFile(ISOLINE_SOURCE_DIR "/README.md");
    auto const section = std::ranges::find(readme, std::string_view { "## A taste of the embedding API" });
    REQUIRE(section != readme.end());
    auto const block = between(readme, "```cpp", "```", section);
    std::ostringstream text;
    for (auto const& line: expected)
        text << line << '\n';
    INFO("README.md's example should read:\n" << text.str());
    CHECK(block == expected);
}
