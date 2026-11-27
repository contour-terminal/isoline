// SPDX-License-Identifier: Apache-2.0
#include <isoline/compiler/FeatureSupport.hpp>

#include <core/Assert.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <variant>
#include <vector>

#include <isoline/binary/Module.hpp>
#include <isoline/validate/ValidatedModule.hpp>

namespace isoline::compiler
{

namespace
{
    using binary::AbstractHeapType;
    using binary::ValueKind;

    /// Which heaps a TypeFeatureRow matches.
    enum class HeapMatch : uint8_t
    {
        AnyHeap,  ///< Every type of the row's kind.
        ThisHeap, ///< A reference into the row's heap only.
    };

    /// A form of value type that needs a feature: what checkValueType() refuses when that feature is
    /// deferred.
    struct TypeFeatureRow
    {
        ValueKind kind;        ///< The type's kind.
        HeapMatch match;       ///< Whether the heap matters.
        AbstractHeapType heap; ///< The heap it must point into, for ThisHeap.
        Feature feature;       ///< The feature such a type needs.
    };

    /// Every form of value type that needs a feature, in the order checkValueType() tries them.
    constexpr auto TypeFeatureTable = std::to_array<TypeFeatureRow>({
        { .kind = ValueKind::V128,
          .match = HeapMatch::AnyHeap,
          .heap = AbstractHeapType::Func,
          .feature = Feature::Simd },
        { .kind = ValueKind::Ref,
          .match = HeapMatch::ThisHeap,
          .heap = AbstractHeapType::Concrete,
          .feature = Feature::TypedFunctionReferences },
        { .kind = ValueKind::Ref,
          .match = HeapMatch::ThisHeap,
          .heap = AbstractHeapType::Exn,
          .feature = Feature::ExceptionHandling },
        { .kind = ValueKind::Ref,
          .match = HeapMatch::ThisHeap,
          .heap = AbstractHeapType::NoExn,
          .feature = Feature::ExceptionHandling },
    });

    /// A module position that needs a feature by being there at all, whatever types it names.
    enum class Position : uint8_t
    {
        Tag, ///< A tag, defined or imported.
    };

    /// The last Position enumerator, for checking PositionFeatureTable's size.
    constexpr auto LastPosition = Position::Tag;

    /// A position that needs a feature: what checkPosition() refuses when that feature is deferred.
    struct PositionFeatureRow
    {
        Position position; ///< The position; equal to the row's index.
        Feature feature;   ///< The feature a module holding it needs.
    };

    /// Every position that needs a feature, in Position order.
    constexpr auto PositionFeatureTable = std::to_array<PositionFeatureRow>({
        { .position = Position::Tag, .feature = Feature::ExceptionHandling },
    });

    static_assert(
        [] {
            auto index = std::size_t { 0 };
            for (auto const& row: PositionFeatureTable)
                if (static_cast<std::size_t>(row.position) != index++)
                    return false;
            return index == static_cast<std::size_t>(LastPosition) + 1;
        }(),
        "PositionFeatureTable must hold one row per Position, at the index of its enumerator");

    /// Whether the compiler defers @p feature.
    [[nodiscard]] constexpr bool isDeferred(Feature feature) noexcept
    {
        return CompilerFeatureTable[static_cast<std::size_t>(feature)].support == CompilerSupport::Deferred;
    }

    /// The refusal of a use of @p feature.
    [[nodiscard]] std::unexpected<CompileError> unsupported(Feature feature,
                                                            std::optional<uint32_t> function) noexcept
    {
        return std::unexpected { CompileError {
            .code = CompileErrorCode::UnsupportedFeature, .feature = feature, .function = function } };
    }

    /// Checks a use of @p position: refused when the feature its row names is deferred.
    [[nodiscard]] std::expected<void, CompileError> checkPosition(Position position) noexcept
    {
        auto const feature = PositionFeatureTable[static_cast<std::size_t>(position)].feature;
        if (isDeferred(feature))
            return unsupported(feature, std::nullopt);
        return {};
    }

    /// checkOpcode(), for an instruction in a body or, with no function, in a constant expression.
    [[nodiscard]] std::expected<void, CompileError> checkOpcodeIn(binary::Opcode opcode,
                                                                  std::optional<uint32_t> function) noexcept
    {
        auto const required = binary::opcodeInfo(opcode).required;
        for (auto const& row: CompilerFeatureTable)
            if (row.support == CompilerSupport::Deferred && required.has(row.feature))
                return unsupported(row.feature, function);
        return {};
    }

    /// Applies @p check to each element of @p range, in order, up to the first refusal.
    /// @return Nothing; or that refusal.
    template <typename Range, typename Check>
    [[nodiscard]] std::expected<void, CompileError> each(Range const& range, Check const& check) noexcept
    {
        for (auto const& element: range)
            if (auto checked = check(element); !checked)
                return checked;
        return {};
    }

    /// Checks @p types, in order.
    [[nodiscard]] std::expected<void, CompileError> checkValueTypes(std::span<binary::ValueType const> types,
                                                                    std::optional<uint32_t> function) noexcept
    {
        return each(types,
                    [function](binary::ValueType const& type) { return checkValueType(type, function); });
    }

    /// Checks the value types @p instruction's immediates name: a block type of the value form, a typed
    /// select's types, ref.null's heap. A block type of the type-index form names a function type, which
    /// checkModule() checks with the type section.
    [[nodiscard]] std::expected<void, CompileError> checkImmediateTypes(
        binary::Instruction const& instruction,
        binary::Function const* body,
        std::optional<uint32_t> function) noexcept
    {
        switch (binary::opcodeInfo(instruction.opcode).immediate)
        {
            case binary::ImmediateKind::BlockType:
            case binary::ImmediateKind::TryTable:
                if (instruction.blockKind == binary::BlockType::Kind::Value)
                    return checkValueType(instruction.type, function);
                return {};
            case binary::ImmediateKind::HeapType: return checkValueType(instruction.type, function);
            case binary::ImmediateKind::SelectTypes:
                Require(body != nullptr);
                return checkValueTypes(binary::selectTypesOf(*body, instruction), function);
            default: return {};
        }
    }

