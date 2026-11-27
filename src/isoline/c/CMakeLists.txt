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
    Abi.cpp
    Error.cpp
    WasmTypes.cpp
    WasmVectors.cpp
    detail/Convert.cpp)
set(isolineCHeaders
    abi.h
    error.h
    export.h
    isoline.h
    val.h)
set(isolineWasmHeader "${PROJECT_SOURCE_DIR}/vendor/wasm-c-api/include/wasm.h")

## @brief Creates the C API library @p target of @p type (SHARED or STATIC) from the C API's sources, over
## the C++ API library @p core.
function(_isoline_c_library target type core)
    add_library(${target} ${type} ${isolineCSources})
    target_sources(${target}
        PUBLIC FILE_SET HEADERS
            BASE_DIRS "${PROJECT_SOURCE_DIR}/src" "${PROJECT_SOURCE_DIR}/vendor/wasm-c-api/include"
            FILES ${isolineCHeaders} "${isolineWasmHeader}")
    target_compile_definitions(${target} PRIVATE ISOLINE_C_BUILDING)
    # The headers are C11 (_Static_assert in abi.h): a consumer's C compiles as C11 at least, which MSVC's C
    # compiler, defaulting to an older mode, does not without being told.
    target_compile_features(${target} INTERFACE c_std_11)
    target_link_libraries(${target} PRIVATE ${core})
    isoline_apply_target_policy(${target})
endfunction()

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

# --- isoline_c_static ----------------------------------------------------------
_isoline_c_library(isoline_c_static STATIC isoline::core)
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
# The shared library is built over position-independent twins of isoline_core and of the core-cpp
# libraries it links (cmake/IsolinePicTwins.cmake), so that the executables and a static consumer keep
# the position-dependent code they are compiled to without it: PIC costs Clang's call round trip about
# 2 ns of 70 (P17a Unit B Task 5), and Isoline's bar is performance at or above Wasmtime's. A static
# consumer that wants PIC configures with CMAKE_POSITION_INDEPENDENT_CODE, which compiles everything so
# and makes no twin.
#
# GCC treats every global as interposable in PIC code (no inlining, no direct calls across functions);
# -fno-semantic-interposition gives that up, which is sound here because the shared library exports the C
# API alone, so nothing of isoline_core can be interposed. Clang already compiles PIC that way.
include(IsolinePicTwins)
set(isolineCoreClosure "")
_isoline_c_static_closure(isoline_core isolineCoreClosure)
isoline_pic_twins_wanted(isolinePicTwinsWanted)
# The twins, as original=twin pairs, for the check that holds them to their originals (tests/c).
set(isolinePicTwinPairs "")
set(isolinePicTwinOptions "")
if(isolinePicTwinsWanted)
    # core-cpp's libraries link Threads::Threads, an imported target of the directory that found it.
    find_package(Threads REQUIRED)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        set(isolinePicTwinOptions -fno-semantic-interposition)
    endif()
    isoline_pic_twins(${isolineCoreClosure} EXTRA_OPTIONS ${isolinePicTwinOptions})
    set(isolineSharedCore isoline_core_pic)
    foreach(original IN LISTS isolineCoreClosure)
        list(APPEND isolinePicTwinPairs "${original}=${original}_pic")
    endforeach()
else()
    set(isolineSharedCore isoline::core)
endif()
set_property(GLOBAL PROPERTY ISOLINE_PIC_TWIN_PAIRS "${isolinePicTwinPairs}")
set_property(GLOBAL PROPERTY ISOLINE_PIC_TWIN_OPTIONS "${isolinePicTwinOptions}")

_isoline_c_library(isoline_c_shared SHARED ${isolineSharedCore})
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

# The C tests run the C API's code in this library, which no test executable contains: the coverage report
# reads it beside them (cmake/IsolineCoverage.cmake).
isoline_coverage_binary(isoline_c_shared)

# What it exports: wasm_* and isoline_*, under the ABI's version node (cmake/IsolineCExports.cmake).
isoline_c_export_rules(isoline_c_shared MAP "${CMAKE_CURRENT_SOURCE_DIR}/isoline_c.map"
                       SYMBOLS "${CMAKE_CURRENT_SOURCE_DIR}/exported-symbols.txt")

