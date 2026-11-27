# SPDX-License-Identifier: Apache-2.0
#
# The C API's tests: C programs over the public headers (label c-api), and the lints of the vendored
# wasm.h, the symbol list, the library names and the C headers (label lint).

# The C API compiles with the language features isoline_core does (isoline_apply_language_features()), but its
# headers are C, so its interface asks a consumer for no C++ standard, contracts native or not: a C program
# links it as it is. Read through the link interface, transitively: a target it links for its consumers, not
# merely for the link ($<LINK_ONLY:...>), gives them its compile features too.
function(_isoline_c_interface_features target outVar)
    set(features ${${outVar}})
    get_target_property(aliased ${target} ALIASED_TARGET)
    if(aliased)
        set(target ${aliased})
    endif()
    get_property(seen GLOBAL PROPERTY _ISOLINE_C_INTERFACE_SEEN)
    if(target IN_LIST seen)
        set(${outVar} "${features}" PARENT_SCOPE)
        return()
    endif()
    set_property(GLOBAL APPEND PROPERTY _ISOLINE_C_INTERFACE_SEEN ${target})
    get_target_property(own ${target} INTERFACE_COMPILE_FEATURES)
    if(own)
        list(APPEND features "${target}: ${own}")
    endif()
    get_target_property(links ${target} INTERFACE_LINK_LIBRARIES)
    foreach(link IN LISTS links)
        # A generator expression is unwrapped first, conservatively: what a build or a condition would link
        # counts, what only the link reaches ($<LINK_ONLY:...>) or only an install (INSTALL_INTERFACE) does not.
        set(isUnwrapping ON)
        while(isUnwrapping)
            if(link MATCHES "^[$]<(BUILD_INTERFACE|BUILD_LOCAL_INTERFACE|COMPILE_ONLY):(.*)>$")
                set(link "${CMAKE_MATCH_2}")
            elseif(link MATCHES "^[$]<[$]<.*>:(.*)>$")
                set(link "${CMAKE_MATCH_1}")
            else()
                set(isUnwrapping OFF)
            endif()
        endwhile()
        if(link MATCHES "^[$]<(LINK_ONLY|INSTALL_INTERFACE):" OR NOT TARGET "${link}")
            continue()
        endif()
        _isoline_c_interface_features(${link} features)
    endforeach()
    set(${outVar} "${features}" PARENT_SCOPE)
endfunction()
foreach(target IN ITEMS isoline_c_shared isoline_c_static)
    set_property(GLOBAL PROPERTY _ISOLINE_C_INTERFACE_SEEN "")
    set(isolineCInterfaceFeatures "")
    _isoline_c_interface_features(${target} isolineCInterfaceFeatures)
    if(isolineCInterfaceFeatures MATCHES "cxx_std_")
        message(FATAL_ERROR "${target} asks its consumers for a C++ standard (${isolineCInterfaceFeatures}), "
                            "but its headers are C")
    endif()
endforeach()

