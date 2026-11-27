// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <filesystem>

// What the isoline executable asks of its host at startup, where the platforms differ. Each platform's
// definitions are a file of their own, chosen by CMake and linked into the executable (and the tests)
// alone, never into a library: reading the umask changes process-global state.
namespace isoline::cli
{

/// The mode the host gives a new file: 0666 less the umask on POSIX, read and write on Windows. It
/// reads the umask by setting it and setting it back, which is safe only while one thread runs: the
/// tool calls it once, at startup.
/// @return The mode.
[[nodiscard]] std::filesystem::perms defaultNewFileMode() noexcept;

/// Puts the process's standard input and output in binary mode where the platform has a text mode
/// that would translate line ends (Windows), so that a module read from or written to them arrives
/// byte for byte. Elsewhere it does nothing.
void useBinaryStandardStreams() noexcept;

} // namespace isoline::cli