# --- pkg-config ----------------------------------------------------------------
#
# Libs.private names what a static link of isoline_c needs beyond it (pkg-config --static): the
# Isoline and core-cpp libraries it is built over and the C++ runtime (plan P17a, M3); Cflags.private the
# definitions that select the static library's declarations.
include(GNUInstallDirs)


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
foreach(library IN LISTS isolineCoreClosure)
    string(APPEND ISOLINE_PC_LIBS_PRIVATE "-l$<TARGET_LINKER_FILE_BASE_NAME:${library}> ")
endforeach()
# The libraries a native language feature links its libraries with (cmake/IsolineLanguageFeatures.cmake): on a
# native-contracts build, stdc++exp, which holds the contract violation handler the closure's objects call.
# They are PRIVATE links of isoline_core, so the closure above does not name them.
foreach(feature IN LISTS ISOLINE_TARGET_LANGUAGE_FEATURES)
    string(TOUPPER "${feature}" feature)
    if(ISOLINE_${feature}_PATH STREQUAL "native")
        foreach(library IN LISTS ISOLINE_${feature}_LINK_LIBRARIES)
            string(APPEND ISOLINE_PC_LIBS_PRIVATE "-l${library} ")
        endforeach()
    endif()
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
    install(TARGETS isoline_c_static ${isolineCoreClosure}
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
    # The cases out of host memory replace the global operator new, which is the whole program's: their
    # binary is isoline_c_memory_test, below.
    set(isolineCHostMemoryTestSources "${CMAKE_CURRENT_SOURCE_DIR}/ErrorHostMemory_test.cpp"
                                       "${CMAKE_CURRENT_SOURCE_DIR}/TypesHostMemory_test.cpp"
                                       "${CMAKE_CURRENT_SOURCE_DIR}/VectorHostMemory_test.cpp")
    list(REMOVE_ITEM isolineCTestSources ${isolineCHostMemoryTestSources})
    # The C test support compiled as C, a callback the C++ tests call through the C API's trampoline.
    add_executable(isoline_c_test ${isolineCTestSources} "${CMAKE_CURRENT_SOURCE_DIR}/testing/CallingConventionProbe.c")
    target_link_libraries(isoline_c_test PRIVATE isoline::c_static Catch2::Catch2WithMain)
    target_include_directories(isoline_c_test PRIVATE "${PROJECT_SOURCE_DIR}/src")
    isoline_apply_target_policy(isoline_c_test TEST)
    # hardened-stdlib: the tests the clang-libcxx-hardened preset runs, where libc++ traps on a broken
    # precondition of its own (a string_view longer than PTRDIFF_MAX, an index past the end), so a bound on
    # host input that the C layer forgets fails there even where ASan sees no bad access.
    add_test(NAME isoline_c_test COMMAND isoline_c_test)
    set_tests_properties(isoline_c_test PROPERTIES LABELS "c-api;unit;hardened-stdlib" TIMEOUT 300)
    # A C++ exception other than std::bad_alloc inside boundary() is a defect, and ends the process rather
    # than becoming an error. The pass is the case's terminate handler's line, not an exit status: a case
    # that merely failed must not pass, and CTest's WILL_FAIL counts a process that aborts as failed.
    add_test(NAME isoline_c_test.logicErrorTerminates COMMAND isoline_c_test "Boundary.logicErrorTerminates")
    set_tests_properties(isoline_c_test.logicErrorTerminates PROPERTIES LABELS "c-api;unit;hardened-stdlib" TIMEOUT 120
                         PASS_REGULAR_EXPRESSION "terminated in boundary[(][)]"
                         FAIL_REGULAR_EXPRESSION "did not end the process")

    # Out of host memory, under a refusing allocator (tests/memory/AllocationCounter.cpp). TSan's runtime
    # defines operator new itself, not replaceably, so a TSan build leaves it out, as it does
    # isoline_memory_test.
    if(NOT ISOLINE_SANITIZE STREQUAL "thread")
        add_executable(isoline_c_memory_test ${isolineCHostMemoryTestSources}
                                             "${PROJECT_SOURCE_DIR}/tests/memory/AllocationCounter.cpp")
        target_link_libraries(isoline_c_memory_test PRIVATE isoline::c_static Catch2::Catch2WithMain)
        target_include_directories(isoline_c_memory_test PRIVATE "${PROJECT_SOURCE_DIR}/src" "${PROJECT_SOURCE_DIR}/tests/memory")
        isoline_apply_target_policy(isoline_c_memory_test TEST)
        isoline_add_capped_test(NAME isoline_c_memory_test TARGET isoline_c_memory_test LABELS "c-api;unit" TIMEOUT 120)
    endif()
endif()
