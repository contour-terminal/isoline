// SPDX-License-Identifier: Apache-2.0
#include <core/Utf8Path.hpp>
#include <core/platform/NativeFileSystem.hpp>
#include <core/platform/testing/InMemoryFileSystem.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <filesystem>
#include <format>
#include <ranges>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <isoline/cli/FileAccess.hpp>
#include <isoline/cli/testing/FaultyFileSystem.hpp>

using core::platform::NativeFileSystem;
using core::platform::testing::InMemoryFileSystem;
using isoline::cli::FileErrorKind;
using isoline::cli::OutputDevice;
using isoline::cli::StandardStreamPath;
using isoline::cli::StandardStreams;
using isoline::cli::SystemFileAccess;
using isoline::cli::testing::Fault;
using isoline::cli::testing::FaultyFileSystem;
using std::filesystem::perms;

namespace
{

/// The mode the tests give a new file.
constexpr auto NewFileMode = perms::owner_read | perms::owner_write | perms::group_read;

/// The bytes of @p text.
std::vector<std::byte> bytesOf(std::string_view text)
{
    auto const bytes = std::as_bytes(std::span { text });
    return { bytes.begin(), bytes.end() };
}

/// A fresh directory of this suite under the build tree, emptied first.
std::filesystem::path scratchDirectory(std::string_view name)
{
    auto const directory = std::filesystem::path { ISOLINE_CLI_SCRATCH_DIR } / name;
    std::filesystem::remove_all(directory);
    std::filesystem::create_directories(directory);
    return directory;
}

/// Standard streams over string streams, standard output no terminal.
struct Streams
{
    std::istringstream input;
    std::ostringstream output;

    [[nodiscard]] StandardStreams streams(OutputDevice device = OutputDevice::Other)
    {
        return StandardStreams { .input = input, .output = output, .outputDevice = device };
    }
};

} // namespace

TEST_CASE("SystemFileAccess.files")
{
    auto streams = Streams {};
    auto const fileSystem = NativeFileSystem {};
    auto files = SystemFileAccess { fileSystem, streams.streams(), NewFileMode };
    auto const directory = scratchDirectory("files");
    auto const path = core::pathToUtf8(directory / "a.bin");
    // Every byte value, line ends among them, survives the round trip.
    auto contents = std::vector<std::byte> {};
    for (auto const value: std::views::iota(0, 256))
        contents.push_back(static_cast<std::byte>(value));

    SECTION("a written file reads back")
    {
        REQUIRE(files.write(path, contents).has_value());
        auto const read = files.read(path, contents.size());
        REQUIRE(read.has_value());
        CHECK(*read == contents);
        // Writing replaces what the file held, and leaves no temporary file behind.
        REQUIRE(files.write(path, bytesOf("x")).has_value());
        CHECK(files.read(path, 1) == bytesOf("x"));
        CHECK(std::ranges::distance(std::filesystem::directory_iterator { directory }) == 1);
    }
    SECTION("a file past the limit is refused")
    {
        REQUIRE(files.write(path, contents).has_value());
        auto const read = files.read(path, contents.size() - 1);
        REQUIRE_FALSE(read.has_value());
        CHECK(read.error().kind == FileErrorKind::TooLarge);
    }
    SECTION("a missing file")
    {
        auto const read = files.read(core::pathToUtf8(directory / "missing"), 1);
        REQUIRE_FALSE(read.has_value());
        CHECK(read.error().kind == FileErrorKind::Unavailable);
        CHECK_FALSE(read.error().reason.empty());
    }
    SECTION("a name that is not UTF-8 names no file")
    {
        // On Windows it names none at all; elsewhere it names a file that is not there.
        auto const read = files.read(core::pathToUtf8(directory) + "/\xff", 1);
        REQUIRE_FALSE(read.has_value());
        CHECK(read.error().kind == FileErrorKind::Unavailable);
    }
    SECTION("a directory reads as no file")
    {
        // libc++ reads a directory opened on POSIX as an empty stream, so it is refused by name.
        auto const read = files.read(core::pathToUtf8(directory), 1);
        REQUIRE_FALSE(read.has_value());
        CHECK(read.error().kind == FileErrorKind::Unavailable);
        CHECK(read.error().reason == "Is a directory");
    }
    SECTION("a file in a missing directory cannot be written")
    {
        auto const written = files.write(core::pathToUtf8(directory / "missing" / "a.bin"), contents);
        REQUIRE_FALSE(written.has_value());
        CHECK(written.error().kind == FileErrorKind::Unavailable);
        CHECK_FALSE(written.error().reason.empty());
    }
    SECTION("the same file, by two names")
    {
        REQUIRE(files.write(path, contents).has_value());
        std::filesystem::create_hard_link(directory / "a.bin", directory / "b.bin");
        CHECK(files.isSameFile(path, core::pathToUtf8(directory / "b.bin")));
        CHECK(files.isSameFile(path, core::pathToUtf8(directory / "." / "a.bin")));
        CHECK_FALSE(files.isSameFile(path, core::pathToUtf8(directory / "c.bin")));
        // A file to be, spelled two ways.
        CHECK(files.isSameFile(core::pathToUtf8(directory / "c.bin"),
                               core::pathToUtf8(directory / "." / "c.bin")));
        CHECK_FALSE(files.isSameFile(StandardStreamPath, StandardStreamPath));
        CHECK_FALSE(files.isSameFile(path, StandardStreamPath));
        // A relative name of a file to be is resolved from the current directory.
        CHECK(
            files.isSameFile("isoline-cli-missing.bin",
                             core::pathToUtf8(std::filesystem::current_path() / "isoline-cli-missing.bin")));
        // `-` is standard output, never a file named so.
        CHECK_FALSE(
            files.isSameFile(core::pathToUtf8(std::filesystem::current_path() / "-"), StandardStreamPath));
    }
}

