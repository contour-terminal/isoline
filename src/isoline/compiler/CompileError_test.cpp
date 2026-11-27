// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <ranges>
#include <set>
#include <string>
#include <string_view>

#include <isoline/Features.hpp>
#include <isoline/compiler/CompileError.hpp>

using isoline::Feature;
using isoline::compiler::CompileError;
using isoline::compiler::CompileErrorCode;
using isoline::compiler::CompileErrorCodeCount;
using isoline::compiler::compileErrorRow;
using isoline::compiler::CompileErrorTable;
using isoline::compiler::describe;
using isoline::compiler::ImplementationLimit;

TEST_CASE("CompileError.tableIsComplete")
{
    // One row per code, in order, each with its own text; the implementation limits are the caps a
    // valid module can run into, and nothing else (R4).
    REQUIRE(CompileErrorTable.size() == CompileErrorCodeCount);
    auto texts = std::set<std::string_view> {};
    for (auto const index: std::views::iota(std::size_t { 0 }, CompileErrorCodeCount))
    {
        auto const& row = CompileErrorTable[index];
        INFO("row " << index << ": " << row.text);
        CHECK(static_cast<std::size_t>(row.code) == index);
        CHECK(&compileErrorRow(row.code) == &row);
        CHECK_FALSE(row.text.empty());
        CHECK(texts.insert(row.text).second);
    }
    auto const limits = std::set {
        CompileErrorCode::FunctionTooLarge,       CompileErrorCode::TooManyInstructions,
        CompileErrorCode::TooManyBlockParameters, CompileErrorCode::FrameTooLarge,
        CompileErrorCode::ArtifactTooLarge,       CompileErrorCode::BranchOutOfRange,
    };
    for (auto const& row: CompileErrorTable)
    {
        INFO(row.text);
        CHECK((row.limit == ImplementationLimit::Yes) == limits.contains(row.code));
    }
}

TEST_CASE("CompileError.describe")
{
    CHECK(describe(CompileError { .code = CompileErrorCode::Cancelled }) == "compile cancelled");
    CHECK(describe(CompileError {
              .code = CompileErrorCode::UnsupportedFeature, .feature = Feature::Simd, .function = 3 })
          == "feature not supported by the compiler: simd in function 3");
    CHECK(describe(CompileError { .code = CompileErrorCode::FunctionTooLarge,
                                  .function = 0,
                                  .requested = 7'654'322,
                                  .limit = 7'654'321 })
          == "function too large in function 0 (7654322 requested, limit 7654321)");
    CHECK(describe(CompileError { .code = CompileErrorCode::ArtifactTooLarge, .requested = 1, .limit = 0 })
          == "compiled code too large (1 requested, limit 0)");
}
