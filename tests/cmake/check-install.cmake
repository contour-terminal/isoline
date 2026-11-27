# SPDX-License-Identifier: Apache-2.0
#
# Isoline installs as a CMake package that a project outside its tree finds and links.
#
#   cmake -DBUILD_DIR=<isoline build> -DSOURCE_DIR=<isoline> -DPREFIX=<empty prefix>
#         -DCONSUMER_SOURCE=<tests/consumer> -DMODE=<mode> -DFORBIDDEN_OPTIONS_FILE=<list file>
#         [-DCXX_COMPILER=<c>] [-DGENERATOR=<g>] [-DGENERATOR_PLATFORM=<p>] [-DGENERATOR_TOOLSET=<t>]
#         [-DGENERATOR_IS_MULTI_CONFIG=ON|OFF] [-DMAKE_PROGRAM=<p>] [-DMT=<mt>] [-DAR=<ar>]
#         [-DRANLIB=<ranlib>] [-DCONFIG=<config>]
#         [-DEXPECT_IMPORT_STD=ON|OFF] [-DCORE_CPP_PREFIX=<prefix>] -P tests/cmake/check-install.cmake
#   cmake -DCORE_CPP_ONLY=ON -DBUILD_DIR=... -DSOURCE_DIR=... -DPREFIX=<shared prefix> [toolchain as above]
#         -P tests/cmake/check-install.cmake
#
# The second form takes step 1 alone, into PREFIX, and stops: install-core-cpp, the CTest setup fixture
# every mode shares, so core-cpp is built once per build tree rather than once per mode. A mode then names
# that prefix as CORE_CPP_PREFIX, and its step 1 copies it.
# MODE is a row of CONSUMER_SOURCE/ConsumerModes.cmake, which names what it installs and imports.
# Every build it makes sits beside PREFIX, in PREFIX's parent directory (the registration puts it at
# <build>/install-tests/<mode>/prefix). It takes these steps, and any step that fails prints its
# output and fails the check:
#   1. core-cpp, the package's one dependency, is configured, built and installed from
#      SOURCE_DIR/vendor/core-cpp into PREFIX as a project of its own: Isoline does not install it,
#      a packager installs it first. Offline, with nothing but what Isoline links, and with no
#      sanitizer. Its patch version is bumped by one, so the package is proven to accept a core-cpp
#      patch release (SameMinorVersion, never EXACT).
#   2. `cmake --install BUILD_DIR --component isoline` into PREFIX.
#   3. PREFIX holds no built module interface (*.pcm, *.gcm, *.ifc): a module installs as source, and
#      its BMI is the consumer's to build with the consumer's flags.
#   4. No installed *Targets*.cmake, target-*.cmake or cxx-modules-*.cmake names an option of
#      FORBIDDEN_OPTIONS_FILE, which the build wrote with its own warning, sanitizer and coverage
#      options: those are Isoline's development flags, never a consumer's.
#   5. No installed file that is not binary (by its content: a NUL byte) names SOURCE_DIR or BUILD_DIR,
#      in any case and with either separator, other than as PREFIX itself: the package is relocatable.
#      PREFIX is then copied elsewhere and removed, and the steps below consume the copy, which proves it.
#   6. tests/cmake/package-probe, configured against the moved prefix once per row of a probe table, finds the
#      package as consumers spell it (no COMPONENTS, a component, a multi-word component) and refuses
#      it as SameMinorVersion must (an older and a newer minor version) and for an unknown component.
#      In a mode that imports, it also finds isoline::core_module in component core, and the config
#      refuses it, and only it, to a target of a consumer whose generator is Visual Studio (D11).
#   7. tests/consumer, configured against the moved prefix alone, builds and runs in MODE. Its configure must
#      print no CMake warning, and its build no compiler or linker warning and no module-scanning flag
#      unless the mode imports. Three exceptions are tolerated, by their text, and nothing else (D13):
#       - E1, in a mode that imports, Clang's "argument unused during compilation: '-c'" in the build, at
#         any count: CMake's command that builds an imported module's BMI passes `-c` with --precompile.
#       - E3, in a mode that imports std, CMake's dev warning that `import std;` is experimental, in the
#         configure. It must be there: once it is gone, the check fails, saying the gate table and E3 go.
#       - E4, in a mode that imports std, Clang's reserved-name warning on libstdc++'s own std module
#         sources, in the build. Built by Clang, it must be there too.
#      A mode that imports std where the consumer reports "import std unavailable: <reason>" exits 77
#      (a skip), or fails where EXPECT_IMPORT_STD is ON: the lane says its toolchain has a std module.
# Steps 6 and 7 search no package path but the moved prefix: no system or user package registry, no
# system prefix, no environment path, so a core-cpp installed on the machine cannot stand in for its.

