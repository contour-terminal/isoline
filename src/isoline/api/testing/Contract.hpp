// SPDX-License-Identifier: Apache-2.0
//
// Test-only: what Contour's WebAssembly backend contract asks of an engine, set up against Isoline's
// public api alone, for the [contract] cases (Contract_test.cpp, posix/ContractSignals_test.cpp) and
// the threading cases (Threads_test.cpp). The modules are built from the WebAssembly text each one
// documents; no Contour code is used.
#pragma once

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <expected>
#include <format>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <semaphore>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include <isoline/api/CallError.hpp>
#include <isoline/api/Engine.hpp>
#include <isoline/api/Imports.hpp>
#include <isoline/api/Module.hpp>
#include <isoline/api/Store.hpp>
#include <isoline/api/Types.hpp>
#include <isoline/api/testing/EngineRows.hpp>
#include <isoline/binary/testing/Code.hpp>
#include <isoline/binary/testing/ModuleBuilder.hpp>

namespace isoline::api::testing::contract
{

/// The import module the contract's host functions are defined under.
inline constexpr auto HostModule = std::string_view { "contract:test/host@0" };

/// The WebAssembly page size, in bytes.
inline constexpr auto PageBytes = uint64_t { 64 } * 1024;

/// How long a contract case waits for another thread at most, before it gives up and fails.
inline constexpr auto ThreadDeadline = std::chrono::seconds { 30 };

/// The bytes of @p raw, as Engine::load() takes them.
/// @param raw The module's bytes.
/// @return The same bytes.
[[nodiscard]] inline std::vector<std::byte> bytesOf(std::span<uint8_t const> raw)
{
    auto bytes = std::vector<std::byte>(raw.size());
    std::ranges::transform(raw, bytes.begin(), [](uint8_t byte) { return std::byte { byte }; });
    return bytes;
}

// {{{ modules

constexpr auto TypeI32 = binary::I32;

/// A size of @p count pages (or elements, for a table), without a maximum, of 32-bit addresses.
/// @param count The initial size.
/// @return The limits.
[[nodiscard]] inline binary::Limits atLeast(uint64_t count)
{
    return binary::Limits { .min = count, .max = std::nullopt, .addressType = binary::AddressType::I32 };
}

/// i32.add.
[[nodiscard]] inline binary::testing::Bytes i32Add()
{
    return { 0x6A };
}

/// i32.store with natural alignment.
[[nodiscard]] inline binary::testing::Bytes i32Store()
{
    return binary::testing::memoryAccess(0x36, 2, 0);
}

/// Imports both host functions, exports a memory and a function.
///
///     (module
///       (import "contract:test/host@0" "ping" (func (param i32)))
///       (import "contract:test/host@0" "pair" (func (param i32 i32)))
///       (memory (export "memory") 1)
///       (func (export "add") (param i32 i32) (result i32) (i32.add (local.get 0) (local.get 1))))
[[nodiscard]] inline binary::testing::ModuleBuilder describedModule()
{
    using namespace binary::testing;
    auto builder = ModuleBuilder {};
    builder.type(FuncType { .params = { TypeI32 }, .results = {} })
        .type(FuncType { .params = { TypeI32, TypeI32 }, .results = {} })
        .type(FuncType { .params = { TypeI32, TypeI32 }, .results = { TypeI32 } })
        .importFunction(HostModule, "ping", 0)
        .importFunction(HostModule, "pair", 1)
        .memory(atLeast(1))
        .function(2, {}, code({ localGet(0), localGet(1), i32Add() }))
        .exportEntity("memory", ExternKind::Memory, 0)
        .exportFunction("add", 2);
    return builder;
}

/// Exports only "add": (func (export "add") (param i32 i32) (result i32) ...), nothing imported.
[[nodiscard]] inline binary::testing::ModuleBuilder addModule()
{
    using namespace binary::testing;
    auto builder = ModuleBuilder {};
    builder.type(FuncType { .params = { TypeI32, TypeI32 }, .results = { TypeI32 } })
        .function(0, {}, code({ localGet(0), localGet(1), i32Add() }))
        .exportFunction("add", 0);
    return builder;
}

/// What the instance cases run: calls `ping`, adds, stores, grows its memory, recurses and spins.
/// `ping-then-store` calls `ping(42)`, then stores 1 at address 0; `grow` traps if its memory refuses
/// to grow by 4 pages; `grow-by` returns what memory.grow does; `store` stores a word.
///
///     (module
///       (import "contract:test/host@0" "ping" (func $ping (param i32)))
///       (memory (export "memory") 1)
///       (data (i32.const 16) "hello")
///       (func (export "add") (param i32 i32) (result i32) (i32.add (local.get 0) (local.get 1)))
///       (func (export "ping-then-store") (call $ping (i32.const 42)) (i32.store (i32.const 0) (i32.const
///       1))) (func (export "ping-then-add") (param i32 i32) (result i32)
///         (call $ping (i32.const 42)) (i32.add (local.get 0) (local.get 1)))
///       (func (export "grow") (if (i32.eq (memory.grow (i32.const 4)) (i32.const -1)) (then unreachable)))
///       (func $recurse (export "recurse") (call $recurse))
///       (func (export "spin") (loop (br 0)))
///       (func (export "grow-by") (param i32) (result i32) (memory.grow (local.get 0)))
///       (func (export "store") (param i32 i32) (i32.store (local.get 0) (local.get 1))))
[[nodiscard]] inline binary::testing::ModuleBuilder instanceModule()
{
    using namespace binary::testing;
    auto builder = ModuleBuilder {};
    builder.type(FuncType { .params = { TypeI32 }, .results = {} })
        .type(FuncType { .params = { TypeI32, TypeI32 }, .results = { TypeI32 } })
        .type(FuncType {})
        .type(FuncType { .params = { TypeI32 }, .results = { TypeI32 } })
        .type(FuncType { .params = { TypeI32, TypeI32 }, .results = {} })
        .importFunction(HostModule, "ping", 0)
        .memory(atLeast(1))
        .function(1, {}, code({ localGet(0), localGet(1), i32Add() }))
        .function(2, {}, code({ i32Const(42), call(0), i32Const(0), i32Const(1), i32Store() }))
        .function(1, {}, code({ i32Const(42), call(0), localGet(0), localGet(1), i32Add() }))
        .function(2,
                  {},
                  code({ i32Const(4),
                         memoryGrow(),
                         i32Const(-1),
                         { 0x46, 0x04, 0x40 }, // i32.eq; if
                         unreachable(),
                         { 0x0B } }))
        .function(2, {}, call(5))
        .function(2, {}, loop(br(0)))
        .function(3, {}, code({ localGet(0), memoryGrow() }))
        .function(4, {}, code({ localGet(0), localGet(1), i32Store() }))
        .exportEntity("memory", ExternKind::Memory, 0)
        .exportFunction("add", 1)
        .exportFunction("ping-then-store", 2)
        .exportFunction("ping-then-add", 3)
        .exportFunction("grow", 4)
        .exportFunction("recurse", 5)
        .exportFunction("spin", 6)
        .exportFunction("grow-by", 7)
        .exportFunction("store", 8)
        .data(code({ { 0x00 }, i32Const(16), { 0x0B, 0x05, 'h', 'e', 'l', 'l', 'o' } }));
    return builder;
}

// }}}

// {{{ host functions

/// One host function call, as a HostLog saw it.
struct HostCall
{
    std::string function;            ///< The host function's name: "ping" or "pair".
    std::vector<uint32_t> arguments; ///< Its i32 arguments, in order.
    std::thread::id thread;          ///< The thread it ran on.

