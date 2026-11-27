// SPDX-License-Identifier: Apache-2.0
// ProcessRunner::run() on Windows: CreateProcessW, standard input from NUL, each output to a file, and a
// deadline past which the process is terminated.
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <windows.h>

#include "ProcessRunner.hpp"

namespace isoline::differential
{

namespace
{
    /// The first NTSTATUS error status (severity "error"): a process that ends with one ended abnormally,
    /// by an unhandled exception or a fail-fast.
    constexpr auto ErrorStatusBase = DWORD { 0xC000'0000 };
    /// The status the runner ends a process with at its deadline.
    constexpr auto KilledStatus = UINT { 0xC000'013A };

    /// A handle, closed with its scope.
    class Handle
    {
      public:
        explicit Handle(HANDLE handle) noexcept: _handle { handle } {}
        ~Handle()
        {
            if (isOpen())
                CloseHandle(_handle);
        }
        Handle(Handle const&) = delete;
        Handle(Handle&&) = delete;
        Handle& operator=(Handle const&) = delete;
        Handle& operator=(Handle&&) = delete;

        [[nodiscard]] HANDLE get() const noexcept { return _handle; }
        [[nodiscard]] bool isOpen() const noexcept
        {
            return _handle != nullptr && _handle != INVALID_HANDLE_VALUE;
        }

      private:
        HANDLE _handle;
    };

    /// @p text, UTF-8, as UTF-16.
    [[nodiscard]] std::wstring wide(std::string_view text)
    {
        return std::filesystem::path {
            std::u8string_view { reinterpret_cast<char8_t const*>(text.data()), text.size() }
        }.wstring();
    }

    /// Appends @p argument to @p commandLine quoted as CommandLineToArgvW and the C runtime split it: in
    /// quotes when it is empty or holds a space, a tab or a quote, each quote escaped, and the backslashes
    /// before a quote (or the closing one) doubled.
    void appendQuoted(std::wstring& commandLine, std::wstring_view argument)
    {
        if (!commandLine.empty())
            commandLine += L' ';
        if (!argument.empty() && argument.find_first_of(L" \t\n\v\"") == std::wstring_view::npos)
        {
            commandLine += argument;
            return;
        }
        commandLine += L'"';
        auto backslashes = std::size_t { 0 };
        for (auto const character: argument)
        {
            if (character == L'\\')
            {
                ++backslashes;
                continue;
            }
            if (character == L'"')
                commandLine.append((2 * backslashes) + 1, L'\\');
            else
                commandLine.append(backslashes, L'\\');
            backslashes = 0;
            commandLine += character;
        }
        commandLine.append(2 * backslashes, L'\\');
        commandLine += L'"';
    }

    /// A file the child inherits as one of its standard handles.
    [[nodiscard]] HANDLE inheritable(std::filesystem::path const& path, DWORD access, DWORD disposition)
    {
        auto attributes = SECURITY_ATTRIBUTES { .nLength = sizeof(SECURITY_ATTRIBUTES),
                                                .lpSecurityDescriptor = nullptr,
                                                .bInheritHandle = TRUE };
        return CreateFileW(path.c_str(),
                           access,
                           FILE_SHARE_READ | FILE_SHARE_WRITE,
                           &attributes,
                           disposition,
                           FILE_ATTRIBUTE_NORMAL,
                           nullptr);
    }
    /// Starts @p program with its standard handles, and waits for it until @p timeout, terminating it then.
    /// @return Its exit status, or why it gave no result.
    [[nodiscard]] std::expected<DWORD, ProcessError> runToItsEnd(std::filesystem::path const& program,
                                                                 std::span<std::string const> arguments,
                                                                 std::chrono::milliseconds timeout,
                                                                 Handle const& input,
                                                                 Handle const& output,
                                                                 Handle const& error)
    {
        auto commandLine = std::wstring {};
        appendQuoted(commandLine, program.wstring());
        for (auto const& argument: arguments)
            appendQuoted(commandLine, wide(argument));
        auto startup = STARTUPINFOW {};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdInput = input.get();
        startup.hStdOutput = output.get();
        startup.hStdError = error.get();
        auto process = PROCESS_INFORMATION {};
        if (CreateProcessW(program.c_str(),
                           commandLine.data(),
                           nullptr,
                           nullptr,
                           TRUE,
                           0,
                           nullptr,
                           nullptr,
                           &startup,
                           &process)
            == 0)
            return std::unexpected { ProcessError {
                .kind = ProcessErrorKind::NotStarted,
                .detail = std::format("cannot run {}: error {}", program.string(), GetLastError()) } };
        auto const processHandle = Handle { process.hProcess };
        auto const threadHandle = Handle { process.hThread };
        auto const milliseconds = static_cast<DWORD>(std::min<std::chrono::milliseconds::rep>(
            timeout.count(), std::chrono::milliseconds::rep { INFINITE - 1 }));
        if (WaitForSingleObject(processHandle.get(), milliseconds) == WAIT_TIMEOUT)
        {
            TerminateProcess(processHandle.get(), KilledStatus);
            WaitForSingleObject(processHandle.get(), INFINITE);
            return std::unexpected { ProcessError {
                .kind = ProcessErrorKind::TimedOut,
                .detail = std::format("{} ran past its deadline and was killed", program.string()) } };
        }
        auto exitCode = DWORD { 0 };
        GetExitCodeProcess(processHandle.get(), &exitCode);
        return exitCode;
    }
} // namespace

std::expected<ProcessResult, ProcessError> ProcessRunner::run(std::filesystem::path const& program,
                                                              std::span<std::string const> arguments,
                                                              std::chrono::milliseconds timeout)
{
    auto const stem = _scratchDirectory / std::format("process-{}-{}", GetCurrentProcessId(), _runs++);
    auto const outputPath = std::filesystem::path { stem.wstring() + L".out" };
    auto const errorPath = std::filesystem::path { stem.wstring() + L".err" };
    auto exitCode = DWORD { 0 };
    // Set when the process gave no result; the output files are removed once their handles are closed,
    // since Windows deletes no file that is open.
    auto failure = std::optional<ProcessError> {};
    {
        auto const input = Handle { inheritable(L"NUL", GENERIC_READ, OPEN_EXISTING) };
        auto const output = Handle { inheritable(outputPath, GENERIC_WRITE, CREATE_ALWAYS) };
        auto const error = Handle { inheritable(errorPath, GENERIC_WRITE, CREATE_ALWAYS) };
        if (!input.isOpen() || !output.isOpen() || !error.isOpen())
            failure = ProcessError { .kind = ProcessErrorKind::NotStarted,
                                     .detail = std::format("cannot open the files of {}", program.string()) };

        if (!failure)
        {
            if (auto const ran = runToItsEnd(program, arguments, timeout, input, output, error))
                exitCode = *ran;
            else
                failure = ran.error();
        }
    }
    if (failure)
    {
        removeOutputs(outputPath, errorPath);
        return std::unexpected { std::move(*failure) };
    }
    if (exitCode >= ErrorStatusBase)
    {
        removeOutputs(outputPath, errorPath);
        return std::unexpected { ProcessError {
            .kind = ProcessErrorKind::Killed,
            .detail = std::format("{} ended with status {:#010x}", program.string(), exitCode) } };
    }
    return readOutputs(outputPath, errorPath, static_cast<int>(exitCode));
}

} // namespace isoline::differential
