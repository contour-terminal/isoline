// SPDX-License-Identifier: Apache-2.0
// PortableCodeMemory on Windows: pages from VirtualAlloc, sealed by VirtualProtect, the instruction cache
// flushed as Windows asks of every writer of code (a no-op on x86-64).

#include <cstddef>
#include <cstring>
#include <expected>
#include <memory>
#include <span>

#include <windows.h>

#include <isoline/execution/CodeMemory.hpp>

namespace isoline::execution
{

namespace
{
    /// Pages sealed read-execute, released when destroyed.
    class SealedPages final: public ISealedCode
    {
      public:
        SealedPages(void* base, std::size_t codeBytes) noexcept: _base { base }, _codeBytes { codeBytes } {}

        SealedPages(SealedPages const&) = delete;
        SealedPages(SealedPages&&) = delete;
        SealedPages& operator=(SealedPages const&) = delete;
        SealedPages& operator=(SealedPages&&) = delete;

        ~SealedPages() override { ::VirtualFree(_base, 0, MEM_RELEASE); }

        [[nodiscard]] std::span<std::byte const> code() const noexcept override
        {
            return { static_cast<std::byte const*>(_base), _codeBytes };
        }

      private:
        void* _base;
        std::size_t _codeBytes;
    };
} // namespace

std::expected<std::unique_ptr<ISealedCode>, CodeMemoryError> PortableCodeMemory::seal(
    std::span<std::byte const> bytes)
{
    auto system = SYSTEM_INFO {};
    ::GetSystemInfo(&system);
    auto const mappedBytes = mappedBytesOf(bytes.size(), system.dwPageSize);
    auto* const base = ::VirtualAlloc(nullptr, mappedBytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (base == nullptr)
        return std::unexpected { CodeMemoryError::MapRefused };
    if (!bytes.empty())
        std::memcpy(base, bytes.data(), bytes.size());
    // Owned before it is sealed, so that a refusal releases it too.
    auto sealed = std::make_unique<SealedPages>(base, bytes.size());
    auto previous = DWORD {};
    if (::VirtualProtect(base, mappedBytes, PAGE_EXECUTE_READ, &previous) == 0)
        return std::unexpected { CodeMemoryError::ProtectRefused };
    ::FlushInstructionCache(::GetCurrentProcess(), base, mappedBytes);
    return sealed;
}

} // namespace isoline::execution