    /// Equal when name, arguments and thread are.
    bool operator==(HostCall const&) const = default;
};

/// How the contract's host functions answer.
enum class HostAnswer : uint8_t
{
    Return, ///< They return.
    Fail,   ///< They fail with the embedder's code 1.
};

/// The embedder's own failure: what a host function answering HostAnswer::Fail returns.
[[nodiscard]] inline HostFailure embedderFailure()
{
    return HostFailure { .kind = HostFailureKind::Embedder, .code = 1, .mismatch = std::nullopt };
}

/// Records every call of the contract's host functions, `ping` and `pair`. Thread-safe: one Imports
/// with them may be shared by stores on several threads.
class HostLog
{
  public:
    /// Records a call of @p function with @p args.
    void record(std::string_view function, std::span<Value const> args)
    {
        auto call = HostCall { .function = std::string { function },
                               .arguments = {},
                               .thread = std::this_thread::get_id() };
        for (auto const& arg: args)
            call.arguments.push_back(std::get<I32>(arg).bits);
        auto const lock = std::scoped_lock { _mutex };
        _calls.push_back(std::move(call));
    }

    /// The calls so far, in order.
    [[nodiscard]] std::vector<HostCall> calls() const
    {
        auto const lock = std::scoped_lock { _mutex };
        return _calls;
    }

