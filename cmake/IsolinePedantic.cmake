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
set(_isolineGnuStyleFloatPolicy -ffp-contract=off)
set(_isolineMsvcFloatPolicy /fp:precise)
set(_isolineClangClFloatPolicy /fp:precise /clang:-ffp-contract=off)

## @brief Appends to @p outVar each flag of the remaining arguments the C++ compiler accepts.
function(_isoline_supported_flags outVar)
    set(accepted "")
    foreach(flag IN LISTS ARGN)
        string(REGEX REPLACE "[^A-Za-z0-9_]" "_" probe "ISOLINE_HAS_FLAG${flag}")
        check_cxx_compiler_flag("${flag}" ${probe})
        if(${probe})
            list(APPEND accepted "${flag}")
        endif()
    endforeach()
    set(${outVar} ${${outVar}} ${accepted} PARENT_SCOPE)
endfunction()

set(ISOLINE_COMPILE_OPTIONS "")
set(ISOLINE_LINK_OPTIONS "")
set(ISOLINE_TEST_COMPILE_OPTIONS "")

if(MSVC)
    _isoline_supported_flags(ISOLINE_COMPILE_OPTIONS ${_isolineMsvcWarnings})
    if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        list(APPEND ISOLINE_COMPILE_OPTIONS ${_isolineClangClFloatPolicy})
    else()
        list(APPEND ISOLINE_COMPILE_OPTIONS ${_isolineMsvcFloatPolicy})
    endif()
    if(ISOLINE_WERROR)
        list(APPEND ISOLINE_COMPILE_OPTIONS /WX)
    endif()
elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    _isoline_supported_flags(ISOLINE_COMPILE_OPTIONS -fdiagnostics-color=always ${_isolineGnuStyleWarnings})
    list(APPEND ISOLINE_COMPILE_OPTIONS ${_isolineGnuStyleFloatPolicy})
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        _isoline_supported_flags(ISOLINE_COMPILE_OPTIONS ${_isolineGccOverrides})
    endif()
    _isoline_supported_flags(ISOLINE_TEST_COMPILE_OPTIONS ${_isolineTestWarnings})
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

message(STATUS "[isoline] compile options: ${ISOLINE_COMPILE_OPTIONS}")

## @brief Applies Isoline's warning set, sanitizers, coverage and clang-tidy to @p target.
##
## isoline_apply_target_policy(<target> [TEST])
## TEST marks a test binary, which additionally gets the test-only warning adjustments and, where
## core-cpp provides it, core::testing_dialogs: on Windows a failed assertion, std::abort() or a crash
## then reports to stderr and ends the process, instead of opening a dialog (the Debug CRT's abort
## dialog, Windows Error Reporting) that waits for a click and holds the run until its timeout.
function(isoline_apply_target_policy target)
    cmake_parse_arguments(PARSE_ARGV 1 arg "TEST" "" "")
    target_compile_options(${target} PRIVATE ${ISOLINE_COMPILE_OPTIONS})
    if(arg_TEST)
        target_compile_options(${target} PRIVATE ${ISOLINE_TEST_COMPILE_OPTIONS})
        if(TARGET core::testing_dialogs)
            target_link_libraries(${target} PRIVATE core::testing_dialogs)
        endif()
    endif()
    get_target_property(type ${target} TYPE)
    if(type STREQUAL "EXECUTABLE")
        target_link_options(${target} PRIVATE ${ISOLINE_LINK_OPTIONS})
    endif()
    if(ISOLINE_CLANG_TIDY_COMMAND)
        set_target_properties(${target} PROPERTIES CXX_CLANG_TIDY "${ISOLINE_CLANG_TIDY_COMMAND}")
    endif()
endfunction()