cmake_minimum_required(VERSION 3.25)

set(requiredArguments BUILD_DIR SOURCE_DIR PREFIX)
if(NOT CORE_CPP_ONLY)
    list(APPEND requiredArguments CONSUMER_SOURCE MODE FORBIDDEN_OPTIONS_FILE)
endif()
foreach(required IN LISTS requiredArguments)
    if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
        message(FATAL_ERROR "check-install: -D${required}=... is required")
    endif()
endforeach()

if(NOT CORE_CPP_ONLY)
    include("${CONSUMER_SOURCE}/ConsumerModes.cmake")
    isoline_consumer_mode("${MODE}" mode)
    if(NOT modeFound)
        message(FATAL_ERROR "check-install: MODE must be one of ${modeKnownModes}, not '${MODE}'")
    endif()
    set(components ${modeComponents})
    set(isScanningForModules OFF)
    if(modeImportsIsoline OR modeImportsStd)
        set(isScanningForModules ON)
    endif()
endif()

get_filename_component(workDir "${PREFIX}" DIRECTORY)
set(coreCppBuild "${workDir}/core-cpp-build")
set(consumerBuild "${workDir}/consumer-build")
set(probeBuild "${workDir}/package-probe-build")
set(probePrefix "${workDir}/probe-prefix")
set(movedPrefix "${workDir}/moved-prefix")
# Everything that is judged (the prefix, the probes, the consumer) starts empty. core-cpp's build tree
# is kept between runs, so a rerun builds it incrementally, but only for the same toolchain: it is
# keyed by everything that makes another build of it, and wiped when that changes.
file(REMOVE_RECURSE "${PREFIX}" "${movedPrefix}" "${consumerBuild}" "${probeBuild}" "${probePrefix}")
file(MAKE_DIRECTORY "${PREFIX}")
set(coreCppBuildKey
    "${GENERATOR}|${GENERATOR_PLATFORM}|${GENERATOR_TOOLSET}|${MAKE_PROGRAM}|${CXX_COMPILER}|${MT}|${AR}|${RANLIB}|${CONFIG}")
set(coreCppBuildKeyFile "${coreCppBuild}/check-install-key.txt")
if(EXISTS "${coreCppBuildKeyFile}")
    file(READ "${coreCppBuildKeyFile}" keptKey)
    if(NOT keptKey STREQUAL coreCppBuildKey)
        message(STATUS "check-install: the kept core-cpp build is of another toolchain; starting it afresh")
        file(REMOVE_RECURSE "${coreCppBuild}")
    endif()
endif()

## @brief Runs a command; fails the check with its output unless its exit status is 0.
## @param what What the command does, for the messages.
## @param outputVar Receives the command's standard output and error, joined.
function(check_install_step what outputVar)
    execute_process(COMMAND ${ARGN} RESULT_VARIABLE status OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT status EQUAL 0)
        message(FATAL_ERROR "check-install: ${what} failed (exit ${status}):\n${out}\n${err}")
    endif()
    message(STATUS "check-install: ${what}: ok")
    set(${outputVar} "${out}\n${err}" PARENT_SCOPE)
endfunction()

# The build's toolchain, for every project configured here: (argument, value) rows, each passed only
# when the build has a value for it. The manifest tool and the archiver are among them because the
# isolation below turns the PATH search off, and with it the one that finds MSVC's mt.exe (without it,
# no program links) and an `ar` that is not beside the compiler (the gcc image's g++ is in /usr/local/bin,
# its ar in /usr/bin): without that, no static library links, such as the std module import std builds.
set(toolchainRows
    -G                       "${GENERATOR}"
    -A                       "${GENERATOR_PLATFORM}"
    -T                       "${GENERATOR_TOOLSET}"
    -DCMAKE_MAKE_PROGRAM=    "${MAKE_PROGRAM}"
    -DCMAKE_CXX_COMPILER=    "${CXX_COMPILER}"
    -DCMAKE_MT=              "${MT}"
    -DCMAKE_AR=              "${AR}"
    -DCMAKE_RANLIB=          "${RANLIB}")
