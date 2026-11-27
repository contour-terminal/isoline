// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <initializer_list>
#include <ranges>
#include <span>
#include <utility>
#include <variant>
#include <vector>

#include <isoline/platform/InstructionCache.hpp>
#include <isoline/platform/JitMemory.hpp>
#include <isoline/platform/Platform.hpp>
#include <isoline/platform/PlatformTable.hpp>
#include <isoline/platform/VirtualMemory.hpp>
#include <isoline/platform/testing/JitWriteScopeKeys.hpp>

namespace isoline::platform::testing
{

/// A function of no parameters that returns an int: what returnConstantCode() encodes.
using ConstantFunction = int (*)();

/// The machine code of a function that returns @p value, for @p architecture: x86-64
/// `mov eax, imm32; ret`, arm64 `movz w0, #imm16; ret`.
/// @param architecture X64 or Arm64; any other has no encoding, and yields no bytes.
/// @param value The value it returns.
/// @return The bytes.
[[nodiscard]] inline std::vector<std::byte> returnConstantCode(Architecture architecture, uint16_t value)
{
    auto const bytesOf = [](std::initializer_list<uint32_t> words, std::size_t width) {
        auto bytes = std::vector<std::byte> {};
        for (auto const word: words)
            for (auto const index: std::views::iota(std::size_t { 0 }, width))
                bytes.push_back(static_cast<std::byte>((word >> (8 * index)) & 0xFF));
        return bytes;
    };
    switch (architecture)
    {
        case Architecture::X64: {
            auto bytes = std::vector<std::byte> { std::byte { 0xB8 } };
            auto const immediate = bytesOf({ value }, 4);
            bytes.insert(bytes.end(), immediate.begin(), immediate.end());
            bytes.push_back(std::byte { 0xC3 });
            return bytes;
        }
        case Architecture::Arm64: return bytesOf({ 0x52800000U | (uint32_t { value } << 5), 0xD65F03C0U }, 4);
        case Architecture::Other: break;
    }
    return {};
}

/// Where a test's first function starts in its region: past the 8 bytes before an indirect call's target
/// that UBSan's function check reads (-fsanitize=function), so that the check reads the region's zeroes
/// instead of the page before it; 16, the alignment Windows CFG asks of every call target.
inline constexpr auto FirstFunctionOffset = std::size_t { 16 };

/// The services sealing test code goes through.
struct ExecutableCodeServices
{
    IVirtualMemory& virtualMemory;       ///< Reserves, writes and seals on ReserveWriteThenExecute rows.
    IJitMemory& jitMemory;               ///< Maps and toggles on the MAP_JIT rows.
    IInstructionCache& instructionCache; ///< Makes what was written executable by every thread.
};

/// Test code, written into one region and sealed by a platform row's CodeMemoryKind: map, write,
/// make executable, synchronize the instruction cache, never writable and executable at once for any
/// thread. A test stand-in for the platform code memory, which seals production code the same way. Rewritable
/// in place, so that a test can change code other threads have already run.
class ExecutableCode
{
  public:
    /// Maps one allocation granule for @p kind.
    /// @param kind The platform row's code memory; not Unavailable.
    /// @param services The services; must outlive the code.
    /// @return The code; or the mapping's failure.
    [[nodiscard]] static std::expected<ExecutableCode, VirtualMemoryFailure> create(
        CodeMemoryKind kind, ExecutableCodeServices services)
    {
        auto const bytes = services.virtualMemory.allocationGranularity();
        auto region =
            mapsJit(kind)
                ? services.jitMemory.mapJit(
                      bytes, togglesJitPerThread(kind) ? JitMapping::ReadWriteExecute : JitMapping::ReadWrite)
                : services.virtualMemory.reserve(bytes);
        return region.transform(
            [kind, services](Region mapped) { return ExecutableCode { kind, services, mapped }; });
    }

    ExecutableCode(ExecutableCode&& other) noexcept:
        _kind { other._kind },
        _services { other._services },
        _region { std::exchange(other._region, Region {}) }
    {
    }

    ExecutableCode(ExecutableCode const&) = delete;
    ExecutableCode& operator=(ExecutableCode const&) = delete;
    ExecutableCode& operator=(ExecutableCode&&) = delete;

    /// Releases the region.
    ~ExecutableCode()
    {
        if (_region.base == nullptr)
            return;
        [[maybe_unused]] auto const released =
            mapsJit(_kind) ? _services.jitMemory.release(_region) : _services.virtualMemory.release(_region);
    }

    /// Writes @p code at @p offset and seals the whole region again, then synchronizes the instruction
    /// cache. No thread may be running the region meanwhile, except where the toggle gives each thread
    /// its own view (MapJitWithThreadToggle), where others may run what is not being written.
    /// @param offset Where, in bytes from the region's start.
    /// @param code The machine code.
    /// @return Nothing; or which step failed: a virtual-memory failure, or the instruction cache's.
    [[nodiscard]] std::expected<void, std::variant<VirtualMemoryFailure, InstructionCacheFailure>> write(
        std::size_t offset, std::span<std::byte const> code)
    {
        using Failure = std::variant<VirtualMemoryFailure, InstructionCacheFailure>;
        auto const copy = [this, offset, code] {
            std::memcpy(_region.base + offset, code.data(), code.size());
        };
        auto sealed = std::expected<void, VirtualMemoryFailure> {};
        if (togglesJitPerThread(_kind))
        {
            auto const scope = JitWriteScope { _services.jitMemory, JitWriteScopeKeys::key() };
            copy();
        }
        else
        {
            sealed = _services.virtualMemory.protect(_region, Protection::ReadWrite).and_then([&] {
                copy();
                return _services.virtualMemory.protect(_region, Protection::ReadExecute);
            });
        }
        if (!sealed)
            return std::unexpected { Failure { sealed.error() } };
        auto const synchronized =
            _services.instructionCache.synchronize(std::span { _region.base + offset, code.size() });
        if (!synchronized)
            return std::unexpected { Failure { synchronized.error() } };
        return {};
    }

    /// The region's bytes, for a test that writes them itself.
    /// @return Them.
    [[nodiscard]] std::span<std::byte> bytes() const noexcept { return { _region.base, _region.bytes }; }

    /// The function at @p offset.
    /// @param offset Its first instruction, in bytes from the region's start.
    /// @return It.
    [[nodiscard]] ConstantFunction function(std::size_t offset) const noexcept
    {
        return reinterpret_cast<ConstantFunction>(_region.base + offset);
    }

  private:
    ExecutableCode(CodeMemoryKind kind, ExecutableCodeServices services, Region region) noexcept:
        _kind { kind }, _services { services }, _region { region }
    {
    }

    CodeMemoryKind _kind;             ///< How it is sealed.
    ExecutableCodeServices _services; ///< What it is sealed with.
    Region _region;                   ///< Where it is.
};

} // namespace isoline::platform::testing
