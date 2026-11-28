// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <variant>

#include <isoline/Features.hpp>
#include <isoline/api/EngineIdentity.hpp>
#include <isoline/api/Types.hpp>
#include <isoline/binary/DecodeError.hpp>
#include <isoline/runtime/ModuleCode.hpp>
#include <isoline/validate/ValidationError.hpp>

namespace isoline::validate
{
class ValidatedModule;
} // namespace isoline::validate

namespace isoline::api
{

class Engine;
class Store;

namespace testing
{
    struct ModuleAccess;
} // namespace testing

/// The type of a tag: the function type whose params are its exceptions' payload, and whose results
/// are empty.
struct TagType
{
    FuncType const* type; ///< The function type, inside the module that names the tag.
};

/// The type of what an import requires or an export provides, one alternative per binary::ExternKind.
///
/// Each alternative points into the module it describes, which an api::Module keeps alive: a
/// description costs the same whatever the size of the type it names. Concrete type indices inside a
/// FuncType, TableType or GlobalType index that module's type section.
using ExternType =
    std::variant<FuncType const*, TableType const*, MemoryType const*, GlobalType const*, TagType>;

/// One of a module's imports: what it names, and the type it requires.
///
/// The names and the type point into the module, valid as long as any copy of the api::Module that
/// gave it lives.
struct ImportDescription
{
    std::string_view module; ///< The name of the module it imports from.
    std::string_view name;   ///< The name of the entity within that module.
    ExternType type;         ///< The type it requires of the entity.
};

/// One of a module's exports: its name, and the type of the entity it names.
///
/// The name and the type point into the module, valid as long as any copy of the api::Module that
/// gave it lives.
struct ExportDescription
{
    std::string_view name; ///< The exported name.
    ExternType type;       ///< The type of the exported entity.
};

/// A module larger than LoadLimits::maxModuleBytes, refused before it was decoded.
struct ModuleTooLarge
{
    uint64_t bytes; ///< The module's size in bytes.
    uint64_t limit; ///< LoadLimits::maxModuleBytes, which it exceeds.

    /// Two refusals are equal when size and limit are.
    bool operator==(ModuleTooLarge const&) const = default;
};

/// The host ran out of memory while loading a module, or before a store changed (std::bad_alloc, or
/// std::length_error for a container asked past its max_size(), caught at the API boundary).
///
/// It says nothing about the module: neither malformed nor past a limit, it may load (or instantiate)
/// once memory is free. LoadLimits bound what loading may hold to a multiple of the module's size, so
/// a host that sees this is short of memory, not attacked by the module.
struct HostMemoryExhausted
{
    /// All such refusals are equal.
    bool operator==(HostMemoryExhausted const&) const = default;
};

/// Which of the interpreter's limits a module's lowering exceeded.
enum class CompileLimit : uint8_t
{
    Budget =
        0, ///< The bytes lowering holds at once, LoadLimits::maxCompileBytes: raising it lifts the refusal.
    FrameSlots, ///< The slots one frame may have, which the code's 32-bit slot operands address.
    CodeWords,  ///< The code words of one function, which the code's 32-bit branch targets reach.
};

/// The last CompileLimit enumerator; it must name whichever enumerator is listed last above.
inline constexpr auto LastCompileLimit = CompileLimit::CodeWords;

/// One row of CompileLimitTable: a compile limit, the unit its requests and limits count in, and what it
/// bounds.
struct CompileLimitRow
{
    CompileLimit kind;     ///< The limit.
    std::string_view name; ///< How messages and the embedding guide name it.
    std::string_view unit; ///< What CompileLimitExceeded::requested and ::limit count.
    std::string_view rule; ///< What it bounds, and what lifts it.
};

/// Every compile limit, in CompileLimit's order: the one table the CLI's messages and the embedding guide's
/// generated block read.
inline constexpr auto CompileLimitTable = std::to_array<CompileLimitRow>({
    { .kind = CompileLimit::Budget,
      .name = "compile budget",
      .unit = "bytes",
      .rule = "what lowering a module holds at once, its code included; LoadLimits::maxCompileBytes, "
              "and raising it lifts the refusal" },
    { .kind = CompileLimit::FrameSlots,
      .name = "frame slots",
      .unit = "slots",
      .rule = "the slots of one function's frame, at most 2^32 - 1; only decode limits raised far past "
              "their defaults admit a function past it" },
    { .kind = CompileLimit::CodeWords,
      .name = "code words",
      .unit = "words",
      .rule = "the code words of one function, which 32-bit branch targets reach; only a body limit "
              "raised past a gigabyte admits a function past it" },
});

/// The row of @p kind.
/// @param kind A compile limit.
/// @return Its CompileLimitTable row.
[[nodiscard]] constexpr CompileLimitRow const& compileLimitRow(CompileLimit kind) noexcept
{
    return CompileLimitTable[static_cast<std::size_t>(kind)];
}

/// The codeIndex of a CompileLimitExceeded that came outside any function: lowering the module's own
/// records.
inline constexpr auto ModuleLevel = std::numeric_limits<uint32_t>::max();

/// A module the interpreter refused to lower: its code needs more than one of its limits allows.
struct CompileLimitExceeded
{
    /// The function lowering was refused at, by its index in the code section; ModuleLevel when the
    /// refusal came outside any function.
    uint32_t codeIndex = 0;
    uint64_t requested = 0; ///< What the refused reservation asked for, in its limit's unit.
    uint64_t limit = 0;     ///< What the limit allows, in its unit (compileLimitRow()).
    CompileLimit kind = CompileLimit::Budget; ///< Which limit.
    /// Two refusals are equal when function, request, limit and kind are.
    bool operator==(CompileLimitExceeded const&) const = default;
};

/// Why Engine::load() refused a module: too large to decode, the host out of memory, malformed, invalid,
/// or too large to lower.
using LoadError =
    std::variant<ModuleTooLarge, HostMemoryExhausted, DecodeError, ValidationError, CompileLimitExceeded>;

/// Whether @p error refuses a module only for an implementation limit rather than for being malformed or
/// invalid, which no limits change. Raising LoadLimits lifts ModuleTooLarge, a decode or validation limit,
/// and a CompileLimitExceeded of CompileLimit::Budget; the other compile limits are the interpreter's own
/// bounds, which no LoadLimits raise (the reference engine loads such a module).
/// @param error Why the module was refused.
/// @return Whether a ModuleTooLarge or a CompileLimitExceeded, or a decode or validation error whose row
///         is an implementation limit (binary::isImplementationLimit(), validate::isImplementationLimit()).
[[nodiscard]] bool isImplementationLimit(LoadError const& error) noexcept;

/// The feature an engine must enable to get past @p error, when the module uses one it lacks.
/// @param error Why the module was refused.
/// @return The feature, for a decode or validation error that names the disabled proposal which would
///         decode or admit the module (binary::DecodeError::disabled,
///         validate::ValidationError::disabled); otherwise nothing. Enabling it (closed over its
///         prerequisites) gets the module past that point, where another may be named.
[[nodiscard]] std::optional<Feature> requiredFeature(LoadError const& error) noexcept;

/// A module an Engine loaded: decoded, validated, described, and for the interpreter lowered.
///
/// Its imports are only described, never resolved: a module loads whatever it imports, and whether
/// they are satisfied is a question for instantiation. A module is immutable and cheap to copy, every
/// copy sharing one validated module, one set of descriptions and one lowered code, so it may be
/// instantiated by any number of stores, on any thread.
class Module
{
  public:
    /// The passkey of the constructor: only an Engine constructs a Module.
    class Key
    {
        Key() = default;
        friend class Engine;
    };