set(toolchainArgs "")
while(toolchainRows)
    list(POP_FRONT toolchainRows argument value)
    if(value STREQUAL "")
        continue()
    endif()
    if(argument MATCHES "=$")
        list(APPEND toolchainArgs "${argument}${value}")
    else()
        list(APPEND toolchainArgs "${argument}" "${value}")
    endif()
endwhile()
# A multi-config generator chooses the configuration when building, and warns that a
# CMAKE_BUILD_TYPE it does not read is unused.
set(configArgs "")
set(testConfigArgs "")
if(CONFIG)
    if(NOT GENERATOR_IS_MULTI_CONFIG)
        list(APPEND toolchainArgs "-DCMAKE_BUILD_TYPE=${CONFIG}")
    endif()
    set(configArgs --config "${CONFIG}")
    set(testConfigArgs --build-config "${CONFIG}")
endif()
# Steps 6 and 7 find packages in the moved prefix alone.
set(isolationArgs
    -DCMAKE_FIND_USE_PACKAGE_ROOT_PATH=OFF
    -DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF
    -DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=OFF
    -DCMAKE_FIND_USE_CMAKE_SYSTEM_PATH=OFF
    -DCMAKE_FIND_USE_SYSTEM_ENVIRONMENT_PATH=OFF
    -DCMAKE_FIND_USE_CMAKE_ENVIRONMENT_PATH=OFF)

# --- 1. core-cpp, installed by itself -------------------------------------------------------------

# Run by core-cpp's project() (CMAKE_PROJECT_<name>_INCLUDE), so everything that reads its version
# reads the bumped one, the installed core-cppConfigVersion.cmake included.
set(patchBump "${workDir}/core-cpp-patch-bump.cmake")
file(WRITE "${patchBump}"
    "# Written by tests/cmake/check-install.cmake: core-cpp as its next patch release.\n"
    "math(EXPR core-cpp_VERSION_PATCH \"\${core-cpp_VERSION_PATCH} + 1\")\n"
    "string(JOIN . core-cpp_VERSION \${core-cpp_VERSION_MAJOR} \${core-cpp_VERSION_MINOR}\n"
    "            \${core-cpp_VERSION_PATCH})\n"
    "set(PROJECT_VERSION_PATCH \"\${core-cpp_VERSION_PATCH}\")\n"
    "set(PROJECT_VERSION \"\${core-cpp_VERSION}\")\n"
    "message(STATUS \"check-install: core-cpp installs as \${core-cpp_VERSION}\")\n")

# Only what Isoline links (core::base), and nothing that would reach the network: no tests, no
# examples, no fetched dependency, none of the optional modules.
set(coreCppOptions
    -DCORE_CPP_INSTALL=ON
    -DCORE_CPP_TESTING=OFF
    -DCORE_CPP_CATCH2_MAIN=OFF
    -DCORE_CPP_BUILD_EXAMPLES=OFF
    -DCORE_CPP_FETCH_DEPS=OFF
    -DCORE_CPP_WITH_TUI=OFF
    -DCORE_CPP_WITH_TUI_OUTPUT=OFF
    -DCORE_CPP_WITH_TUI_CORE=OFF
    -DCORE_CPP_WITH_TLS=OFF
    -DCORE_CPP_WITH_SYSTEMD=OFF
    -DFETCHCONTENT_FULLY_DISCONNECTED=ON)
if(CORE_CPP_PREFIX)
    # install-core-cpp built it once for every mode (the setup fixture): its prefix is copied.
    if(NOT EXISTS "${CORE_CPP_PREFIX}/check-install-core-cpp.done")
        message(FATAL_ERROR "check-install: ${CORE_CPP_PREFIX} holds no finished core-cpp install; "
                            "install-core-cpp, the setup fixture, did not run or failed")
    endif()
    file(COPY "${CORE_CPP_PREFIX}/" DESTINATION "${PREFIX}" PATTERN "check-install-core-cpp.done" EXCLUDE)
    message(STATUS "check-install: core-cpp copied from ${CORE_CPP_PREFIX}: ok")
