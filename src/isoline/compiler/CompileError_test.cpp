// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <array>
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
        CompileErrorCode::PoolTooLarge,           CompileErrorCode::TooManyBranchArguments,
        CompileErrorCode::LiveSetTooLarge,
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
    CHECK(describe(
              CompileError { .code = CompileErrorCode::InternalVerificationFailed,
                             .function = 2,
                             .pass = "fold-constants",
                             .rule = isoline::compiler::intermediate::VerifierErrorCode::MissingTerminator })
          == "internal compiler error: ill-formed intermediate code in function 2 after pass fold-constants: "
             "block "
             "does not end with a terminator");
}

TEST_CASE("CompileError.everyCodeHasItsExactText")
{
    // The text of every code, as describe() renders a bare error of it: a changed text is a changed
    // published message, so each is pinned here rather than checked only for being non-empty.
    struct Expected
    {
        CompileErrorCode code;
        std::string_view text;
    };
    auto const expected = std::to_array<Expected>({
        { .code = CompileErrorCode::UnsupportedFeature, .text = "feature not supported by the compiler" },
        { .code = CompileErrorCode::FunctionTooLarge, .text = "function too large" },
        { .code = CompileErrorCode::TooManyInstructions, .text = "too many instructions" },
        { .code = CompileErrorCode::TooManyBlockParameters, .text = "too many block parameters" },
        { .code = CompileErrorCode::PoolTooLarge, .text = "operand pool too large" },
        { .code = CompileErrorCode::TooManyBranchArguments, .text = "too many branch arguments" },
        { .code = CompileErrorCode::BudgetExceeded, .text = "compile memory budget exceeded" },
        { .code = CompileErrorCode::HostMemoryExhausted, .text = "host memory exhausted" },
        { .code = CompileErrorCode::FrameTooLarge, .text = "frame too large" },
        { .code = CompileErrorCode::ArtifactTooLarge, .text = "compiled code too large" },
        { .code = CompileErrorCode::BranchOutOfRange, .text = "branch out of range" },
        { .code = CompileErrorCode::LiveSetTooLarge, .text = "too many values live across blocks" },
        { .code = CompileErrorCode::Cancelled, .text = "compile cancelled" },
        { .code = CompileErrorCode::InternalVerificationFailed,
          .text = "internal compiler error: ill-formed intermediate code" },
    });
    REQUIRE(expected.size() == CompileErrorCodeCount);
    for (auto const index: std::views::iota(std::size_t { 0 }, CompileErrorCodeCount))
    {
        auto const& [code, text] = expected[index];
        INFO(text);
        CHECK(static_cast<std::size_t>(code) == index);
        CHECK(compileErrorRow(code).text == text);
        CHECK(describe(CompileError { .code = code }) == text);
    }
}
