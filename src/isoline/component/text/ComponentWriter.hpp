// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <expected>
#include <string>
#include <variant>

#include <isoline/Features.hpp>
#include <isoline/binary/DecodeError.hpp>
#include <isoline/component/binary/Component.hpp>
#include <isoline/text/ModuleWriter.hpp>

namespace isoline::component::text
{

/// An embedded core module that does not decode, which ComponentWriter cannot write as text.
struct CoreModuleMalformed
{
    uint32_t ordinal = 0;               ///< The module's ordinal, pre-order over nested components.
    isoline::binary::DecodeError error; ///< The core decoder's error, at its offset in the module.
};

/// Why ComponentWriter refused a component: its text would exceed WriterOptions::maxTextBytes, or a
/// core module's ModuleWriter refused it, or a core module does not decode.
using ComponentWriteError = std::variant<isoline::text::WriteError, CoreModuleMalformed>;

/// Writes components in the text format, the inverse of ComponentReader.
///
/// Every index is printed numerically, each definition that adds an index followed by its `(;n;)`
/// comment, and every type use as an explicit `(type n)`, so that reading the text back never
/// re-derives a type or names one by an identifier; core modules are nested through P10's
/// isoline::text::ModuleWriter, decoded under the writer's core features. Custom sections,
/// `component-name` among them, are written `(@custom "name" "…")` where they stand, and so is the name
/// section of a core module that names itself (isoline::text::NameSectionForm::Custom), as an
/// identifier on `(core module …)` would name the module in the component's name section too. So the
/// text reads back to the component it was given: binary::encodeComponent() of ComponentReader's pool
/// equals binary::canonicalizeComponent() of the input with P10CoreModules.
///
/// Writing walks nested components and component and instance types from an explicit stack, never by
/// recursion, in time and memory linear in the component and its text.
class ComponentWriter
{
  public:
    /// A writer of the given layout and output limit.
    /// @param options How to lay out the text, and the most it may be.
    /// @param coreFeatures The proposals the embedded core modules are decoded under.
    ComponentWriter(isoline::text::WriterOptions options, FeatureSet coreFeatures) noexcept:
        _options { options }, _coreFeatures { coreFeatures }
    {
    }

    /// Writes a component: any pool binary::decodeComponent() produced, valid or not.
    /// @param pool The component.
    /// @return Its text, a `(component …)` form, or why it cannot be written.
    [[nodiscard]] std::expected<std::string, ComponentWriteError> write(
        binary::ComponentPool const& pool) const;

  private:
    isoline::text::WriterOptions _options; ///< The layout and the output limit.
    FeatureSet _coreFeatures;              ///< What the embedded core modules decode under.
};

} // namespace isoline::component::text
