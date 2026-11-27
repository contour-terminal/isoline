// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>
#include <variant>

#include <isoline/c/error.h>
#include <isoline/runtime/Limits.hpp>
#include <isoline/runtime/Trap.hpp>

namespace isoline::c::detail
{

/// Who owns an error.
enum class ErrorLifetime : uint8_t
{
    Owned,    ///< Its receiver, who deletes it once.
    Immortal, ///< Nobody: a static the library hands out where it cannot allocate one, never deleted.
};

/// Deletes an owned error, its causes with it, and leaves an immortal one alone.
struct ErrorDeleter
{
    /// @param error The error to delete; null, or immortal, is nothing to do.
    void operator()(isoline_error_t* error) const noexcept;
};

/// An error and the ownership of it: a cause in a chain, or an error being built.
using ErrorPtr = std::unique_ptr<isoline_error_t, ErrorDeleter>;

/// The index of the import an error is reported at, in the module's import section.
struct ImportIndex
{
    uint32_t index = 0; ///< The import's index.

    /// Equal when they name the same import.
    bool operator==(ImportIndex const&) const = default;
};

/// What an error carries beyond its code, detail and message, for the getters that read it
/// (isoline_error_limit, isoline_error_refusal, isoline_error_import_index, isoline_error_trap): the limit
/// exceeded, the refusal of an allocation or a growth, the import it is reported at, or the trap a call
/// ended with; or nothing.
using ErrorDetail =
    std::variant<std::monostate, runtime::Limit, runtime::Refusal, ImportIndex, runtime::Trap>;

/// The most bytes an error's message holds, its terminator not counted.
inline constexpr auto MaxMessageBytes = std::size_t { 4096 };

/// The most errors a chain holds, its first included: what isoline_error_cause() follows ends there.
inline constexpr auto MaxCauseDepth = std::size_t { 8 };

/// The longest prefix of @p text of at most @p maxBytes bytes that does not end inside a UTF-8 sequence:
/// a cut before a continuation byte moves back to the start of its sequence, by at most three bytes.
/// @param text UTF-8 text; bytes that are not valid UTF-8 are cut as they fall.
/// @param maxBytes The most bytes the prefix may hold.
/// @return The prefix.
[[nodiscard]] auto utf8Prefix(std::string_view text, std::size_t maxBytes) noexcept -> std::string_view;

/// The one immortal error of code ISOLINE_ERROR_HOST_MEMORY_EXHAUSTED: what every function that would
/// return an error returns when it cannot allocate one. It is constant-initialized, so handing it out
/// needs no memory, and isoline_error_delete() leaves it alone.
/// @return The error; never null.
[[nodiscard]] auto hostMemoryExhaustedError() noexcept -> isoline_error_t*;

/// Makes an error.
/// @param code Its code; any value (isoline_error_new() checks the embedder's).
/// @param detail Its detail.
/// @param message Its message, cut to MaxMessageBytes at a UTF-8 boundary.
/// @param extra What it carries beyond those.
/// @return The error, owned by the caller; out of host memory, hostMemoryExhaustedError(). Never null.
[[nodiscard]] auto newError(isoline_error_code_t code,
                            uint32_t detail,
                            std::string_view message,
                            ErrorDetail const& extra = {}) noexcept -> isoline_error_t*;

/// Gives @p error the cause @p cause: @p cause becomes the last link of @p error's chain, and a chain
/// longer than MaxCauseDepth loses the links past it, the deepest first, so that what the caller is told
/// first stays. An immortal error ends every chain it is in, as the head or as a link: it is never
/// written, so nothing is appended after it, and @p cause is deleted instead.
/// @param error The error, owned by the caller.
/// @param cause The cause, whose ownership passes to @p error; null is none.
/// @return @p error.
[[nodiscard]] auto withCause(isoline_error_t* error, isoline_error_t* cause) noexcept -> isoline_error_t*;

} // namespace isoline::c::detail

/// An error of the C API (error.h): immutable once it is handed out, so any thread may read it.
struct isoline_error_t
{
    isoline_error_code_t code = 0; ///< Its code.
    uint32_t detail = 0;           ///< Its detail: the enumerator's number within its code's family.
    /// Its message: storage's bytes, or a string literal's, either NUL-terminated after them.
    std::string_view message {};
    std::unique_ptr<char[]> storage {};       ///< The message's bytes and terminator, when it owns them.
    isoline::c::detail::ErrorPtr cause {};    ///< The error it arose from, or null.
    isoline::c::detail::ErrorDetail extra {}; ///< What it carries beyond code, detail and message.
    isoline::c::detail::ErrorLifetime lifetime = isoline::c::detail::ErrorLifetime::Owned; ///< Who owns it.
};