else()
    check_install_step("configuring vendor/core-cpp" ignored
        "${CMAKE_COMMAND}" -S "${SOURCE_DIR}/vendor/core-cpp" -B "${coreCppBuild}" ${toolchainArgs}
        ${coreCppOptions} "-DCMAKE_PROJECT_core-cpp_INCLUDE=${patchBump}")
    check_install_step("building vendor/core-cpp" ignored
        "${CMAKE_COMMAND}" --build "${coreCppBuild}" ${configArgs})
    check_install_step("installing core-cpp into ${PREFIX}" ignored
        "${CMAKE_COMMAND}" --install "${coreCppBuild}" --prefix "${PREFIX}" --component core-cpp ${configArgs})
    file(WRITE "${coreCppBuildKeyFile}" "${coreCppBuildKey}")
endif()
if(CORE_CPP_ONLY)
    file(WRITE "${PREFIX}/check-install-core-cpp.done" "${coreCppBuildKey}")
    message(STATUS "check-install: core-cpp installed into ${PREFIX} for every mode")
    return()
endif()

# --- 2. Isoline ------------------------------------------------------------------------------------

foreach(component IN LISTS components)
    check_install_step("installing component ${component} into ${PREFIX}" ignored
        "${CMAKE_COMMAND}" --install "${BUILD_DIR}" --prefix "${PREFIX}" --component "${component}"
        ${configArgs})
endforeach()

# --- 3. no built module interface -----------------------------------------------------------------

file(GLOB_RECURSE moduleInterfaces LIST_DIRECTORIES false
    "${PREFIX}/*.pcm" "${PREFIX}/*.gcm" "${PREFIX}/*.ifc")
if(moduleInterfaces)
    list(JOIN moduleInterfaces "\n  " listed)
    message(FATAL_ERROR
        "check-install: the prefix holds built module interfaces, which never install:\n  ${listed}")
endif()

# --- 4. no development flag in an exported target ---------------------------------------------------

file(STRINGS "${FORBIDDEN_OPTIONS_FILE}" forbiddenOptions)
# The export files, and the module information beside them (target-*.cmake holds a module target's
# IMPORTED_CXX_MODULES_COMPILE_OPTIONS, cxx-modules-*.cmake includes them).
file(GLOB_RECURSE targetFiles LIST_DIRECTORIES false
    "${PREFIX}/*Targets*.cmake" "${PREFIX}/*/target-*.cmake" "${PREFIX}/*/cxx-modules-*.cmake")
if(NOT targetFiles)
    message(FATAL_ERROR "check-install: the prefix holds no *Targets*.cmake to check")
endif()
set(leaked "")
foreach(targetFile IN LISTS targetFiles)
    file(READ "${targetFile}" contents)
    foreach(option IN LISTS forbiddenOptions)
        string(FIND "${contents}" "${option}" at)
        if(NOT at EQUAL -1)
            file(RELATIVE_PATH shown "${PREFIX}" "${targetFile}")
            string(APPEND leaked "\n  ${shown} names ${option}")
        endif()
    endforeach()
endforeach()
if(leaked)
    message(FATAL_ERROR
        "check-install: an installed target carries a development flag of the build:${leaked}\n"
        "Put it on the target as \$<BUILD_INTERFACE:...>, or PRIVATE.")
endif()
message(STATUS "check-install: no development flag in the exported targets: ok")

# --- 5. relocatable: no build path in an installed file, and consumed from elsewhere ----------------

include("${CMAKE_CURRENT_LIST_DIR}/check-install-paths.cmake")