TEST_CASE("SystemFileAccess.failedWriteKeepsTheOldFile")
{
    auto streams = Streams {};
    auto inner = InMemoryFileSystem {};
    inner.addDirectory("/work");
    inner.addFile("/work/a.wasm", "old");
    auto const writeWith = [&](Fault fault) {
        auto const fileSystem = FaultyFileSystem { inner, { fault } };
        auto files = SystemFileAccess { fileSystem, streams.streams(), NewFileMode };
        return files.write("/work/a.wasm", bytesOf("new"));
    };
    for (auto const fault: { Fault::Permissions,
                             Fault::CreateNewFile,
                             Fault::OpenWrite,
                             Fault::Write,
                             Fault::SetPermissions,
                             Fault::SyncFile,
                             Fault::ReplaceFile })
    {
        INFO(static_cast<int>(fault));
        auto const written = writeWith(fault);
        REQUIRE_FALSE(written.has_value());
        CHECK(written.error().kind == FileErrorKind::Unavailable);
        CHECK(inner.readFile("/work/a.wasm") == "old");
        // The temporary file is gone.
        auto const entries = inner.listDirectory("/work");
        REQUIRE(entries.has_value());
        CHECK(entries->size() == 1);
    }
    SECTION("a directory that cannot be synced: the file is written, and the failure reported")
    {
        auto const written = writeWith(Fault::SyncDirectory);
        REQUIRE_FALSE(written.has_value());
        CHECK(written.error().reason.starts_with("written, but not synced to the disk: "));
        CHECK(inner.readFile("/work/a.wasm") == "new");
    }
    SECTION("a temporary file that cannot be removed is named")
    {
        auto const fileSystem = FaultyFileSystem { inner, { Fault::SyncFile, Fault::Remove } };
        auto files = SystemFileAccess { fileSystem, streams.streams(), NewFileMode };
        auto const written = files.write("/work/a.wasm", bytesOf("new"));
        REQUIRE_FALSE(written.has_value());
        CHECK(written.error().reason == "syncFile failed; the temporary file is left behind: remove failed");
        CHECK(inner.readFile("/work/a.wasm") == "old");
    }
    SECTION("an exception removes the temporary file too")
    {
        auto const fileSystem = FaultyFileSystem { inner, { Fault::SyncFileThrows } };
        auto files = SystemFileAccess { fileSystem, streams.streams(), NewFileMode };
        auto const write = [&] {
            return files.write("/work/a.wasm", bytesOf("new")).has_value();
        };
        CHECK_THROWS_AS(write(), std::runtime_error);
        CHECK(inner.readFile("/work/a.wasm") == "old");
        auto const entries = inner.listDirectory("/work");
        REQUIRE(entries.has_value());
        CHECK(entries->size() == 1);
    }
    SECTION("an unreadable file")
    {
        auto const fileSystem = FaultyFileSystem { inner, { Fault::OpenRead } };
        auto files = SystemFileAccess { fileSystem, streams.streams(), NewFileMode };
        auto const read = files.read("/work/a.wasm", 8);
        REQUIRE_FALSE(read.has_value());
        CHECK(read.error().reason == "openRead failed");
    }
}