  private:
    mutable std::mutex _mutex;
    std::vector<HostCall> _calls;
};

/// The contract's host functions, `ping` (i32) and `pair` (i32, i32), recording into @p log.
/// @param log Where they record; kept alive by them.
/// @param answer How they answer.
/// @return The definitions.
[[nodiscard]] inline Imports hostImports(std::shared_ptr<HostLog> const& log,
                                         HostAnswer answer = HostAnswer::Return)
{
    auto const recording = [&](std::string_view name) {
        return makeHostFunction(
            [log, answer, name](HostContext&,
                                std::span<Value const> args,
                                std::span<Value>) noexcept -> std::expected<void, Abrupt> {
                log->record(name, args);
                if (answer == HostAnswer::Fail)
                    return std::unexpected { Abrupt { embedderFailure() } };
                return {};
            });
    };
    auto imports = Imports {};
    imports.define(std::string { HostModule },
                   "ping",
                   FuncType { .params = { TypeI32 }, .results = {} },
                   recording("ping"));
    imports.define(std::string { HostModule },
                   "pair",
                   FuncType { .params = { TypeI32, TypeI32 }, .results = {} },
                   recording("pair"));
    return imports;
}

// }}}

/// An engine and a store of the contract's limits, and its host functions: what Contour calls a runtime.
class Runtime
{
  public:
    /// A runtime of @p config whose memories @p allocator allocates.
    /// @param config The engine's configuration, with the store's caps.
    /// @param allocator Allocates the store's memories; a heap allocator when null.
    /// @param answer How the host functions answer.
    /// @param observer Observes the store's polls; none when null. Must outlive the runtime.
    explicit Runtime(EngineConfig const& config = configOf({}),
                     std::unique_ptr<IMemoryAllocator> allocator = nullptr,
                     HostAnswer answer = HostAnswer::Return,
                     IPollObserver* observer = nullptr):
        _engine { config },
        _allocator { allocator ? std::move(allocator) : std::make_unique<HeapMemoryAllocator>() },
        _log { std::make_shared<HostLog>() },
        _imports { hostImports(_log, answer) },
        _store { _engine, *_allocator, observer }
    {
    }

    /// The engine.
    [[nodiscard]] Engine const& engine() const noexcept { return _engine; }
    /// The store.
    [[nodiscard]] Store& store() noexcept { return _store; }
    /// The host functions' definitions.
    [[nodiscard]] Imports const& imports() const noexcept { return _imports; }
    /// What the host functions recorded.
    [[nodiscard]] HostLog const& log() const noexcept { return *_log; }

    /// Loads @p builder's module, which must load.
    [[nodiscard]] Module load(binary::testing::ModuleBuilder const& builder) const
    {
        auto module = _engine.load(builder.build());
        REQUIRE(module.has_value());
        return *std::move(module);
    }

    /// Instantiates @p module with the host functions, which must succeed.
    [[nodiscard]] Instance instantiate(Module const& module)
    {
        auto instance = _store.instantiate(module, _imports);
        REQUIRE(instance.has_value());
        return *instance;
    }

