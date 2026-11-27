// SPDX-License-Identifier: Apache-2.0
// ProcessRunner::run() on POSIX systems: fork and execv, standard input from /dev/null, each output to a
// file.
#include <sys/wait.h>

#include <cerrno>
#include <expected>
#include <filesystem>
#include <format>
#include <span>
#include <string>
#include <system_error>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

#include "ProcessRunner.hpp"

namespace isoline::differential
{

namespace
{
    /// The status bit a shell adds to the number of the signal that ended a process.
    constexpr auto SignalStatusBase = 128;
    /// The status a child that could not execute its program exits with, as a shell's does.
    constexpr auto CannotExecute = 127;

    /// A file descriptor, closed with its scope.
    class Descriptor
    {
      public:
        explicit Descriptor(int descriptor) noexcept: _descriptor { descriptor } {}
        ~Descriptor()
        {
            if (_descriptor >= 0)
                ::close(_descriptor);
        }
        Descriptor(Descriptor const&) = delete;
        Descriptor(Descriptor&&) = delete;
        Descriptor& operator=(Descriptor const&) = delete;
        Descriptor& operator=(Descriptor&&) = delete;

        [[nodiscard]] int get() const noexcept { return _descriptor; }

      private:
        int _descriptor;
    };
} // namespace

std::expected<ProcessResult, std::string> ProcessRunner::run(std::filesystem::path const& program,
                                                             std::span<std::string const> arguments)
{
    auto const stem = _scratchDirectory / std::format("process-{}-{}", ::getpid(), _runs++);
    auto const outputPath = std::filesystem::path { stem.string() + ".out" };
    auto const errorPath = std::filesystem::path { stem.string() + ".err" };

    auto const writeFlags = O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC;
    auto const input = Descriptor { ::open("/dev/null", O_RDONLY | O_CLOEXEC) };
    auto const output = Descriptor { ::open(outputPath.c_str(), writeFlags, S_IRUSR | S_IWUSR) };
    auto const error = Descriptor { ::open(errorPath.c_str(), writeFlags, S_IRUSR | S_IWUSR) };
    if (input.get() < 0 || output.get() < 0 || error.get() < 0)
        return std::unexpected { std::format(
            "cannot open the files of {}: {}", program.string(), std::generic_category().message(errno)) };

    // Everything the child needs is made before it is forked: between fork and exec it may only call
    // async-signal-safe functions. execv() keeps this process's environment.
    auto storage = std::vector<std::string> { program.string() };
    storage.insert(storage.end(), arguments.begin(), arguments.end());
    auto argv = std::vector<char*> {};
    for (auto& argument: storage)
        argv.push_back(argument.data());
    argv.push_back(nullptr);

    auto const child = ::fork();
    if (child < 0)
        return std::unexpected { std::format(
            "cannot run {}: {}", program.string(), std::generic_category().message(errno)) };
    if (child == 0)
    {
        if (::dup2(input.get(), STDIN_FILENO) < 0 || ::dup2(output.get(), STDOUT_FILENO) < 0
            || ::dup2(error.get(), STDERR_FILENO) < 0)
            ::_exit(CannotExecute);
        ::execv(argv.front(), argv.data());
        ::_exit(CannotExecute);
    }
    auto status = 0;
    while (::waitpid(child, &status, 0) == -1)
    {
        if (errno != EINTR)
            return std::unexpected { std::format(
                "cannot wait for {}: {}", program.string(), std::generic_category().message(errno)) };
    }

    auto result = ProcessResult {};
    result.exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : SignalStatusBase + WTERMSIG(status);
    auto standardOutput = readWholeFile(outputPath);
    auto standardError = readWholeFile(errorPath);
    auto ignored = std::error_code {};
    std::filesystem::remove(outputPath, ignored);
    std::filesystem::remove(errorPath, ignored);
    if (!standardOutput)
        return std::unexpected { standardOutput.error() };
    if (!standardError)
        return std::unexpected { standardError.error() };
    result.standardOutput = std::move(*standardOutput);
    result.standardError = std::move(*standardError);
    return result;
}

} // namespace isoline::differential
