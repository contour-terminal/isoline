# SPDX-License-Identifier: Apache-2.0
#
# isoline_c: Isoline's C API, the standard wasm.h and Isoline's isoline.h, over the C++ API (isoline_core).
# Two libraries are built from the same sources:
#
#   isoline_c_shared (isoline::c)         exports wasm_* and isoline_* and nothing else; its file names
#                                         carry the ABI version (plan P17a, D8).
#   isoline_c_static (isoline::c_static)  every symbol, the internal C++ ones included; a program using
#                                         it defines ISOLINE_C_STATIC and LIBWASM_STATIC, which the
#                                         target gives its consumers.

# The ABI version, read from the header that publishes it, so that the file names, the version node and
# isoline_abi_version() cannot disagree.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS abi.h isoline_c.map)
file(STRINGS abi.h isolineAbiDefines REGEX "^#define ISOLINE_ABI_VERSION_(MAJOR|MINOR) [0-9]+$")
foreach(define IN LISTS isolineAbiDefines)
    string(REGEX MATCH "^#define ISOLINE_ABI_VERSION_(MAJOR|MINOR) ([0-9]+)$" matched "${define}")
    set(isolineAbi${CMAKE_MATCH_1} "${CMAKE_MATCH_2}")
endforeach()
if(NOT DEFINED isolineAbiMAJOR OR NOT DEFINED isolineAbiMINOR)
    message(FATAL_ERROR "src/isoline/c/abi.h defines no ISOLINE_ABI_VERSION_MAJOR or _MINOR")
endif()
# Before 1.0 every minor may break the one before it, so the names carry major.minor; from 1.0 the major.
if(isolineAbiMAJOR EQUAL 0)
    set(isolineAbiName "0.${isolineAbiMINOR}")
else()
    set(isolineAbiName "${isolineAbiMAJOR}")
endif()
message(STATUS "[isoline] C API: ABI ${isolineAbiMAJOR}.${isolineAbiMINOR}, library names carry ${isolineAbiName}")

# The version script's node is the ABI's, ISOLINE_C_<name>.
file(READ isoline_c.map isolineVersionScript)
string(REPLACE "." "[.]" isolineAbiNamePattern "${isolineAbiName}")
if(NOT isolineVersionScript MATCHES "^ISOLINE_C_${isolineAbiNamePattern} [{]")
    message(FATAL_ERROR "src/isoline/c/isoline_c.map names no version node ISOLINE_C_${isolineAbiName}")
endif()

set(isolineCSources
    Abi.cpp)
set(isolineCHeaders
    abi.h
    export.h
    isoline.h)
set(isolineWasmHeader "${PROJECT_SOURCE_DIR}/vendor/wasm-c-api/include/wasm.h")

## @brief Creates the C API library @p target of @p type (SHARED or STATIC) from the C API's sources.
function(_isoline_c_library target type)
    add_library(${target} ${type} ${isolineCSources})
    target_sources(${target}
        PUBLIC FILE_SET HEADERS
            BASE_DIRS "${PROJECT_SOURCE_DIR}/src" "${PROJECT_SOURCE_DIR}/vendor/wasm-c-api/include"
            FILES ${isolineCHeaders} "${isolineWasmHeader}")
    target_compile_definitions(${target} PRIVATE ISOLINE_C_BUILDING)
    # The headers are C11 (_Static_assert in abi.h): a consumer's C compiles as C11 at least, which MSVC's C
    # compiler, defaulting to an older mode, does not without being told.
    target_compile_features(${target} INTERFACE c_std_11)
    target_link_libraries(${target} PRIVATE isoline::core)
    isoline_apply_target_policy(${target})
endfunction()

# --- isoline_c_static ----------------------------------------------------------
_isoline_c_library(isoline_c_static STATIC)
add_library(isoline::c_static ALIAS isoline_c_static)
target_compile_definitions(isoline_c_static PUBLIC ISOLINE_C_STATIC INTERFACE LIBWASM_STATIC)
# On Windows the shared library's import library is isoline_c.lib, so the static one is named apart.
if(WIN32)
    set_target_properties(isoline_c_static PROPERTIES OUTPUT_NAME isoline_c_static)
else()
    set_target_properties(isoline_c_static PROPERTIES OUTPUT_NAME isoline_c)
endif()

