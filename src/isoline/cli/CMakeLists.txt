# SPDX-License-Identifier: Apache-2.0

# The isoline command-line tool, isoline::cli: its verbs as a library, which the tests drive over an
# in-memory file system, and the executable around them. It builds on the text format and core-cpp's
# command-line scaffold, neither of which an embedder of isoline::core links.

# What the executable asks of the host at startup that differs by platform (the mode of a new file,
# binary standard streams): one file per platform, chosen here rather than by #ifdef, and linked into
# the executable and the tests only, never into isoline_cli, since reading the umask changes
# process-global state.
if(WIN32)
    set(isolineCliHost HostWindows.cpp)
    # The entry point, the executable's alone: wmain(), which takes the command line in UTF-16.
    set(isolineCliEntry EntryWindows.cpp)
else()
    set(isolineCliHost HostPosix.cpp)
    set(isolineCliEntry EntryPosix.cpp)
endif()

add_library(isoline_cli STATIC)
add_library(isoline::cli ALIAS isoline_cli)
target_sources(isoline_cli
    PRIVATE
        FileAccess.cpp
        IsolineApp.cpp
        VerbArguments.cpp
        Verbs.cpp
    PUBLIC FILE_SET HEADERS
        BASE_DIRS "${PROJECT_SOURCE_DIR}/src"
        FILES
            FileAccess.hpp
            IsolineApp.hpp
            VerbArguments.hpp
            Verbs.hpp)
target_link_libraries(isoline_cli PUBLIC isoline::text core::cli core::platform)
isoline_apply_target_policy(isoline_cli)

add_executable(isoline main.cpp ${isolineCliEntry} ${isolineCliHost})
target_link_libraries(isoline PRIVATE isoline::cli)
isoline_apply_target_policy(isoline)
# The tool's own tests run it as a process, so its main() and entry points are reached through it alone.
isoline_coverage_binary(isoline)

if(ISOLINE_TESTING)
    # Every *_test.cpp in this directory is one translation unit of the tool's test binary, and on POSIX
    # those of posix/ too, which use what only POSIX systems have: file modes, the umask, /dev/null, and
    # /dev/full where the host has it (Linux; not macOS, where its case is skipped).
    file(GLOB isolineCliTestSources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*_test.cpp")
    if(UNIX)
        file(GLOB isolineCliPosixTestSources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/posix/*_test.cpp")
        list(APPEND isolineCliTestSources ${isolineCliPosixTestSources})
    endif()
    add_executable(isoline_cli_test ${isolineCliTestSources} ${isolineCliHost})
    target_link_libraries(isoline_cli_test PRIVATE isoline::cli Catch2::Catch2WithMain)
    # Where SystemFileAccess's tests write their files, under the build tree, and the source tree, whose
    # README.md the exit status table is checked in.
    target_compile_definitions(isoline_cli_test PRIVATE
                               ISOLINE_CLI_SCRATCH_DIR="${CMAKE_CURRENT_BINARY_DIR}/scratch"
                               ISOLINE_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
    isoline_apply_target_policy(isoline_cli_test TEST)
    add_test(NAME isoline_cli_test COMMAND isoline_cli_test)
    set_tests_properties(isoline_cli_test PROPERTIES LABELS unit TIMEOUT 120)
endif()