check_install_comparable_paths("${SOURCE_DIR}" sourcePath)
check_install_comparable_paths("${BUILD_DIR}" buildPath)
check_install_comparable_paths("${PREFIX}" prefixPath)
# Every installed file, whatever its name, that is not binary.
file(GLOB_RECURSE installedFiles LIST_DIRECTORIES false "${PREFIX}/*")
set(leakedPaths "")
set(textFileCount 0)
foreach(installedFile IN LISTS installedFiles)
    check_install_is_binary("${installedFile}" isBinary)
    if(isBinary)
        continue()
    endif()
    math(EXPR textFileCount "${textFileCount} + 1")
    file(READ "${installedFile}" contents)
    check_install_comparable_paths("${contents}" contents)
    # PREFIX lies beneath BUILD_DIR, and both beneath SOURCE_DIR in a preset build: a file may name the
    # prefix it is installed in (none of Isoline's does), but no other path of the build.
    string(REPLACE "${prefixPath}" "<prefix>" contents "${contents}")
    foreach(path IN ITEMS "${buildPath}" "${sourcePath}")
        check_install_names_path("${contents}" "${path}" isNamed)
        if(isNamed)
            file(RELATIVE_PATH shown "${PREFIX}" "${installedFile}")
            string(APPEND leakedPaths "\n  ${shown} names ${path}")
            break()
        endif()
    endforeach()
endforeach()
if(textFileCount EQUAL 0)
    message(FATAL_ERROR "check-install: the prefix holds no text file to check for build paths")
endif()
if(leakedPaths)
    message(FATAL_ERROR "check-install: an installed file names a path of the build, so the package does not "
                        "relocate:${leakedPaths}")
endif()
message(STATUS "check-install: no build path in an installed file: ok")

# The steps below consume a copy of the prefix in another directory, with the original gone.
file(COPY "${PREFIX}/" DESTINATION "${movedPrefix}")
file(REMOVE_RECURSE "${PREFIX}")
message(STATUS "check-install: the prefix moved to ${movedPrefix}")
set(PREFIX "${movedPrefix}")

# --- 6. the package, as consumers ask for it ------------------------------------------------------

file(GLOB_RECURSE configFiles LIST_DIRECTORIES false "${PREFIX}/*/isolineConfigVersion.cmake")
if(NOT configFiles)
    message(FATAL_ERROR "check-install: the prefix holds no isolineConfigVersion.cmake")
endif()
list(GET configFiles 0 configVersionFile)
get_filename_component(packageDir "${configVersionFile}" DIRECTORY)
file(STRINGS "${configVersionFile}" versionLine REGEX "^set\\(PACKAGE_VERSION \"[0-9.]+\"\\)$")
if(NOT versionLine MATCHES "\"([0-9]+)\\.([0-9]+)\\.[0-9]+\"")
    message(FATAL_ERROR "check-install: ${configVersionFile} states no version")
endif()
set(major ${CMAKE_MATCH_1})
set(minor ${CMAKE_MATCH_2})
math(EXPR newerMinor "${minor} + 1")
math(EXPR olderMinor "${minor} - 1")

# A copy of the prefix with a second, multi-word component beside core, to prove the config's
# component discovery maps file names back to snake_case names.
file(COPY "${PREFIX}/" DESTINATION "${probePrefix}")
file(RELATIVE_PATH packageSubdir "${PREFIX}" "${packageDir}")
file(WRITE "${probePrefix}/${packageSubdir}/isolineRecipeFixtureTargets.cmake"
    "# Written by tests/cmake/check-install.cmake: a stand-in second component.\n"
    "if(NOT TARGET isoline::recipe_fixture)\n"
    "    add_library(isoline::recipe_fixture INTERFACE IMPORTED)\n"
    "endif()\n")

# The probes: (what, prefix, version, components or -, targets it must define, targets a probe target
# links or -, the generator name the config reads or - for the build's, the error it must fail with or -
# to succeed).
set(probes
    "no COMPONENTS"            "${PREFIX}"      "${major}.${minor}"      "-"
        "isoline::core"                          "-" "-" "-"
    "COMPONENTS core"          "${PREFIX}"      "${major}.${minor}"      "core"
        "isoline::core"                          "-" "-" "-"
    "a multi-word component"   "${probePrefix}" "${major}.${minor}"      "recipe_fixture"
        "isoline::recipe_fixture"                "-" "-" "-"
    "every component"          "${probePrefix}" "${major}.${minor}"      "-"
        "isoline::core|isoline::recipe_fixture"  "-" "-" "-"
    "an unknown component"     "${PREFIX}"      "${major}.${minor}"      "nope"
        "-"                                      "-" "-" "isoline has no component nope"
    "a newer minor version"    "${PREFIX}"      "${major}.${newerMinor}" "-"
        "-"                                      "-" "-" "considered but not accepted")
