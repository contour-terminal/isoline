// SPDX-License-Identifier: Apache-2.0
#pragma once

// Executable memory before P7 (P5 Decision "Executable memory before P7"): generated code is copied into
// pages mapped read-write, which are then sealed read-execute, so that no page is ever writable and
// executable at once. The interface is the seam: P7's platform code memory (W^X toggles, MAP_JIT, Windows
// CFG, the instruction-cache maintenance a non-x86-64 core needs) implements the same ICodeMemory. The
// platform's half of PortableCodeMemory is CMake-selected (CodeMemoryPosix.cpp, CodeMemoryWindows.cpp).

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>

namespace isoline::execution
{

/// Why code could not be sealed.
enum class CodeMemoryError : uint8_t
{
    MapRefused,     ///< The system refused the read-write pages (out of address space or memory).
    ProtectRefused, ///< The system refused to make them read-execute.
};

/// Code sealed into executable memory, unmapped when destroyed. Its bytes never change.
class ISealedCode
{
  public:
    virtual ~ISealedCode() = default;

    /// The code: the bytes seal() was given, at the address they run at.
    [[nodiscard]] virtual std::span<std::byte const> code() const noexcept = 0;

  protected:
    ISealedCode() = default;
    ISealedCode(ISealedCode const&) = default;
    ISealedCode(ISealedCode&&) = default;
    ISealedCode& operator=(ISealedCode const&) = default;
    ISealedCode& operator=(ISealedCode&&) = default;
};

/// Where generated code is made executable: the DI seam for the platform's executable memory.
class ICodeMemory
{
  public:
    virtual ~ICodeMemory() = default;

    /// Maps @p bytes read-write, copies them, then seals read-execute: never writable and executable at once.
    /// On success the code is ready for instruction fetch on every core (see Decision "Executable memory";
    /// amended for P6, 2026-11-26: F5).
    /// @param bytes The code; empty code is sealed too, as one page holding nothing.
    /// @return The sealed code; or why the system refused.
    [[nodiscard]] virtual std::expected<std::unique_ptr<ISealedCode>, CodeMemoryError> seal(
        std::span<std::byte const> bytes) = 0;

  protected:
    ICodeMemory() = default;
    ICodeMemory(ICodeMemory const&) = default;
    ICodeMemory(ICodeMemory&&) = default;
    ICodeMemory& operator=(ICodeMemory const&) = default;
    ICodeMemory& operator=(ICodeMemory&&) = default;
};

/// The portable code memory: anonymous pages mapped read-write, filled, then protected read-execute
/// (mmap and mprotect, or VirtualAlloc and VirtualProtect with FlushInstructionCache). It does no other
/// instruction-cache maintenance, so it meets seal()'s contract only where instruction fetch is coherent with
/// data writes (x86-64); elsewhere it is a test vehicle until P7's code memory replaces it. Stateless: one
/// serves every engine.
class PortableCodeMemory final: public ICodeMemory
{
  public:
    [[nodiscard]] std::expected<std::unique_ptr<ISealedCode>, CodeMemoryError> seal(
        std::span<std::byte const> bytes) override;
};

/// The bytes @p bytes take as whole pages of @p pageBytes: at least one page.
/// @param bytes The code's size.
/// @param pageBytes The page size, a power of two.
/// @return The mapping's size.
[[nodiscard]] constexpr std::size_t mappedBytesOf(std::size_t bytes, std::size_t pageBytes) noexcept
{
    auto const wanted = bytes == 0 ? std::size_t { 1 } : bytes;
    return (wanted + pageBytes - 1) & ~(pageBytes - 1);
}

} // namespace isoline::execution