  private:
    Engine _engine;
    std::unique_ptr<IMemoryAllocator> _allocator;
    std::shared_ptr<HostLog> _log;
    Imports _imports;
    Store _store; ///< Last: it uses the engine and the allocator.
};

/// Instance @p instance's exported function @p name, which must exist.
[[nodiscard]] inline Function functionOf(Instance const& instance, std::string_view name)
{
    auto const function = instance.function(name);
    REQUIRE(function.has_value());
    return *function;
}

/// Instance @p instance's memory exported as "memory", which must exist.
[[nodiscard]] inline Memory memoryOf(Instance const& instance)
{
    auto const memory = instance.memory("memory");
    REQUIRE(memory.has_value());
    return *memory;
}

/// Calls @p function of @p instance with i32 @p args, asserting nothing: safe on a thread other than the
/// test thread.
/// @return Its i32 results, or how it failed.
[[nodiscard]] inline std::expected<std::vector<uint32_t>, CallError> callI32(
    Instance const& instance, Function const& function, std::span<uint32_t const> args = {})
{
    auto values = std::vector<Value> {};
    for (auto const arg: args)
        values.emplace_back(I32 { arg });
    auto results = std::vector<Value>(function.type().results.size());
    return instance.call(function, values, results).transform([&] {
        auto words = std::vector<uint32_t> {};
        for (auto const& result: results)
            words.push_back(std::get<I32>(result).bits);
        return words;
    });
}

/// Calls @p instance's export @p name, which must exist, with i32 @p args; on the test thread only, as
/// it asserts the export.
/// @return Its i32 results, or how it failed.
[[nodiscard]] inline std::expected<std::vector<uint32_t>, CallError> callI32(
    Instance const& instance, std::string_view name, std::span<uint32_t const> args = {})
{
    return callI32(instance, functionOf(instance, name), args);
}

/// The 32-bit little-endian word at @p address of @p instance's memory, which must be there.
[[nodiscard]] inline uint32_t wordAt(Instance const& instance, uint64_t address)
{
    auto const view = instance.memoryRange(memoryOf(instance), address, 4);
    REQUIRE(view.has_value());
    auto word = uint32_t { 0 };
    for (auto const index: std::views::iota(std::size_t { 0 }, std::size_t { 4 }))
        word |= std::to_integer<uint32_t>((*view)[index]) << (index * 8);
    return word;
}

/// A std::thread joined on destruction: not a std::jthread, which Apple's libc++ lacks. Whatever it
/// runs must end on its own (bounded waits only), so the join is bounded too.
class JoiningThread
{
  public:
    /// Starts @p body on a thread of its own.
    /// @param body What the thread runs; it may be move-only.
    template <typename Body>
    explicit JoiningThread(Body body): _thread { std::move(body) }
    {
    }
    JoiningThread(JoiningThread const&) = delete;
    JoiningThread(JoiningThread&&) = delete;
    JoiningThread& operator=(JoiningThread const&) = delete;
    JoiningThread& operator=(JoiningThread&&) = delete;
    ~JoiningThread() { join(); }

    /// Waits for the thread to end; again, does nothing.
    void join()
    {
        if (_thread.joinable())
            _thread.join();
    }

  private:
    std::thread _thread;
};

/// What @p future holds, waiting at most ThreadDeadline for it. A thread past the deadline means the
/// engine deadlocked: the process exits with a message, as the test cannot fail without joining the
/// thread, which would hang it. It exits through std::_Exit(), never std::abort(): an MSVC Debug CRT
/// turns an abort into a dialog that waits for a click, which hangs the suite until its timeout.
/// @param future The other thread's result.
/// @param what What that thread does, for the message.
/// @return The result.
template <typename T>
[[nodiscard]] T awaitOrExit(std::future<T>& future, std::string_view what)
{
    if (future.wait_for(ThreadDeadline) != std::future_status::ready)
    {
        auto const message =
            std::format("{}: no result within {} s; the engine deadlocked\n", what, ThreadDeadline.count());
        std::fputs(message.c_str(), stderr);
        std::fflush(stderr);
        std::_Exit(EXIT_FAILURE);
    }
    return future.get();
}

/// A body run on a thread of its own, whose result the test thread collects within ThreadDeadline.
///
/// A thread of its own, never a std::async() task: MSVC's STL runs those on the Windows thread pool,
/// which starts a queued task only once it sees a worker free, so tasks that wait for each other (a
/// StartGate) can each sit in the queue past every deadline. Catch2's assertions are not thread-safe:
/// the body records, the test thread checks.
template <typename T>
class Worker
{
  public:
    /// Starts @p body on a thread of its own.
    /// @param body What the thread runs; its result is the worker's.
    template <typename Body>
    explicit Worker(Body body):
        _task { std::move(body) }, _result { _task.get_future() }, _thread { [this] { _task(); } }
    {
    }
    Worker(Worker const&) = delete;
    Worker(Worker&&) = delete;
    Worker& operator=(Worker const&) = delete;
    Worker& operator=(Worker&&) = delete;
    ~Worker() = default;

    /// What the body returned (or threw), waiting at most ThreadDeadline for it (awaitOrExit()).
    /// @param what What the body does, for the message when it misses the deadline.
    /// @return The body's result.
    [[nodiscard]] T result(std::string_view what) { return awaitOrExit(_result, what); }

