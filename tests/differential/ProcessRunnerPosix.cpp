// SPDX-License-Identifier: Apache-2.0
// ProcessRunner::run() on POSIX systems: fork and execv, standard input from /dev/null, each output to a
// file, a pipe that reports a failed exec, and a deadline past which the child is killed.
#include <sys/wait.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <expected>
#include <filesystem>
#include <format>
#include <span>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

#include "ProcessRunner.hpp"

namespace isoline::differential
{

namespace
{
    /// The status a child that could not execute its program exits with, as a shell's does.
    constexpr auto CannotExecute = 127;
    /// How long the runner sleeps between two looks at the child: from the first to the longest, doubling.
    constexpr auto FirstWait = std::chrono::microseconds { 100 };
    constexpr auto LongestWait = std::chrono::milliseconds { 10 };

    /// A file descriptor, closed with its scope.
    class Descriptor
    {
      public:
        explicit Descriptor(int descriptor) noexcept: _descriptor { descriptor } {}
        ~Descriptor() { close(); }
        Descriptor(Descriptor const&) = delete;
        Descriptor(Descriptor&&) = delete;
        Descriptor& operator=(Descriptor const&) = delete;
        Descriptor& operator=(Descriptor&&) = delete;

        [[nodiscard]] int get() const noexcept { return _descriptor; }

        /// Closes it now, before its scope ends.
        void close() noexcept
        {
            if (_descriptor >= 0)
                ::close(_descriptor);
            _descriptor = -1;
        }

      private:
        int _descriptor;
    };

    [[nodiscard]] std::string systemError(int error)
    {
        return std::generic_category().message(error);
    }

    /// Waits for @p child until @p deadline; kills it past the deadline.
    /// @return Its wait status, or TimedOut once it was killed, or why it could not be waited for.
    [[nodiscard]] std::expected<int, ProcessError> waitUntil(pid_t child,
                                                             std::chrono::steady_clock::time_point deadline,
                                                             std::filesystem::path const& program)
    {
        auto pause = std::chrono::duration_cast<std::chrono::microseconds>(FirstWait);
        while (true)
        {
            auto status = 0;
            auto const waited = ::waitpid(child, &status, WNOHANG);
            if (waited == child)
                return status;
            if (waited < 0 && errno != EINTR)
                return std::unexpected { ProcessError {
                    .kind = ProcessErrorKind::Output,
                    .detail = std::format("cannot wait for {}: {}", program.string(), systemError(errno)) } };
            if (std::chrono::steady_clock::now() >= deadline)
            {
                ::kill(child, SIGKILL);
                while (::waitpid(child, &status, 0) < 0 && errno == EINTR)
                {
                }
                return std::unexpected { ProcessError {
                    .kind = ProcessErrorKind::TimedOut,
                    .detail = std::format("{} ran past its deadline and was killed", program.string()) } };
            }
            std::this_thread::sleep_for(pause);
            pause = std::min(pause * 2, std::chrono::duration_cast<std::chrono::microseconds>(LongestWait));
        }
    }
} // namespace

std::expected<ProcessResult, ProcessError> ProcessRunner::run(std::filesystem::path const& program,
                                                              std::span<std::string const> arguments,
                                                              std::chrono::milliseconds timeout)
{
    auto const deadline = std::chrono::steady_clock::now() + timeout;
    auto const stem = _scratchDirectory / std::format("process-{}-{}", ::getpid(), _runs++);
    auto const outputPath = std::filesystem::path { stem.string() + ".out" };
    auto const errorPath = std::filesystem::path { stem.string() + ".err" };

    auto const writeFlags = O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC;
    auto const input = Descriptor { ::open("/dev/null", O_RDONLY | O_CLOEXEC) };
    auto const output = Descriptor { ::open(outputPath.c_str(), writeFlags, S_IRUSR | S_IWUSR) };
    auto const error = Descriptor { ::open(errorPath.c_str(), writeFlags, S_IRUSR | S_IWUSR) };
    if (input.get() < 0 || output.get() < 0 || error.get() < 0)
    {
        auto const reason = systemError(errno);
        removeOutputs(outputPath, errorPath);
        return std::unexpected { ProcessError {
            .kind = ProcessErrorKind::NotStarted,
            .detail = std::format("cannot open the files of {}: {}", program.string(), reason) } };
    }
    // The child writes exec's errno here when exec fails; the pipe closes unwritten when exec succeeds.
    // pipe() and then FD_CLOEXEC, not pipe2(), which macOS lacks; no other thread of a test forks between.
    auto pipeEnds = std::array<int, 2> { -1, -1 };
    auto const piped = ::pipe(pipeEnds.data()) == 0;
    auto readEnd = Descriptor { pipeEnds[0] };
    auto writeEnd = Descriptor { pipeEnds[1] };
    if (!piped || ::fcntl(readEnd.get(), F_SETFD, FD_CLOEXEC) != 0
        || ::fcntl(writeEnd.get(), F_SETFD, FD_CLOEXEC) != 0)
    {
        auto const reason = systemError(errno);
        removeOutputs(outputPath, errorPath);
        return std::unexpected { ProcessError {
            .kind = ProcessErrorKind::NotStarted,
            .detail = std::format("cannot run {}: {}", program.string(), reason) } };
    }

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
    {
        auto const reason = systemError(errno);
        removeOutputs(outputPath, errorPath);
        return std::unexpected { ProcessError {
            .kind = ProcessErrorKind::NotStarted,
            .detail = std::format("cannot run {}: {}", program.string(), reason) } };
    }
    if (child == 0)
    {
        if (::dup2(input.get(), STDIN_FILENO) >= 0 && ::dup2(output.get(), STDOUT_FILENO) >= 0
            && ::dup2(error.get(), STDERR_FILENO) >= 0)
            ::execv(argv.front(), argv.data());
        auto const failure = errno;
        [[maybe_unused]] auto const written = ::write(writeEnd.get(), &failure, sizeof(failure));
        ::_exit(CannotExecute);
    }
    writeEnd.close();
    auto failure = 0;
    auto received = ::ssize_t { 0 };
    do
        received = ::read(readEnd.get(), &failure, sizeof(failure));
    while (received < 0 && errno == EINTR);

    auto const status = waitUntil(child, deadline, program);
    if (std::cmp_equal(received, sizeof(failure)))
    {
        removeOutputs(outputPath, errorPath);
        return std::unexpected { ProcessError {
            .kind = ProcessErrorKind::NotStarted,
            .detail = std::format("cannot run {}: {}", program.string(), systemError(failure)) } };
    }
    if (!status)
    {
        removeOutputs(outputPath, errorPath);
        return std::unexpected { status.error() };
    }
    if (WIFSIGNALED(*status))
    {
        removeOutputs(outputPath, errorPath);
        return std::unexpected { ProcessError {
            .kind = ProcessErrorKind::Killed,
            .detail = std::format("{} ended by signal {}", program.string(), WTERMSIG(*status)) } };
    }
    return readOutputs(outputPath, errorPath, WEXITSTATUS(*status));
}

} // namespace isoline::differential
