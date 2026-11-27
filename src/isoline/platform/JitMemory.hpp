// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <core/Assert.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <thread>
#include <utility>

#include <isoline/meta/EnumRows.hpp>
#include <isoline/platform/Platform.hpp>
#include <isoline/platform/VirtualMemory.hpp>

namespace isoline::platform
{

/// What the calling thread may do with every MAP_JIT region, on a platform whose write protection of
/// them is per thread (CodeMemoryKind::MapJitWithThreadToggle).
enum class JitAccess : uint8_t
{
    /// Execute and read, never write: the state every thread starts in, and the one a thread that never
    /// asks for Write keeps for its whole life.
    Execute,
    /// Write and read, never execute: the calling thread's view only, every other thread keeps its own.
    Write,
};

/// The last JitAccess enumerator; it must name whichever enumerator is listed last above.
inline constexpr auto LastJitAccess = JitAccess::Write;

/// How a MAP_JIT region is mapped.
enum class JitMapping : uint8_t
{
    /// Readable and writable; made executable afterwards with IVirtualMemory::protect(ReadExecute), so it
    /// is never writable and executable at once (macOS x86-64, MapJitWithProtection).
    ReadWrite,
    /// Readable, writable and executable as far as the page tables go, but each thread sees it either
    /// writable or executable, never both (JitAccess): macOS arm64 only, MapJitWithThreadToggle.
    ReadWriteExecute,
};

/// The last JitMapping enumerator; it must name whichever enumerator is listed last above.
inline constexpr auto LastJitMapping = JitMapping::ReadWriteExecute;

/// Whether a code memory maps with MAP_JIT.
enum class JitMappingUse : uint8_t
{
    None,   ///< It reserves with IVirtualMemory, never MAP_JIT.
    MapJit, ///< It maps with IJitMemory::mapJit() (macOS).
};

/// The last JitMappingUse enumerator; it must name whichever enumerator is listed last above.
inline constexpr auto LastJitMappingUse = JitMappingUse::MapJit;

/// How a code memory keeps its pages from being writable and executable at once.
enum class JitWriteProtection : uint8_t
{
    /// By page protection: written while read-write, then made read-execute (mprotect, VirtualProtect).
    PageProtection,
    /// By the per-thread MAP_JIT toggle (pthread_jit_write_protect_np): each thread sees the pages
    /// writable or executable, never both (macOS arm64).
    PerThreadToggle,
};

/// The last JitWriteProtection enumerator; it must name whichever enumerator is listed last above.
inline constexpr auto LastJitWriteProtection = JitWriteProtection::PerThreadToggle;

/// What one CodeMemoryKind does with MAP_JIT: one row of CodeMemoryTable.
struct CodeMemoryRow
{
    CodeMemoryKind kind = CodeMemoryKind::Unavailable; ///< The kind.
    JitMappingUse jit = JitMappingUse::None;           ///< Whether it maps with MAP_JIT.
    JitWriteProtection writeProtection = JitWriteProtection::PageProtection; ///< How W^X holds.
};

/// Every CodeMemoryKind, at its enumerator's index.
inline constexpr auto CodeMemoryTable = std::to_array<CodeMemoryRow>({
    { .kind = CodeMemoryKind::ReserveWriteThenExecute,
      .jit = JitMappingUse::None,
      .writeProtection = JitWriteProtection::PageProtection },
    { .kind = CodeMemoryKind::MapJitWithProtection,
      .jit = JitMappingUse::MapJit,
      .writeProtection = JitWriteProtection::PageProtection },
    { .kind = CodeMemoryKind::MapJitWithThreadToggle,
      .jit = JitMappingUse::MapJit,
      .writeProtection = JitWriteProtection::PerThreadToggle },
    { .kind = CodeMemoryKind::Unavailable,
      .jit = JitMappingUse::None,
      .writeProtection = JitWriteProtection::PageProtection },
});

static_assert(meta::firstMisplacedRow<LastCodeMemoryKind>(CodeMemoryTable, &CodeMemoryRow::kind)
                  == meta::NoMisplacedRow,
              "CodeMemoryTable has one row per enumerator, at its index");

/// The row of @p kind.
/// @param kind The kind.
/// @return Its row of CodeMemoryTable; @p kind must be an enumerator (checked).
[[nodiscard]] constexpr CodeMemoryRow const& codeMemoryRow(CodeMemoryKind kind) noexcept
{
    Require(std::to_underlying(kind) < CodeMemoryTable.size());
    return CodeMemoryTable[std::to_underlying(kind)];
}

/// Whether code memory of @p kind maps with MAP_JIT at all (its row's jit column).
/// @param kind The platform row's code memory.
/// @return Whether it does (both macOS rows).
[[nodiscard]] constexpr bool mapsJit(CodeMemoryKind kind) noexcept
{
    return codeMemoryRow(kind).jit == JitMappingUse::MapJit;
}

/// Whether code memory of @p kind protects MAP_JIT regions from writes per thread
/// (pthread_jit_write_protect_np), so that IJitMemory::setThreadAccess() means something (its row's
/// writeProtection column).
/// @param kind The platform row's code memory.
/// @return Whether it does (macOS arm64 only).
[[nodiscard]] constexpr bool togglesJitPerThread(CodeMemoryKind kind) noexcept
{
    return codeMemoryRow(kind).writeProtection == JitWriteProtection::PerThreadToggle;
}

/// What one JitMapping maps: one row of JitMappingTable.
struct JitMappingRow
{
    JitMapping mapping = JitMapping::ReadWrite; ///< The mapping.
    /// Whether its pages are executable as far as the page tables go: then they are writable and
    /// executable at once, unless the per-thread toggle gives each thread one of the two.
    PageAccess execute = PageAccess::Denied;
};

/// Every JitMapping, at its enumerator's index.
inline constexpr auto JitMappingTable = std::to_array<JitMappingRow>({
    { .mapping = JitMapping::ReadWrite, .execute = PageAccess::Denied },
    { .mapping = JitMapping::ReadWriteExecute, .execute = PageAccess::Allowed },
});

static_assert(meta::firstMisplacedRow<LastJitMapping>(JitMappingTable, &JitMappingRow::mapping)
                  == meta::NoMisplacedRow,
              "JitMappingTable has one row per enumerator, at its index");

/// Whether code memory of @p kind may map a MAP_JIT region as @p mapping: it maps with MAP_JIT, and an
/// executable mapping only where the per-thread toggle keeps every thread's view writable or executable,
/// never both.
/// @param kind The platform row's code memory.
/// @param mapping The mapping; must be an enumerator (checked).
/// @return Whether it may.
[[nodiscard]] constexpr bool allowsJitMapping(CodeMemoryKind kind, JitMapping mapping) noexcept
{
    Require(std::to_underlying(mapping) < JitMappingTable.size());
    auto const& row = JitMappingTable[std::to_underlying(mapping)];
    return mapsJit(kind) && (row.execute == PageAccess::Denied || togglesJitPerThread(kind));
}

/// The process's MAP_JIT memory (macOS): mapping, the calling thread's write protection, and releasing.
/// Reached only through this interface, so that a test can record the toggles or refuse a mapping.
class IJitMemory
{
  public:
    virtual ~IJitMemory() = default;

