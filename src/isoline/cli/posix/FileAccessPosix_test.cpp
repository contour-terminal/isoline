// SPDX-License-Identifier: Apache-2.0
#include <core/Utf8Path.hpp>
#include <core/platform/NativeFileSystem.hpp>

#include <catch2/catch_test_macros.hpp>

#include <sys/stat.h>

#include <cstddef>
#include <filesystem>
#include <sstream>
#include <vector>

#include <isoline/cli/FileAccess.hpp>
#include <isoline/cli/Host.hpp>

using isoline::cli::FileErrorKind;
using isoline::cli::OutputDevice;
using isoline::cli::StandardStreams;
using isoline::cli::SystemFileAccess;
using std::filesystem::perms;

TEST_CASE("SystemFileAccess.deviceWrittenInPlace")
{
    auto input = std::istringstream {};
    auto output = std::ostringstream {};
    auto const fileSystem = core::platform::NativeFileSystem {};
    auto files = SystemFileAccess {
        fileSystem,
        StandardStreams { .input = input, .output = output, .outputDevice = OutputDevice::Other },
        std::filesystem::perms::owner_read | std::filesystem::perms::owner_write
    };
    auto const bytes = std::vector<std::byte>(16, std::byte { 0x2a });
    SECTION("a device takes the bytes where it is")
    {
        REQUIRE(files.write("/dev/null", bytes).has_value());
        CHECK(std::filesystem::is_character_file("/dev/null"));
    }
    SECTION("a full device refuses them")
    {
        // /dev/full fails every write with ENOSPC, which the flush reports. Linux and FreeBSD have it;
        // macOS does not.
        if (!std::filesystem::exists("/dev/full"))
            SKIP("this host has no /dev/full");
        auto const written = files.write("/dev/full", bytes);
        REQUIRE_FALSE(written.has_value());
        CHECK(written.error().kind == FileErrorKind::Unavailable);
        CHECK(std::filesystem::is_character_file("/dev/full"));
    }
}

TEST_CASE("SystemFileAccess.defaultNewFileMode")
{
    // The mode is 0666 less the umask, and reading it leaves the umask as it was.
    auto const previous = ::umask(027);
    auto const mode = isoline::cli::defaultNewFileMode();
    auto const after = ::umask(previous);
    CHECK(mode == static_cast<std::filesystem::perms>(0640));
    CHECK(after == 027);
}

TEST_CASE("SystemFileAccess.modes")
{
    // POSIX keeps a file's mode; Windows only a read-only attribute.
    auto input = std::istringstream {};
    auto output = std::ostringstream {};
    auto const fileSystem = core::platform::NativeFileSystem {};
    auto const newFileMode = perms::owner_read | perms::owner_write | perms::group_read;
    auto files = SystemFileAccess {
        fileSystem,
        StandardStreams { .input = input, .output = output, .outputDevice = OutputDevice::Other },
        newFileMode
    };
    auto const directory = std::filesystem::path { ISOLINE_CLI_SCRATCH_DIR } / "modes";
    std::filesystem::remove_all(directory);
    std::filesystem::create_directories(directory);
    auto const path = core::pathToUtf8(directory / "a.bin");
    auto const bytes = std::vector<std::byte>(4, std::byte { 0x2a });
    REQUIRE(files.write(path, bytes).has_value());
    CHECK((std::filesystem::status(directory / "a.bin").permissions() & perms::all) == newFileMode);
    // A replaced file keeps its mode.
    auto const mode = perms::owner_read | perms::owner_write | perms::others_read;
    std::filesystem::permissions(directory / "a.bin", mode);
    REQUIRE(files.write(path, bytes).has_value());
    CHECK((std::filesystem::status(directory / "a.bin").permissions() & perms::all) == mode);
}

TEST_CASE("SystemFileAccess.symlinkReplaced")
{
    // A link is replaced by a file of its target's mode; the target is left as it was.
    auto input = std::istringstream {};
    auto output = std::ostringstream {};
    auto const fileSystem = core::platform::NativeFileSystem {};
    auto files = SystemFileAccess {
        fileSystem,
        StandardStreams { .input = input, .output = output, .outputDevice = OutputDevice::Other },
        perms::owner_read | perms::owner_write
    };
    auto const directory = std::filesystem::path { ISOLINE_CLI_SCRATCH_DIR } / "symlink";
    std::filesystem::remove_all(directory);
    std::filesystem::create_directories(directory);
    auto const old = std::vector<std::byte>(3, std::byte { 'o' });
    REQUIRE(files.write(core::pathToUtf8(directory / "target"), old).has_value());
    auto const mode = perms::owner_read | perms::owner_write | perms::group_read | perms::others_read;
    std::filesystem::permissions(directory / "target", mode);
    std::filesystem::create_symlink("target", directory / "link");
    auto const bytes = std::vector<std::byte>(5, std::byte { 'n' });
    REQUIRE(files.write(core::pathToUtf8(directory / "link"), bytes).has_value());
    CHECK_FALSE(std::filesystem::is_symlink(directory / "link"));
    CHECK(files.read(core::pathToUtf8(directory / "link"), 16) == bytes);
    CHECK(files.read(core::pathToUtf8(directory / "target"), 16) == old);
    CHECK((std::filesystem::status(directory / "link").permissions() & perms::all) == mode);
}
