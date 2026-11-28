// SPDX-License-Identifier: Apache-2.0
#include <isoline/validate/ValidatedModule.hpp>

#include <core/Assert.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <ranges>
#include <span>
#include <utility>
#include <vector>

#include <isoline/Features.hpp>
#include <isoline/binary/DecodeError.hpp>
#include <isoline/binary/Decoder.hpp>
#include <isoline/binary/Module.hpp>
#include <isoline/validate/ValidationError.hpp>
#include <isoline/validate/Validator.hpp>

namespace isoline::validate
{

namespace
{
    /// The static stack cost of each of @p module's functions (ValidatedModule::frameCost()).
    /// @param module The module.
    /// @param shape Its functions' frame shapes, one per code section entry (checked).
    /// @return The costs, by code index.
    [[nodiscard]] std::vector<uint64_t> frameCostsOf(binary::Module const& module, ModuleShape const& shape)
    {
        Require(shape.functions.size() == module.code.size());
        auto costs = std::vector<uint64_t> {};
        costs.reserve(module.code.size());
        for (auto const index: std::views::iota(std::size_t { 0 }, module.code.size()))
        {
            auto const& function = module.code[index];
            auto const& frame = shape.functions[index];
            costs.push_back(uint64_t { module.types.at(function.typeIndex).params.size() }
                            + function.localCount + frame.maxOperands + frame.maxLabels + 1);
        }
        return costs;
    }
} // namespace

ValidatedModule::ValidatedModule(
    Key /*key*/, binary::Module module, FeatureSet features, ModuleShape shape, uint64_t sourceBytes):
    _module { std::move(module) },
    _features { features },
    _shape { std::move(shape) },
    _frameCosts { frameCostsOf(_module, _shape) },
    _sourceBytes { sourceBytes }
{
}

std::expected<std::shared_ptr<ValidatedModule const>, ModuleError> ValidatedModule::load(
    std::span<std::byte const> bytes, FeatureSet features, binary::DecodeLimits const& limits)
{
    using Result = std::expected<std::shared_ptr<ValidatedModule const>, ModuleError>;
    return binary::decodeModule(bytes, features, limits)
        .transform_error([](binary::DecodeError const& error) { return ModuleError { error }; })
        .and_then([features, &limits, sourceBytes = bytes.size()](binary::Module module) -> Result {
            auto shape = validateModule(module, features, limits);
            if (!shape)
                return std::unexpected { ModuleError { std::move(shape.error()) } };
            return std::make_shared<ValidatedModule const>(
                Key {}, std::move(module), features, std::move(*shape), sourceBytes);
        });
}

FrameShape ValidatedModule::frameShape(uint32_t codeIndex) const
{
    Require(codeIndex < _shape.functions.size());
    return _shape.functions[codeIndex];
}

std::shared_ptr<binary::Module const> ValidatedModule::moduleOf(
    std::shared_ptr<ValidatedModule const> const& validated)
{
    Require(validated != nullptr);
    return { validated, &validated->_module };
}

} // namespace isoline::validate