  private:
    std::packaged_task<T()> _task; ///< The body; run once, on _thread.
    std::future<T> _result;        ///< What the body returned or threw.
    JoiningThread _thread;         ///< Last: joined before the task it runs is destroyed.
};

/// A worker's result type is what its body returns.
template <typename Body>
Worker(Body) -> Worker<std::invoke_result_t<Body&>>;

/// Runs @p body on a thread of its own, prepared by @p engine, and hands back what it returned. Catch2's
/// assertions are not thread-safe: the thread records, the test thread checks.
/// @return What @p body returned; nothing if the thread could not be prepared.
template <typename Body>
[[nodiscard]] auto onPreparedThread(Engine const& engine, Body const& body)
{
    auto worker = Worker { [&] {
        auto result = std::optional<decltype(body())> {};
        auto const preparation = engine.prepareThread();
        if (preparation.has_value())
            result.emplace(body());
        return result;
    } };
    return worker.result("a prepared thread");
}

/// The steps a call that spins may take: a backstop far above what a working interrupt lets it take
/// (HandoverInterrupt::BackEdges and a few more), so that a broken interrupt fails its case within
/// seconds instead of hanging the suite.
inline constexpr auto SpinBackstopSteps = uint64_t { 1 } << 24;

/// How a HandoverInterrupt's interrupting thread ended its wait.
enum class Handover : uint8_t
{
    TimedOut,    ///< No handover came before the deadline, or the case finished first.
    Interrupted, ///< It interrupted the store at the handover.
};

/// Interrupts a store from another thread at a fixed point of a spinning call, so that the steps the
/// call takes do not depend on how threads are scheduled: at the BackEdges-th loop back-edge the
/// executing thread hands over and waits (at most ThreadDeadline) until the interrupting thread has
/// interrupted the store; the next poll then sees the interrupt. A broken interrupt lets the call spin
/// on, into SpinBackstopSteps.
class HandoverInterrupt final: public IPollObserver
{
  public:
    /// The loop back-edge at which the executing thread hands over.
    static constexpr auto BackEdges = uint64_t { 1'000 };

    void onPoll(PollSite site) noexcept override
    {
        if (site != PollSite::LoopBackEdge || ++_backEdges != BackEdges)
            return;
        _reached.release();
        [[maybe_unused]] auto const interrupted = _interrupted.try_acquire_for(ThreadDeadline);
    }

    /// On the interrupting thread: waits for the handover, interrupts @p interrupter, and hands back.
    /// @param interrupter The store's interrupter.
    /// @return Interrupted, or TimedOut when no handover came (or finish() was called first).
    [[nodiscard]] Handover interruptAtHandover(Interrupter& interrupter)
    {
        if (!_reached.try_acquire_for(ThreadDeadline) || _finished.load(std::memory_order_acquire))
            return Handover::TimedOut;
        interrupter.interrupt();
        _interrupted.release();
        return Handover::Interrupted;
    }

    /// On the test thread, once the call returned: wakes an interrupting thread still waiting, so that
    /// joining it never waits for the deadline.
    void finish() noexcept
    {
        _finished.store(true, std::memory_order_release);
        _reached.release();
    }

  private:
    uint64_t _backEdges = 0;                   ///< Counted on the executing thread only.
    std::atomic<bool> _finished { false };     ///< Set by finish().
    std::counting_semaphore<2> _reached { 0 }; ///< Released at the handover, and by finish().
    std::binary_semaphore _interrupted { 0 };  ///< Released once the store was interrupted.
};

/// How a StartGate's wait ended.
enum class GateOutcome : uint8_t
{
    TimedOut, ///< ThreadDeadline passed before every thread arrived.
    Opened,   ///< Every thread arrived.
};

/// Lets several threads start together, each waiting at most ThreadDeadline for the others: a bounded
/// std::latch, whose outcome each thread records for the test thread to check.
class StartGate
{
  public:
    /// A gate for @p threads threads.
    explicit StartGate(int threads) noexcept: _waiting { threads } {}

    /// Arrives, and waits for the others or the deadline.
    /// @return Opened once every thread arrived; TimedOut when the deadline passed first, a failure of
    ///         the case.
    [[nodiscard]] GateOutcome arriveAndWait()
    {
        auto lock = std::unique_lock { _mutex };
        if (--_waiting <= 0)
        {
            _opened.notify_all();
            return GateOutcome::Opened;
        }
        return _opened.wait_for(lock, ThreadDeadline, [this] { return _waiting <= 0; })
                   ? GateOutcome::Opened
                   : GateOutcome::TimedOut;
    }

  private:
    std::mutex _mutex;               ///< Guards _waiting.
    std::condition_variable _opened; ///< Notified once _waiting reaches 0.
    int _waiting;                    ///< The threads yet to arrive.
};

} // namespace isoline::api::testing::contract
