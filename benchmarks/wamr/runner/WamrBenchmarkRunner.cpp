// SPDX-License-Identifier: Apache-2.0
//
// isoline-benchmark-wamr <module.wasm> <iterations> [--invocations <count>]
//
// The WAMR counterpart of isoline-benchmark (benchmarks/BenchmarkRunner.cpp), with the same command line and
// the same JSON object on stdout: it loads a benchmark guest into WAMR's interpreter as the variant it was
// built as (ISOLINE_WAMR_VARIANT: contour, with Contour's patches and options, or default), calls its
// run(iterations) <count> times and reports the checksum and the load and invocation times by the steady
// clock. scripts/benchmark.py times the whole process with its own clock. A composition root, like
// BenchmarkRunner's main: the only place here that reads the clock and the file system.
#include <array>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <expected>
#include <format>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <wasm_export.h>

namespace
{

using Clock = std::chrono::steady_clock;

/// How the process ends, as scripts/benchmark.py reads it: the same statuses as isoline-benchmark's.
enum class ExitStatus : uint8_t
{
    Success = 0,   ///< The guest ran; the JSON object is on stdout.
    RunFailed = 1, ///< The guest could not be read, loaded or run.
    Usage = 2,     ///< The command line is wrong.
};

/// The WAMR stack of the instance and of its execution environment: deep enough for the guests' recursion
/// (shootout-ackermann), which Isoline bounds by StoreLimits::maxCallDepth.
constexpr auto StackBytes = uint32_t { 8U * 1024U * 1024U };

/// What the command line asks for.
struct Arguments
{
    std::string module;       ///< The guest's path.
    uint32_t iterations = 0;  ///< run()'s argument.
    uint32_t invocations = 1; ///< How often run() is called.
};

/// @p text as a whole unsigned 32-bit number, or nothing.
std::optional<uint32_t> numberOf(std::string_view text)
{
    auto value = uint32_t { 0 };
    auto const [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc {} || end != text.data() + text.size())
        return std::nullopt;
    return value;
}

/// The arguments of @p argv, or the message that refuses them.
std::expected<Arguments, std::string> parse(std::span<char const* const> argv)
{
    auto arguments = Arguments {};
    auto positional = std::vector<std::string_view> {};
    auto rest = argv.subspan(1);
    while (!rest.empty())
    {
        auto const word = std::string_view { rest.front() };
        rest = rest.subspan(1);
        if (!word.starts_with("--"))
        {
            positional.push_back(word);
            continue;
        }
        if (word != "--invocations" || rest.empty())
            return std::unexpected { std::format("unknown option, or one without its value: {}", word) };
        auto const count = numberOf(rest.front());
        rest = rest.subspan(1);
        if (!count || *count == 0)
            return std::unexpected { std::string { "--invocations takes a positive number" } };
        arguments.invocations = *count;
    }
    if (positional.size() != 2)
        return std::unexpected { std::string { "expected a module and an iteration count" } };
    arguments.module = std::string { positional[0] };
    auto const iterations = numberOf(positional[1]);
    if (!iterations || *iterations > static_cast<uint32_t>(std::numeric_limits<int32_t>::max()))
        return std::unexpected { std::string { "the iteration count is no number from 0 to 2^31-1" } };
    arguments.iterations = *iterations;
    return arguments;
}

/// The bytes of the file at @p path, or nothing when it cannot be read. Read at its size in one read: slurped
/// through an istreambuf_iterator instead, GCC 14's release build reports a null dereference in streambuf.
std::optional<std::vector<uint8_t>> readFile(std::string const& path)
{
    auto file = std::ifstream { path, std::ios::binary | std::ios::ate };
    if (!file)
        return std::nullopt;
    auto const size = static_cast<std::streamoff>(file.tellg());
    if (size < 0)
        return std::nullopt;
    auto bytes = std::vector<uint8_t>(static_cast<std::size_t>(size));
    if (!file.seekg(0).read(reinterpret_cast<char*>(bytes.data()), size))
        return std::nullopt;
    return bytes;
}

/// The runtime, initialized for the process and destroyed at its end.
class Runtime
{
  public:
    Runtime() noexcept
    {
        auto arguments = RuntimeInitArgs {};
        std::memset(&arguments, 0, sizeof arguments);
        arguments.mem_alloc_type = Alloc_With_System_Allocator;
        _isInitialized = wasm_runtime_full_init(&arguments);
    }
    ~Runtime()
    {
        if (_isInitialized)
            wasm_runtime_destroy();
    }
    Runtime(Runtime const&) = delete;
    Runtime& operator=(Runtime const&) = delete;
    Runtime(Runtime&&) = delete;
    Runtime& operator=(Runtime&&) = delete;

    /// Whether the runtime came up.
    [[nodiscard]] bool isInitialized() const noexcept { return _isInitialized; }

