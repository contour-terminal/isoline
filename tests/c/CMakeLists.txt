# SPDX-License-Identifier: Apache-2.0
#
# The C API's tests: C programs over the public headers (label c-api), and the lints of the vendored
# wasm.h, the symbol list, the library names and the C headers (label lint).

# --- the C tests ---------------------------------------------------------------
#
# One executable per *_test.c, run as c_api.<name> for <name>_test.c, linked against the shared library
# as a C program would be. They are linked by the C++ driver nonetheless: they take core::testing_dialogs
# (the Windows dialog suppression every test binary links), and a sanitizer's C++ half (UBSan's vptr
# check, which the library's C++ code uses) is linked only by it.
file(GLOB isolineCTestPrograms CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*_test.c")
foreach(program IN LISTS isolineCTestPrograms)
    get_filename_component(name "${program}" NAME_WE)
    string(REGEX REPLACE "_test$" "" name "${name}")
    add_executable(c_api_${name} "${program}")
    target_link_libraries(c_api_${name} PRIVATE isoline::c)
    set_target_properties(c_api_${name} PROPERTIES
        LINKER_LANGUAGE CXX
        # Beside the shared library, so that a Windows program finds the DLL in its own directory.
        RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/bin")
    isoline_apply_target_policy(c_api_${name} TEST)
    add_test(NAME c_api.${name} COMMAND c_api_${name})
    set_tests_properties(c_api.${name} PROPERTIES LABELS c-api TIMEOUT 120)
endforeach()

# --- the vendored wasm.h -------------------------------------------------------
#
# vendor/wasm-c-api is byte for byte upstream at the commit its MANIFEST names, and the check refuses
# every way a copy can drift from it (the selftest breaks a scratch copy each way).
set(isolineVendorTool "${PROJECT_SOURCE_DIR}/cmake/WasmCApiVendor.cmake")
add_test(NAME vendor-wasm-c-api-manifest
         COMMAND "${CMAKE_COMMAND}" -DMODE=check "-DDEST=${PROJECT_SOURCE_DIR}/vendor/wasm-c-api" -P "${isolineVendorTool}")
add_test(NAME vendor-wasm-c-api-manifest-selftest
         COMMAND "${CMAKE_COMMAND}" -DMODE=selftest "-DDEST=${PROJECT_SOURCE_DIR}/vendor/wasm-c-api"
                 "-DSCRATCH=${CMAKE_CURRENT_BINARY_DIR}/vendor-selftest" -P "${isolineVendorTool}")
set_tests_properties(vendor-wasm-c-api-manifest vendor-wasm-c-api-manifest-selftest PROPERTIES LABELS lint)

# --- the symbols ---------------------------------------------------------------
#
# exported-symbols.txt lists, from what the headers declare, the functions implemented so far
# (--implemented, until the list is total), and the shared library exports exactly those. The header
# scan needs a GCC-style preprocessor, which a clang-cl tree has not; the export check runs everywhere.
# Python and the object tools are required: a lint that quietly goes unregistered checks nothing.
set(isolineSymbolsScript "${PROJECT_SOURCE_DIR}/scripts/c-api-symbols.py")
set(isolineSymbolList "${PROJECT_SOURCE_DIR}/src/isoline/c/exported-symbols.txt")
find_package(Python3 REQUIRED COMPONENTS Interpreter)
if(CMAKE_C_COMPILER_FRONTEND_VARIANT STREQUAL "GNU")
    add_test(NAME c-api-symbols-selftest
             COMMAND "${Python3_EXECUTABLE}" -I "${PROJECT_SOURCE_DIR}/scripts/c-api-symbols_test.py" "${CMAKE_C_COMPILER}")
    add_test(NAME c-api-symbols-list
             COMMAND "${Python3_EXECUTABLE}" -I "${isolineSymbolsScript}" check-list --compiler "${CMAKE_C_COMPILER}"
                     -I "${PROJECT_SOURCE_DIR}/src" -I "${PROJECT_SOURCE_DIR}/vendor/wasm-c-api/include"
                     --list "${isolineSymbolList}" --implemented "${PROJECT_SOURCE_DIR}/src/isoline/c/isoline.h")
    set_tests_properties(c-api-symbols-selftest c-api-symbols-list PROPERTIES LABELS lint)
endif()

# How each object format is inspected: format|export-listing tool|name-reading tool|the file whose names
# are read|the name it must carry. The tools are looked up by the names given, the first found wins.
get_target_property(isolineAbiName isoline_c_shared ISOLINE_C_ABI_NAME)
set(isolineObjectFormatRows
    "elf|llvm-nm,nm|llvm-readelf,readelf|library|libisoline_c.so.${isolineAbiName}"
    "pe|llvm-readobj|llvm-readobj|program|isoline_c-${isolineAbiName}.dll"
    "macho|nm|otool|library|libisoline_c.${isolineAbiName}.dylib")
set(isolineExportTools "")
foreach(row IN LISTS isolineObjectFormatRows)
    if(row MATCHES "^${ISOLINE_C_OBJECT_FORMAT}[|]([^|]+)[|]([^|]+)[|]([a-z]+)[|](.+)$")
        string(REPLACE "," ";" isolineExportTools "${CMAKE_MATCH_1}")
        string(REPLACE "," ";" isolineNameTools "${CMAKE_MATCH_2}")
        set(isolineNamedFile "${CMAKE_MATCH_3}")
        set(isolineExpectedName "${CMAKE_MATCH_4}")
    endif()
endforeach()
if(NOT isolineExportTools)
    message(FATAL_ERROR "tests/c: no row of isolineObjectFormatRows for the object format '${ISOLINE_C_OBJECT_FORMAT}'")
endif()
find_program(ISOLINE_C_EXPORT_TOOL NAMES ${isolineExportTools} REQUIRED)
find_program(ISOLINE_C_NAME_TOOL NAMES ${isolineNameTools} REQUIRED)

## @brief Adds the test @p name: the shared library @p library exports exactly what @p list names.
function(_isoline_c_exports_test name library list)
    add_test(NAME ${name}
             COMMAND "${Python3_EXECUTABLE}" -I "${isolineSymbolsScript}" exports --format "${ISOLINE_C_OBJECT_FORMAT}"
                     --tool "${ISOLINE_C_EXPORT_TOOL}" --list "${list}" "$<TARGET_FILE:${library}>")
    set_tests_properties(${name} PROPERTIES LABELS "lint;c-api")
endfunction()
_isoline_c_exports_test(c-exported-symbols isoline_c_shared "${isolineSymbolList}")

# The export rules themselves, on a fixture that defines what isoline_c_shared does not yet: a C++
# function of default visibility and the standard library's template instantiations beside its one C
# function. Built where the rules have rows (ELF, Mach-O); a DLL exports only what is dllexport.
set(isolineFormatHasRules OFF)
foreach(row IN LISTS ISOLINE_C_EXPORT_RULE_ROWS)
    if(row MATCHES "^${ISOLINE_C_OBJECT_FORMAT}[|]")
        set(isolineFormatHasRules ON)
    endif()
endforeach()
if(isolineFormatHasRules)
    add_library(isoline_c_export_fixture SHARED fixture/ExportFixture.cpp)
    target_include_directories(isoline_c_export_fixture PRIVATE "${PROJECT_SOURCE_DIR}/src")
    target_compile_definitions(isoline_c_export_fixture PRIVATE ISOLINE_C_BUILDING)
    isoline_apply_target_policy(isoline_c_export_fixture)
    isoline_c_export_rules(isoline_c_export_fixture MAP "${PROJECT_SOURCE_DIR}/src/isoline/c/isoline_c.map"
                           SYMBOLS "${CMAKE_CURRENT_SOURCE_DIR}/fixture/exported-symbols.txt")
    _isoline_c_exports_test(c-export-rules isoline_c_export_fixture "${CMAKE_CURRENT_SOURCE_DIR}/fixture/exported-symbols.txt")
endif()

# The name a loader resolves: the shared library's SONAME or install name, or, on Windows, the DLL a
# program's import table names.
if(isolineNamedFile STREQUAL "program")
    set(isolineNamedPath "$<TARGET_FILE:c_api_abi>")
else()
    set(isolineNamedPath "$<TARGET_FILE:isoline_c_shared>")
endif()
add_test(NAME c-library-names
         COMMAND "${CMAKE_COMMAND}" "-DFORMAT=${ISOLINE_C_OBJECT_FORMAT}" "-DTOOL=${ISOLINE_C_NAME_TOOL}"
                 "-DFILE=${isolineNamedPath}" "-DEXPECTED=${isolineExpectedName}"
                 -P "${CMAKE_CURRENT_SOURCE_DIR}/CheckLibraryName.cmake")
set_tests_properties(c-library-names PROPERTIES LABELS lint)

# --- clang-tidy over the C headers ---------------------------------------------
#
# The C headers are C, not C++ in the house style, so the one .clang-tidy keeps them out of its header
# filter (ExcludeHeaderFilterRegex) and ignores the C names in the C++ that defines them. One C++
# translation unit including every header, linted as the build lints, must report nothing.
if(ISOLINE_CLANG_TIDY_COMMAND)
    file(GLOB isolineCHeaders CONFIGURE_DEPENDS RELATIVE "${PROJECT_SOURCE_DIR}/src" "${PROJECT_SOURCE_DIR}/src/isoline/c/*.h")
    set(isolineHeaderUnit "// Generated by tests/c/CMakeLists.txt: every C header of the C API, for clang-tidy.\n")
    foreach(header IN LISTS isolineCHeaders)
        string(APPEND isolineHeaderUnit "#include <${header}>\n")
    endforeach()
    file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/CHeaders.cpp" CONTENT "${isolineHeaderUnit}")
    add_test(NAME clang-tidy-c-headers
             COMMAND "${ISOLINE_CLANG_TIDY_COMMAND}" "--config-file=${PROJECT_SOURCE_DIR}/.clang-tidy"
                     "${CMAKE_CURRENT_BINARY_DIR}/CHeaders.cpp" --
                     -std=c++23 -I "${PROJECT_SOURCE_DIR}/src" -I "${PROJECT_SOURCE_DIR}/vendor/wasm-c-api/include")
    # The C++ that implements the C API spells wasm.h's and the C ABI's names in C's snake_case, which
    # the .clang-tidy's naming rule ignores for those names alone (CNamesTidyFixture.cpp).
    add_test(NAME clang-tidy-c-names
             COMMAND "${ISOLINE_CLANG_TIDY_COMMAND}" "--config-file=${PROJECT_SOURCE_DIR}/.clang-tidy"
                     "${CMAKE_CURRENT_SOURCE_DIR}/CNamesTidyFixture.cpp" -- -std=c++23)
    # clang-tidy reports through warnings (errors, under WarningsAsErrors) and exits nonzero for them.
    set_tests_properties(clang-tidy-c-headers clang-tidy-c-names PROPERTIES LABELS lint
                         FAIL_REGULAR_EXPRESSION "warning:|error:")
endif()

# --- the installed layout ------------------------------------------------------
#
# An install of the component isoline-c into a scratch prefix holds the libraries, every C header beneath
# include/isoline/, the package's component c and a pkg-config file a consumer builds against
# (tests/c/CheckInstall.cmake). A cross build checks the layout alone: its consumer would need the cross
# toolchain.
if(ISOLINE_INSTALL)
    set(isolineConsumerArguments "")
    if(NOT CMAKE_CROSSCOMPILING)
        # The pkg-config consumer, wherever pkg-config is the way a C program finds a library (not
        # Windows, whose programs find it through the CMake package, check-install's mode c, or by hand).
        if(NOT WIN32)
            find_program(ISOLINE_PKG_CONFIG NAMES pkg-config pkgconf REQUIRED)
            list(APPEND isolineConsumerArguments "-DC_COMPILER=${CMAKE_C_COMPILER}"
                        "-DPKG_CONFIG=${ISOLINE_PKG_CONFIG}" "-DC_FLAGS=${CMAKE_C_FLAGS}"
                        "-DLINK_FLAGS=${ISOLINE_LINK_OPTIONS}")
        endif()
    endif()
    add_test(NAME c-install-layout
             COMMAND "${CMAKE_COMMAND}" "-DBUILD=${PROJECT_BINARY_DIR}" "-DSOURCE=${PROJECT_SOURCE_DIR}"
                     "-DPREFIX=${CMAKE_CURRENT_BINARY_DIR}/install-layout" "-DCONFIG=$<CONFIG>"
                     "-DCXX_COMPILER=${CMAKE_CXX_COMPILER}" "-DCXX_FLAGS=${CMAKE_CXX_FLAGS}"
                     "-DCXX_FRONTEND=${CMAKE_CXX_COMPILER_FRONTEND_VARIANT}" ${isolineConsumerArguments}
                     -P "${CMAKE_CURRENT_SOURCE_DIR}/CheckInstall.cmake")
    set_tests_properties(c-install-layout PROPERTIES LABELS lint)
endif()
