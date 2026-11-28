// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <core/Assert.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <variant>
#include <vector>

#include <isoline/Features.hpp>
#include <isoline/binary/DecodeError.hpp>
#include <isoline/binary/Decoder.hpp>
#include <isoline/binary/Module.hpp>
#include <isoline/validate/ValidationError.hpp>
#include <isoline/validate/Validator.hpp>

namespace isoline::validate
{

/// Why a module did not load: its bytes did not decode, or the decoded module is not valid.
using ModuleError = std::variant<binary::DecodeError, ValidationError>;

/// A module that decoded and validated, with the features it did so under and the shape of each of
/// its functions' frames.
///
/// It is the proof that a module passed validation: only load() creates one, and it is immutable
/// thereafter, so it is shared as a std::shared_ptr<ValidatedModule const> between every instance of
/// it, on any thread.
class ValidatedModule
{
  public:
    /// The passkey of the constructor: only load() constructs one.
    class Key
    {
        Key() = default;
        friend class ValidatedModule;
    };

    /// Holds a module that load() decoded and validated; reached through std::make_shared in load().
    /// @param key The passkey only load() has.
    /// @param module The decoded module.
    /// @param features The features it was decoded and validated with.
    /// @param shape Its functions' frame shapes, as validateModule found them.
    ValidatedModule(Key key, binary::Module module, FeatureSet features, ModuleShape shape);

    /// Decodes and validates a module with the same features, which the result records.
    /// @param bytes The module's bytes, from the magic number on.
    /// @param features The proposals it is decoded and validated with.
    /// @param limits The implementation limits decoding enforces.
    /// @return The validated module, or the first decode or validation error.
    [[nodiscard]] static std::expected<std::shared_ptr<ValidatedModule const>, ModuleError> load(
        std::span<std::byte const> bytes, FeatureSet features, binary::DecodeLimits const& limits);

    /// load(), checking the bodies where @p order says (validateModule()).
    /// @param bytes The module's bytes, from the magic number on.
    /// @param features The proposals it is decoded and validated with.
    /// @param limits The implementation limits decoding enforces.
    /// @param order Where the bodies are checked among the module's rules.
    /// @return The validated module, or the first decode or validation error.
    [[nodiscard]] static std::expected<std::shared_ptr<ValidatedModule const>, ModuleError> load(
        std::span<std::byte const> bytes,
        FeatureSet features,
        binary::DecodeLimits const& limits,
        BodyOrder order);

    /// The decoded module.
    [[nodiscard]] binary::Module const& module() const noexcept { return _module; }

    /// The features it was decoded and validated with.
    [[nodiscard]] FeatureSet features() const noexcept { return _features; }

    /// The frame shape of one of its functions.
    /// @param codeIndex The function's index in the code section, imports not counted (checked).
    /// @return Its frame shape.
    [[nodiscard]] FrameShape frameShape(uint32_t codeIndex) const;

    /// The static stack cost of one of its functions' frames, computed once at load: params + locals +
    /// its frame shape's most operands + its deepest label nesting + 1, the + 1 for the frame itself,
    /// so that a frame holding nothing is not free. Every engine charges it at the function's entry
    /// (runtime::Store::chargeFrame()) against StoreLimits::maxStackValues, so the call that exhausts
    /// the stack is the same in every engine.
    /// @param codeIndex The function's index in the code section, imports not counted (checked).
    /// @return Its cost in stack values.
    [[nodiscard]] uint64_t frameCost(uint32_t codeIndex) const
    {
        Require(codeIndex < _frameCosts.size());
        return _frameCosts[codeIndex];
    }

    /// The module of @p validated, sharing ownership with it (the aliasing constructor).
    /// @param validated The validated module (checked: not null).
    /// @return Its module, alive as long as the result or any other owner of @p validated is.
    [[nodiscard]] static std::shared_ptr<binary::Module const> moduleOf(
        std::shared_ptr<ValidatedModule const> const& validated);

  private:
    binary::Module _module;
    FeatureSet _features;
    ModuleShape _shape;
    std::vector<uint64_t> _frameCosts; ///< frameCost() of each function, by code index.
};

} // namespace isoline::validate
