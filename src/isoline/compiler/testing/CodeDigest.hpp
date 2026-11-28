// SPDX-License-Identifier: Apache-2.0
#pragma once

// The SHA-256 digest of a CompiledCode's bytes and metadata, for the determinism checks: equal digests within
// a process (Compiler.isDeterministic) and across two (tests/determinism). Test support, linked into no
// shipped binary.

#include <core/Sha256.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include <isoline/compiler/CompiledCode.hpp>

namespace isoline::compiler::testing
{

/// The digest of @p code: its bytes, then every metadata vector, each prefixed by its size, every word in
/// little endian order, so that the digest is the same on every host.
[[nodiscard]] inline core::Sha256::Digest digestOf(CompiledCode const& code)
{
    auto hasher = core::Sha256 {};
    auto const word = [&](uint64_t value) {
        auto bytes = std::array<std::byte, sizeof(uint64_t)> {};
        for (auto& byte: bytes)
        {
            byte = static_cast<std::byte>(value & 0xffU);
            value >>= 8U;
        }
        hasher.update(bytes);
    };
    auto const words = [&](auto const& values) {
        word(values.size());
        for (auto const value: values)
            word(value);
    };
    word(code.code.size());
    hasher.update(code.code);
    words(code.functionOffsets);
    words(code.adapterOffsets);
    word(code.trampolineOffset);
    words(code.frameCosts);
    words(code.hostScratchValues);
    words(code.adapterAreaBytes);
    return hasher.finish();
}

/// @p digest in lower-case hexadecimal.
[[nodiscard]] inline std::string hexOf(core::Sha256::Digest const& digest)
{
    constexpr auto Digits = std::string_view { "0123456789abcdef" };
    auto text = std::string {};
    for (auto const byte: digest)
    {
        text.push_back(Digits[std::to_integer<std::size_t>(byte) >> 4U]);
        text.push_back(Digits[std::to_integer<std::size_t>(byte) & 0xfU]);
    }
    return text;
}

} // namespace isoline::compiler::testing
