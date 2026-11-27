// SPDX-License-Identifier: Apache-2.0
#include <isoline/api/Module.hpp>

#include <core/Assert.hpp>
#include <core/Overloaded.hpp>
#include <core/Utils.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <utility>
#include <variant>
#include <vector>

#include <isoline/binary/Module.hpp>
#include <isoline/binary/Types.hpp>
#include <isoline/validate/ValidatedModule.hpp>

namespace isoline::api
{

using binary::ExternKind;
using binary::ExternKindCount;

/// What every copy of a Module shares: the validated module, the identity of the engine that loaded
/// it, the code that engine made of it, and the descriptions, which point into the validated module.
struct Module::Contents
{
    std::shared_ptr<validate::ValidatedModule const> validated; ///< Never null.
    EngineIdentity engine;                                      ///< The loading engine's identity.
    std::shared_ptr<runtime::ModuleCode const> code;            ///< Null for the reference engine.
    std::vector<ImportDescription> imports;                     ///< One per import, in order.
    std::vector<ExportDescription> exports;                     ///< One per export, in order.
};

namespace
{
    /// The type @p import requires, pointing into @p module.
    ExternType importType(binary::Module const& module, binary::Import const& import) noexcept
    {
        switch (import.kind)
        {
            case ExternKind::Func: return &module.types[import.typeIndex];
            case ExternKind::Table: return &import.table;
            case ExternKind::Memory: return &import.memory;
            case ExternKind::Global: return &import.global;
            case ExternKind::Tag: break; // described below the switch
        }
        return TagType { .type = &module.types[import.typeIndex] };
    }

    /// The type of the entity @p index of @p kind that @p module defines, imports not counted,
    /// pointing into @p module.
    ExternType definedType(binary::Module const& module, ExternKind kind, std::size_t index) noexcept
    {
        switch (kind)
        {
            case ExternKind::Func: return &module.types[module.functions[index]];
            case ExternKind::Table: return &module.tables[index].type;
            case ExternKind::Memory: return &module.memories[index];
            case ExternKind::Global: return &module.globals[index].type;
            case ExternKind::Tag: break; // described below the switch
        }
        return TagType { .type = &module.types[module.tags[index].typeIndex] };
    }

    /// One description per import of @p module.
    std::vector<ImportDescription> describeImports(binary::Module const& module)
    {
        auto descriptions = std::vector<ImportDescription> {};
        descriptions.reserve(module.imports.size());
        for (auto const& import: module.imports)
            descriptions.push_back(ImportDescription { .module = binary::importModuleOf(module, import),
                                                       .name = import.name,
                                                       .type = importType(module, import) });
        return descriptions;
    }

    /// One description per export of @p module, whose imports @p imports describes.
    ///
    /// An export's index counts the imports of its kind first, so each kind's imports are listed by
    /// position once, and every export is then resolved in constant time.
    std::vector<ExportDescription> describeExports(binary::Module const& module,
                                                   std::span<ImportDescription const> imports)
    {
        auto importsByKind = std::array<std::vector<std::size_t>, ExternKindCount> {};
        for (auto const [position, import]: core::views::enumerate(module.imports))
            importsByKind[static_cast<std::size_t>(import.kind)].push_back(position);

        auto descriptions = std::vector<ExportDescription> {};
        descriptions.reserve(module.exports.size());
        for (auto const& entry: module.exports)
        {
            auto const& imported = importsByKind[static_cast<std::size_t>(entry.kind)];
            auto const type = entry.index < imported.size()
                                  ? imports[imported[entry.index]].type
                                  : definedType(module, entry.kind, entry.index - imported.size());
            descriptions.push_back(ExportDescription { .name = entry.name, .type = type });
        }
        return descriptions;
    }
} // namespace

std::shared_ptr<Module::Contents const> Module::describe(
    std::shared_ptr<validate::ValidatedModule const> validated,
    EngineIdentity const& engine,
    std::shared_ptr<runtime::ModuleCode const> code)
{
    Require(validated != nullptr);
    auto const& module = validated->module();
    auto imports = describeImports(module);
    auto exports = describeExports(module, imports);
    return std::make_shared<Contents const>(Contents { .validated = std::move(validated),
                                                       .engine = engine,
                                                       .code = std::move(code),
                                                       .imports = std::move(imports),
                                                       .exports = std::move(exports) });
}

bool isImplementationLimit(LoadError const& error) noexcept
{
    struct Visitor
    {
        bool operator()(ModuleTooLarge const&) const noexcept { return true; }
        bool operator()(HostMemoryExhausted const&) const noexcept { return false; }
        bool operator()(CompileLimitExceeded const&) const noexcept { return true; }
        bool operator()(binary::DecodeError const& decode) const noexcept
        {
            return binary::isImplementationLimit(decode.code);
        }
        bool operator()(validate::ValidationError const& validation) const noexcept
        {
            return validate::isImplementationLimit(validation.code);
        }
    };
    return std::visit(Visitor {}, error);
}

std::optional<Feature> requiredFeature(LoadError const& error) noexcept
{
    return std::visit(core::Overloaded {
                          [](binary::DecodeError const& decode) { return decode.disabled; },
                          [](validate::ValidationError const& validation) { return validation.disabled; },
                          [](auto const&) { return std::optional<Feature> {}; },
                      },
                      error);
}

Module::Module(Key /*key*/,
               std::shared_ptr<validate::ValidatedModule const> validated,
               EngineIdentity const& engine,
               std::shared_ptr<runtime::ModuleCode const> code):
    _contents { describe(std::move(validated), engine, std::move(code)) }
{
}

std::span<ImportDescription const> Module::imports() const& noexcept
{
    return _contents->imports;
}

std::span<ExportDescription const> Module::exports() const& noexcept
{
    return _contents->exports;
}

FeatureSet Module::features() const noexcept
{
    return _contents->validated->features();
}

EngineIdentity const& Module::engine() const& noexcept
{
    return _contents->engine;
}

std::shared_ptr<runtime::ModuleCode const> const& Module::code() const noexcept
{
    return _contents->code;
}

std::shared_ptr<validate::ValidatedModule const> const& Module::validated() const noexcept
{
    return _contents->validated;
}

} // namespace isoline::api