# SameMinorVersion, not SameMajorVersion or AnyNewerVersion: an older minor version is refused too.
if(olderMinor GREATER_EQUAL 0)
    list(APPEND probes
        "an older minor version" "${PREFIX}"    "${major}.${olderMinor}" "-"
        "-"                                      "-" "-" "considered but not accepted")
endif()
# A mode that imports installs the module target in its library's component (D11), which the config
# refuses to a target of a Visual Studio consumer that links it, and only to that.
if(modeImportsIsoline)
    list(APPEND probes
        "the module target in its library's component" "${PREFIX}" "${major}.${minor}" "core"
            "isoline::core|isoline::core_module" "isoline::core_module" "-" "-"
        "the headers under Visual Studio"              "${PREFIX}" "${major}.${minor}" "core"
            "isoline::core" "isoline::core" "Visual Studio 17 2022" "-"
        "the module under Visual Studio"               "${PREFIX}" "${major}.${minor}" "core"
            "isoline::core_module" "isoline::core_module" "Visual Studio 17 2022"
            "installed C\\+\\+ modules need a Ninja generator \\(CMake cannot build BMIs for imported targets under Visual Studio\\)")
endif()

while(probes)
    list(POP_FRONT probes what probeRoot version probeComponents targets links generatorName expectedError)
    file(REMOVE_RECURSE "${probeBuild}")
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -S "${SOURCE_DIR}/tests/cmake/package-probe" -B "${probeBuild}"
                ${toolchainArgs} ${isolationArgs} "-DCMAKE_PREFIX_PATH=${probeRoot}"
                "-DPROBE_VERSION=${version}" "-DPROBE_COMPONENTS=${probeComponents}"
                "-DPROBE_TARGETS=${targets}" "-DPROBE_LINKS=${links}" "-DPROBE_GENERATOR=${generatorName}"
        RESULT_VARIABLE status OUTPUT_VARIABLE out ERROR_VARIABLE err)
    string(REGEX REPLACE "[ \t\r\n]+" " " flatOutput "${out} ${err}")
    if(expectedError STREQUAL "-")
        if(NOT status EQUAL 0)
            message(FATAL_ERROR
                "check-install: the package probe '${what}' failed (exit ${status}):\n${out}\n${err}")
        endif()
    elseif(status EQUAL 0 OR NOT flatOutput MATCHES "${expectedError}")
        message(FATAL_ERROR "check-install: the package probe '${what}' should fail with '${expectedError}', "
                            "and did not (exit ${status}):\n${out}\n${err}")
    endif()
    message(STATUS "check-install: the package probe '${what}': as expected")
endwhile()

# --- 7. the consumer -------------------------------------------------------------------------------

## @brief Fails the check if @p output, of @p what, has a line matching @p pattern.
function(check_install_no_match what output pattern)
    string(REGEX MATCHALL "[^\n]*${pattern}[^\n]*" matches "${output}")
    if(matches)
        list(JOIN matches "\n  " listed)
        message(FATAL_ERROR "check-install: ${what}:\n  ${listed}\n\nIts whole output:\n${output}")
    endif()
endfunction()

check_install_step("configuring tests/consumer in mode ${MODE}" configureOutput
    "${CMAKE_COMMAND}" -S "${CONSUMER_SOURCE}" -B "${consumerBuild}" ${toolchainArgs} ${isolationArgs}
    "-DCMAKE_PREFIX_PATH=${PREFIX}" "-DISOLINE_CONSUMER_MODE=${MODE}")

# import std, where the consumer found it unavailable: a skip, or a failure where the lane expects it.
file(STRINGS "${consumerBuild}/CMakeCache.txt" unavailableLine REGEX "^ISOLINE_CONSUMER_IMPORT_STD_UNAVAILABLE:")
string(REGEX REPLACE "^[^=]*=" "" importStdUnavailable "${unavailableLine}")
if(importStdUnavailable)
    if(EXPECT_IMPORT_STD)
        message(FATAL_ERROR "check-install: this lane expects import std (ISOLINE_EXPECT_IMPORT_STD), but "
                            "import std unavailable: ${importStdUnavailable}")
    endif()
    message(STATUS "check-install: skipped (mode ${MODE}): import std unavailable: ${importStdUnavailable}")
    cmake_language(EXIT 77)
endif()

