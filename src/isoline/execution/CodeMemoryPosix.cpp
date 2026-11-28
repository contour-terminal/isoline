// SPDX-License-Identifier: Apache-2.0
// PortableCodeMemory on POSIX systems: anonymous pages from mmap, sealed by mprotect.

#include <sys/mman.h>

#include <cstddef>
#include <cstring>
#include <expected>
#include <memory>
#include <span>

#include <unistd.h>

#include <isoline/execution/CodeMemory.hpp>

namespace isoline::execution
{

namespace
{
    /// Pages sealed read-execute, unmapped when destroyed.
    class SealedPages final: public ISealedCode
    {
      public:
        SealedPages(void* base, std::size_t mappedBytes, std::size_t codeBytes) noexcept:
            _base { base }, _mappedBytes { mappedBytes }, _codeBytes { codeBytes }
        {
        }

        SealedPages(SealedPages const&) = delete;
        SealedPages(SealedPages&&) = delete;
        SealedPages& operator=(SealedPages const&) = delete;
        SealedPages& operator=(SealedPages&&) = delete;

        ~SealedPages() override { ::munmap(_base, _mappedBytes); }

        [[nodiscard]] std::span<std::byte const> code() const noexcept override
        {
            return { static_cast<std::byte const*>(_base), _codeBytes };
        }

      private:
        void* _base;
        std::size_t _mappedBytes;
        std::size_t _codeBytes;
    };
} // namespace

std::expected<std::unique_ptr<ISealedCode>, CodeMemoryError> PortableCodeMemory::seal(
    std::span<std::byte const> bytes)
{
    auto const pageBytes = static_cast<std::size_t>(::sysconf(_SC_PAGESIZE));
    auto const mappedBytes = mappedBytesOf(bytes.size(), pageBytes);
    auto* const base =
        ::mmap(nullptr, mappedBytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (base == MAP_FAILED)
        return std::unexpected { CodeMemoryError::MapRefused }; // uncovered(address-space-refused)
    if (!bytes.empty())
        std::memcpy(base, bytes.data(), bytes.size());
    // Owned before it is sealed, so that a refusal unmaps it too.
    auto sealed = std::make_unique<SealedPages>(base, mappedBytes, bytes.size());
    if (::mprotect(base, mappedBytes, PROT_READ | PROT_EXEC) != 0)
        return std::unexpected { CodeMemoryError::ProtectRefused }; // uncovered(execute-refused)
    return sealed;
}

} // namespace isoline::execution
