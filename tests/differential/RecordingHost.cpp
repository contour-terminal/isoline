// SPDX-License-Identifier: Apache-2.0
#include "RecordingHost.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "Comparison.hpp"
#include <isoline/binary/Module.hpp>
#include <isoline/binary/Types.hpp>
#include <isoline/runtime/Abrupt.hpp>
#include <isoline/runtime/HostFunction.hpp>
#include <isoline/runtime/Limits.hpp>
#include <isoline/runtime/Store.hpp>
#include <isoline/runtime/Value.hpp>

namespace isoline::differential
{

namespace
{
    /// FNV-1a over 64-bit words, the digest a host function derives its results from.
    constexpr auto DigestBasis = uint64_t { 0xcbf2'9ce4'8422'2325 };
    constexpr auto DigestPrime = uint64_t { 0x0000'0100'0000'01b3 };
    /// One host call in FailureStride, chosen by its arguments' digest, fails.
    constexpr auto FailureStride = uint64_t { 64 };
    /// The embedder's code a failing host call reports.
    constexpr auto HostFailureCode = uint32_t { 0x15 };
    /// One host call in ReentryStride, chosen by its digest, re-enters the guest; one in WriteStride writes a
    /// byte of the caller's memory 0. Each reads its own bits of the digest, so the choices are independent.
    constexpr auto ReentryStride = uint64_t { 16 };
    constexpr auto ReentryShift = 8U;
    constexpr auto WriteStride = uint64_t { 4 };
    constexpr auto WriteShift = 16U;
    /// The digest's bits that choose the export a re-entry calls, and the address a write goes to.
    constexpr auto TargetShift = 32U;

    /// When a host function re-enters the guest.
    enum class Reentry : uint8_t
    {
        BySeed, ///< One call in ReentryStride, chosen by its arguments: every import but the named ones.
        Always, ///< Every call: what an edge module imports to recurse through the host to the limits.
    };

    /// How a host function re-enters the guest: when, and which export it calls.
    struct ReentryRule
    {
        Reentry reentry = Reentry::BySeed; ///< When it re-enters.
        std::string_view target;           ///< The export it calls; empty: one chosen by the call's digest.
    };

    /// The imports whose host functions re-enter otherwise than by the seed, by their field name.
    struct NamedImport
    {
        std::string_view name; ///< The import's field name.
        ReentryRule rule;      ///< How it re-enters.
    };
    constexpr auto NamedImports = std::to_array<NamedImport>({
        { .name = "reenter", .rule = { .reentry = Reentry::Always, .target = {} } },
        { .name = "reenterDeep", .rule = { .reentry = Reentry::Always, .target = "deep" } },
    });

    [[nodiscard]] ReentryRule reentryOf(std::string_view name) noexcept
    {
        auto const row = std::ranges::find(NamedImports, name, &NamedImport::name);
        return row == NamedImports.end() ? ReentryRule {} : row->rule;
    }

    [[nodiscard]] constexpr uint64_t combine(uint64_t digest, uint64_t word) noexcept
    {
        return (digest ^ word) * DigestPrime;
    }

    /// The bits of a value, as words: what a host function's digest reads.
    [[nodiscard]] uint64_t digestOf(uint64_t digest, runtime::Value const& value) noexcept
    {
        return std::visit(
            [digest]<typename T>(T const& alternative) {
                if constexpr (std::is_same_v<T, runtime::V128>)
                    return combine(combine(digest, alternative.lanes[0]), alternative.lanes[1]);
                else if constexpr (std::is_same_v<T, runtime::Ref>)
                    return combine(combine(digest, std::to_underlying(alternative.kind)),
                                   alternative.address);
                else
                    return combine(digest, uint64_t { alternative.bits });
            },
            value);
    }

    /// Whether a value type names a store's TypeId (a concrete heap) or admits no null: what a RecordingHost
    /// cannot make up.
    [[nodiscard]] bool isUnsupported(binary::ValueType const& type) noexcept
    {
        return type.kind == binary::ValueKind::Ref
               && (type.heap.kind == binary::AbstractHeapType::Concrete
                   || type.nullable == binary::Nullable::No);
    }

    [[nodiscard]] bool isUnsupported(binary::FuncType const& type) noexcept
    {
        return std::ranges::any_of(type.params, [](auto const& param) { return isUnsupported(param); })
               || std::ranges::any_of(type.results, [](auto const& result) { return isUnsupported(result); });
    }

    /// A value of @p type derived from @p digest: its bits, or the null of its heap.
    [[nodiscard]] runtime::Value derivedValue(binary::ValueType const& type, uint64_t digest) noexcept
    {
        switch (type.kind)
        {
            case binary::ValueKind::I32: return runtime::I32 { static_cast<uint32_t>(digest) };
            case binary::ValueKind::I64: return runtime::I64 { digest };
            case binary::ValueKind::F32: return runtime::F32 { static_cast<uint32_t>(digest) };
            case binary::ValueKind::F64: return runtime::F64 { digest };
            case binary::ValueKind::V128:
                return runtime::V128 { .lanes = { digest, combine(digest, digest) } };
            case binary::ValueKind::Ref: break;
        }
        return runtime::Ref { .kind = runtime::RefKind::Null, .heap = type.heap };
    }

    /// Calls an export of the caller instance, @p target or one chosen by @p digest, with arguments derived
    /// from the digest, as an embedder would from a host function; records the call and how it ended, which
    /// the host swallows. An export of a type the host cannot make arguments for is not chosen.
    void reenter(runtime::HostContext& context,
                 uint32_t importIndex,
                 std::string_view target,
                 uint64_t digest,
                 HostLog& log)
    {
        auto const caller = context.callerInstance();
        if (!caller)
            return;
        if (log.reentriesLeft == 0)
        {
            log.trace.push_back(
                std::format("{} reenters nothing: the re-entry budget is spent", importIndex));
            return;
        }
        --log.reentriesLeft;
        auto& trace = log.trace;
        auto const& store = context.store();
        auto targets = std::vector<std::pair<std::string_view, runtime::FuncAddr>> {};
        for (auto const& [name, value]: store.moduleInstance(*caller).exports)
        {
            if (value.kind == binary::ExternKind::Func && !isUnsupported(store.functionType(value.address))
                && (target.empty() || name == target))
                targets.emplace_back(name, value.address);
        }
        if (targets.empty())
            return;
        auto const& [name, function] = targets[(digest >> TargetShift) % targets.size()];
        auto const& type = store.functionType(function);
        auto args = std::vector<runtime::Value> {};
        for (auto const index: std::views::iota(std::size_t { 0 }, type.params.size()))
            args.push_back(derivedValue(type.params[index], combine(digest, index)));
        auto results = std::vector<runtime::Value>(type.results.size());
        auto const outcome = context.invoke(function, args, results);
        auto const ended = outcome ? CallOutcome { std::move(results) } : CallOutcome { outcome.error() };
        trace.push_back(std::format("{} reenters {}({}): {}",
                                    importIndex,
                                    name,
                                    describe(std::span<runtime::Value const> { args }),
                                    describe(ended)));
    }

    /// Writes a byte derived from @p digest into the caller's memory 0, at an address derived from it too,
    /// so that the guest reads what a host wrote between its own accesses; records the write.
    void writeCallerMemory(runtime::HostContext& context,
                           uint32_t importIndex,
                           uint64_t digest,
                           std::vector<std::string>& trace)
    {
        auto const memory = context.callerMemory(0);
        if (!memory || memory->empty())
            return;
        auto const address = (digest >> TargetShift) % memory->size();
        auto const byte = static_cast<uint8_t>(digest);
        (*memory)[address] = std::byte { byte };
        trace.push_back(std::format("{} writes {:#04x} at {}", importIndex, byte, address));
    }

    /// The host function bound to function import @p importIndex of type @p type, which re-enters the guest
    /// as @p reentry says.
    [[nodiscard]] std::shared_ptr<runtime::IHostFunction> recordingFunction(uint32_t importIndex,
                                                                            binary::FuncType type,
                                                                            ReentryRule reentry,
                                                                            std::shared_ptr<HostLog> log)
    {
        return runtime::makeHostFunction(
            [importIndex, type = std::move(type), reentry, log = std::move(log)](
                runtime::HostContext& context,
                std::span<runtime::Value const> args,
                std::span<runtime::Value> results) noexcept -> std::expected<void, runtime::Abrupt> {
                auto digest = combine(DigestBasis, importIndex);
                for (auto const& argument: args)
                    digest = digestOf(digest, argument);
                if (digest % FailureStride == 0)
                {
                    log->trace.push_back(std::format("{}({}) -> host failure", importIndex, describe(args)));
                    return std::unexpected { runtime::HostFailure { .kind =
                                                                        runtime::HostFailureKind::Embedder,
                                                                    .code = HostFailureCode,
                                                                    .mismatch = std::nullopt } };
                }
                if (reentry.reentry == Reentry::Always || (digest >> ReentryShift) % ReentryStride == 0)
                    reenter(context, importIndex, reentry.target, digest, *log);
                if ((digest >> WriteShift) % WriteStride == 0)
                    writeCallerMemory(context, importIndex, digest, log->trace);
                for (auto const index: std::views::iota(std::size_t { 0 }, results.size()))
                {
                    digest = combine(digest, index);
                    results[index] = derivedValue(type.results[index], digest);
                }
                log->trace.push_back(std::format("{}({}) -> {}",
                                                 importIndex,
                                                 describe(args),
                                                 describe(std::span<runtime::Value const> { results })));
                return {};
            });
    }
} // namespace

std::expected<void, UnsupportedImport> checkImports(binary::Module const& module)
{
    for (auto const index: std::views::iota(uint32_t { 0 }, static_cast<uint32_t>(module.imports.size())))
    {
        auto const& import = module.imports[index];
        auto unsupported = false;
        switch (import.kind)
        {
            case binary::ExternKind::Func:
            case binary::ExternKind::Tag: unsupported = isUnsupported(module.types[import.typeIndex]); break;
            case binary::ExternKind::Table: unsupported = isUnsupported(import.table.element); break;
            case binary::ExternKind::Global: unsupported = isUnsupported(import.global.type); break;
            case binary::ExternKind::Memory: break;
        }
        if (unsupported)
            return std::unexpected { UnsupportedImport { .importIndex = index } };
    }
    return {};
}

RecordingHost::RecordingHost(): _log { std::make_shared<HostLog>() }
{
}

std::expected<std::vector<runtime::ExternValue>, ImportRefusal> RecordingHost::bind(
    runtime::Store& store, binary::Module const& module)
{
    auto externs = std::vector<runtime::ExternValue> {};
    externs.reserve(module.imports.size());
    for (auto const index: std::views::iota(uint32_t { 0 }, static_cast<uint32_t>(module.imports.size())))
    {
        auto const& import = module.imports[index];
        auto address = uint32_t { 0 };
        switch (import.kind)
        {
            case binary::ExternKind::Func: {
                auto const& type = module.types[import.typeIndex];
                auto const function =
                    store.addHostFunction(type, recordingFunction(index, type, reentryOf(import.name), _log));
                if (!function)
                    return std::unexpected { ImportRefusal { function.error() } };
                address = *function;
                break;
            }
            case binary::ExternKind::Table: {
                auto const table = store.addTable(
                    import.table,
                    runtime::Ref { .kind = runtime::RefKind::Null, .heap = import.table.element.heap });
                if (!table)
                    return std::unexpected { std::visit(
                        [](auto const& error) { return ImportRefusal { error }; }, table.error()) };
                address = *table;
                break;
            }
            case binary::ExternKind::Memory: {
                auto const memory = store.addMemory(import.memory);
                if (!memory)
                    return std::unexpected { std::visit(
                        [](auto const& error) { return ImportRefusal { error }; }, memory.error()) };
                address = *memory;
                break;
            }
            case binary::ExternKind::Global:
                address = store.addGlobal(import.global,
                                          derivedValue(import.global.type, combine(DigestBasis, index)));
                break;
            case binary::ExternKind::Tag: {
                auto const tag = store.addTag(store.internType(module.types[import.typeIndex]));
                if (!tag)
                    return std::unexpected { ImportRefusal { tag.error() } };
                address = *tag;
                break;
            }
        }
        externs.push_back(runtime::ExternValue { .kind = import.kind, .address = address });
    }
    return externs;
}

std::vector<std::string> RecordingHost::takeTrace()
{
    _log->reentriesLeft = MaxReentriesPerCall;
    return std::exchange(_log->trace, {});
}

} // namespace isoline::differential
