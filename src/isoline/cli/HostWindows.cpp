// SPDX-License-Identifier: Apache-2.0
// What the tool asks of a Windows host, chosen by CMake.
#include <cstdio>
#include <filesystem>

#include <fcntl.h>
#include <io.h>

#include <isoline/cli/Host.hpp>

namespace isoline::cli
{

// Windows keeps no mode but the read-only attribute, which a new file does not have.
std::filesystem::perms defaultNewFileMode() noexcept
{
    using std::filesystem::perms;
    return perms::owner_read | perms::owner_write | perms::group_read | perms::group_write
           | perms::others_read | perms::others_write;
}

// The CRT opens the standard streams in text mode, which turns LF into CR LF on output and drops a
// byte 0x1a (Ctrl-Z) on input as the end of the file.
void useBinaryStandardStreams() noexcept
{
    (void) _setmode(_fileno(stdin), _O_BINARY);
    (void) _setmode(_fileno(stdout), _O_BINARY);
}

} // namespace isoline::cli
