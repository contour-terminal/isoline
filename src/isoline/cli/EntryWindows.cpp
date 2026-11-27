// SPDX-License-Identifier: Apache-2.0
#include <core/WideString.hpp>

#include <cstddef>
#include <span>
#include <string>
#include <vector>

#include <isoline/cli/Entry.hpp>

// Windows: the command line arrives in UTF-16, as Windows keeps it, and is converted to UTF-8 here, once.
// main()'s narrow argv would be the ANSI code page's spelling of it, which cannot spell every path.
int wmain(int argc, wchar_t* argv[])
{
    auto const wide = std::span { argv, static_cast<std::size_t>(argc) };
    auto utf8 = std::vector<std::string> {};
    utf8.reserve(wide.size());
    for (auto const* const argument: wide)
        utf8.push_back(core::toUtf8(argument));
    auto pointers = std::vector<char const*> {};
    pointers.reserve(utf8.size());
    for (auto const& argument: utf8)
        pointers.push_back(argument.c_str());
    return isoline::cli::runTool(argc, pointers.data());
}
