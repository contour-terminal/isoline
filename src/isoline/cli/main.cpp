// SPDX-License-Identifier: Apache-2.0
#include <core/Environment.hpp>
#include <core/log/LogSink.hpp>
#include <core/platform/NativeFileSystem.hpp>

#include <iostream>

#include <isoline/cli/Entry.hpp>
#include <isoline/cli/FileAccess.hpp>
#include <isoline/cli/Host.hpp>
#include <isoline/cli/IsolineApp.hpp>
#include <isoline/cli/Verbs.hpp>
#include <isoline/text/TextLimits.hpp>

// The isoline tool's composition root: the process's environment, file system and standard streams.
// The platform's entry point (EntryPosix.cpp, EntryWindows.cpp) hands it the command line in UTF-8.
namespace isoline::cli
{

int runTool(int argc, char const* argv[])
{
    useBinaryStandardStreams();
    auto const environment = core::LiveEnvironment {};
    auto const fileSystem = core::platform::NativeFileSystem {};
    auto const streams = StandardStreams {
        .input = std::cin,
        .output = std::cout,
        .outputDevice = core::log::isStdOutTerminal() ? OutputDevice::Terminal : OutputDevice::Other,
    };
    auto files = SystemFileAccess { fileSystem, streams, defaultNewFileMode() };
    auto app = IsolineApp { environment, files, std::cerr, isoline::text::TextLimits {}, Verbs };
    return app.run(argc, argv);
}

} // namespace isoline::cli
