// SPDX-License-Identifier: Apache-2.0
#include <isoline/component/text/Forms.hpp>

#include <cstddef>
#include <expected>
#include <span>
#include <string>
#include <vector>

#include <isoline/Features.hpp>
#include <isoline/binary/DecodeError.hpp>
#include <isoline/binary/Encoder.hpp>
#include <isoline/component/Limits.hpp>
#include <isoline/component/binary/Encoder.hpp>
#include <isoline/component/text/ComponentReader.hpp>
#include <isoline/text/ModuleReader.hpp>

namespace isoline::component::text
{

std::expected<std::vector<std::byte>, isoline::text::TextError> compileComponent(
    isoline::text::SExpressionTree const& tree,
    isoline::text::NodeId form,
    isoline::text::TextLimits const& limits)
{
    auto const tooLarge =
        isoline::text::TextError { isoline::text::TextErrorCode::OutputTooLarge, tree.positionOf(form) };
    // A binary form is its bytes, as wasm-tools compiles it: the decoder reports what is wrong with them.
    if (auto const keyword = binaryKeywordOf(tree, form))
        return binaryFormBytes(tree, form, *keyword, limits.maxModuleBytes);
    auto const reader = ComponentReader { limits,
                                          ComponentLimits { .maxComponentBytes = limits.maxModuleBytes },
                                          everyComponentFeature() };
    return reader.read(tree, form)
        .and_then([&](binary::ComponentPool const& pool)
                      -> std::expected<std::vector<std::byte>, isoline::text::TextError> {
            auto bytes = binary::encodeComponent(pool);
            if (bytes.size() > limits.maxModuleBytes)
                return std::unexpected { tooLarge };
            return bytes;
        });
}

std::expected<std::vector<std::byte>, std::string> P10CoreModules::canonicalize(
    std::span<std::byte const> module)
{
    return isoline::binary::canonicalize(module, FeatureSet::all()).transform_error([](auto const& error) {
        return isoline::binary::specMessage(error);
    });
}

} // namespace isoline::component::text