# --- isoline_c_shared ----------------------------------------------------------
#
# The static libraries linked into it, isoline_core and core-cpp (a parent project's core-cpp is in
# CORE_CPP_TARGETS too), are compiled position independent. That changes their code for every program
# that links them, the CLI, the tests and a C++ embedder included, not only for the shared library: a
# static library is compiled once. What PIC costs on x86-64 and arm64 is the GOT indirection of
# preemptible symbols. GCC treats every global as interposable in PIC code (no inlining, no direct calls
# across functions); -fno-semantic-interposition gives that up, which is sound here because the shared
# library exports the C API alone, so nothing of isoline_core can be interposed. Clang already compiles
# PIC that way, and the flag measured no different there. Measured against PIE on x86-64 release builds
# (P17a Unit B Task 5): GCC shows no cost beyond noise; Clang's call round trip (Latency.callRoundTrip)
# costs about 2 ns of 70 (2-3%) more, while the interpreter's hot loop and the interrupt latency show none.
get_property(isolineCoreCppTargets GLOBAL PROPERTY CORE_CPP_TARGETS)
set_target_properties(isoline_core ${isolineCoreCppTargets} PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    foreach(pic IN ITEMS isoline_core ${isolineCoreCppTargets})
        isoline_apply_build_options(${pic} COMPILE -fno-semantic-interposition)
    endforeach()
endif()

_isoline_c_library(isoline_c_shared SHARED)
add_library(isoline::c ALIAS isoline_c_shared)
set_target_properties(isoline_c_shared PROPERTIES
    VERSION "${isolineAbiMAJOR}.${isolineAbiMINOR}.${PROJECT_VERSION_PATCH}"
    SOVERSION "${isolineAbiName}"
    MACHO_COMPATIBILITY_VERSION "${isolineAbiMAJOR}.${isolineAbiMINOR}"
    MACHO_CURRENT_VERSION "${isolineAbiMAJOR}.${isolineAbiMINOR}.${PROJECT_VERSION_PATCH}"
    # Beside the test programs, so that a Windows program finds the DLL in its own directory.
    RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/bin"
    # The ABI name the tests check the file names against (tests/c/CMakeLists.txt).
    ISOLINE_C_ABI_NAME "${isolineAbiName}")
if(WIN32)
    # isoline_c-0.<minor>.dll, with the import library isoline_c.lib.
    set_target_properties(isoline_c_shared PROPERTIES OUTPUT_NAME "isoline_c-${isolineAbiName}" ARCHIVE_OUTPUT_NAME isoline_c)
else()
    set_target_properties(isoline_c_shared PROPERTIES OUTPUT_NAME isoline_c)
endif()

# What it exports: wasm_* and isoline_*, under the ABI's version node (cmake/IsolineCExports.cmake).
isoline_c_export_rules(isoline_c_shared MAP "${CMAKE_CURRENT_SOURCE_DIR}/isoline_c.map"
                       SYMBOLS "${CMAKE_CURRENT_SOURCE_DIR}/exported-symbols.txt")

# --- pkg-config ----------------------------------------------------------------
#
# Libs.private names what a static link of isoline_c needs beyond it (pkg-config --static): the
# Isoline and core-cpp libraries it is built over and the C++ runtime (plan P17a, M3); Cflags.private the
# definitions that select the static library's declarations.
include(GNUInstallDirs)

## @brief Appends to @p outVar @p target and the static libraries it links, transitively, in link order.
function(_isoline_c_static_closure target outVar)
    set(closure ${${outVar}})
    get_target_property(aliased ${target} ALIASED_TARGET)
    if(aliased)
        set(target ${aliased})
    endif()
    get_target_property(type ${target} TYPE)
    if(NOT type STREQUAL "STATIC_LIBRARY" OR target IN_LIST closure)
        set(${outVar} ${closure} PARENT_SCOPE)
        return()
    endif()
    list(APPEND closure ${target})
    get_target_property(linked ${target} INTERFACE_LINK_LIBRARIES)
    foreach(library IN LISTS linked)
        string(REGEX REPLACE "^[$]<LINK_ONLY:(.+)>$" "\\1" library "${library}")
        if(TARGET ${library})
            _isoline_c_static_closure(${library} closure)
        endif()
    endforeach()
    set(${outVar} ${closure} PARENT_SCOPE)
endfunction()
set(isolinePcLibraries "")
_isoline_c_static_closure(isoline_core isolinePcLibraries)

# The C++ runtime a static link needs, by the standard library the build links: runtime|library. GCC
# links libstdc++, MSVC its own runtime implicitly; Clang links libc++ where it is the platform's library
# (Apple, the BSDs) or -stdlib=libc++ asks for it, and libstdc++ elsewhere.
set(isolineCxxRuntimeRows
    "libstdc++|-lstdc++"
    "libc++|-lc++"
    "msvc|")
if(MSVC)
    set(isolineCxxRuntimeKey msvc)
elseif(CMAKE_CXX_FLAGS MATCHES "-stdlib=libc[+][+]"
       OR (CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND (APPLE OR CMAKE_SYSTEM_NAME MATCHES "BSD")))
    set(isolineCxxRuntimeKey libc++)
else()
    set(isolineCxxRuntimeKey libstdc++)
endif()
set(isolineCxxRuntime "")
foreach(row IN LISTS isolineCxxRuntimeRows)
    if(row MATCHES "^([^|]+)[|](.*)$" AND CMAKE_MATCH_1 STREQUAL isolineCxxRuntimeKey)
        set(isolineCxxRuntime "${CMAKE_MATCH_2}")
    endif()
endforeach()

# The prefix is the .pc file's own location, so an install moved elsewhere still describes itself.
file(RELATIVE_PATH isolinePcToPrefix "/prefix/${CMAKE_INSTALL_LIBDIR}/pkgconfig" "/prefix")
string(REGEX REPLACE "/$" "" isolinePcToPrefix "${isolinePcToPrefix}")
set(ISOLINE_PC_PREFIX "\${pcfiledir}/${isolinePcToPrefix}")
# The static closure's own directory beneath the library directory (see install, below).
set(isolineCStaticLibdir "${CMAKE_INSTALL_LIBDIR}/isoline")
set(ISOLINE_PC_LIBS_PRIVATE "-L\${libdir}/isoline ")
foreach(library IN LISTS isolinePcLibraries)
    string(APPEND ISOLINE_PC_LIBS_PRIVATE "-l$<TARGET_LINKER_FILE_BASE_NAME:${library}> ")
endforeach()
string(APPEND ISOLINE_PC_LIBS_PRIVATE "${isolineCxxRuntime}")
string(STRIP "${ISOLINE_PC_LIBS_PRIVATE}" ISOLINE_PC_LIBS_PRIVATE)
configure_file(isoline-c.pc.in "${CMAKE_CURRENT_BINARY_DIR}/isoline-c.pc.configured" @ONLY)
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/isoline-c.pc"
     INPUT "${CMAKE_CURRENT_BINARY_DIR}/isoline-c.pc.configured")