# E3 (D13): the one CMake warning a consumer may print, and only where it imports std. Each occurrence
# is a header line and the message beneath it.
set(importStdWarning
    "CMake Warning \\(dev\\) at [^\n]*\n  CMake's support for `import std.` in C\\+\\+23 and newer is experimental\\.")
# Counted in a copy without `;`, which would split a match into list elements.
string(REPLACE ";" "," configureText "${configureOutput}")
string(REGEX MATCHALL "CMake Warning" configureWarnings "${configureText}")
string(REGEX MATCHALL "${importStdWarning}" importStdWarnings "${configureText}")
list(LENGTH configureWarnings warningCount)
list(LENGTH importStdWarnings importStdWarningCount)
if(modeImportsStd)
    if(importStdWarningCount EQUAL 0)
        message(FATAL_ERROR "check-install: import std is no longer experimental in CMake ${CMAKE_VERSION}: drop "
                            "the gate table of tests/consumer/CMakeLists.txt and E3:\n${configureOutput}")
    endif()
    math(EXPR warningCount "${warningCount} - ${importStdWarningCount}")
endif()
if(NOT warningCount EQUAL 0)
    check_install_no_match("configuring tests/consumer warned" "${configureOutput}" "CMake Warning")
endif()
# What the consumer was configured with comes from PREFIX, and from nowhere else.
file(STRINGS "${consumerBuild}/CMakeCache.txt" packageDirs REGEX "^(isoline|core-cpp)_DIR:PATH=")
foreach(package IN ITEMS isoline core-cpp)
    set(found "")
    foreach(line IN LISTS packageDirs)
        if(line MATCHES "^${package}_DIR:PATH=(.*)$")
            set(found "${CMAKE_MATCH_1}")
        endif()
    endforeach()
    file(TO_CMAKE_PATH "${PREFIX}" normalizedPrefix)
    string(FIND "${found}" "${normalizedPrefix}/" at)
    if(NOT at EQUAL 0)
        message(FATAL_ERROR "check-install: the consumer found ${package} in '${found}', not in ${PREFIX}")
    endif()
endforeach()

# Verbose, so the compile commands are in the output for the scanning check.
check_install_step("building tests/consumer" buildOutput
    "${CMAKE_COMMAND}" --build "${consumerBuild}" --verbose ${configArgs})
# The consumer's compiler, as its configure identified it, and its standard library, as the consumer
# recorded it, for the rows below.
file(GLOB consumerCompilerFiles "${consumerBuild}/CMakeFiles/*/CMakeCXXCompiler.cmake")
set(consumerCompilerId "")
foreach(compilerFile IN LISTS consumerCompilerFiles)
    file(STRINGS "${compilerFile}" compilerIdLine REGEX "^set\\(CMAKE_CXX_COMPILER_ID \"[^\"]*\"\\)$")
    if(compilerIdLine MATCHES "\"([^\"]*)\"")
        set(consumerCompilerId "${CMAKE_MATCH_1}")
    endif()
endforeach()
file(STRINGS "${consumerBuild}/CMakeCache.txt" standardLibraryLine REGEX "^ISOLINE_CONSUMER_STANDARD_LIBRARY:")
string(REGEX REPLACE "^[^=]*=" "" consumerStandardLibrary "${standardLibraryLine}")
# CMake 4.3 tells the standard library of the compilers it does for (Clang of the GNU front end, GCC): a
# row keyed by it, which only a mode that imports std has, must not go quiet because it was not told.
if(modeImportsStd AND consumerCompilerId MATCHES "^(Clang|GNU)$" AND consumerStandardLibrary STREQUAL "")
    message(FATAL_ERROR "check-install: the consumer recorded no standard library of ${consumerCompilerId} "
                        "(ISOLINE_CONSUMER_STANDARD_LIBRARY), which the rows of toleratedBuildDiagnostics need")
endif()