    /// Maps @p bytes with MAP_JIT, as @p mapping.
    /// @param bytes The size, a non-zero multiple of the page size (checked).
    /// @param mapping How; one this platform's row allows (checked: allowsJitMapping()).
    /// @return The region; or ReserveRefused with the system's code (0 when @p bytes does not fit the
    ///         system's size type).
    [[nodiscard]] virtual std::expected<Region, VirtualMemoryFailure> mapJit(uint64_t bytes,
                                                                             JitMapping mapping) noexcept = 0;

    /// Sets the calling thread's view of every MAP_JIT region: no other thread's changes. Only on a row
    /// that toggles per thread (checked: togglesJitPerThread()).
    /// @param access What the calling thread may do from now on.
    virtual void setThreadAccess(JitAccess access) noexcept = 0;

    /// Releases a region mapJit() returned, whole.
    /// @param region The region, page-aligned and not empty (checked).
    /// @return Nothing; or ReleaseRefused with the system's code.
    [[nodiscard]] virtual std::expected<void, VirtualMemoryFailure> release(Region region) noexcept = 0;
};

namespace detail
{

    /// Checks mapJit()'s precondition: code memory of @p kind may map as @p mapping.
    /// @param kind The platform row's code memory.
    /// @param mapping The mapping asked for.
    void requireJitMapping(CodeMemoryKind kind, JitMapping mapping) noexcept;

