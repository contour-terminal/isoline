// SPDX-License-Identifier: Apache-2.0
#include <isoline/runtime/Instantiate.hpp>

#include <core/Assert.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <utility>
#include <variant>
#include <vector>

#include <isoline/binary/Module.hpp>
#include <isoline/binary/Types.hpp>
#include <isoline/runtime/Segment.hpp>
#include <isoline/runtime/Store.hpp>
#include <isoline/runtime/Trap.hpp>
#include <isoline/runtime/Value.hpp>

namespace isoline::runtime
{

namespace
{
    using binary::ConstExpr;
    using binary::ElementSegment;
    using binary::ExternKind;
    using binary::Import;
    using binary::Limits;
    using binary::Module;
    using binary::SegmentMode;

    /// The outcome of one step of instantiation.
    using Step = std::expected<void, InstantiationError>;

    /// Whether limits @p actual, of an entity whose current size is @p currentSize, match @p required.
    ///
    /// Limits match by subtyping: the entity is at least as large as required, and if a maximum is
    /// required, it has one no larger.
    [[nodiscard]] bool limitsMatch(uint64_t currentSize,
                                   Limits const& actual,
                                   Limits const& required) noexcept
    {
        if (actual.addressType != required.addressType || currentSize < required.min)
            return false;
        return !required.max || (actual.max && *actual.max <= *required.max);
    }

    /// Whether @p table matches the table type @p required: its element type, and its limits.
    [[nodiscard]] bool tableMatches(TableInstance const& table, binary::TableType const& required) noexcept
    {
        return table.type.element == required.element
               && limitsMatch(table.elements.size(), table.type.limits, required.limits);
    }

    /// Whether @p memory matches the memory type @p required: its limits, in pages.
    [[nodiscard]] bool memoryMatches(MemoryInstance const& memory,
                                     binary::MemoryType const& required) noexcept
    {
        return limitsMatch(memory.bytes.size() / PageSize, memory.type.limits, required.limits);
    }

    /// Whether @p global matches the global type @p required: its value type and mutability, exactly.
    [[nodiscard]] bool globalMatches(GlobalInstance const& global,
                                     binary::GlobalType const& required) noexcept
    {
        return global.type.type == required.type && global.type.mutability == required.mutability;
    }

    /// The index space of @p kind in @p instance, or nullptr for a kind P1 has none of (Tag).
    [[nodiscard]] std::vector<uint32_t>* indexSpace(ModuleInstance& instance, ExternKind kind) noexcept
    {
        switch (kind)
        {
            case ExternKind::Func: return &instance.functions;
            case ExternKind::Table: return &instance.tables;
            case ExternKind::Memory: return &instance.memories;
            case ExternKind::Global: return &instance.globals;
            case ExternKind::Tag: break;
        }
        return nullptr;
    }

    /// The address or offset a constant expression of an address type evaluated to.
    [[nodiscard]] uint64_t addressOf(Value const& value) noexcept
    {
        if (auto const* i64 = std::get_if<I64>(&value))
            return i64->bits;
        Require(std::holds_alternative<I32>(value));
        return std::get<I32>(value).bits;
    }

    /// One run of instantiate(): the steps of spec 4.5.4, sharing the instance being built.
    class Instantiation
    {
      public:
        Instantiation(Store& store,
                      ICodeExecutor& executor,
                      std::shared_ptr<Module const> module,
                      std::span<ExternValue const> imports):
            _store { store }, _executor { executor }, _module { std::move(module) }, _imports { imports }
        {
            Require(_module != nullptr);
            Require(_imports.size() <= _module->imports.size());
        }

        [[nodiscard]] std::expected<uint32_t, InstantiationError> run()
        {
            return checkImports()
                .and_then([this] { return allocate(); })
                .and_then([this] { return initializeTables(); })
                .and_then([this] { return initializeMemories(); })
                .and_then([this] { return runStart(); })
                .transform([this] { return _address; });
        }

      private:
        [[nodiscard]] Module const& module() const noexcept { return *_module; }

        [[nodiscard]] ModuleInstance& instance() { return _store.moduleInstance(_address); }

        // --- Imports ---

