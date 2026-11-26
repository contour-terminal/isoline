# SPDX-License-Identifier: Apache-2.0
#
# The warning set, sanitizers, coverage and clang-tidy for Isoline's OWN targets.
#
# Adapted from Contour's cmake/PedanticCompiler.cmake. Unlike there, nothing is directory-scoped:
# isoline_apply_target_policy(<target>) puts the flags on the targets Isoline creates, so the
# vendored core-cpp and a fetched Catch2 are compiled with their own settings, never with -Werror.
# The one exception is the sanitizer set, which the vendored core-cpp's targets get too.
#
# The warning set is data: one list per compiler front end. A flag the compiler does not know is
# dropped at configure time (check_cxx_compiler_flag), so one list serves GCC and Clang alike.

include_guard(GLOBAL)
include(CheckCCompilerFlag)
include(CheckCXXCompilerFlag)

set(_isolineGnuStyleWarnings
    -Wall
    -Wextra
    -pedantic
    -Wduplicate-enum
    -Wduplicated-cond
    -Wextra-semi
    -Wfinal-dtor-non-final-class
    -Wimplicit-fallthrough
    -Wlogical-op
    -Wnewline-eof
    -Wnull-dereference
    -Wpessimizing-move
    -Wredundant-move
    -Wsuggest-destructor-override
    -Wno-unknown-attributes
    -Wno-unknown-pragmas)

# Contour's set also names -Wdangling-reference, which only GCC knows and Contour switches off there
# for its false positives (gcc.gnu.org commit 6b927b12), so it is left out here.

# GCC >= 16 emits -Wnull-dereference false positives from inside libstdc++ and Catch2 headers: kept as
# a warning, but not an error.
set(_isolineGccOverrides
    -Wno-error=null-dereference)

# Test binaries only: Catch2's TEST_CASE expands __COUNTER__, which Clang flags as a C2y extension
# under -pedantic. Contour's test targets carry the same flag.
set(_isolineTestWarnings
    -Wno-c2y-extensions)

# MSVC and clang-cl: /W4 is what -Wall -Wextra means there (clang-cl's -Wall is -Weverything).
set(_isolineMsvcWarnings
    /W4
    /permissive-
    /utf-8
    /Zc:__cplusplus)

# The C sources (the C API's tests) and every C header a consumer includes: the set the C API promises
# its headers compile under (plan P17a, Global Constraints, D9), which c-headers-standalone compiles each
# header alone with (tests/c/HeaderLint.cmake).
set(ISOLINE_C_API_GNU_WARNINGS
    -Wall
    -Wextra
    -Wpedantic
    -Wstrict-prototypes
    -Wmissing-prototypes
    -Wconversion
    -Wsign-conversion
    -Wshadow
    -Wcast-qual
    -Wundef)
set(_isolineMsvcCWarnings
    /W4
    /utf-8)

