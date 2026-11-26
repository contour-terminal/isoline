// SPDX-License-Identifier: Apache-2.0
#include <isoline/binary/BranchHints.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <ranges>
#include <span>
#include <utility>
#include <vector>

#include <isoline/binary/ByteReader.hpp>
#include <isoline/binary/DecodeError.hpp>
#include <isoline/binary/Module.hpp>
#include <isoline/binary/Types.hpp>

namespace isoline::binary
{

namespace
{
    /// What a hint byte means, by its value; any other byte is malformed.
    constexpr auto Likelihoods = std::to_array<BranchLikelihood>({
        BranchLikelihood::Unlikely,
        BranchLikelihood::Likely,
    });

    /// The size every branch hint item declares: that of its one hint byte.
    constexpr auto HintSize = uint32_t { 1 };

    using HintResult = std::expected<void, CustomSectionError>;

    /// A failure of the section's grammar, reported where the reader reported it.
    [[nodiscard]] CustomSectionError malformed(DecodeError const& error) noexcept
    {
        return CustomSectionError { CustomErrorCode::BranchHintMalformed, error.offset };
    }

    /// Reads a branch hint section's payload into hints, all or nothing.
    class BranchHintReader
    {
      public:
        BranchHintReader(Module const& module, std::span<std::byte const> payload, std::size_t payloadOffset):
            _module { module },
            _reader { payload, payloadOffset },
            _importedFunctions { module.importedCount(ExternKind::Func) }
        {
        }

        /// Reads the whole payload: a vector of functions' hints, and nothing after it.
        /// @return Every hint, in section order, or the first error.
        [[nodiscard]] std::expected<std::vector<BranchHint>, CustomSectionError> read()
        {
            auto const count = _reader.readLength();
            if (!count)
                return std::unexpected { malformed(count.error()) };
            auto previous = std::optional<uint32_t> {};
            for ([[maybe_unused]] auto const entry: std::views::iota(uint32_t { 0 }, *count))
                if (auto result = readFunction(previous); !result)
                    return std::unexpected { result.error() };
            if (!_reader.atEnd())
                return std::unexpected { CustomSectionError { CustomErrorCode::BranchHintMalformed,
                                                              _reader.offset() } };
            return std::move(_hints);
        }

      private:
        /// Reads one function's entry: its index, after @p previous, then its hints.
        [[nodiscard]] HintResult readFunction(std::optional<uint32_t>& previous)
        {
            auto const at = _reader.offset();
            auto const function = _reader.readU32();
            if (!function)
                return std::unexpected { malformed(function.error()) };
            if (previous && *function <= *previous)
                return std::unexpected { CustomSectionError { CustomErrorCode::BranchHintOutOfOrder, at } };
            previous = *function;
            // Only a defined function has code, and so branch sites.
            if (*function < _importedFunctions || *function - _importedFunctions >= _module.code.size())
                return std::unexpected { CustomSectionError { CustomErrorCode::BranchHintInvalidTarget,
                                                              at } };
            auto const& sites = _module.code[*function - _importedFunctions].branchSites;

            auto const count = _reader.readLength();
            if (!count)
                return std::unexpected { malformed(count.error()) };
            auto previousOffset = std::optional<uint32_t> {};
            for ([[maybe_unused]] auto const entry: std::views::iota(uint32_t { 0 }, *count))
                if (auto result = readHint(*function, sites, previousOffset); !result)
                    return result;
            return {};
        }

        /// Reads one hint of @p function, whose offset must follow @p previous and be one of @p sites.
        [[nodiscard]] HintResult readHint(uint32_t function,
                                          std::span<BranchSite const> sites,
                                          std::optional<uint32_t>& previous)
        {
            auto const at = _reader.offset();
            auto const offset = _reader.readU32();
            if (!offset)
                return std::unexpected { malformed(offset.error()) };
            if (previous && *offset <= *previous)
                return std::unexpected { CustomSectionError { CustomErrorCode::BranchHintOutOfOrder, at } };
            previous = *offset;

            auto const sizeAt = _reader.offset();
            auto const size = _reader.readU32();
            if (!size)
                return std::unexpected { malformed(size.error()) };
            if (*size != HintSize)
                return std::unexpected { CustomSectionError { CustomErrorCode::BranchHintMalformed,
                                                              sizeAt } };
            auto const hintAt = _reader.offset();
            auto const hint = _reader.readByte();
            if (!hint)
                return std::unexpected { malformed(hint.error()) };
            auto const value = std::to_integer<std::size_t>(*hint);
            if (value >= Likelihoods.size())
                return std::unexpected { CustomSectionError { CustomErrorCode::BranchHintMalformed,
                                                              hintAt } };

            // The sites are in body order, which is increasing offset order.
            auto const site = std::ranges::lower_bound(sites, *offset, {}, &BranchSite::offset);
            if (site == sites.end() || site->offset != *offset)
                return std::unexpected { CustomSectionError { CustomErrorCode::BranchHintInvalidTarget,
                                                              at } };
            _hints.push_back(BranchHint {
                .function = function, .instruction = site->instruction, .likelihood = Likelihoods[value] });
            return {};
        }

        Module const& _module;
        ByteReader _reader;
        std::size_t _importedFunctions;
        std::vector<BranchHint> _hints;
    };
} // namespace

void decodeBranchHints(Module& module, std::span<std::byte const> payload, std::size_t payloadOffset)
{
    auto hints = BranchHintReader { module, payload, payloadOffset }.read();
    if (!hints)
    {
        module.customErrors.push_back(hints.error());
        return;
    }
    module.branchHints.insert(module.branchHints.end(), hints->begin(), hints->end());
}

} // namespace isoline::binary
