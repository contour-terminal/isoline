// SPDX-License-Identifier: Apache-2.0
#include <isoline/runtime/Instantiate.hpp>

#include <core/Utils.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <utility>
#include <variant>
#include <vector>

#include <isoline/binary/Module.hpp>
#include <isoline/binary/TypeCanon.hpp>
#include <isoline/binary/Types.hpp>
#include <isoline/meta/Check.hpp>
#include <isoline/runtime/Abrupt.hpp>
#include <isoline/runtime/ConstantExpression.hpp>
#include <isoline/runtime/Interrupt.hpp>
#include <isoline/runtime/Limits.hpp>
#include <isoline/runtime/ModuleCode.hpp>
#include <isoline/runtime/Segment.hpp>
#include <isoline/runtime/Store.hpp>
#include <isoline/runtime/Trap.hpp>
#include <isoline/runtime/Value.hpp>
#include <isoline/validate/Subtyping.hpp>
#include <isoline/validate/ValidatedModule.hpp>

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

    /// Whether @p table matches the table type @p required, in store form: its element type, exactly,
    /// and its limits.
    [[nodiscard]] bool tableMatches(TableInstance const& table, binary::TableType const& required) noexcept
    {
        return table.type.element == required.element
               && limitsMatch(table.elements.size(), table.type.limits, required.limits);
    }

    /// Whether @p memory matches the memory type @p required: its page size, exactly, and its limits,
    /// in pages.
    [[nodiscard]] bool memoryMatches(MemoryInstance const& memory,
                                     binary::MemoryType const& required) noexcept
    {
        return memory.type.pageSizeLog2 == required.pageSizeLog2
               && limitsMatch(memory.bytes().size() / binary::pageSize(memory.type),
                              memory.type.limits,
                              required.limits);
    }

    /// Whether @p global matches the global type @p required, in store form: its mutability exactly,
    /// and its value type by subtyping when immutable, exactly when mutable, as it may be written
    /// through either side then.
    [[nodiscard]] bool globalMatches(GlobalInstance const& global,
                                     binary::GlobalType const& required) noexcept
    {
        if (global.type.mutability != required.mutability)
            return false;
        if (required.mutability == binary::Mutability::Var)
            return global.type.type == required.type;
        return validate::isStoreSubtype(global.type.type, required.type);
    }

    /// The index space of @p kind in @p instance.
    [[nodiscard]] std::vector<uint32_t>& indexSpace(ModuleInstance& instance, ExternKind kind) noexcept
    {
        switch (kind)
        {
            case ExternKind::Func: return instance.functions;
            case ExternKind::Table: return instance.tables;
            case ExternKind::Memory: return instance.memories;
            case ExternKind::Global: return instance.globals;
            case ExternKind::Tag: break;
        }
        return instance.tags;
    }

    /// @p a + @p b, or 2^64 - 1 when that overflows.
    [[nodiscard]] constexpr uint64_t saturatingAdd(uint64_t a, uint64_t b) noexcept
    {
        return b > std::numeric_limits<uint64_t>::max() - a ? std::numeric_limits<uint64_t>::max() : a + b;
    }

    /// @p a * @p b, or 2^64 - 1 when that overflows. Precondition: @p b is not zero.
    [[nodiscard]] constexpr uint64_t saturatingMultiply(uint64_t a, uint64_t b) noexcept
    {
        return a > std::numeric_limits<uint64_t>::max() / b ? std::numeric_limits<uint64_t>::max() : a * b;
    }

    /// A store's refusal to allocate, as the instantiation error it is.
    [[nodiscard]] InstantiationError instantiationError(AllocationError const& error) noexcept
    {
        return std::visit([](auto const& refusal) { return InstantiationError { refusal }; }, error);
    }

    /// The address or offset a constant expression of an address type evaluated to.
    [[nodiscard]] uint64_t addressOf(Value const& value) noexcept
    {
        if (auto const* i64 = std::get_if<I64>(&value))
            return i64->bits;
        ISOLINE_INVARIANT(std::holds_alternative<I32>(value));
        return std::get<I32>(value).bits;
    }

    /// One run of instantiate(): the steps of spec 4.5.4, sharing the instance being built.
    class Instantiation
    {
      public:
        Instantiation(Store& store,
                      ICodeExecutor& executor,
                      std::shared_ptr<validate::ValidatedModule const> validated,
                      std::shared_ptr<ModuleCode const> code,
                      std::span<ExternValue const> imports):
            _store { store },
            _executor { executor },
            _validated { std::move(validated) },
            _module { validate::ValidatedModule::moduleOf(_validated) },
            _code { std::move(code) },
            _imports { imports }
        {
            Require(_imports.size() <= _module->imports.size());
        }

        /// Instantiates, polling at Instantiation first: an interrupted store instantiates nothing.
        /// What prepare() allocates may throw; once commit() changes the store, nothing is thrown.
        [[nodiscard]] std::expected<uint32_t, InstantiationError> run()
        {
            if (auto const polled = _store.poll(PollSite::Instantiation); !polled)
                return std::unexpected { InstantiationError { polled.error() } };
            return prepare().and_then([this](ModuleInstance pending) { return commit(std::move(pending)); });
        }

      private:
        [[nodiscard]] Module const& module() const noexcept { return *_module; }

        [[nodiscard]] ModuleInstance& instance() { return _store.moduleInstance(_address); }

        // --- Types ---

        /// Finds the TypeId each of the module's types has in the store, or will get once
        /// internTypes() interns it, without interning any: a failed instantiation leaves the store's
        /// types as they were. A type not interned yet gets the next free TypeId, as internType()
        /// will hand them out in type order; equal types share one (canonicalTypeIds). Such an id
        /// names no function, table or global of the store yet, so nothing an import binds to
        /// matches it, as nothing in the store has that type.
        void resolveTypes()
        {
            auto const canonical = binary::canonicalTypeIds(module().types);
            auto next = _store.typeCount();
            _types.reserve(module().types.size());
            for (auto const [index, type]: core::views::enumerate(module().types))
            {
                if (canonical[index] != index)
                    _types.push_back(_types[canonical[index]]);
                else if (auto const found = _store.findType(storeForm(type)))
                    _types.push_back(*found);
                else
                {
                    _types.push_back(next++);
                    _newTypeUnits += Store::typeUnits(type);
                }
            }
        }

        /// Interns the module's types, once linking and allocation have succeeded; each gets the
        /// TypeId resolveTypes() found for it.
        void internTypes()
        {
            for (auto const [index, type]: core::views::enumerate(module().types))
            {
                // Interned outside the check: a check only reads.
                auto const interned = _store.internType(storeForm(type));
                Require(interned == _types[index]);
            }
        }

        /// @p type with a concrete type index rewritten to its TypeId.
        [[nodiscard]] binary::ValueType storeForm(binary::ValueType const& type) const
        {
            return binary::mapTypeIndex(type, [this](uint32_t index) { return _types.at(index); });
        }

        [[nodiscard]] binary::FuncType storeForm(binary::FuncType const& type) const
        {
            return binary::mapTypeIndices(type, [this](uint32_t index) { return _types.at(index); });
        }

        [[nodiscard]] binary::TableType storeForm(binary::TableType type) const
        {
            type.element = storeForm(type.element);
            return type;
        }

        [[nodiscard]] binary::GlobalType storeForm(binary::GlobalType type) const
        {
            type.type = storeForm(type.type);
            return type;
        }

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
                    return _store.function(value.address).typeId == _types.at(import.typeIndex);
                case ExternKind::Table:
                    return tableMatches(_store.table(value.address), storeForm(import.table));
                case ExternKind::Memory: return memoryMatches(_store.memory(value.address), import.memory);
                case ExternKind::Global:
                    return globalMatches(_store.global(value.address), storeForm(import.global));
                case ExternKind::Tag: break; // matched below the switch
            }
            // A tag's type matches only its own: an exception's payload is read by the tag's type.
            return _store.tag(value.address).typeId == _types.at(import.typeIndex);
        }

        // --- Allocation ---

        /// What the module's own entities claim of the store's limits: its defined memories and
        /// tables with their minimum sizes, the types the store does not hold yet, and its tags.
        [[nodiscard]] InstanceDemand demand() const noexcept
        {
            auto demand = InstanceDemand { .memories = module().memories.size(),
                                           .tables = module().tables.size(),
                                           .memoryBytes = 0,
                                           .tableElements = 0,
                                           .typeUnits = _newTypeUnits,
                                           .tags = module().tags.size() };
            for (auto const& memory: module().memories)
                demand.memoryBytes = saturatingAdd(
                    demand.memoryBytes, saturatingMultiply(memory.limits.min, binary::pageSize(memory)));
            for (auto const& table: module().tables)
                demand.tableElements = saturatingAdd(demand.tableElements, table.type.limits.min);
            return demand;
        }

        /// Everything before the store changes: the module's TypeIds, the imports' check, its demand
        /// against the store's limits, and the instance to be, with its imports in its index spaces.
        /// It changes nothing in the store, so running out of memory here (std::bad_alloc or
        /// std::length_error, which it lets through) leaves the store as it was.
        [[nodiscard]] std::expected<ModuleInstance, InstantiationError> prepare()
        {
            resolveTypes();
            if (auto const linked = checkImports(); !linked)
                return std::unexpected { linked.error() };
            if (auto const fits = _store.checkInstanceDemand(demand()); !fits)
                return std::unexpected { InstantiationError { fits.error() } };
            auto pending = ModuleInstance { .validated = _validated,
                                            .module = _module,
                                            .code = _code,
                                            .types = _types,
                                            .functions = {},
                                            .tables = {},
                                            .memories = {},
                                            .globals = {},
                                            .tags = {},
                                            .elements = {},
                                            .data = {},
                                            .exports = {} };
            for (auto const& value: _imports)
                indexSpace(pending, value.kind).push_back(value.address);
            return pending;
        }

        /// Changes the store: allocates the tables and memories into @p pending, not yet in the store,
        /// so an allocation failure leaves no half-built instance registered and no type interned, and
        /// releases the tables and memories it allocated (Store::releaseTrailing); then interns the
        /// types, registers the instance, allocates the rest, initializes it and runs its start
        /// function. noexcept: the store is not exception-safe midway, so running out of memory here
        /// terminates, as it does in invokeChecked().
        [[nodiscard]] std::expected<uint32_t, InstantiationError> commit(ModuleInstance pending) noexcept
        {
            return allocate(pending)
                .and_then([this] { return initializeTables(); })
                .and_then([this] { return initializeMemories(); })
                .and_then([this] { return runStart(); })
                .transform([this] { return _address; });
        }

        /// Allocates @p pending's tables and memories, then registers it and allocates the rest.
        [[nodiscard]] Step allocate(ModuleInstance& pending)
        {
            auto const firstTable = static_cast<TableAddr>(_store.tableCount());
            auto const firstMemory = static_cast<MemAddr>(_store.memoryCount());
            return allocateTables(pending)
                .and_then([&] { return allocateMemories(pending); })
                .or_else([&](InstantiationError const& error) -> Step {
                    _store.releaseTrailing(firstTable, firstMemory);
                    return std::unexpected { error };
                })
                .and_then([&] {
                    internTypes();
                    _address = _store.addModuleInstance(std::move(pending));
                    allocateFunctions();
                    allocateTags();
                    return allocateGlobals();
                })
                .and_then([this] { return fillInitializedTables(); })
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
                auto const address =
                    _store.addModuleFunction(_types.at(module().functions[index]), _address, index);
                instance().functions.push_back(address);
            }
        }

        /// Allocates the module's tags: each a new tag, so that two instances' tags differ.
        void allocateTags()
        {
            for (auto const& tag: module().tags)
            {
                // checkInstanceDemand() found room for every tag.
                auto const address = _store.addTag(_types.at(tag.typeIndex));
                Require(address.has_value());
                instance().tags.push_back(*address);
            }
        }

        [[nodiscard]] Step allocateTables(ModuleInstance& pending)
        {
            for (auto const& table: module().tables)
            {
                auto const type = storeForm(table.type);
                auto const address = _store.addTable(
                    type, Ref { .kind = RefKind::Null, .heap = type.element.heap, .address = 0 });
                if (!address)
                    return std::unexpected { instantiationError(address.error()) };
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
                    return std::unexpected { instantiationError(address.error()) };
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
                instance().globals.push_back(_store.addGlobal(storeForm(global.type), value));
            }
            return {};
        }

        /// Fills every defined table that has an initializer with its value. The tables were
        /// allocated holding the null before the instance existed, so that an allocation failure
        /// leaves none registered; an initializer, which may name the instance's functions and
        /// globals, is evaluated once it does.
        [[nodiscard]] Step fillInitializedTables()
        {
            auto const imported = module().importedCount(ExternKind::Table);
            for (auto const [index, table]: core::views::enumerate(module().tables))
            {
                if (table.init.empty())
                    continue;
                auto const value = evaluate(table.init);
                Require(std::holds_alternative<Ref>(value));
                auto& elements = _store.table(instance().tables.at(imported + index)).elements;
                std::ranges::fill(elements, std::get<Ref>(value));
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

        /// Polls at BulkGranule before every BulkPollElements items of an element segment's
        /// evaluation, the first included, whichever their encoding.
        /// @param item The index of the item about to be evaluated.
        /// @return Nothing to go on, or Trap{Interrupted}.
        [[nodiscard]] std::expected<void, Trap> pollItem(std::size_t item) noexcept
        {
            if (item % BulkPollElements != 0)
                return {};
            return _store.poll(PollSite::BulkGranule);
        }

        /// The references @p segment's items evaluate to.
        [[nodiscard]] std::expected<std::vector<Ref>, Trap> referencesOf(ElementSegment const& segment)
        {
            auto refs = std::vector<Ref> {};
            if (auto const* indices = std::get_if<std::vector<uint32_t>>(&segment.items))
            {
                refs.reserve(indices->size());
                for (auto const [item, index]: core::views::enumerate(*indices))
                {
                    if (auto const polled = pollItem(item); !polled)
                        return std::unexpected { polled.error() };
                    refs.push_back(Ref { .kind = RefKind::Func,
                                         .heap = { .kind = binary::AbstractHeapType::Func },
                                         .address = instance().functions[index] });
                }
                return refs;
            }
            auto const& expressions = std::get<std::vector<ConstExpr>>(segment.items);
            refs.reserve(expressions.size());
            for (auto const [item, expression]: core::views::enumerate(expressions))
            {
                if (auto const polled = pollItem(item); !polled)
                    return std::unexpected { polled.error() };
                auto const value = evaluate(expression);
                Require(std::holds_alternative<Ref>(value));
                refs.push_back(std::get<Ref>(value));
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
                auto const& space = indexSpace(instance(), entry.kind);
                Require(entry.index < space.size());
                instance().exports.emplace_back(
                    entry.name, ExternValue { .kind = entry.kind, .address = space[entry.index] });
            }
        }

        // --- Initialization ---

        /// What a segment copy polls before each granule.
        [[nodiscard]] auto granulePoll() noexcept
        {
            return [this]() noexcept {
                return _store.poll(PollSite::BulkGranule);
            };
        }

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
                    auto const refs = std::span<Ref const> { *instance().elements[index] };
                    auto& elements = _store.table(instance().tables[segment.table]).elements;
                    if (auto const copied = copySegment(refs,
                                                        0,
                                                        refs.size(),
                                                        std::span { elements },
                                                        addressOf(offset),
                                                        TrapCode::OutOfBoundsTableAccess,
                                                        granulePoll());
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
                auto const source = std::span<std::byte const> { segment.bytes };
                auto const bytes = _store.memory(instance().memories[segment.memory]).bytes();
                if (auto const copied = copySegment(source,
                                                    0,
                                                    source.size(),
                                                    bytes,
                                                    addressOf(offset),
                                                    TrapCode::OutOfBoundsMemoryAccess,
                                                    granulePoll());
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
            if (auto const polled = _store.poll(PollSite::Instantiation); !polled)
                return std::unexpected { InstantiationError { polled.error() } };
            auto const function = instance().functions[*module().start];
            return _executor.invoke(_store, function, {}, {}).transform_error([](Abrupt const& abrupt) {
                return std::visit([](auto const& outcome) { return InstantiationError { outcome }; }, abrupt);
            });
        }

        /// Evaluates @p expression in the instance (runtime::evaluateConstantExpression()).
        [[nodiscard]] Value evaluate(ConstExpr const& expression) const
        {
            return evaluateConstantExpression(_store, _address, expression);
        }

        Store& _store;
        ICodeExecutor& _executor;
        std::shared_ptr<validate::ValidatedModule const> _validated;
        std::shared_ptr<Module const> _module; ///< _validated's module.
        std::shared_ptr<ModuleCode const>
            _code; ///< The code the instance keeps; null for the reference engine.
        std::span<ExternValue const> _imports;
        std::vector<TypeId> _types; ///< The TypeId of each of the module's types, by type index.
        uint64_t _newTypeUnits = 0; ///< The type units of the types the store does not hold yet.
        uint32_t _address = 0;
    };

    /// The engine @p code is code of: the reference engine's needs none, so null code is its.
    [[nodiscard]] ExecutionEngine engineOf(ModuleCode const* code) noexcept
    {
        return code ? code->engine() : ExecutionEngine::Reference;
    }
} // namespace

std::expected<uint32_t, InstantiationError> instantiate(
    Store& store,
    ICodeExecutor& executor,
    std::shared_ptr<validate::ValidatedModule const> module,
    std::span<ExternValue const> imports)
{
    return instantiate(store, executor, std::move(module), nullptr, imports);
}

std::expected<uint32_t, InstantiationError> instantiate(
    Store& store,
    ICodeExecutor& executor,
    std::shared_ptr<validate::ValidatedModule const> module,
    std::shared_ptr<ModuleCode const> code,
    std::span<ExternValue const> imports)
{
    return instantiateReporting(store, executor, std::move(module), std::move(code), imports).instance;
}

InstantiationOutcome instantiateReporting(Store& store,
                                          ICodeExecutor& executor,
                                          std::shared_ptr<validate::ValidatedModule const> module,
                                          std::span<ExternValue const> imports)
{
    return instantiateReporting(store, executor, std::move(module), nullptr, imports);
}

InstantiationOutcome instantiateReporting(Store& store,
                                          ICodeExecutor& executor,
                                          std::shared_ptr<validate::ValidatedModule const> module,
                                          std::shared_ptr<ModuleCode const> code,
                                          std::span<ExternValue const> imports)
{
    // The reference engine runs the validated module itself: code that claims to be its is a mix-up.
    Require(code == nullptr || code->engine() != ExecutionEngine::Reference);
    Require(engineOf(code.get()) == executor.engine());
    // One invocation, with a fresh step budget its polls charge and its start function shares, and
    // its own refusal scope, so the start function's refused grows stay in it.
    if (auto const nesting = store.checkInvocationNesting(); !nesting)
        return InstantiationOutcome { .instance = std::unexpected { InstantiationError { nesting.error() } },
                                      .refusedGrowth = {} };
    auto const invocation = InvocationScope { store };
    auto const scope = RefusalScope { store };
    auto const instance = Instantiation { store, executor, std::move(module), std::move(code), imports }.run();
    return InstantiationOutcome { .instance = instance, .refusedGrowth = scope.record() };
}

} // namespace isoline::runtime