# Floating-point contraction: off for every Isoline target, so that no compiler fuses a multiply and
# an add into one fused multiply-add (one rounding instead of two). WebAssembly's f32 and f64
# arithmetic is one correctly rounded operation per instruction, and the shared float kernels
# (src/isoline/numerics/Float.hpp) are that operation; GCC contracts by default (-ffp-contract=fast)
# wherever the target has an FMA instruction (arm64, s390x, x86-64 with -mfma), Clang within an
# expression (-ffp-contract=on). One row per front end, applied unprobed, as a dropped flag would
# be a silent semantic change:
#  - GCC and Clang: -ffp-contract=off.
#  - MSVC: /fp:precise, its default, named explicitly; since Visual Studio 2022 17.0 it generates no
#    contractions (only /fp:contract or /fp:fast does).
#  - clang-cl: /fp:precise is Clang's -ffp-model=precise there, which contracts within an expression,
#    so the GCC-style flag follows it, passed through /clang:.
#  - Any other front end stops the configure: it has no row, and its default may contract (Intel's
#    icx and icpx default to -fp-model=fast), so building with it would change results silently.
set(_isolineGnuStyleFloatPolicy -ffp-contract=off)
set(_isolineMsvcFloatPolicy /fp:precise)
set(_isolineClangClFloatPolicy /fp:precise /clang:-ffp-contract=off)
if(MSVC AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    set(_isolineFloatPolicy ${_isolineClangClFloatPolicy})
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    set(_isolineFloatPolicy ${_isolineMsvcFloatPolicy})
elseif(CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
    set(_isolineFloatPolicy ${_isolineGnuStyleFloatPolicy})
else()
    message(FATAL_ERROR "[isoline] no floating-point contraction row for the compiler "
                        "'${CMAKE_CXX_COMPILER_ID}': add one to cmake/IsolinePedantic.cmake.")
endif()

## @brief Appends to @p outVar each flag of the remaining arguments the @p language compiler accepts.
function(_isoline_supported_flags outVar language)
    set(accepted "")
    foreach(flag IN LISTS ARGN)
        string(REGEX REPLACE "[^A-Za-z0-9_]" "_" probe "ISOLINE_HAS_${language}_FLAG${flag}")
        if(language STREQUAL "C")
            check_c_compiler_flag("${flag}" ${probe})
        else()
            check_cxx_compiler_flag("${flag}" ${probe})
        endif()
        if(${probe})
            list(APPEND accepted "${flag}")
        endif()
    endforeach()
    set(${outVar} ${${outVar}} ${accepted} PARENT_SCOPE)
endfunction()

# The warning sets are per language (ISOLINE_CXX_WARNINGS, ISOLINE_C_WARNINGS); ISOLINE_COMPILE_OPTIONS
# is what both languages get: -Werror, the sanitizers and coverage.
set(ISOLINE_CXX_WARNINGS "")
set(ISOLINE_C_WARNINGS "")
set(ISOLINE_COMPILE_OPTIONS "")
set(ISOLINE_LINK_OPTIONS "")
set(ISOLINE_TEST_COMPILE_OPTIONS "")

if(MSVC)
    _isoline_supported_flags(ISOLINE_CXX_WARNINGS CXX ${_isolineMsvcWarnings})
    _isoline_supported_flags(ISOLINE_C_WARNINGS C ${_isolineMsvcCWarnings})
    list(APPEND ISOLINE_COMPILE_OPTIONS ${_isolineFloatPolicy})
    if(ISOLINE_WERROR)
        list(APPEND ISOLINE_COMPILE_OPTIONS /WX)
    endif()
elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    _isoline_supported_flags(ISOLINE_CXX_WARNINGS CXX -fdiagnostics-color=always ${_isolineGnuStyleWarnings})
    list(APPEND ISOLINE_COMPILE_OPTIONS ${_isolineFloatPolicy})
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        _isoline_supported_flags(ISOLINE_CXX_WARNINGS CXX ${_isolineGccOverrides})
    endif()
    _isoline_supported_flags(ISOLINE_C_WARNINGS C -fdiagnostics-color=always ${ISOLINE_C_API_GNU_WARNINGS})
    _isoline_supported_flags(ISOLINE_TEST_COMPILE_OPTIONS CXX ${_isolineTestWarnings})
    if(ISOLINE_WERROR)
        list(APPEND ISOLINE_COMPILE_OPTIONS -Werror)
    endif()
else()
    message(STATUS "[isoline] pedantic warnings: unsupported compiler ${CMAKE_CXX_COMPILER_ID}")
endif()

if(ISOLINE_SANITIZE STREQUAL "address")
    # ASan and UBSan compose; ASan and TSan do not. The runtime has to be linked in, so the same
    # flags go onto the link line.
    set(_isolineSanitizerFlags -fsanitize=address,undefined -fno-sanitize-recover=undefined
                               -fno-omit-frame-pointer)
elseif(ISOLINE_SANITIZE STREQUAL "thread")
    set(_isolineSanitizerFlags -fsanitize=thread -fno-omit-frame-pointer)
elseif(ISOLINE_SANITIZE STREQUAL "")
    set(_isolineSanitizerFlags "")
else()
    message(FATAL_ERROR "ISOLINE_SANITIZE must be empty, address or thread, not '${ISOLINE_SANITIZE}'.")
endif()
if(_isolineSanitizerFlags AND MSVC)
    message(FATAL_ERROR "ISOLINE_SANITIZE is supported with GCC and Clang only.")
endif()
list(APPEND ISOLINE_COMPILE_OPTIONS ${_isolineSanitizerFlags})
list(APPEND ISOLINE_LINK_OPTIONS ${_isolineSanitizerFlags})

# The vendored core-cpp is compiled with the same sanitizers, so no core-cpp code runs uninstrumented
# beside Isoline's (the mix that makes TSan report races that are not there). As a subproject
# core-cpp refuses CORE_CPP_SANITIZERS; it names its compiled libraries in the global property
# CORE_CPP_TARGETS for a parent to instrument instead.
if(_isolineSanitizerFlags AND ISOLINE_VENDORED_CORE_CPP)
    get_property(_isolineCoreCppTargets GLOBAL PROPERTY CORE_CPP_TARGETS)
    foreach(coreCppTarget IN LISTS _isolineCoreCppTargets)
        target_compile_options(${coreCppTarget} PRIVATE ${_isolineSanitizerFlags})
    endforeach()
endif()

if(ISOLINE_COVERAGE)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "ISOLINE_COVERAGE needs Clang (source-based coverage).")
    endif()
    # Atomic counters: the [threading] tests run guests on several threads at once, and plain
    # increments race and lose counts, so a count derived by subtraction wraps past zero.
    list(APPEND ISOLINE_COMPILE_OPTIONS -fprofile-instr-generate -fcoverage-mapping -fprofile-update=atomic)
    list(APPEND ISOLINE_LINK_OPTIONS -fprofile-instr-generate)
endif()

set(ISOLINE_CLANG_TIDY_COMMAND "")
if(ISOLINE_CLANG_TIDY)
    find_program(ISOLINE_CLANG_TIDY_EXE NAMES clang-tidy REQUIRED)
    set(ISOLINE_CLANG_TIDY_COMMAND "${ISOLINE_CLANG_TIDY_EXE}")
    message(STATUS "[isoline] clang-tidy: ${ISOLINE_CLANG_TIDY_EXE}")
endif()

message(STATUS "[isoline] compile options: ${ISOLINE_COMPILE_OPTIONS}; C++ warnings: ${ISOLINE_CXX_WARNINGS}; "
               "C warnings: ${ISOLINE_C_WARNINGS}")

## @brief Applies @p target's build-only options: COMPILE ones to its compiles, LINK ones to its links, each
## wrapped in $<BUILD_INTERFACE:...>, so that none is exported with the target (CMake exports a module
## target's own compile options for an importer's rebuild of its BMI, IMPORTED_CXX_MODULES_COMPILE_OPTIONS;
## unwrapped, -Werror and the sanitizers would reach every consumer of the installed package, D2, D11).
## CXX_COMPILE and C_COMPILE apply to the compiles of that language alone, for a target of both: a test
## that compiles C beside C++ (an installed library compiles one language, and gets its warnings as
## COMPILE, since the install tests read its options outside any compile, where $<COMPILE_LANGUAGE> has no
## meaning). Every option is recorded in the global property ISOLINE_BUILD_OPTIONS, the one record the
## install tests derive their forbidden options from (check-install step 4), so an option applied here,
## through isoline_apply_target_policy() or beside it, cannot leak into an installed target unseen.
##
## isoline_apply_build_options(<target> [COMPILE <option>...] [CXX_COMPILE <option>...]
##                             [C_COMPILE <option>...] [LINK <option>...])
function(isoline_apply_build_options target)
    cmake_parse_arguments(PARSE_ARGV 1 arg "" "" "COMPILE;CXX_COMPILE;C_COMPILE;LINK")
    if(arg_COMPILE)
        target_compile_options(${target} PRIVATE "$<BUILD_INTERFACE:${arg_COMPILE}>")
    endif()
    foreach(language IN ITEMS CXX C)
        if(arg_${language}_COMPILE)
            target_compile_options(${target} PRIVATE
                "$<BUILD_INTERFACE:$<$<COMPILE_LANGUAGE:${language}>:${arg_${language}_COMPILE}>>")
        endif()
    endforeach()
    if(arg_LINK)
        target_link_options(${target} PRIVATE "$<BUILD_INTERFACE:${arg_LINK}>")
    endif()
    set_property(GLOBAL APPEND PROPERTY ISOLINE_BUILD_OPTIONS
                 ${arg_COMPILE} ${arg_CXX_COMPILE} ${arg_C_COMPILE} ${arg_LINK})
endfunction()

## @brief Applies Isoline's warning sets, sanitizers, coverage and clang-tidy to @p target, its options
## through isoline_apply_build_options(). The warning set is the language's its sources are written in
## (ISOLINE_CXX_WARNINGS, ISOLINE_C_WARNINGS), each to its own compiles where a target has both, so call
## it once the target has its sources.
##
## isoline_apply_target_policy(<target> [TEST])
## TEST marks a test binary, which additionally gets the test-only warning adjustments and, where
## core-cpp provides it, core::testing_dialogs: on Windows a failed assertion, std::abort() or a crash
## then reports to stderr and ends the process, instead of opening a dialog (the Debug CRT's abort
## dialog, Windows Error Reporting) that waits for a click and holds the run until its timeout. A test
## executable is also one of the binaries the coverage report reads (cmake/IsolineCoverage.cmake).
function(isoline_apply_target_policy target)
    cmake_parse_arguments(PARSE_ARGV 1 arg "TEST" "" "")
    set(cxxWarnings ${ISOLINE_CXX_WARNINGS})
    if(arg_TEST)
        list(APPEND cxxWarnings ${ISOLINE_TEST_COMPILE_OPTIONS})
        if(TARGET core::testing_dialogs)
            target_link_libraries(${target} PRIVATE core::testing_dialogs)
        endif()
    endif()
    # The languages its sources are written in: C for a .c source, C++ for every other.
    get_target_property(sources ${target} SOURCES)
    set(hasC OFF)
    set(hasCxx OFF)
    foreach(source IN LISTS sources)
        if(source MATCHES "[.]c$")
            set(hasC ON)
        elseif(source MATCHES "[.](cpp|cxx|cc|cppm|ixx)$")
            set(hasCxx ON)
        endif()
    endforeach()
    set(compileOptions ${ISOLINE_COMPILE_OPTIONS})
    set(languageOptions "")
    if(hasC AND hasCxx)
        set(languageOptions CXX_COMPILE ${cxxWarnings} C_COMPILE ${ISOLINE_C_WARNINGS})
    elseif(hasC)
        list(APPEND compileOptions ${ISOLINE_C_WARNINGS})
    else()
        list(APPEND compileOptions ${cxxWarnings})
    endif()
    set(linkOptions "")
    get_target_property(type ${target} TYPE)
    if(type STREQUAL "EXECUTABLE" OR type STREQUAL "SHARED_LIBRARY")
        set(linkOptions ${ISOLINE_LINK_OPTIONS})
    endif()
    if(arg_TEST AND type STREQUAL "EXECUTABLE")
        isoline_coverage_binary(${target})
    endif()
    isoline_apply_build_options(${target} COMPILE ${compileOptions} ${languageOptions} LINK ${linkOptions})
    isoline_apply_tidy_command(${target})
endfunction()

## @brief Sets @p target's clang-tidy command, where clang-tidy runs: ISOLINE_CLANG_TIDY_COMMAND with the
## checks of the target's ISOLINE_LIFTED_TIDY_CHECKS property lifted (E2, isoline_lift_module_tidy_checks()).
## Both isoline_apply_target_policy() and the lift call it, so their order does not matter. C++ sources only:
## the one .clang-tidy is C++-oriented (naming, modernize-*), and a C subset would need a second config,
## which the one-config rule forbids; the C sources (the C API's tests) build under the strict C warning set
## and the sanitizers instead (plan P17a, I4).
function(isoline_apply_tidy_command target)
    if(NOT ISOLINE_CLANG_TIDY_COMMAND)
        return()
    endif()
    set(command "${ISOLINE_CLANG_TIDY_COMMAND}")
    get_property(lifted TARGET ${target} PROPERTY ISOLINE_LIFTED_TIDY_CHECKS)
    if(lifted)
        list(JOIN lifted "," checks)
        list(APPEND command "--checks=${checks}")
    endif()
    set_target_properties(${target} PROPERTIES CXX_CLANG_TIDY "${command}")
endfunction()
