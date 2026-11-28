// SPDX-License-Identifier: Apache-2.0
//
// Hostile inputs read and written in time linear in their size: wall-clock ratios, each against a control,
// so that they hold on any host. A quadratic path is a ratio of GrowthScale squared, or a crafted input many
// times its control; the bounds sit far below either.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <format>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

#include <isoline/Features.hpp>
#include <isoline/binary/Decoder.hpp>
#include <isoline/text/ModuleWriter.hpp>
#include <isoline/text/TextError.hpp>
#include <isoline/text/TextLimits.hpp>
#include <isoline/text/TopLevel.hpp>
#include <isoline/text/testing/Growth.hpp>

using isoline::text::compileText;
using isoline::text::CoreForms;
using isoline::text::ModuleWriter;
using isoline::text::TextErrorCode;
using isoline::text::TextLimits;
using isoline::text::WriterOptions;
using isoline::text::testing::LinearGrowthBound;
using isoline::text::testing::ratioOf;

namespace
{

/// The most a crafted input may take relative to its control of the same size and shape.
constexpr auto CraftedBound = 2.0;

/// @p count copies of @p unit.
std::string repeated(std::string_view unit, std::size_t count)
{
    auto text = std::string {};
    text.reserve(unit.size() * count);
    for ([[maybe_unused]] auto const index: std::views::iota(std::size_t { 0 }, count))
        text += unit;
    return text;
}

/// Compiles @p source, which must fail with @p code.
void refuse(std::string const& source, TextErrorCode code)
{
    auto const compiled = compileText(source, TextLimits {}, CoreForms);
    REQUIRE_FALSE(compiled.has_value());
    CHECK(compiled.error().code == code);
}

/// libstdc++'s 64-bit std::hash<std::string> (_Hash_bytes, a MurmurHash2 variant) mixes each 8-byte
/// block as data(x) = shift(x * Multiplier) * Multiplier. Two names whose blocks are pairs (a, b) with
/// data(b) = data(a) ^ 2^63, each block written twice, hash alike whichever of each pair they take: the
/// two flips of a doubled block cancel. These are the reviewer's names2.py, ported.
constexpr auto Multiplier = uint64_t { 0xC6A4'A793'5BD1'E995 };

constexpr uint64_t shifted(uint64_t value) noexcept
{
    return value ^ (value >> 47U);
}

/// The inverse of Multiplier modulo 2^64, by Newton's iteration (each step doubles the correct bits).
constexpr uint64_t inverseMultiplier() noexcept
{
    auto inverse = Multiplier;
    for ([[maybe_unused]] auto const step: std::views::iota(0, 6))
        inverse *= 2 - (Multiplier * inverse);
    return inverse;
}
static_assert(Multiplier * inverseMultiplier() == 1);

constexpr uint64_t mixed(uint64_t block) noexcept
{
    return shifted(block * Multiplier) * Multiplier;
}

constexpr uint64_t unmixed(uint64_t mix) noexcept
{
    return shifted(mix * inverseMultiplier()) * inverseMultiplier();
}

/// The bytes of a block, little-endian, as _Hash_bytes loads them.
std::array<char, 8> bytesOf(uint64_t block)
{
    auto bytes = std::array<char, 8> {};
    for (auto const index: std::views::iota(std::size_t { 0 }, bytes.size()))
        bytes.at(index) = static_cast<char>((block >> (8U * index)) & 0xFFU);
    return bytes;
}

/// A fixed sequence of printable bytes (splitmix64), the same on every host and run: the inputs are
/// data, not samples.
class PrintableBytes
{
  public:
    /// Starts the sequence at @p state.
    explicit PrintableBytes(uint64_t state) noexcept: _state { state } {}

    /// The next byte, from 0x20 to 0x7E.
    char next() noexcept
    {
        _state += 0x9E37'79B9'7F4A'7C15ULL;
        auto mix = _state;
        mix = (mix ^ (mix >> 30U)) * 0xBF58'476D'1CE4'E5B9ULL;
        mix = (mix ^ (mix >> 27U)) * 0x94D0'49BB'1331'11EBULL;
        mix ^= mix >> 31U;
        return static_cast<char>(0x20 + (mix % 0x5F));
    }

  private:
    uint64_t _state; ///< The generator's state.
};

/// Which second block names() pairs each first block with.
enum class Pairing : uint8_t
{
    /// Its own, b with data(b) = data(a) ^ 2^63: the two flips of a doubled block cancel, so libstdc++
    /// hashes every name alike.
    Colliding,
    /// The next pair's, rotated: the same blocks, so the same bytes, lengths and escapes, and no two whose
    /// flips cancel. (Laying out one pair's a and b in either order is no control: one flip either way.)
    Rotated,
};

/// 2^@p pairs names, each 16 * @p pairs bytes of ASCII, from pairs of 8-byte blocks (a, b) with
/// data(b) = data(a) ^ 2^63, paired as @p pairing says: each name takes, for every pair, one of its two
/// blocks twice, its choice given by one bit of the name's ordinal. A b of idchars only is too rare to find
/// (none in millions of tries), so these names are no identifiers: the writer writes them as
/// `(@name "…")`, and its identifier scopes, ordered sets too, never hold them. The writing below is held
/// to its control all the same.
std::vector<std::string> names(std::size_t pairs, Pairing pairing)
{
    auto printable = PrintableBytes { 1 };
    auto firsts = std::vector<std::array<char, 8>> {};
    auto seconds = std::vector<std::array<char, 8>> {};
    while (firsts.size() < pairs)
    {
        auto first = uint64_t { 0 };
        for (auto const index: std::views::iota(0U, 8U))
            first |= static_cast<uint64_t>(static_cast<unsigned char>(printable.next())) << (8U * index);
        auto const second = unmixed(mixed(first) ^ (uint64_t { 1 } << 63U));
        if ((second & 0x8080'8080'8080'8080ULL) == 0)
        {
            firsts.push_back(bytesOf(first));
            seconds.push_back(bytesOf(second));
        }
    }
    if (pairing == Pairing::Rotated)
        std::ranges::rotate(seconds, seconds.begin() + 1);
    auto result = std::vector<std::string> {};
    for (auto const choice: std::views::iota(uint64_t { 0 }, uint64_t { 1 } << pairs))
    {
        auto name = std::string {};
        for (auto const index: std::views::iota(std::size_t { 0 }, pairs))
        {
            auto const& block = ((choice >> index) & 1U) == 0 ? firsts.at(index) : seconds.at(index);
            name.append(block.data(), block.size());
            name.append(block.data(), block.size());
        }
        result.push_back(std::move(name));
    }
    return result;
}

/// @p name as a quoted identifier, escaping what a string may not hold as itself.
std::string identifier(std::string_view name)
{
    auto text = std::string { "$\"" };
    for (auto const character: name)
    {
        auto const byte = static_cast<unsigned char>(character);
        if (byte < 0x20 || byte == 0x7F || character == '"' || character == '\\')
            text += std::format("\\{:02x}", byte);
        else
            text += character;
    }
    return text + '"';
}

/// A module that binds every name of @p names once in one index space, as @p shape lays it out.
struct NameShape
{
    std::string_view what;        ///< The index space.
    std::string_view open;        ///< What precedes every name.
    std::string_view close;       ///< What follows every name.
    std::string_view moduleStart; ///< What opens the module.
    std::string_view moduleEnd;   ///< What closes it.
    std::string_view trailer {};  ///< What follows every name's close, once all names are written.
};

constexpr auto NameShapes = std::to_array<NameShape>({
    { .what = "functions", .open = "(func ", .close = ")\n", .moduleStart = "(module\n", .moduleEnd = ")" },
    { .what = "locals",
      .open = "(local ",
      .close = " i32)\n",
      .moduleStart = "(module (func\n",
      .moduleEnd = "))" },
    // Nested, so that every label is in scope at once: block $a block $b … end end.
    { .what = "labels",
      .open = "block ",
      .close = "\n",
      .moduleStart = "(module (func\n",
      .moduleEnd = "))",
      .trailer = "end\n" },
});

std::string moduleOf(NameShape const& shape, std::vector<std::string> const& names)
{
    auto source = std::string { shape.moduleStart };
    for (auto const& name: names)
        source.append(shape.open).append(identifier(name)).append(shape.close);
    for ([[maybe_unused]] auto const& name: names)
        source.append(shape.trailer);
    return source.append(shape.moduleEnd);
}

} // namespace

TEST_CASE("LinearTime.deferredErrorsAreLocatedOnce", "[linear-time]")
{
    // A source can defer an error per field (every start field after the first, every import after a
    // definition); locating each from the source's start would be quadratic, so only the reported one is.
    constexpr auto Fields = std::size_t { 10'000 };
    struct Case
    {
        std::string_view what;
        std::string_view prefix;
        std::string_view field;
        TextErrorCode code;
    };
    for (auto const& entry: std::to_array<Case>({
             { .what = "start fields",
               .prefix = "(func)",
               .field = "(start 0) ",
               .code = TextErrorCode::MultipleStart },
             { .what = "imports after definitions",
               .prefix = "",
               .field = R"((func) (import "a" "b" (func)) )",
               .code = TextErrorCode::ImportAfterDefinition },
         }))
    {
        INFO(entry.what);
        auto const small = std::string { entry.prefix } + repeated(entry.field, Fields);
        auto const large = std::string { entry.prefix } + repeated(entry.field, Fields * 4);
        auto const ratio = ratioOf([&] { refuse(small, entry.code); }, [&] { refuse(large, entry.code); });
        CAPTURE(ratio);
        CHECK(ratio < LinearGrowthBound);
    }
}

TEST_CASE("LinearTime.craftedNamesCostWhatOthersDo", "[linear-time]")
{
    // 8'192 names that libstdc++'s string hash maps to one value cost reading and writing what as many
    // names of the same bytes do: identifiers are looked up in ordered maps, which a
    // source cannot collide. A hashed table made each lookup a comparison with every name before it.
    auto const crafted = names(13, Pairing::Colliding);
    auto const control = names(13, Pairing::Rotated);
    for (auto const& shape: NameShapes)
    {
        INFO(shape.what);
        auto const craftedSource = moduleOf(shape, crafted);
        auto const controlSource = moduleOf(shape, control);
        auto const compile = [](std::string const& source) {
            auto bytes = compileText(source, TextLimits {}, CoreForms);
            REQUIRE(bytes.has_value());
            return std::move(*bytes);
        };
        auto const readRatio =
            ratioOf([&] { (void) compile(controlSource); }, [&] { (void) compile(craftedSource); });
        CAPTURE(readRatio);
        CHECK(readRatio < CraftedBound);

        auto const craftedModule =
            isoline::binary::decodeModule(compile(craftedSource), isoline::FeatureSet::all());
        auto const controlModule =
            isoline::binary::decodeModule(compile(controlSource), isoline::FeatureSet::all());
        REQUIRE(craftedModule.has_value());
        REQUIRE(controlModule.has_value());
        auto const write = [](isoline::binary::Module const& module) {
            REQUIRE(ModuleWriter { WriterOptions {} }.write(module).has_value());
        };
        auto const writeRatio = ratioOf([&] { write(*controlModule); }, [&] { write(*craftedModule); });
        CAPTURE(writeRatio);
        CHECK(writeRatio < CraftedBound);
    }
}