# --- install -------------------------------------------------------------------
#
# The shared library is a row of ISOLINE_INSTALL_LIBRARIES (cmake/IsolineInstallRows.cmake): component
# isoline::c of the package isoline, installed in its own install component, isoline-c, by
# cmake/IsolineInstall.cmake. Its headers go to <includedir>/isoline/ (ISOLINE_INSTALL_HEADER_SUBDIRECTORY),
# wasm.h among them, so that this wasm.h never collides with another runtime's (Wasmtime's and Wasmer's
# development packages install include/wasm.h). The include directory a consumer is given, by pkg-config's
# Cflags and by isoline::c, is that one, so `#include <wasm.h>` and `#include <isoline/c/isoline.h>` both
# work.
#
# The static library's link closure is internal C++ libraries, which the CMake package does not export:
# pkg-config --static names them (Libs.private). The closure installs, in the same component, beneath
# <libdir>/isoline/, a directory of the C API's own, so that its archives (isoline_core, and the core-cpp
# libraries it links) never stand in for the package's or a system's; Libs.private gives its -L.
set_target_properties(isoline_c_shared PROPERTIES ISOLINE_INSTALL_HEADER_SUBDIRECTORY isoline)
if(ISOLINE_INSTALL)
    install(TARGETS isoline_c_static ${isolinePcLibraries}
            ARCHIVE DESTINATION "${isolineCStaticLibdir}" COMPONENT isoline-c)
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/isoline-c.pc" DESTINATION "${CMAKE_INSTALL_LIBDIR}/pkgconfig"
            COMPONENT isoline-c)
endif()

# --- tests ---------------------------------------------------------------------
#
# The C API's C++ tests (*_test.cpp under this directory): its internals, linked against the static
# library, which carries them. The C tests, which see the public headers only, are tests/c/.
if(ISOLINE_TESTING)
    file(GLOB_RECURSE isolineCTestSources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*_test.cpp")
    add_executable(isoline_c_test ${isolineCTestSources})
    target_link_libraries(isoline_c_test PRIVATE isoline::c_static Catch2::Catch2WithMain)
    target_include_directories(isoline_c_test PRIVATE "${PROJECT_SOURCE_DIR}/src")
    isoline_apply_target_policy(isoline_c_test TEST)
    add_test(NAME isoline_c_test COMMAND isoline_c_test)
    set_tests_properties(isoline_c_test PROPERTIES LABELS "c-api;unit" TIMEOUT 300)
endif()