  private:
    bool _isInitialized = false; ///< Whether wasm_runtime_full_init() succeeded.
};

/// What a run computed and how long its phases took.
struct RunResult
{
    uint64_t checksum = 0;     ///< The last run()'s result.
    Clock::duration load {};   ///< Loading and instantiation.
    Clock::duration invoke {}; ///< Every run() call together.
};

/// Loads, instantiates and runs the guest in @p bytes; WAMR keeps pointers into them while the module lives.
std::expected<RunResult, std::string> runGuest(std::vector<uint8_t>& bytes, Arguments const& arguments)
{
    auto error = std::array<char, 256> {};
    auto const loadStart = Clock::now();
    auto const module = std::unique_ptr<WASMModuleCommon, decltype(&wasm_runtime_unload)> {
        wasm_runtime_load(bytes.data(), static_cast<uint32_t>(bytes.size()), error.data(), error.size()),
        &wasm_runtime_unload
    };
    if (!module)
        return std::unexpected { std::format("load failed: {}", error.data()) };
    auto const instance = std::unique_ptr<WASMModuleInstanceCommon, decltype(&wasm_runtime_deinstantiate)> {
        wasm_runtime_instantiate(module.get(), StackBytes, 0, error.data(), error.size()),
        &wasm_runtime_deinstantiate
    };
    if (!instance)
        return std::unexpected { std::format("instantiate failed: {}", error.data()) };
    auto const load = Clock::now() - loadStart;

    auto* const run = wasm_runtime_lookup_function(instance.get(), "run");
    if (run == nullptr)
        return std::unexpected { std::string { "export failed: the guest exports no function named run" } };
    auto const environment = std::unique_ptr<WASMExecEnv, decltype(&wasm_runtime_destroy_exec_env)> {
        wasm_runtime_create_exec_env(instance.get(), StackBytes), &wasm_runtime_destroy_exec_env
    };
    if (!environment)
        return std::unexpected { std::string { "call failed: no execution environment" } };

    // run(i32) -> i64: the argument in the first cell, the result in the first two on return.
    auto cells = std::array<uint32_t, 2> {};
    auto const invokeStart = Clock::now();
    for ([[maybe_unused]] auto const invocation: std::views::iota(uint32_t { 0 }, arguments.invocations))
    {
        cells = { arguments.iterations, 0 };
        if (!wasm_runtime_call_wasm(environment.get(), run, 1, cells.data()))
            return std::unexpected { std::format("call failed: {}",
                                                 wasm_runtime_get_exception(instance.get())) };
    }
    auto const invoke = Clock::now() - invokeStart;
    auto checksum = uint64_t { 0 };
    std::memcpy(&checksum, cells.data(), sizeof checksum);
    return RunResult { .checksum = checksum, .load = load, .invoke = invoke };
}

/// @p duration in whole nanoseconds.
int64_t nanoseconds(Clock::duration duration)
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count();
}

/// @p text as a JSON string literal.
std::string jsonString(std::string_view text)
{
    auto quoted = std::string { "\"" };
    for (auto const c: text)
    {
        if (c == '"' || c == '\\')
            quoted += std::format("\\{}", c);
        else if (static_cast<unsigned char>(c) < 0x20)
            quoted += std::format("\\u{:04x}", static_cast<unsigned>(c));
        else
            quoted += c;
    }
    return quoted + "\"";
}

} // namespace

int main(int argc, char const* argv[])
{
    auto const arguments = parse(std::span { argv, static_cast<std::size_t>(argc) });
    if (!arguments)
    {
        std::cerr << "isoline-benchmark-wamr: " << arguments.error()
                  << "\nusage: isoline-benchmark-wamr <module.wasm> <iterations> [--invocations <count>]\n";
        return static_cast<int>(ExitStatus::Usage);
    }
    auto bytes = readFile(arguments->module);
    if (!bytes)
    {
        std::cerr << "isoline-benchmark-wamr: cannot read " << arguments->module << '\n';
        return static_cast<int>(ExitStatus::RunFailed);
    }
    auto const runtime = Runtime {};
    if (!runtime.isInitialized())
    {
        std::cerr << "isoline-benchmark-wamr: the runtime did not initialize\n";
        return static_cast<int>(ExitStatus::RunFailed);
    }
    auto const outcome = runGuest(*bytes, *arguments);
    if (!outcome)
    {
        std::cerr << std::format("isoline-benchmark-wamr: {}: {}\n", arguments->module, outcome.error());
        return static_cast<int>(ExitStatus::RunFailed);
    }
    std::cout << std::format(
        "{{\"runner\": \"wamr\", \"engine\": {}, \"dispatch\": \"none\", \"module\": {}, "
        "\"iterations\": {}, \"invocations\": {}, \"checksum\": {}, \"load_ns\": {}, "
        "\"invoke_ns\": {}}}\n",
        jsonString("wamr-" ISOLINE_WAMR_VARIANT),
        jsonString(arguments->module),
        arguments->iterations,
        arguments->invocations,
        outcome->checksum,
        nanoseconds(outcome->load),
        nanoseconds(outcome->invoke));
    return static_cast<int>(ExitStatus::Success);
}
