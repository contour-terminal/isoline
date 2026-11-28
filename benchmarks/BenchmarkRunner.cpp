// SPDX-License-Identifier: Apache-2.0
//
// isoline-benchmark <module.wasm> <iterations> [--engine interpreter|reference] [--dispatch switch|tailcall]
//                   [--invocations <count>]
//
// Loads a benchmark guest (benchmarks/guests) on one Isoline engine, calls its run(iterations) <count> times
// (once by default) and prints one JSON object to stdout: the engine, the checksum run() returned, and the
// load and invocation times by the steady clock. scripts/benchmark.py times the whole process with its own
// clock, which is what the benchmark reports; the inner times are for the cold-start row's breakdown and for
// the host→guest call round trip (iterations 0, many invocations). The composition root: the only place
// here that reads the clock and the file system.
#include <core/Clock.hpp>

#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <format>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "GuestRun.hpp"
#include <isoline/api/Engine.hpp>
#include <isoline/api/ExecutionEngines.hpp>
#include <isoline/interpreter/Dispatch.hpp>

namespace
{

/// How the process ends: as scripts/benchmark.py reads it.
enum class ExitStatus : uint8_t
{
    Success = 0,   ///< The guest ran; the JSON object is on stdout.
    RunFailed = 1, ///< The guest could not be read, loaded or run.
    Usage = 2,     ///< The command line is wrong.
};

/// What the command line asks for.
struct Arguments
{
    std::string module;                          ///< The guest's path.
    uint32_t iterations = 0;                     ///< run()'s argument.
    uint32_t invocations = 1;                    ///< How often run() is called.
    isoline::api::EngineConfig config {};        ///< The engine.
    std::string_view engineName = "interpreter"; ///< The engine, as named.
    std::string_view dispatchName = "best";      ///< The dispatch, as named.
};

/// The usage line.
constexpr auto Usage =
    std::string_view { "usage: isoline-benchmark <module.wasm> <iterations> [--engine interpreter|reference] "
                       "[--dispatch switch|tailcall] [--invocations <count>]\n" };

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
        if (rest.empty())
            return std::unexpected { std::format("{} needs a value", word) };
        auto const value = std::string_view { rest.front() };
        rest = rest.subspan(1);
        if (word == "--engine")
        {
            auto const* const row = isoline::api::engines::rowNamed(value);
            if (row == nullptr)
                return std::unexpected { std::format("unknown engine '{}'", value) };
            arguments.config.engine = row->engine;
            arguments.engineName = row->name;
        }
        else if (word == "--dispatch")
        {
            auto const dispatch = isoline::interpreter::dispatchNamed(value);
            if (!dispatch)
                return std::unexpected { std::format("unknown dispatch '{}'", value) };
            if (!isoline::interpreter::isAvailable(*dispatch))
                return std::unexpected { std::format("this build did not compile the {} dispatch", value) };
            arguments.config.dispatch = *dispatch;
            arguments.dispatchName = value;
        }
        else if (word == "--invocations")
        {
            auto const count = numberOf(value);
            if (!count || *count == 0)
                return std::unexpected { std::format("--invocations takes a positive number, not '{}'",
                                                     value) };
            arguments.invocations = *count;
        }
        else
            return std::unexpected { std::format("unknown option {}", word) };
    }
    if (positional.size() != 2)
        return std::unexpected { std::string { "expected a module and an iteration count" } };
    arguments.module = std::string { positional[0] };
    auto const iterations = numberOf(positional[1]);
    if (!iterations || *iterations > static_cast<uint32_t>(std::numeric_limits<int32_t>::max()))
        return std::unexpected { std::format("the iteration count '{}' is no number from 0 to 2^31-1",
                                             positional[1]) };
    arguments.iterations = *iterations;
    return arguments;
}

/// The bytes of the file at @p path, or nothing when it cannot be read. Read at its size in one read: slurped
/// through an istreambuf_iterator instead, GCC 14's release build reports a null dereference in streambuf.
std::optional<std::vector<std::byte>> readFile(std::string const& path)
{
    auto file = std::ifstream { path, std::ios::binary | std::ios::ate };
    if (!file)
        return std::nullopt;
    auto const size = static_cast<std::streamoff>(file.tellg());
    if (size < 0)
        return std::nullopt;
    auto bytes = std::vector<std::byte>(static_cast<std::size_t>(size));
    if (!file.seekg(0).read(reinterpret_cast<char*>(bytes.data()), size))
        return std::nullopt;
    return bytes;
}

/// @p duration in whole nanoseconds.
int64_t nanoseconds(core::SteadyDuration duration)
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count();
}

/// @p text as a JSON string literal: the guests' paths are the only strings, so quotes and backslashes are
/// what needs escaping, and control characters.
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
        std::cerr << "isoline-benchmark: " << arguments.error() << '\n' << Usage;
        return static_cast<int>(ExitStatus::Usage);
    }
    auto const bytes = readFile(arguments->module);
    if (!bytes)
    {
        std::cerr << "isoline-benchmark: cannot read " << arguments->module << '\n';
        return static_cast<int>(ExitStatus::RunFailed);
    }
    auto const clock = core::SteadyClock {};
    auto const outcome = isoline::benchmarks::runGuest(arguments->config,
                                                       isoline::benchmarks::GuestRunRequest {
                                                           .module = *bytes,
                                                           .iterations = arguments->iterations,
                                                           .invocations = arguments->invocations,
                                                       },
                                                       clock);
    if (!outcome)
    {
        std::cerr << std::format("isoline-benchmark: {}: {} failed: {}\n",
                                 arguments->module,
                                 isoline::benchmarks::nameOf(outcome.error().error),
                                 outcome.error().detail);
        return static_cast<int>(ExitStatus::RunFailed);
    }
    std::cout << std::format("{{\"runner\": \"isoline\", \"engine\": {}, \"dispatch\": {}, \"module\": {}, "
                             "\"iterations\": {}, \"invocations\": {}, \"checksum\": {}, \"load_ns\": {}, "
                             "\"invoke_ns\": {}}}\n",
                             jsonString(arguments->engineName),
                             jsonString(arguments->dispatchName),
                             jsonString(arguments->module),
                             arguments->iterations,
                             arguments->invocations,
                             outcome->checksum,
                             nanoseconds(outcome->load),
                             nanoseconds(outcome->invoke));
    return static_cast<int>(ExitStatus::Success);
}