        [[nodiscard]] Step checkImports() const
        {
            for (auto const index: std::views::iota(std::size_t { 0 }, module().imports.size()))
            {
                auto const importIndex = static_cast<uint32_t>(index);
                if (index >= _imports.size())
                    return std::unexpected { LinkError { LinkErrorCode::UnknownImport, importIndex } };
                if (!matches(module().imports[index], _imports[index]))
                    return std::unexpected { LinkError { LinkErrorCode::IncompatibleImportType,
                                                         importIndex } };
            }
            return {};
        }

        /// Whether @p value is of the kind and a subtype of the type @p import requires.
        [[nodiscard]] bool matches(Import const& import, ExternValue const& value) const
        {
            if (value.kind != import.kind)
                return false;
            switch (import.kind)
            {
                case ExternKind::Func:
                    return _store.function(value.address).type == module().types[import.typeIndex];
                case ExternKind::Table: return tableMatches(_store.table(value.address), import.table);
                case ExternKind::Memory: return memoryMatches(_store.memory(value.address), import.memory);
                case ExternKind::Global: return globalMatches(_store.global(value.address), import.global);
                case ExternKind::Tag: break;
            }
            return false;
        }

        // --- Allocation ---

        /// Allocates the tables and memories first, into an instance not yet in the store, so an
        /// allocation failure leaves no half-built instance registered; then registers it and
        /// allocates the rest.
        [[nodiscard]] Step allocate()
        {
            auto pending = ModuleInstance { .module = _module,
                                            .functions = {},
                                            .tables = {},
                                            .memories = {},
                                            .globals = {},
                                            .elements = {},
                                            .data = {},
                                            .exports = {} };
            for (auto const& value: _imports)
                indexSpace(pending, value.kind)->push_back(value.address);
            return allocateTables(pending)
                .and_then([&] { return allocateMemories(pending); })
                .and_then([&] {
                    _address = _store.addModuleInstance(std::move(pending));
                    allocateFunctions();
                    return allocateGlobals();
                })
                .and_then([this] { return evaluateElements(); })
                .transform([this] {
                    viewData();
                    resolveExports();
                });
        }

        void allocateFunctions()
        {
            for (auto const index:
                 std::views::iota(uint32_t { 0 }, static_cast<uint32_t>(module().functions.size())))
            {
                auto const& type = module().types[module().functions[index]];
                auto const address = _store.addModuleFunction(type, _address, index);
                instance().functions.push_back(address);
            }
        }

        [[nodiscard]] Step allocateTables(ModuleInstance& pending)
        {
            for (auto const& type: module().tables)
            {
                auto const address = _store.addTable(
                    type, Ref { .kind = RefKind::Null, .heap = type.element.heap, .address = 0 });
                if (!address)
                    return std::unexpected { address.error() };
                pending.tables.push_back(*address);
            }
            return {};
        }

        [[nodiscard]] Step allocateMemories(ModuleInstance& pending)
        {
            for (auto const& type: module().memories)
            {
                auto const address = _store.addMemory(type);
                if (!address)
                    return std::unexpected { address.error() };
                pending.memories.push_back(*address);
            }
            return {};
        }

        /// Allocates each global once its initializer is evaluated, so an initializer sees the
        /// globals before it, as extended constant expressions may name them.
        [[nodiscard]] Step allocateGlobals()
        {
            for (auto const& global: module().globals)
            {
                auto const value = evaluate(global.init);
                if (!value)
                    return std::unexpected { value.error() };
                instance().globals.push_back(_store.addGlobal(global.type, *value));
            }
            return {};
        }

        [[nodiscard]] Step evaluateElements()
        {
            for (auto const& segment: module().elements)
            {
                auto refs = referencesOf(segment);
                if (!refs)
                    return std::unexpected { refs.error() };
                instance().elements.emplace_back(std::move(*refs));
            }
            return {};
        }