# The build diagnostics a consumer may print, as (exception, where, compiler that must print it or -,
# standard library or -, line pattern) rows (D13); a line that matches its row's pattern is dropped,
# whatever the count, and every other warning still fails the check. `where` is `imports` (a mode that
# imports anything) or `import-std` (a mode that imports std). A row that names a standard library applies
# only where the consumer uses it. A row that names a compiler is its own tripwire: built by that compiler
# there, with that standard library, it must print it.
#  - E1: Clang's report of CMake's `-c` on the --precompile command that builds an imported module's BMI,
#    three times per synthetic target: from clang++ with its option, and from clang-scan-deps' two scans
#    without it. Its tripwire is cmake-precompile-tripwire.
#  - E4: turning `import std;` on makes CMake build the standard library's own std module sources in the
#    consumer, and Clang warns on their `export module std;`: `'std' is a reserved name for a module`, in a
#    file of the toolchain, never of Isoline or the consumer. One row per standard library, whose sources
#    differ: libstdc++'s bits/std.cc and bits/std.compat.cc (Clang 22.1.8 with libstdc++ 16), libc++'s
#    std.cppm and std.compat.cppm (Clang 22.1.8 with libc++ 22, the clang-22-libc++-install lane).
#    It is a warning only while -Werror stays off that target, which takes CMAKE_CXX_FLAGS
#    (docs/modules.md).
set(toleratedBuildDiagnostics
    "E1" "imports" "-" "-"
        "[^\n]*warning: argument unused during compilation: '-c'( \\[-Wunused-command-line-argument\\])?"
    "E4" "import-std" "Clang" "libstdc++"
        "[^\n]*/bits/std(\\.compat)?\\.cc:[0-9]+:[0-9]+: warning: 'std' is a reserved name for a module \\[-Wreserved-module-identifier\\]"
    "E4" "import-std" "Clang" "libc++"
        "[^\n]*/libc\\+\\+/v1/std(\\.compat)?\\.cppm:[0-9]+:[0-9]+: warning: 'std' is a reserved name for a module \\[-Wreserved-module-identifier\\]")
set(judgedBuildOutput "${buildOutput}")
set(rows "${toleratedBuildDiagnostics}")
while(rows)
    list(POP_FRONT rows exception where requiredBy standardLibrary pattern)
    if(where STREQUAL "imports")
        set(applies ${isScanningForModules})
    elseif(where STREQUAL "import-std")
        set(applies ${modeImportsStd})
    else()
        message(FATAL_ERROR "check-install: a toleratedBuildDiagnostics row of ${exception} is for `imports` or "
                            "`import-std`, not `${where}`")
    endif()
    if(NOT standardLibrary STREQUAL "-" AND NOT standardLibrary STREQUAL consumerStandardLibrary)
        set(applies OFF)
    endif()
    if(NOT applies)
        continue()
    endif()
    if(requiredBy STREQUAL consumerCompilerId AND NOT judgedBuildOutput MATCHES "${pattern}")
        message(FATAL_ERROR "check-install: ${consumerCompilerId} with ${consumerStandardLibrary} no longer prints "
                            "${exception}'s diagnostic in mode ${MODE} (`${pattern}`): drop ${exception}, its row "
                            "of toleratedBuildDiagnostics (tests/cmake/check-install.cmake) and of D13 "
                            "(docs/modules.md)\n${buildOutput}")
    endif()
    string(REGEX REPLACE "${pattern}" "" judgedBuildOutput "${judgedBuildOutput}")
endwhile()
check_install_no_match("building tests/consumer warned" "${judgedBuildOutput}"
    "([Ww]arning:|warning C[0-9]+|warning LNK[0-9]+)")
# The flags and tools by which the compilers CMake supports scan a source for module imports, or
# compile it as a module consumer: Clang, GCC and MSVC.
set(moduleScanningFlags
    "clang-scan-deps" "-fdeps-format=" "-fmodules-ts" "-fmodule-mapper=" "/scanDependencies" "/ifcOutput")
if(NOT isScanningForModules)
    foreach(flag IN LISTS moduleScanningFlags)
        string(FIND "${buildOutput}" "${flag}" at)
        if(NOT at EQUAL -1)
            message(FATAL_ERROR "check-install: mode ${MODE} imports nothing, but its consumer was "
                                "scanned for modules (${flag}):\n${buildOutput}")
        endif()
    endforeach()
    message(STATUS "check-install: the consumer was not scanned for modules: ok")
endif()
check_install_step("running tests/consumer" ignored
    "${CMAKE_CTEST_COMMAND}" --test-dir "${consumerBuild}" --output-on-failure --no-tests=error
    ${testConfigArgs})

message(STATUS "check-install: passed (mode ${MODE})")
