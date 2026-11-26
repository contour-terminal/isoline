// SPDX-License-Identifier: Apache-2.0
#include <isoline/validate/ValidatedModule.hpp>

#include <core/Assert.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <utility>

#include <isoline/Features.hpp>
#include <isoline/binary/DecodeError.hpp>
#include <isoline/binary/Decoder.hpp>
#include <isoline/binary/Module.hpp>
#include <isoline/validate/ValidationError.hpp>
#include <isoline/validate/Validator.hpp>

namespace isoline::validate
{

ValidatedModule::ValidatedModule(Key /*key*/, binary::Module module, FeatureSet features, ModuleShape shape):
    _module { std::move(module) }, _features { features }, _shape { std::move(shape) }
{
}

std::expected<std::shared_ptr<ValidatedModule const>, ModuleError> ValidatedModule::load(
    std::span<std::byte const> bytes, FeatureSet features, binary::DecodeLimits const& limits)
{
    using Result = std::expected<std::shared_ptr<ValidatedModule const>, ModuleError>;
    return binary::decodeModule(bytes, features, limits)
        .transform_error([](binary::DecodeError const& error) { return ModuleError { error }; })
        .and_then([features](binary::Module module) -> Result {
            auto shape = validateModule(module, features);
            if (!shape)
                return std::unexpected { ModuleError { std::move(shape.error()) } };
            return std::make_shared<ValidatedModule const>(
                Key {}, std::move(module), features, std::move(*shape));
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
