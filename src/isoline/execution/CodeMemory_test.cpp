// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

#include <isoline/execution/CodeMemory.hpp>
#include <isoline/execution/testing/CompileOne.hpp>
#include <isoline/execution/testing/PageProtection.hpp>

using isoline::execution::mappedBytesOf;
using isoline::execution::PortableCodeMemory;
using isoline::execution::testing::hostRunsCompiledCode;
using isoline::execution::testing::protectionOf;

namespace
{

/// Where a called entry lies in sealed code, at the least.
constexpr auto EntryOffset = std::size_t { 16 };

/// x86-64's `mov eax, 42; ret`.
constexpr auto ReturnFortyTwo = std::to_array<std::byte>({ std::byte { 0xb8 },
                                                           std::byte { 0x2a },
                                                           std::byte { 0 },
                                                           std::byte { 0 },
                                                           std::byte { 0 },
                                                           std::byte { 0xc3 } });

} // namespace

TEST_CASE("CodeMemory.sealedCodeIsNotWritable")
{
    auto memory = PortableCodeMemory {};
    SECTION("code: read and execute, never write, holding the bytes it was given")
    {
        auto const sealed = memory.seal(ReturnFortyTwo);
        REQUIRE(sealed.has_value());
        auto const code = (*sealed)->code();
        CHECK(std::ranges::equal(code, ReturnFortyTwo));
        CHECK(protectionOf(code.data()) == std::optional<std::string> { "r-x" });
    }
    SECTION("no code: one page, sealed alike")
    {
        auto const sealed = memory.seal({});
        REQUIRE(sealed.has_value());
        CHECK((*sealed)->code().empty());
        CHECK(protectionOf((*sealed)->code().data()) == std::optional<std::string> { "r-x" });
    }
    SECTION("unmapped once destroyed")
    {
        auto sealed = memory.seal(ReturnFortyTwo);
        REQUIRE(sealed.has_value());
        auto const* const address = (*sealed)->code().data();
        sealed->reset();
        CHECK(protectionOf(address) != std::optional<std::string> { "r-x" });
    }
}

TEST_CASE("CodeMemory.runsARet")
{
    if (!hostRunsCompiledCode())
        SKIP("the code is x86-64's");
    // The entry lies past the mapping's first 16 bytes, as every entry of compiled code does: a caller
    // instrumented by UBSan's function check reads the bytes before an indirect call's target, which at a
    // mapping's first byte lie in whatever precedes it (unmapped, or another mapping's bytes it then follows
    // as a pointer).
    auto padded = std::vector<std::byte>(EntryOffset, std::byte { 0xcc });
    std::ranges::copy(ReturnFortyTwo, std::back_inserter(padded));
    auto memory = PortableCodeMemory {};
    auto const sealed = memory.seal(padded);
    REQUIRE(sealed.has_value());
    using Function = uint32_t (*)() noexcept;
    auto const function = reinterpret_cast<Function>(
        reinterpret_cast<std::uintptr_t>((*sealed)->code().subspan(EntryOffset).data()));
    CHECK(function() == 42);
}

TEST_CASE("CodeMemory.mapsWholePages")
{
    constexpr auto Page = std::size_t { 4096 };
    CHECK(mappedBytesOf(0, Page) == Page);
    CHECK(mappedBytesOf(1, Page) == Page);
    CHECK(mappedBytesOf(Page, Page) == Page);
    CHECK(mappedBytesOf(Page + 1, Page) == 2 * Page);
}