    /// Checks one instruction: its opcode, then the value types its immediates name.
    /// @param instruction The instruction.
    /// @param body The function it is in, whose side tables hold a typed select's types; none for a
    ///             constant expression, which has no typed select.
    /// @param function The function's code index; none for a constant expression.
    [[nodiscard]] std::expected<void, CompileError> checkInstruction(
        binary::Instruction const& instruction,
        binary::Function const* body,
        std::optional<uint32_t> function) noexcept
    {
        return checkOpcodeIn(instruction.opcode, function).and_then([&] {
            return checkImmediateTypes(instruction, body, function);
        });
    }

    /// Checks every instruction of @p expression, a constant expression.
    [[nodiscard]] std::expected<void, CompileError> checkExpression(
        binary::ConstExpr const& expression) noexcept
    {
        return each(expression, [](binary::Instruction const& instruction) {
            return checkInstruction(instruction, nullptr, std::nullopt);
        });
    }

    /// Checks a definition's type, then its constant expression.
    [[nodiscard]] std::expected<void, CompileError> checkTyped(binary::ValueType const& type,
                                                               binary::ConstExpr const& expression) noexcept
    {
        return checkValueType(type, std::nullopt).and_then([&] { return checkExpression(expression); });
    }

    /// Checks a function type of the type section, which every function, import, tag and block of the
    /// type-index form names.
    [[nodiscard]] std::expected<void, CompileError> checkFunctionType(binary::FuncType const& type) noexcept
    {
        return checkValueTypes(type.params, std::nullopt).and_then([&] {
            return checkValueTypes(type.results, std::nullopt);
        });
    }

    /// Checks an import's own type positions: a table's element, a global's type; a tag is its
    /// position's row. A function's type is the type section's.
    [[nodiscard]] std::expected<void, CompileError> checkImport(binary::Import const& import) noexcept
    {
        switch (import.kind)
        {
            case binary::ExternKind::Table: return checkValueType(import.table.element, std::nullopt);
            case binary::ExternKind::Global: return checkValueType(import.global.type, std::nullopt);
            case binary::ExternKind::Tag: return checkPosition(Position::Tag);
            case binary::ExternKind::Func:
            case binary::ExternKind::Memory: return {};
        }
        // Every ExternKind is a case above.
        core::unreachable(); // uncovered(extern-kind-without-case)
    } // uncovered(extern-kind-without-case)

    /// Checks an element segment: its type, its offset, and its items when they are expressions.
    [[nodiscard]] std::expected<void, CompileError> checkElementSegment(
        binary::ElementSegment const& segment) noexcept
    {
        auto const* items = std::get_if<std::vector<binary::ConstExpr>>(&segment.items);
        return checkTyped(segment.type, segment.offset).and_then([&] {
            return items != nullptr ? each(*items, checkExpression) : std::expected<void, CompileError> {};
        });
    }

    /// Checks the tags: any is refused by its position's row.
    [[nodiscard]] std::expected<void, CompileError> checkTags(binary::Module const& module) noexcept
    {
        if (module.tags.empty())
            return {};
        return checkPosition(Position::Tag);
    }

    /// Checks one body: its locals, then each instruction, dead code included.
    [[nodiscard]] std::expected<void, CompileError> checkBody(binary::Function const& body,
                                                              uint32_t function) noexcept
    {
        return each(body.locals,
                    [function](binary::LocalRun const& run) { return checkValueType(run.type, function); })
            .and_then([&] {
                return each(body.body, [&](binary::Instruction const& instruction) {
                    return checkInstruction(instruction, &body, function);
                });
            });
    }
} // namespace

std::expected<void, CompileError> checkOpcode(binary::Opcode opcode, uint32_t function) noexcept
{
    return checkOpcodeIn(opcode, function);
}

std::expected<void, CompileError> checkValueType(binary::ValueType const& type,
                                                 std::optional<uint32_t> function) noexcept
{
    for (auto const& row: TypeFeatureTable)
    {
        auto const matches =
            type.kind == row.kind && (row.match == HeapMatch::AnyHeap || type.heap.kind == row.heap);
        if (matches && isDeferred(row.feature))
            return unsupported(row.feature, function);
    }
    return {};
}

std::expected<void, CompileError> checkModule(validate::ValidatedModule const& module)
{
    auto const& decoded = module.module();
    auto function = uint32_t { 0 };
    return each(decoded.types, checkFunctionType)
        .and_then([&] { return each(decoded.imports, checkImport); })
        .and_then([&] {
            return each(decoded.tables, [](binary::Table const& table) {
                return checkTyped(table.type.element, table.init);
            });
        })
        .and_then([&] { return checkTags(decoded); })
        .and_then([&] {
            return each(decoded.globals, [](binary::Global const& global) {
                return checkTyped(global.type.type, global.init);
            });
        })
        .and_then([&] { return each(decoded.elements, checkElementSegment); })
        .and_then([&] {
            return each(decoded.data,
                        [](binary::DataSegment const& segment) { return checkExpression(segment.offset); });
        })
        .and_then([&] {
            return each(decoded.code,
                        [&function](binary::Function const& body) { return checkBody(body, function++); });
        });
}

} // namespace isoline::compiler
