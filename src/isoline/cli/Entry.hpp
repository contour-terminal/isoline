// SPDX-License-Identifier: Apache-2.0
#pragma once

// The isoline executable's entry, apart from the platform's entry point: POSIX passes the command line to
// main() as the bytes it was given, Windows to wmain() in UTF-16, which EntryWindows.cpp converts to
// UTF-8 so that a path of any characters reaches the tool, whatever the ANSI code page.

namespace isoline::cli
{

/// Runs the tool on a command line in UTF-8, with the process's environment, file system and streams.
/// @param argc The argument count, the program name included.
/// @param argv The arguments, in UTF-8.
/// @return The exit status.
[[nodiscard]] int runTool(int argc, char const* argv[]);

} // namespace isoline::cli
