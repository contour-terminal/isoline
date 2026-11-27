// SPDX-License-Identifier: Apache-2.0
// What the tool asks of a POSIX host, chosen by CMake for every platform but Windows.
#include <sys/stat.h>

#include <filesystem>

#include <isoline/cli/Host.hpp>

namespace isoline::cli
{

std::filesystem::perms defaultNewFileMode() noexcept
{
    // umask() has no read-only form: it is set, and set back at once.
    auto const mask = ::umask(0);
    ::umask(mask);
    return static_cast<std::filesystem::perms>(0666U & ~static_cast<unsigned>(mask));
}

// POSIX streams have no text mode: every byte passes as it is.
void useBinaryStandardStreams() noexcept
{
}

} // namespace isoline::cli