    /// Describes a module an engine loaded; reached through Engine::load().
    /// @param key The passkey only an Engine has.
    /// @param validated The module, decoded and validated (checked: not null).
    /// @param engine The identity of the engine that loaded it.
    /// @param code What the engine made of @p validated to run it: the interpreter's lowered module; null
    ///             for the reference engine, which runs the validated module itself.
    Module(Key key,
           std::shared_ptr<validate::ValidatedModule const> validated,
           EngineIdentity const& engine,
           std::shared_ptr<runtime::ModuleCode const> code);

    /// A copy, sharing this module's contents.
    ///
    /// There is deliberately no move constructor or move assignment: a moved-from module would hold no
    /// contents, and every member reads them. Moving a module copies it instead, which costs one atomic
    /// increment and leaves the source whole.
    Module(Module const&) = default;

    /// Makes this module a copy of @p other, sharing its contents; moving assigns the same way.
    /// @return This module.
    Module& operator=(Module const&) = default;

    ~Module() = default;

    /// Its imports, in the order of its import section.
    /// @return One description per import, the items of a compact import group each counted alone,
    ///         valid while any copy of this module lives.
    [[nodiscard]] std::span<ImportDescription const> imports() const& noexcept;

    /// Deleted: the descriptions of a temporary module would dangle at the end of the expression.
    [[nodiscard]] std::span<ImportDescription const> imports() const&& = delete;

    /// Its exports, in the order of its export section.
    /// @return One description per export, valid while any copy of this module lives.
    [[nodiscard]] std::span<ExportDescription const> exports() const& noexcept;

    /// Deleted: the descriptions of a temporary module would dangle at the end of the expression.
    [[nodiscard]] std::span<ExportDescription const> exports() const&& = delete;

    /// The features it was decoded and validated with: those of the engine that loaded it.
    /// @return The feature set.
    [[nodiscard]] FeatureSet features() const noexcept;

    /// The identity of the engine that loaded it, which a store instantiating it must share.
    /// @return The identity, valid while any copy of this module lives.
    [[nodiscard]] EngineIdentity const& engine() const& noexcept;

    /// Deleted: the identity of a temporary module would dangle at the end of the expression.
    [[nodiscard]] EngineIdentity const& engine() const&& = delete;

    /// What its engine made of it to run it, which every store instantiating it shares: lowered once,
    /// when it was loaded.
    /// @return The interpreter's lowered module; null for the reference engine.
    [[nodiscard]] std::shared_ptr<runtime::ModuleCode const> const& code() const noexcept;

  private:
    friend class Store;
    friend struct testing::ModuleAccess;

    struct Contents;

    /// Describes @p validated's imports and exports.
    /// @param validated The module (checked: not null).
    /// @param engine The identity of the engine that loaded it.
    /// @param code What the engine made of @p validated to run it; null for the reference engine.
    /// @return What every copy of the Module shares.
    [[nodiscard]] static std::shared_ptr<Contents const> describe(
        std::shared_ptr<validate::ValidatedModule const> validated,
        EngineIdentity const& engine,
        std::shared_ptr<runtime::ModuleCode const> code);

    /// The validated module it describes, which a Store instantiates.
    /// @return The validated module, shared with every copy of this one.
    [[nodiscard]] std::shared_ptr<validate::ValidatedModule const> const& validated() const noexcept;

    std::shared_ptr<Contents const> _contents; ///< Shared by every copy; never null, as nothing moves it out.
};

} // namespace isoline::api
