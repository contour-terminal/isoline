// SPDX-License-Identifier: Apache-2.0
#include <isoline/compiler/machine/CodeBuffer.hpp>

#include <core/Assert.hpp>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <ranges>
#include <utility>

namespace isoline::compiler::machine
{

namespace
{
    /// The magnitude of @p value, as unsigned (well-defined for the smallest int64_t too).
    [[nodiscard]] uint64_t magnitude(int64_t value) noexcept
    {
        return value < 0 ? uint64_t { 0 } - static_cast<uint64_t>(value) : static_cast<uint64_t>(value);
    }

    /// Lays @p displacement into the field of @p row at @p field, part by part, over whatever the field's
    /// bits held (Require: the bits below the row's lowest are zero).
    void patch(FixupKindRow const& row, int64_t displacement, std::span<uint8_t> field) noexcept
    {
        auto const bits = static_cast<uint64_t>(displacement);
        auto const alignment = (uint64_t { 1 } << lowestDisplacementBit(row)) - 1;
        Require((bits & alignment) == 0);
        auto word = uint64_t { 0 };
        for (auto const index: std::views::iota(std::size_t { 0 }, field.size()))
            word |= uint64_t { field[index] } << (8U * index);
        for (auto const& part: partsOf(row))
        {
            auto const mask = (uint64_t { 1 } << part.width) - 1;
            word &= ~(mask << part.bitOffset);
            word |= ((bits >> part.sourceShift) & mask) << part.bitOffset;
        }
        for (auto const index: std::views::iota(std::size_t { 0 }, field.size()))
            field[index] = static_cast<uint8_t>(word >> (8U * index));
    }
} // namespace

CodeBuffer::CodeBuffer(std::pmr::memory_resource& memory):
    _bytes { &memory }, _labels { &memory }, _fixups { &memory }
{
}

CodeLabel CodeBuffer::createLabel()
{
    Require(_labels.size() < std::numeric_limits<uint32_t>::max());
    auto const label = CodeLabel { static_cast<uint32_t>(_labels.size()) };
    _labels.push_back(Unbound);
    return label;
}

void CodeBuffer::bind(CodeLabel label)
{
    auto const index = std::to_underlying(label);
    Require(index < _labels.size() && _labels[index] == Unbound);
    _labels[index] = offset();
}

std::optional<uint64_t> CodeBuffer::labelOffset(CodeLabel label) const noexcept
{
    auto const index = std::to_underlying(label);
    Require(index < _labels.size());
    if (_labels[index] == Unbound)
        return std::nullopt;
    return _labels[index];
}

void CodeBuffer::emit(std::span<uint8_t const> bytes)
{
    _bytes.insert(_bytes.end(), bytes.begin(), bytes.end());
}

void CodeBuffer::emitLittleEndian(uint64_t value, uint32_t count)
{
    Require(count <= 8);
    for (auto const index: std::views::iota(uint32_t { 0 }, count))
        _bytes.push_back(static_cast<uint8_t>(value >> (8U * index)));
}

void CodeBuffer::emitFixup(FixupRequest const& request)
{
    auto const& row = fixupKindRow(request.kind);
    Require(request.anchor.has_value() == (row.anchor == Anchor::Label));
    Require(std::to_underlying(request.target) < _labels.size());
    Require(!request.anchor || std::to_underlying(*request.anchor) < _labels.size());
    _fixups.push_back(Fixup { .request = request, .site = offset() });
}

std::expected<void, CompileError> CodeBuffer::resolve()
{
    for (auto const& fixup: _fixups)
    {
        auto const& row = fixupKindRow(fixup.request.kind);
        auto const target = labelOffset(fixup.request.target);
        auto const anchor = fixup.request.anchor ? labelOffset(*fixup.request.anchor) : fixup.site;
        Require(target.has_value() && anchor.has_value());
        Require(fixup.site + fieldBytes(row) <= _bytes.size());
        // Offsets are below 2^63 (a buffer of that size cannot exist), so the difference cannot overflow; the
        // addend is a small constant of the encoder's, or a test's, and is checked against wrapping.
        auto const distance = static_cast<int64_t>(*target) - static_cast<int64_t>(*anchor);
        auto const addend = fixup.request.addend;
        Require(addend <= 0 || distance <= std::numeric_limits<int64_t>::max() - addend);
        Require(addend >= 0 || distance >= std::numeric_limits<int64_t>::min() - addend);
        auto const displacement = distance + addend;
        auto const minimum = minimumDisplacement(row);
        auto const maximum = maximumDisplacement(row);
        if (displacement < minimum || displacement > maximum)
            return std::unexpected { CompileError {
                .code = CompileErrorCode::BranchOutOfRange,
                .requested = magnitude(displacement),
                .limit = magnitude(displacement < 0 ? minimum : maximum),
            } };
        patch(row, displacement, std::span { _bytes }.subspan(fixup.site, fieldBytes(row)));
    }
    return {};
}

} // namespace isoline::compiler::machine
