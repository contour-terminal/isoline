// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <ranges>
#include <span>
#include <type_traits>

#include <isoline/interpreter/LittleEndian.hpp>

using isoline::interpreter::loadLittleEndian;
using isoline::interpreter::storeLittleEndian;

namespace
{

/// The byte order that is not the host's: what a host of the other order sees, simulated here.
constexpr auto OtherOrder =
    std::endian::native == std::endian::little ? std::endian::big : std::endian::little;

/// The bytes of @p value lowest first, composed by shifts alone, so on every host alike.
template <typename T>
[[nodiscard]] constexpr std::array<std::byte, sizeof(T)> littleEndianBytes(T value) noexcept
{
    auto const bits = static_cast<std::make_unsigned_t<T>>(value);
    auto bytes = std::array<std::byte, sizeof(T)> {};
    for (auto const index: std::views::iota(std::size_t { 0 }, sizeof(T)))
        bytes[index] = static_cast<std::byte>(static_cast<uint64_t>(bits) >> (8U * index));
    return bytes;
}

/// The bytes of @p value highest first, composed by shifts alone.
template <typename T>
[[nodiscard]] constexpr std::array<std::byte, sizeof(T)> bigEndianBytes(T value) noexcept
{
    auto bytes = littleEndianBytes(value);
    std::ranges::reverse(bytes);
    return bytes;
}

/// A value whose every byte differs, so a byte out of place shows.
template <typename T>
[[nodiscard]] constexpr T distinctBytes() noexcept
{
    auto bits = uint64_t { 0 };
    for (auto const index: std::views::iota(std::size_t { 0 }, sizeof(T)))
        bits |= uint64_t { 0x81 + index } << (8U * index);
    return static_cast<T>(static_cast<std::make_unsigned_t<T>>(bits));
}

} // namespace

TEST_CASE("LittleEndian.bigEndianLayout")
{
    // On a big-endian host, the big template writes Wasm's little-endian bytes. On a little-endian host
    // the same template does what a big-endian host would do with the host's own order, so it must
    // write the value highest byte first: both branches run, and are checked, on every host.
    constexpr auto WasmBytes =
        std::array { std::byte { 0x44 }, std::byte { 0x33 }, std::byte { 0x22 }, std::byte { 0x11 } };
    constexpr auto SwappedBytes =
        std::array { std::byte { 0x11 }, std::byte { 0x22 }, std::byte { 0x33 }, std::byte { 0x44 } };
    auto bytes = std::array<std::byte, 8> {};
    storeLittleEndian<uint32_t, std::endian::big>(bytes.data(), 0x1122'3344);
    CHECK(std::ranges::equal(std::span { bytes }.first<4>(),
                             std::endian::native == std::endian::big ? WasmBytes : SwappedBytes));
    CHECK(
        std::ranges::all_of(std::span { bytes }.subspan<4>(), [](std::byte b) { return b == std::byte {}; }));
    CHECK((loadLittleEndian<uint32_t, std::endian::big>(bytes.data()) == 0x1122'3344));

    // The little template mirrors it.
    storeLittleEndian<uint32_t, std::endian::little>(bytes.data(), 0x1122'3344);
    CHECK(std::ranges::equal(std::span { bytes }.first<4>(),
                             std::endian::native == std::endian::little ? WasmBytes : SwappedBytes));
    CHECK((loadLittleEndian<uint32_t, std::endian::little>(bytes.data()) == 0x1122'3344));
}

TEMPLATE_TEST_CASE("LittleEndian.nativeOrderMatchesByteWiseComposition",
                   "",
                   uint8_t,
                   uint16_t,
                   uint32_t,
                   uint64_t,
                   int8_t,
                   int16_t,
                   int32_t,
                   int64_t)
{
    auto const value = distinctBytes<TestType>();

    // The host's own order: Wasm's little-endian bytes, whatever the host is.
    auto bytes = std::array<std::byte, sizeof(TestType)> {};
    storeLittleEndian<TestType, std::endian::native>(bytes.data(), value);
    CHECK(bytes == littleEndianBytes(value));
    auto const composed = littleEndianBytes(value);
    CHECK(loadLittleEndian<TestType, std::endian::native>(composed.data()) == value);
    CHECK(loadLittleEndian<TestType>(composed.data()) == value);

    // The other order, as a host of it would see the host's memory: the bytes reversed.
    storeLittleEndian<TestType, OtherOrder>(bytes.data(), value);
    CHECK(bytes == bigEndianBytes(value));
    auto const reversed = bigEndianBytes(value);
    CHECK(loadLittleEndian<TestType, OtherOrder>(reversed.data()) == value);

    // An unaligned address works alike.
    auto unaligned = std::array<std::byte, sizeof(TestType) + 1> {};
    storeLittleEndian<TestType>(unaligned.data() + 1, value);
    CHECK(std::ranges::equal(std::span { unaligned }.subspan(1), littleEndianBytes(value)));
    CHECK(loadLittleEndian<TestType>(unaligned.data() + 1) == value);
}

/// Whether a store can deduce its type from a value of type V, which a promoted argument would widen.
template <typename V>
concept StoreDeducesItsType = requires(std::byte* address, V value) { storeLittleEndian(address, value); };

TEST_CASE("LittleEndian.storeWritesExactlyItsWidth")
{
    // Every store names its type: none deduces it from the value.
    static_assert(!StoreDeducesItsType<int>);
    static_assert(!StoreDeducesItsType<uint16_t>);
    CHECK_FALSE(StoreDeducesItsType<int>);

    // A promoted argument (u16 + 1 is an int) converts to the named type: two bytes are written, not
    // four, and the guard bytes around them stay as they were.
    constexpr auto Guard = std::byte { 0xA5 };
    auto bytes = std::array<std::byte, 6> {};
    std::ranges::fill(bytes, Guard);
    auto const value = uint16_t { 0x1233 };
    storeLittleEndian<uint16_t>(bytes.data() + 2, value + 1);
    CHECK(bytes == std::array { Guard, Guard, std::byte { 0x34 }, std::byte { 0x12 }, Guard, Guard });
}
