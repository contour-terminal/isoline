// SPDX-License-Identifier: Apache-2.0
#include <isoline/fastinterpreter/Code.hpp>

#include <core/Assert.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <memory>
#include <memory_resource>
#include <ranges>
#include <span>
#include <utility>
#include <vector>

#include <isoline/fastinterpreter/Backends.hpp>

namespace isoline::fastinterpreter
{

namespace
{
    /// Whether every container of @p function allocates from @p arena.
    [[nodiscard]] bool allocatesFrom(LoweredFunction const& function,
                                     std::pmr::memory_resource const* arena) noexcept
    {
        auto const resources = std::array {
            function.code.get_allocator().resource(),      function.localInit.get_allocator().resource(),
            function.operands.get_allocator().resource(),  function.sites.get_allocator().resource(),
            function.handlers.get_allocator().resource(),  function.catches.get_allocator().resource(),
            function.constants.get_allocator().resource(),
        };
        return std::ranges::count(resources, arena) == std::ssize(resources);
    }

    /// Whether @p functions and every container of each allocate from @p arena.
    [[nodiscard]] bool allocateFrom(std::pmr::memory_resource const* arena,
                                    LoweringVector<LoweredFunction> const& functions) noexcept
    {
        return functions.get_allocator().resource() == arena
               && std::ranges::all_of(functions, [arena](LoweredFunction const& function) {
                      return allocatesFrom(function, arena);
                  });
    }

    /// One handler's header word and its operation: what operationOfHeader() looks TailCall headers up in.
    struct HeaderOperation
    {
        Word header;           ///< The handler's address.
        OperationId operation; ///< Its operation.
    };

    /// The TailCall header words with their operations, sorted by word; empty where TailCall is unavailable.
    [[nodiscard]] std::span<HeaderOperation const> tailCallHeaderOperations()
    {
        static auto const sorted = [] {
            auto const words = tailCallHeaderWords();
            auto entries = std::vector<HeaderOperation> {};
            entries.reserve(words.size());
            for (auto const index: std::views::iota(std::size_t { 0 }, words.size()))
                entries.push_back({ .header = words[index], .operation = static_cast<OperationId>(index) });
            std::ranges::sort(entries, {}, &HeaderOperation::header);
            // Distinct handlers at one address (identical code folding) would name two operations alike.
            Require(std::ranges::adjacent_find(entries, {}, &HeaderOperation::header) == entries.end());
            return entries;
        }();
        return sorted;
    }
} // namespace

Word headerWord(Dispatch dispatch, OperationId operation)
{
    switch (dispatch)
    {
        case Dispatch::Switch: return Word { std::to_underlying(operation) };
        case Dispatch::TailCall: break;
    }
    auto const words = tailCallHeaderWords();
    Require(words.size() == OperationCount);
    return words[std::to_underlying(operation)];
}

OperationId operationOfHeader(Word header)
{
    if (header < OperationCount)
        return static_cast<OperationId>(header);
    auto const entries = tailCallHeaderOperations();
    auto const found = std::ranges::lower_bound(entries, header, {}, &HeaderOperation::header);
    Require(found != entries.end() && found->header == header);
    return found->operation;
}

std::size_t operationExtent(OperationId operation, std::span<Word const> code)
{
    auto const& row = operationRow(operation);
    Require(row.words <= code.size());
    if (row.form != OperationForm::BranchTable)
        return row.words;
    // Its last immediate is the number of labels before the default, whose record follows theirs.
    auto const records = (code[row.words - 1] + 1) * BranchTableEntryWords;
    Require(records <= code.size() - row.words);
    return row.words + records;
}

LoweredModule::LoweredModule(std::shared_ptr<std::pmr::memory_resource> arena,
                             LoweringVector<LoweredFunction> functions,
                             Dispatch dispatch,
                             runtime::ExecutionMode mode,
                             uint64_t loweredBytes,
                             LoweringStats stats):
    _arena { std::move(arena) },
    _functions { std::move(functions) },
    _dispatch { dispatch },
    _mode { mode },
    _loweredBytes { loweredBytes },
    _stats { stats }
{
    Require(allocateFrom(_arena.get(), _functions));
}

runtime::ExecutionEngine LoweredModule::engine() const noexcept
{
    return runtime::ExecutionEngine::Fast;
}

std::span<LoweredFunction const> LoweredModule::functions() const noexcept
{
    return _functions;
}

Dispatch LoweredModule::dispatch() const noexcept
{
    return _dispatch;
}

runtime::ExecutionMode LoweredModule::mode() const noexcept
{
    return _mode;
}

uint64_t LoweredModule::loweredBytes() const noexcept
{
    return _loweredBytes;
}

LoweringStats const& LoweredModule::stats() const noexcept
{
    return _stats;
}

} // namespace isoline::fastinterpreter
