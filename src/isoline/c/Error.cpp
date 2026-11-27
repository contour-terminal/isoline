// SPDX-License-Identifier: Apache-2.0
#include <isoline/c/error.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <new>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>

#include <isoline/c/detail/Boundary.hpp>
#include <isoline/c/detail/Error.hpp>
#include <isoline/c/detail/ErrorRows.hpp>

namespace isoline::c::detail
{

namespace
{
    /// Holds the immortal error, and never destroys it: a union's member is destroyed only by its own
    /// destructor, which destroys nothing. So the error stays valid through the process's exit, whatever
    /// another static's destructor, or another thread, still does with it. Constant-initialized, so it exists
    /// before any code runs and handing it out allocates nothing; no isoline_error_t is ever written once it
    /// is handed out, this one included.
    union ImmortalError {
        isoline_error_t error;

        constexpr ImmortalError() noexcept:
            error {
                .code = ISOLINE_ERROR_HOST_MEMORY_EXHAUSTED,
                .message = "out of host memory",
                .lifetime = ErrorLifetime::Immortal,
            }
        { // uncovered(immortal-error-lifetime)
        } // uncovered(immortal-error-lifetime)

        ImmortalError(ImmortalError const&) = delete;
        ImmortalError(ImmortalError&&) = delete;
        auto operator=(ImmortalError const&) -> ImmortalError& = delete;
        auto operator=(ImmortalError&&) -> ImmortalError& = delete;

        // Destroys nothing: the error outlives every use of it.
        constexpr ~ImmortalError() {} // uncovered(immortal-error-lifetime)
    };

    constinit ImmortalError hostMemoryExhausted {};

    /// Whether @p byte continues a UTF-8 sequence (10xxxxxx) rather than starting one.
    [[nodiscard]] constexpr auto isContinuationByte(char byte) noexcept -> bool
    {
        return (static_cast<unsigned char>(byte) & 0xC0U) == 0x80U;
    }

    /// The longest a UTF-8 sequence's continuation runs: three bytes after its lead byte.
    constexpr auto MaxContinuationBytes = 3;

    /// An error of code ISOLINE_ERROR_INVALID_ARGUMENT whose message is @p format applied to @p arguments,
    /// formatted into a buffer of its own rather than a std::string: out of host memory, only the error
    /// itself fails to allocate (MSVC's debug std::string allocates in a noexcept constructor, where a
    /// refusal ends the process).
    template <typename... Arguments>
    [[nodiscard]] auto invalidArgument(std::format_string<Arguments...> format, Arguments&&... arguments)
        -> isoline_error_t*
    {
        auto buffer = std::array<char, 128> {};
        auto const written =
            std::format_to_n(buffer.data(), buffer.size(), format, std::forward<Arguments>(arguments)...);
        auto const length = std::min(static_cast<std::size_t>(written.size), buffer.size());
        return newError(ISOLINE_ERROR_INVALID_ARGUMENT, 0, std::string_view { buffer.data(), length });
    }
} // namespace

void ErrorDeleter::operator()(isoline_error_t* error) const noexcept
{
    if (error != nullptr && error->lifetime == ErrorLifetime::Owned)
        delete error;
}

auto utf8Prefix(std::string_view text, std::size_t maxBytes) noexcept -> std::string_view
{
    if (text.size() <= maxBytes)
        return text;
    auto cut = maxBytes;
    for ([[maybe_unused]] auto const step: std::views::iota(0, MaxContinuationBytes))
    {
        if (cut == 0 || !isContinuationByte(text[cut]))
            break;
        --cut;
    }
    return text.substr(0, cut);
}

auto hostMemoryExhaustedError() noexcept -> isoline_error_t*
{
    return &hostMemoryExhausted.error;
}

auto newError(isoline_error_code_t code,
              uint32_t detail,
              std::string_view message,
              ErrorDetail const& extra) noexcept -> isoline_error_t*
{
    return boundary(
        [&] {
            auto const text = utf8Prefix(message, MaxMessageBytes);
            auto error = ErrorPtr { std::make_unique<isoline_error_t>().release() };
            error->storage = std::make_unique_for_overwrite<char[]>(text.size() + 1);
            std::ranges::copy(text, error->storage.get());
            error->storage[text.size()] = '\0';
            error->code = code;
            error->detail = detail;
            error->message = std::string_view { error->storage.get(), text.size() };
            error->extra = extra;
            return error.release();
        },
        [] { return hostMemoryExhaustedError(); });
}

auto withCause(isoline_error_t* error, isoline_error_t* cause) noexcept -> isoline_error_t*
{
    auto owned = ErrorPtr { cause };
    if (!owned)
        return error;
    // An immortal error ends every chain it is in: it is never written, so nothing is appended after it.
    auto* last = error;
    while (last->lifetime == ErrorLifetime::Owned && last->cause)
        last = last->cause.get();
    if (last->lifetime == ErrorLifetime::Immortal)
        return error;
    last->cause = std::move(owned);

    // The links past the eighth go, the deepest with them. The eighth is the new cause or an owned link
    // before it: an immortal link ends the chain, so it is never the eighth of a longer one.
    auto* link = error;
    for ([[maybe_unused]] auto const depth: std::views::iota(std::size_t { 1 }, MaxCauseDepth))
    {
        if (!link->cause)
            return error;
        link = link->cause.get();
    }
    if (link->lifetime == ErrorLifetime::Owned)
        link->cause.reset();
    return error;
}

} // namespace isoline::c::detail

using isoline::c::detail::boundary;
using isoline::c::detail::ErrorDeleter;
using isoline::c::detail::hostMemoryExhaustedError;
using isoline::c::detail::invalidArgument;
using isoline::c::detail::isKnownCode;
using isoline::c::detail::MaxMessageBytes;
using isoline::c::detail::newError;

isoline_error_t* ISOLINE_CALL isoline_error_new(isoline_error_code_t code,
                                                char const* message,
                                                size_t length) noexcept
{
    return boundary(
        [&] {
            if (message == nullptr && length != 0)
                return invalidArgument("isoline_error_new: the message is NULL, its length {}", length);
            if (!isKnownCode(code))
                return invalidArgument("isoline_error_new: {} is no error code", code);
            // No byte past the one after the cut is read, however long the length says the message is, and no
            // view longer than that is made of it.
            auto const read = std::min(length, MaxMessageBytes + 1);
            return newError(
                code, 0, message == nullptr ? std::string_view {} : std::string_view { message, read });
        },
        [] { return hostMemoryExhaustedError(); });
}

void ISOLINE_CALL isoline_error_delete(isoline_error_t* error) noexcept
{
    ErrorDeleter {}(error);
}

isoline_error_code_t ISOLINE_CALL isoline_error_code(isoline_error_t const* error) noexcept
{
    return error == nullptr ? 0 : error->code;
}

uint32_t ISOLINE_CALL isoline_error_detail(isoline_error_t const* error) noexcept
{
    return error == nullptr ? 0 : error->detail;
}

char const* ISOLINE_CALL isoline_error_code_name(isoline_error_code_t code) noexcept
{
    return isoline::c::detail::errorCodeName(code);
}

char const* ISOLINE_CALL isoline_error_message(isoline_error_t const* error) noexcept
{
    return error == nullptr ? "" : error->message.data();
}

size_t ISOLINE_CALL isoline_error_message_length(isoline_error_t const* error) noexcept
{
    return error == nullptr ? 0 : error->message.size();
}

isoline_error_t const* ISOLINE_CALL isoline_error_cause(isoline_error_t const* error) noexcept
{
    return error == nullptr ? nullptr : error->cause.get();
}
