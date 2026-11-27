# SPDX-License-Identifier: Apache-2.0
#
# Isoline installs as a CMake package that a project outside its tree finds and links.
#
#   cmake -DBUILD_DIR=<isoline build> -DSOURCE_DIR=<isoline> -DPREFIX=<empty prefix>
#         -DCONSUMER_SOURCE=<tests/consumer> -DMODE=include -DFORBIDDEN_OPTIONS_FILE=<list file>
#         [-DCXX_COMPILER=<c>] [-DGENERATOR=<g>] [-DGENERATOR_PLATFORM=<p>] [-DGENERATOR_TOOLSET=<t>]
#         [-DGENERATOR_IS_MULTI_CONFIG=ON|OFF] [-DMAKE_PROGRAM=<p>] [-DMT=<mt>] [-DCONFIG=<config>]
#         [-DEXPECT_IMPORT_STD=ON|OFF] -P tests/cmake/check-install.cmake
#
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
#   4. No installed *Targets*.cmake names an option of FORBIDDEN_OPTIONS_FILE, which the build wrote
#      with its own warning, sanitizer and coverage options: those are Isoline's development flags,
#      never a consumer's.
#   5. tests/cmake/package-probe, configured against PREFIX once per row of a probe table, finds the
#      package as consumers spell it (no COMPONENTS, a component, a multi-word component) and refuses
#      it as SameMinorVersion must (an older and a newer minor version) and for an unknown component.
#   6. tests/consumer, configured against PREFIX alone, builds and runs in MODE. Its configure must
#      print no CMake warning, and its build no compiler or linker warning and no module-scanning flag
#      unless the mode imports.
# Steps 5 and 6 search no package path but PREFIX: no system or user package registry, no system
# prefix, no environment path, so a core-cpp installed on the machine cannot stand in for PREFIX's.

cmake_minimum_required(VERSION 3.25)

foreach(required IN ITEMS BUILD_DIR SOURCE_DIR PREFIX CONSUMER_SOURCE MODE FORBIDDEN_OPTIONS_FILE)
    if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
        message(FATAL_ERROR "check-install: -D${required}=... is required")
    endif()
endforeach()

# The modes this check knows: (mode, install components, whether its consumer is scanned for modules).
# A mode is a row.
set(checkInstallModes
    "include" "isoline" OFF)
set(components "")
set(knownModes "")
set(isScanningForModules OFF)
set(modeRows ${checkInstallModes})
while(modeRows)
    list(POP_FRONT modeRows rowMode rowComponents rowScansForModules)
    list(APPEND knownModes "${rowMode}")
    if(rowMode STREQUAL MODE)
        string(REPLACE "|" ";" components "${rowComponents}")
        set(isScanningForModules ${rowScansForModules})
    endif()
endwhile()
if(NOT components)
    message(FATAL_ERROR "check-install: MODE must be one of ${knownModes}, not '${MODE}'")
endif()

get_filename_component(workDir "${PREFIX}" DIRECTORY)
set(coreCppBuild "${workDir}/core-cpp-build")
set(consumerBuild "${workDir}/consumer-build")
set(probeBuild "${workDir}/package-probe-build")
set(probePrefix "${workDir}/probe-prefix")
# Everything that is judged (the prefix, the probes, the consumer) starts empty. core-cpp's build tree
# is kept between runs, so a rerun builds it incrementally, but only for the same toolchain: it is
# keyed by everything that makes another build of it, and wiped when that changes.
file(REMOVE_RECURSE "${PREFIX}" "${consumerBuild}" "${probeBuild}" "${probePrefix}")
file(MAKE_DIRECTORY "${PREFIX}")
set(coreCppBuildKey
    "${GENERATOR}|${GENERATOR_PLATFORM}|${GENERATOR_TOOLSET}|${MAKE_PROGRAM}|${CXX_COMPILER}|${MT}|${CONFIG}")
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
# when the build has a value for it. The manifest tool is among them because the isolation below turns
# the PATH search off, and with it the one that finds MSVC's mt.exe: without it, no program links.
set(toolchainRows
    -G                       "${GENERATOR}"
    -A                       "${GENERATOR_PLATFORM}"
    -T                       "${GENERATOR_TOOLSET}"
    -DCMAKE_MAKE_PROGRAM=    "${MAKE_PROGRAM}"
    -DCMAKE_CXX_COMPILER=    "${CXX_COMPILER}"
    -DCMAKE_MT=              "${MT}")
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
# Steps 5 and 6 find packages in PREFIX alone.
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
check_install_step("configuring vendor/core-cpp" ignored
    "${CMAKE_COMMAND}" -S "${SOURCE_DIR}/vendor/core-cpp" -B "${coreCppBuild}" ${toolchainArgs}
    ${coreCppOptions} "-DCMAKE_PROJECT_core-cpp_INCLUDE=${patchBump}")
check_install_step("building vendor/core-cpp" ignored
    "${CMAKE_COMMAND}" --build "${coreCppBuild}" ${configArgs})
check_install_step("installing core-cpp into ${PREFIX}" ignored
    "${CMAKE_COMMAND}" --install "${coreCppBuild}" --prefix "${PREFIX}" --component core-cpp ${configArgs})
file(WRITE "${coreCppBuildKeyFile}" "${coreCppBuildKey}")

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
file(GLOB_RECURSE targetFiles LIST_DIRECTORIES false "${PREFIX}/*Targets*.cmake")
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

# --- 5. the package, as consumers ask for it ------------------------------------------------------

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

# The probes: (what, prefix, version, components or -, targets it must define, the error it must
# fail with or - to succeed).
set(probes
    "no COMPONENTS"            "${PREFIX}"      "${major}.${minor}"      "-"
        "isoline::core"                          "-"
    "COMPONENTS core"          "${PREFIX}"      "${major}.${minor}"      "core"
        "isoline::core"                          "-"
    "a multi-word component"   "${probePrefix}" "${major}.${minor}"      "recipe_fixture"
        "isoline::recipe_fixture"                "-"
    "every component"          "${probePrefix}" "${major}.${minor}"      "-"
        "isoline::core|isoline::recipe_fixture"  "-"
    "an unknown component"     "${PREFIX}"      "${major}.${minor}"      "nope"
        "-"                                      "isoline has no component nope"
    "a newer minor version"    "${PREFIX}"      "${major}.${newerMinor}" "-"
        "-"                                      "considered but not accepted")
# SameMinorVersion, not SameMajorVersion or AnyNewerVersion: an older minor version is refused too.
if(olderMinor GREATER_EQUAL 0)
    list(APPEND probes
        "an older minor version" "${PREFIX}"    "${major}.${olderMinor}" "-"
        "-"                                      "considered but not accepted")
endif()

while(probes)
    list(POP_FRONT probes what probeRoot version probeComponents targets expectedError)
    file(REMOVE_RECURSE "${probeBuild}")
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -S "${SOURCE_DIR}/tests/cmake/package-probe" -B "${probeBuild}"
                ${toolchainArgs} ${isolationArgs} "-DCMAKE_PREFIX_PATH=${probeRoot}"
                "-DPROBE_VERSION=${version}" "-DPROBE_COMPONENTS=${probeComponents}"
                "-DPROBE_TARGETS=${targets}"
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

# --- 6. the consumer -------------------------------------------------------------------------------

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
check_install_no_match("configuring tests/consumer warned" "${configureOutput}" "CMake Warning")
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
check_install_no_match("building tests/consumer warned" "${buildOutput}"
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