    /// Checks setThreadAccess()'s precondition: code memory of @p kind toggles per thread.
    /// @param kind The platform row's code memory.
    void requireJitThreadToggle(CodeMemoryKind kind) noexcept;

} // namespace detail

/// The operating system's MAP_JIT memory, by this build's platform row (the CMake-selected
/// JitMemory{MacOS,None}.cpp): on macOS mmap(MAP_JIT), pthread_jit_write_protect_np and munmap; on every
/// other platform there is no MAP_JIT, and every call fails its precondition.
class SystemJitMemory final: public IJitMemory
{
  public:
    /// Reads the page size from the system, once.
    SystemJitMemory() noexcept;

    /// The page size mapJit() and release() work in.
    /// @return It in bytes; 0 where there is no MAP_JIT.
    [[nodiscard]] uint64_t pageSize() const noexcept { return _pageSize; }

    /// @copydoc IJitMemory::mapJit
    [[nodiscard]] std::expected<Region, VirtualMemoryFailure> mapJit(uint64_t bytes,
                                                                     JitMapping mapping) noexcept override;

    /// @copydoc IJitMemory::setThreadAccess
    void setThreadAccess(JitAccess access) noexcept override;

    /// @copydoc IJitMemory::release
    [[nodiscard]] std::expected<void, VirtualMemoryFailure> release(Region region) noexcept override;

  private:
    uint64_t _pageSize = 0; ///< The system's page size; 0 where there is no MAP_JIT.
};

class JitWriteScope;

namespace testing
{
    struct JitWriteScopeKeys;
} // namespace testing

/// Grants the calling thread write access to every MAP_JIT region for its lifetime, and restores
/// Execute on every exit path, an exception's unwinding included. Only on a row that toggles per thread,
/// as setThreadAccess().
///
/// Scopes cannot nest: the inner one's end would restore Execute under the outer one, and no per-thread
/// state exists to count them (Isoline keeps none, thread_local included). So no caller can make one at
/// will: the constructor takes a Key, which only the platform code memory's seal (one scope around its
/// copy) and the test helpers (testing::JitWriteScopeKeys) can make.
///
/// A scope ends on the thread that opened it (checked): ended on another (a coroutine resumed elsewhere
/// across a co_await, say), it could restore only that thread's view and would leave its opener writable,
/// so the process ends instead.
class JitWriteScope
{
  public:
    /// The passkey of the constructor.
    class Key
    {
        Key() noexcept = default;
        // Task 3 adds the platform code memory's seal here, its one other maker.
        friend struct testing::JitWriteScopeKeys;
    };

    /// Sets the calling thread's access to Write, and records the thread.
    /// @param memory The JIT memory; must outlive the scope.
    /// @param key The passkey.
    JitWriteScope(IJitMemory& memory, Key key) noexcept;

    /// Sets the calling thread's access back to Execute; it must be the thread that opened the scope
    /// (checked).
    ~JitWriteScope();

    JitWriteScope(JitWriteScope const&) = delete;
    JitWriteScope(JitWriteScope&&) = delete;
    JitWriteScope& operator=(JitWriteScope const&) = delete;
    JitWriteScope& operator=(JitWriteScope&&) = delete;

  private:
    IJitMemory& _memory;    ///< The memory whose view it toggles.
    std::thread::id _owner; ///< The thread that opened it.
};

} // namespace isoline::platform
