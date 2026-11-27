// SPDX-License-Identifier: Apache-2.0
// ProcessRunner::run() on Windows: CreateProcessW, standard input from NUL, each output to a file.
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <system_error>

#include <windows.h>

#include "ProcessRunner.hpp"

namespace isoline::differential
{

namespace
{
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
} // namespace

std::expected<ProcessResult, std::string> ProcessRunner::run(std::filesystem::path const& program,
                                                             std::span<std::string const> arguments)
{
    auto const stem = _scratchDirectory / std::format("process-{}-{}", GetCurrentProcessId(), _runs++);
    auto const outputPath = std::filesystem::path { stem.wstring() + L".out" };
    auto const errorPath = std::filesystem::path { stem.wstring() + L".err" };
    auto result = ProcessResult {};
    {
        auto const input = Handle { inheritable(L"NUL", GENERIC_READ, OPEN_EXISTING) };
        auto const output = Handle { inheritable(outputPath, GENERIC_WRITE, CREATE_ALWAYS) };
        auto const error = Handle { inheritable(errorPath, GENERIC_WRITE, CREATE_ALWAYS) };
        if (!input.isOpen() || !output.isOpen() || !error.isOpen())
            return std::unexpected { std::format("cannot open the files of {}", program.string()) };

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
            return std::unexpected { std::format(
                "cannot run {}: error {}", program.string(), GetLastError()) };
        auto const processHandle = Handle { process.hProcess };
        auto const threadHandle = Handle { process.hThread };
        WaitForSingleObject(processHandle.get(), INFINITE);
        auto exitCode = DWORD { 0 };
        GetExitCodeProcess(processHandle.get(), &exitCode);
        result.exitCode = static_cast<int>(exitCode);
    }
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