        /// The references @p segment's items evaluate to.
        [[nodiscard]] std::expected<std::vector<Ref>, Trap> referencesOf(ElementSegment const& segment)
        {
            auto refs = std::vector<Ref> {};
            if (auto const* indices = std::get_if<std::vector<uint32_t>>(&segment.items))
            {
                refs.reserve(indices->size());
                for (auto const index: *indices)
                    refs.push_back(Ref { .kind = RefKind::Func,
                                         .heap = { .kind = binary::AbstractHeapType::Func },
                                         .address = instance().functions[index] });
                return refs;
            }
            auto const& expressions = std::get<std::vector<ConstExpr>>(segment.items);
            refs.reserve(expressions.size());
            for (auto const& expression: expressions)
            {
                auto const value = evaluate(expression);
                if (!value)
                    return std::unexpected { value.error() };
                Require(std::holds_alternative<Ref>(*value));
                refs.push_back(std::get<Ref>(*value));
            }
            return refs;
        }

        void viewData()
        {
            for (auto const& segment: module().data)
                instance().data.emplace_back(std::span<std::byte const> { segment.bytes });
        }

        void resolveExports()
        {
            for (auto const& entry: module().exports)
            {
                auto const* space = indexSpace(instance(), entry.kind);
                Require(space != nullptr && entry.index < space->size());
                instance().exports.emplace_back(
                    entry.name, ExternValue { .kind = entry.kind, .address = (*space)[entry.index] });
            }
        }

        // --- Initialization ---

        /// Copies each active element segment into its table, as table.init then elem.drop; drops
        /// each declarative one. Traps at the first segment out of the table's bounds.
        [[nodiscard]] Step initializeTables()
        {
            for (auto const index: std::views::iota(std::size_t { 0 }, module().elements.size()))
            {
                auto const& segment = module().elements[index];
                if (segment.mode == SegmentMode::Passive)
                    continue;
                if (segment.mode == SegmentMode::Active)
                {
                    auto const offset = evaluate(segment.offset);
                    if (!offset)
                        return std::unexpected { offset.error() };
                    auto const refs = std::span<Ref const> { *instance().elements[index] };
                    auto& elements = _store.table(instance().tables[segment.table]).elements;
                    if (auto const copied = copySegment(refs,
                                                        0,
                                                        refs.size(),
                                                        std::span { elements },
                                                        addressOf(*offset),
                                                        TrapCode::OutOfBoundsTableAccess);
                        !copied)
                        return std::unexpected { copied.error() };
                }
                instance().elements[index].reset();
            }
            return {};
        }

        /// Copies each active data segment into its memory, as memory.init then data.drop. Traps at
        /// the first segment out of the memory's bounds.
        [[nodiscard]] Step initializeMemories()
        {
            for (auto const index: std::views::iota(std::size_t { 0 }, module().data.size()))
            {
                auto const& segment = module().data[index];
                if (segment.mode != SegmentMode::Active)
                    continue;
                auto const offset = evaluate(segment.offset);
                if (!offset)
                    return std::unexpected { offset.error() };
                auto const source = std::span<std::byte const> { segment.bytes };
                auto& bytes = _store.memory(instance().memories[segment.memory]).bytes;
                if (auto const copied = copySegment(source,
                                                    0,
                                                    source.size(),
                                                    std::span { bytes },
                                                    addressOf(*offset),
                                                    TrapCode::OutOfBoundsMemoryAccess);
                    !copied)
                    return std::unexpected { copied.error() };
                instance().data[index].reset();
            }
            return {};
        }

        [[nodiscard]] Step runStart()
        {
            if (!module().start)
                return {};
            auto const function = instance().functions[*module().start];
            return _executor.invoke(_store, function, {}, {}).transform_error([](Trap const& trap) {
                return InstantiationError { trap };
            });
        }

        [[nodiscard]] std::expected<Value, Trap> evaluate(ConstExpr const& expression)
        {
            return _executor.evaluateConst(_store, _address, expression);
        }

        Store& _store;
        ICodeExecutor& _executor;
        std::shared_ptr<Module const> _module;
        std::span<ExternValue const> _imports;
        uint32_t _address = 0;
    };
} // namespace

std::expected<uint32_t, InstantiationError> instantiate(Store& store,
                                                        ICodeExecutor& executor,
                                                        std::shared_ptr<binary::Module const> module,
                                                        std::span<ExternValue const> imports)
{
    return Instantiation { store, executor, std::move(module), imports }.run();
}

} // namespace isoline::runtime
