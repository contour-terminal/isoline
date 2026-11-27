// SPDX-License-Identifier: Apache-2.0
#include <isoline/cli/Entry.hpp>

// POSIX: the arguments are the bytes the process was given, which a UTF-8 locale spells paths in.
int main(int argc, char const* argv[])
{
    return isoline::cli::runTool(argc, argv);
}