TEST_CASE("SystemFileAccess.inMemory")
{
    auto streams = Streams {};
    auto fileSystem = InMemoryFileSystem {};
    fileSystem.addDirectory("/work");
    fileSystem.setCurrentPath("/work");
    auto files = SystemFileAccess { fileSystem, streams.streams(), NewFileMode };
    SECTION("a bare name is written in the current directory")
    {
        REQUIRE(files.write("a.wasm", bytesOf("new")).has_value());
        CHECK(fileSystem.readFile("/work/a.wasm") == "new");
    }
    SECTION("files the host does not have are compared by their paths")
    {
        fileSystem.addFile("/work/a.wasm", "a");
        fileSystem.addFile("/work/b.wasm", "b");
        CHECK(files.isSameFile("/work/a.wasm", "/work/./a.wasm"));
        CHECK(files.isSameFile("a.wasm", "/work/a.wasm"));
        CHECK(files.isSameFile("c.wasm", "/work/c.wasm"));
        CHECK_FALSE(files.isSameFile("/work/a.wasm", "/work/b.wasm"));
    }
}

TEST_CASE("SystemFileAccess.temporaryNames")
{
    auto streams = Streams {};
    auto fileSystem = InMemoryFileSystem {};
    fileSystem.addDirectory("/work");
    auto files = SystemFileAccess { fileSystem, streams.streams(), NewFileMode };
    SECTION("those earlier runs left are stepped past")
    {
        for (auto const attempt: std::views::iota(0, 15))
            fileSystem.addFile(std::format("/work/.a.wasm.isoline-{}", attempt), "stale");
        REQUIRE(files.write("/work/a.wasm", bytesOf("new")).has_value());
        CHECK(fileSystem.readFile("/work/a.wasm") == "new");
        CHECK(fileSystem.readFile("/work/.a.wasm.isoline-0") == "stale");
    }
    SECTION("when every one is taken, the write fails")
    {
        for (auto const attempt: std::views::iota(0, 16))
            fileSystem.addFile(std::format("/work/.a.wasm.isoline-{}", attempt), "stale");
        auto const written = files.write("/work/a.wasm", bytesOf("new"));
        REQUIRE_FALSE(written.has_value());
        CHECK_FALSE(fileSystem.exists("/work/a.wasm"));
    }
}

TEST_CASE("SystemFileAccess.standardStreams")
{
    auto streams = Streams {};
    streams.input.str("(module)");
    auto const fileSystem = InMemoryFileSystem {};
    auto files = SystemFileAccess { fileSystem, streams.streams(), NewFileMode };
    SECTION("reading - reads standard input")
    {
        CHECK(files.read(StandardStreamPath, 8) == bytesOf("(module)"));
    }
    SECTION("standard input past the limit is refused")
    {
        auto const read = files.read(StandardStreamPath, 7);
        REQUIRE_FALSE(read.has_value());
        CHECK(read.error().kind == FileErrorKind::TooLarge);
    }
    SECTION("writing - writes standard output")
    {
        REQUIRE(files.write(StandardStreamPath, bytesOf("text")).has_value());
        CHECK(streams.output.str() == "text");
    }
    SECTION("a standard output that fails")
    {
        streams.output.setstate(std::ios::badbit);
        auto const written = files.write(StandardStreamPath, bytesOf("text"));
        REQUIRE_FALSE(written.has_value());
        CHECK(written.error().kind == FileErrorKind::Unavailable);
    }
    SECTION("a standard input that fails")
    {
        streams.input.setstate(std::ios::badbit);
        auto const read = files.read(StandardStreamPath, 8);
        REQUIRE_FALSE(read.has_value());
        CHECK(read.error().kind == FileErrorKind::Unavailable);
    }
    SECTION("a standard input that failed short of its end")
    {
        // failbit without eofbit: the stream stopped, but not at its end, so what it held is unknown.
        streams.input.setstate(std::ios::failbit);
        auto const read = files.read(StandardStreamPath, 8);
        REQUIRE_FALSE(read.has_value());
        CHECK(read.error().kind == FileErrorKind::Unavailable);
    }
}

TEST_CASE("SystemFileAccess.terminal")
{
    auto streams = Streams {};
    auto const fileSystem = InMemoryFileSystem {};
    auto const toTerminal =
        SystemFileAccess { fileSystem, streams.streams(OutputDevice::Terminal), NewFileMode };
    CHECK(toTerminal.isTerminal(StandardStreamPath));
    CHECK_FALSE(toTerminal.isTerminal("a.wasm"));
    auto const toPipe = SystemFileAccess { fileSystem, streams.streams(OutputDevice::Other), NewFileMode };
    CHECK_FALSE(toPipe.isTerminal(StandardStreamPath));
}