# --- the C tests ---------------------------------------------------------------
#
# One executable per *_test.c, run as c_api.<name> for <name>_test.c, linked against the shared library
# as a C program would be. They are linked by the C++ driver nonetheless: they take core::testing_dialogs
# (the Windows dialog suppression every test binary links), and a sanitizer's C++ half (UBSan's vptr
# check, which the library's C++ code uses) is linked only by it.
#
# The programs that ask the host for memory no vector or buffer can have, so that a lost bound asks for
# exabytes, run under the build's memory cap and carry the label memory (cmake/IsolineCappedTest.cmake).
set(isolineCappedCTestPrograms types vectors)
# The programs whose host-input bounds a hardened libc++ holds to as well (the hardened-stdlib label, which the
# clang-libcxx-hardened preset runs): a size check that is lost reads past an array there even where it asks
# for no memory.
set(isolineHardenedCTestPrograms types vectors)
# Everything the hardened-stdlib label runs, built by one target the clang-libcxx-hardened preset builds, so that a
# program given the label is built there too (isoline_c_test is src/isoline/c/'s).
add_custom_target(isoline_hardened_stdlib_tests)
add_dependencies(isoline_hardened_stdlib_tests isoline_c_test)
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
    set(labels c-api)
    if(name IN_LIST isolineHardenedCTestPrograms)
        list(APPEND labels hardened-stdlib)
        add_dependencies(isoline_hardened_stdlib_tests c_api_${name})
    endif()
    if(name IN_LIST isolineCappedCTestPrograms)
        isoline_add_capped_test(NAME c_api.${name} TARGET c_api_${name} LABELS ${labels} TIMEOUT 120)
    else()
        add_test(NAME c_api.${name} COMMAND c_api_${name})
        set_tests_properties(c_api.${name} PROPERTIES LABELS "${labels}" TIMEOUT 120)
    endif()
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
    # Every struct the headers define, wasm.h's vectors included, has its layout rows (src/isoline/c/abi/layout.def),
    # which the layout tests hold to the compiler. Every C header is scanned, not only the umbrella, so a header
    # the umbrella does not include owes its rows all the same.
    file(GLOB isolineLayoutHeaders CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/src/isoline/c/*.h")
    add_test(NAME c-api-layout-rows
             COMMAND "${Python3_EXECUTABLE}" -I "${isolineSymbolsScript}" layout --compiler "${CMAKE_C_COMPILER}"
                     -I "${PROJECT_SOURCE_DIR}/src" -I "${PROJECT_SOURCE_DIR}/vendor/wasm-c-api/include"
                     --layout "${PROJECT_SOURCE_DIR}/src/isoline/c/abi/layout.def" ${isolineLayoutHeaders})
    set_tests_properties(c-api-symbols-selftest c-api-symbols-list c-api-layout-rows PROPERTIES LABELS lint)
endif()

# --- the boundary --------------------------------------------------------------
#
# No C++ exception crosses the C API and every call across it has its convention: each isoline_
# declaration is ISOLINE_API ... ISOLINE_CALL ... ISOLINE_NOEXCEPT, each function pointer a header declares
# is (ISOLINE_CALL* name), each isoline_ definition noexcept and each wasm_ body one detail::boundary() call
# and every exported symbol has exactly one definition those checks saw, so none hides behind a macro or is
# made by one (scripts/c-api-noexcept.py; its selftest breaks each rule once).
file(GLOB_RECURSE isolineBoundaryHeaders CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/src/isoline/c/*.h")
file(GLOB_RECURSE isolineBoundarySources CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/src/isoline/c/*.cpp")
list(FILTER isolineBoundarySources EXCLUDE REGEX "_test[.]cpp$")
add_test(NAME c-api-noexcept-selftest
         COMMAND "${Python3_EXECUTABLE}" -I "${PROJECT_SOURCE_DIR}/scripts/c-api-noexcept_test.py")
add_test(NAME c-api-noexcept
         COMMAND "${Python3_EXECUTABLE}" -I "${PROJECT_SOURCE_DIR}/scripts/c-api-noexcept.py"
                 --symbols "${PROJECT_SOURCE_DIR}/src/isoline/c/exported-symbols.txt"
                 ${isolineBoundaryHeaders} ${isolineBoundarySources})
set_tests_properties(c-api-noexcept-selftest c-api-noexcept PROPERTIES LABELS lint)

# docs/c-api.md's Errors section is written by scripts/c-api-error-docs.py from error.h, ErrorCodeRows and the
# pinned detail lists; the lint fails while the checked-in section differs from what they give.
add_test(NAME c-api-error-docs
         COMMAND "${Python3_EXECUTABLE}" -I "${PROJECT_SOURCE_DIR}/scripts/c-api-error-docs.py" check
                 --source "${PROJECT_SOURCE_DIR}")
set_tests_properties(c-api-error-docs PROPERTIES LABELS lint)

# --- the vectors ---------------------------------------------------------------
#
# WasmVectors.cpp is written by scripts/c-api-vectors.py from detail/VectorRows.def, five definitions per family,
# so the boundary lint above reads every one; the checked-in file must be what the rows give, and the rows
# exactly wasm.h's families (its selftest breaks each rule once).
add_test(NAME c-api-vectors-selftest
         COMMAND "${Python3_EXECUTABLE}" -I "${PROJECT_SOURCE_DIR}/scripts/c-api-vectors_test.py")
add_test(NAME c-api-vectors
         COMMAND "${Python3_EXECUTABLE}" -I "${PROJECT_SOURCE_DIR}/scripts/c-api-vectors.py" check
                 --rows "${PROJECT_SOURCE_DIR}/src/isoline/c/detail/VectorRows.def"
                 --output "${PROJECT_SOURCE_DIR}/src/isoline/c/WasmVectors.cpp"
                 --header "${PROJECT_SOURCE_DIR}/vendor/wasm-c-api/include/wasm.h")
set_tests_properties(c-api-vectors-selftest c-api-vectors PROPERTIES LABELS lint)

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

# --- every header alone --------------------------------------------------------
#
# Each C header of the C API, and the vendored wasm.h, compiles alone as C11, C17 and C++23 under the C
# API's warning set (tests/c/HeaderLint.cmake), with this tree's compiler.
include("${CMAKE_CURRENT_SOURCE_DIR}/HeaderLint.cmake")
file(GLOB isolineLintedHeaders CONFIGURE_DEPENDS RELATIVE "${PROJECT_SOURCE_DIR}/src" "${PROJECT_SOURCE_DIR}/src/isoline/c/*.h")
isoline_c_header_lint(HEADERS ${isolineLintedHeaders} wasm.h
                      INCLUDE_DIRECTORIES "${PROJECT_SOURCE_DIR}/src" "${PROJECT_SOURCE_DIR}/vendor/wasm-c-api/include")

# --- position-independent code -------------------------------------------------
#
# Only the shared library pays for PIC: isoline_core and the core-cpp libraries are compiled as the
# executables need them, and the shared library links their PIC twins, which compile the same sources
# with the same command plus the PIC flags (cmake/IsolinePicTwins.cmake); and no link but the shared
# library's links a twin. Read from Ninja's own commands, so a build with twins needs the Ninja generator,
# which every preset uses: another is a configure error, since a check that quietly goes unregistered
# checks nothing. A build without twins has nothing to check: its code is position independent everywhere
# (CMAKE_POSITION_INDEPENDENT_CODE, Mach-O) or PIC is no compiler mode at all (PE).
get_property(isolinePicTwinPairs GLOBAL PROPERTY ISOLINE_PIC_TWIN_PAIRS)
if(isolinePicTwinPairs AND NOT CMAKE_GENERATOR MATCHES "Ninja")
    message(FATAL_ERROR "[isoline] c-pic-twins reads Ninja's commands, and this build's generator is ${CMAKE_GENERATOR}: "
                        "configure with -G Ninja, or with CMAKE_POSITION_INDEPENDENT_CODE=ON, which makes no twins")
elseif(isolinePicTwinPairs)
    get_property(isolinePicTwinOptions GLOBAL PROPERTY ISOLINE_PIC_TWIN_OPTIONS)
    add_test(NAME c-pic-twins
             COMMAND "${CMAKE_COMMAND}" "-DNINJA=${CMAKE_MAKE_PROGRAM}" "-DBUILD=${PROJECT_BINARY_DIR}"
                     "-DPAIRS=${isolinePicTwinPairs}" -DSHARED=isoline_c_shared "-DCOMPILER=${CMAKE_CXX_COMPILER}"
                     "-DPIC_FLAG=${CMAKE_CXX_COMPILE_OPTIONS_PIC}" "-DTWIN_FLAGS=${isolinePicTwinOptions}"
                     -P "${CMAKE_CURRENT_SOURCE_DIR}/CheckPicTwins.cmake")
    set_tests_properties(c-pic-twins PROPERTIES LABELS lint)
endif()
# How the check reads a command, on fixtures (CheckPicTwins.cmake, SELFTEST): wherever the check can run.
if(isolinePicTwinPairs)
    add_test(NAME c-pic-twins-selftest COMMAND "${CMAKE_COMMAND}" -DSELFTEST=ON -P "${CMAKE_CURRENT_SOURCE_DIR}/CheckPicTwins.cmake")
    set_tests_properties(c-pic-twins-selftest PROPERTIES LABELS lint)
endif()

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
    include(GNUInstallDirs)
    set(isolineConsumerArguments "")
    if(NOT CMAKE_CROSSCOMPILING)
        # The pkg-config consumer, wherever pkg-config is the way a C program finds a library (not
        # Windows, whose programs find it through the CMake package, check-install's mode c, or by hand).
        if(NOT WIN32)
            find_program(ISOLINE_PKG_CONFIG NAMES pkg-config pkgconf REQUIRED)
            list(APPEND isolineConsumerArguments "-DC_COMPILER=${CMAKE_C_COMPILER}"
                        "-DPKG_CONFIG=${ISOLINE_PKG_CONFIG}" "-DC_FLAGS=${CMAKE_C_FLAGS}"
                        "-DLINK_FLAGS=${ISOLINE_LINK_OPTIONS}" "-DNM=${CMAKE_NM}"
                        "-DCONTRACTS_PATH=${ISOLINE_CONTRACTS_PATH}")
        endif()
    endif()
    add_test(NAME c-install-layout
             COMMAND "${CMAKE_COMMAND}" "-DBUILD=${PROJECT_BINARY_DIR}" "-DSOURCE=${PROJECT_SOURCE_DIR}"
                     "-DPREFIX=${CMAKE_CURRENT_BINARY_DIR}/install-layout" "-DCONFIG=$<CONFIG>"
                     "-DLIBDIR=${CMAKE_INSTALL_LIBDIR}"
                     "-DCXX_COMPILER=${CMAKE_CXX_COMPILER}" "-DCXX_FLAGS=${CMAKE_CXX_FLAGS}"
                     "-DCXX_FRONTEND=${CMAKE_CXX_COMPILER_FRONTEND_VARIANT}" ${isolineConsumerArguments}
                     -P "${CMAKE_CURRENT_SOURCE_DIR}/CheckInstall.cmake")
    set_tests_properties(c-install-layout PROPERTIES LABELS lint)
endif()
