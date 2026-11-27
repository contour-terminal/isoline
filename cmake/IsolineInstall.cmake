# SPDX-License-Identifier: Apache-2.0
#
# Isoline as an installed CMake package, and the tests that install it.
#
#   find_package(isoline 0.1 REQUIRED COMPONENTS core)
#   target_link_libraries(app PRIVATE isoline::core)          # or isoline::core_module, to import it
#
# Included last by the top-level CMakeLists.txt, once every library target exists.

include_guard(GLOBAL)
include("${CMAKE_CURRENT_LIST_DIR}/isolineComponentNames.cmake")
# The library rows (ISOLINE_INSTALL_LIBRARIES), the install directories, and the module targets' install
# rule, which isoline_add_module() has already run in each module's own directory.
include("${CMAKE_CURRENT_LIST_DIR}/IsolineInstallRows.cmake")

# --- the package -----------------------------------------------------------------------------------

# Every rule is in this component: `cmake --install <build> --component isoline` installs Isoline and
# none of the rules of a dependency fetched beside it.
set(ISOLINE_INSTALL_COMPONENT isoline)

## @brief Sets @p outVar to the major.minor version of the core-cpp this build links, which the
## package's find_dependency() asks for: core-cpp's own versioning (SameMinorVersion while 0.x)
## then decides what is compatible, so a core-cpp patch release does not break the package.
function(isoline_install_core_cpp_version outVar)
    set(version "")
    get_target_property(coreBase core::base ALIASED_TARGET)
    if(coreBase)
        # A source build (the vendored copy, or a parent's): project(core-cpp) defined the version in
        # the directory that declares the target.
        get_target_property(coreBaseSourceDir ${coreBase} SOURCE_DIR)
        get_directory_property(version DIRECTORY "${coreBaseSourceDir}" DEFINITION core-cpp_VERSION)
    elseif(DEFINED core-cpp_VERSION)
        # A parent's find_package(core-cpp).
        set(version "${core-cpp_VERSION}")
    endif()
    if(NOT version MATCHES "^([0-9]+)\\.([0-9]+)")
        message(FATAL_ERROR "ISOLINE_INSTALL: the version of the core-cpp this build links is unknown "
                            "('${version}'), so the package cannot name the core-cpp it needs.")
    endif()
    set(${outVar} "${CMAKE_MATCH_1}.${CMAKE_MATCH_2}" PARENT_SCOPE)
endfunction()

## @brief Installs one row of ISOLINE_INSTALL_LIBRARIES into its export set: the library with its HEADERS
## file set, beneath <includedir> or, where the library sets ISOLINE_INSTALL_HEADER_SUBDIRECTORY, beneath
## <includedir>/<that subdirectory> (the C API's, so that its wasm.h never lands in <includedir> beside
## another runtime's). Its module target, where it has one, is already in the export set
## (isoline_install_module_target()). The export set itself is installed once, by isoline_install().
function(isoline_install_library library exportName exportSet component isExcludedFromAll)
    isoline_component_name_refusal("${exportName}" refusal)
    if(refusal)
        message(FATAL_ERROR "ISOLINE_INSTALL_LIBRARIES: ${library}'s export name ${exportName} cannot name a "
                            "component: ${refusal}.")
    endif()
    isoline_component_export_set("${exportName}" expectedExportSet)
    if(NOT exportSet STREQUAL expectedExportSet)
        message(FATAL_ERROR "ISOLINE_INSTALL_LIBRARIES: ${library}'s export set is ${exportSet}, but "
                            "isolineConfig.cmake finds the component ${exportName} as ${expectedExportSet}.")
    endif()
    set(exclusion "")
    if(isExcludedFromAll)
        set(exclusion EXCLUDE_FROM_ALL)
    endif()
    set_target_properties(${library} PROPERTIES EXPORT_NAME "${exportName}"
        MSVC_DEBUG_INFORMATION_FORMAT "${ISOLINE_INSTALL_MSVC_DEBUG_INFORMATION_FORMAT}")
    set(headerDirectory "${CMAKE_INSTALL_INCLUDEDIR}")
    get_target_property(headerSubdirectory ${library} ISOLINE_INSTALL_HEADER_SUBDIRECTORY)
    if(headerSubdirectory)
        string(APPEND headerDirectory "/${headerSubdirectory}")
    endif()
    install(TARGETS ${library}
        EXPORT ${exportSet}
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT ${component} ${exclusion}
        LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT ${component} ${exclusion}
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT ${component} ${exclusion}
        FILE_SET HEADERS DESTINATION "${headerDirectory}" COMPONENT ${component} ${exclusion})
endfunction()

## @brief Sets @p outVar to the export names of the rows of ISOLINE_INSTALL_LIBRARIES, other than
## @p library's own, whose library or module target @p library or its module target links: the components
## isolineConfig.cmake loads before @p library's (a module row's IMPORTS are among those links).
function(isoline_install_required_components library outVar)
    set(required "")
    foreach(target IN ITEMS ${library} ${library}_module)
        if(NOT TARGET ${target})
            continue()
        endif()
        get_target_property(links ${target} LINK_LIBRARIES)
        foreach(link IN LISTS links)
            if(NOT TARGET ${link})
                continue()
            endif()
            get_target_property(aliased ${link} ALIASED_TARGET)
            if(aliased)
                set(link "${aliased}")
            endif()
            string(REGEX REPLACE "_module$" "" linkedLibrary "${link}")
            if(linkedLibrary STREQUAL library)
                continue()
            endif()
            isoline_install_library_row(${linkedLibrary} linked)
            if(linkedFound)
                list(APPEND required "${linkedExportName}")
            endif()
        endforeach()
    endforeach()
    list(REMOVE_DUPLICATES required)
    set(${outVar} "${required}" PARENT_SCOPE)
endfunction()

## @brief Installs every row of ISOLINE_INSTALL_LIBRARIES, each export set once, and the package's config
## and version files. Rows of one export set must agree on its install component and EXCLUDE_FROM_ALL.
function(isoline_install)
    set(exportSets "")
    set(rows ${ISOLINE_INSTALL_LIBRARIES})
    set(generated "${PROJECT_BINARY_DIR}/isoline-package")
    while(rows)
        list(POP_FRONT rows library exportName exportSet component isExcludedFromAll)
        isoline_install_library(${library} ${exportName} ${exportSet} ${component} ${isExcludedFromAll})
        # What the component needs loaded first, beside its targets file (isolineConfig.cmake reads it).
        isoline_install_required_components(${library} required)
        string(REGEX REPLACE "Targets$" "Requires" requiresStem "${exportSet}")
        file(CONFIGURE OUTPUT "${generated}/${requiresStem}.cmake"
             CONTENT "# The components isoline::${exportName} needs loaded before it (cmake/IsolineInstall.cmake).\nset(_isolineRequires \"@required@\")\n"
             @ONLY)
        set(requiresExclusion "")
        if(isExcludedFromAll)
            set(requiresExclusion EXCLUDE_FROM_ALL)
        endif()
        install(FILES "${generated}/${requiresStem}.cmake" DESTINATION "${ISOLINE_INSTALL_PACKAGE_DIR}"
                COMPONENT ${component} ${requiresExclusion})
        if(exportSet IN_LIST exportSets)
            if(NOT "${component}|${isExcludedFromAll}" STREQUAL "${exportSetInstall_${exportSet}}")
                message(FATAL_ERROR "ISOLINE_INSTALL_LIBRARIES: the rows of export set ${exportSet} disagree on "
                                    "its component and EXCLUDE_FROM_ALL: ${exportSetInstall_${exportSet}} and "
                                    "${component}|${isExcludedFromAll} (${library}).")
            endif()
        else()
            list(APPEND exportSets ${exportSet})
            set(exportSetInstall_${exportSet} "${component}|${isExcludedFromAll}")
        endif()
    endwhile()
    foreach(exportSet IN LISTS exportSets)
        string(REPLACE "|" ";" install "${exportSetInstall_${exportSet}}")
        list(GET install 0 component)
        list(GET install 1 isExcludedFromAll)
        set(exclusion "")
        if(isExcludedFromAll)
            set(exclusion EXCLUDE_FROM_ALL)
        endif()
        # The module information of an export set's module targets, relative to its destination.
        set(modulesDirectory "")
        get_property(hasModules GLOBAL PROPERTY ISOLINE_INSTALL_MODULES_OF_${exportSet})
        if(hasModules)
            file(RELATIVE_PATH modulesSubdirectory "/${ISOLINE_INSTALL_PACKAGE_DIR}" "/${ISOLINE_INSTALL_MODULE_DIR}")
            set(modulesDirectory CXX_MODULES_DIRECTORY "${modulesSubdirectory}")
        endif()
        install(EXPORT ${exportSet}
            NAMESPACE isoline::
            DESTINATION "${ISOLINE_INSTALL_PACKAGE_DIR}"
            ${modulesDirectory}
            COMPONENT ${component}
            ${exclusion})
    endforeach()

    isoline_install_core_cpp_version(ISOLINE_INSTALL_CORE_CPP_VERSION)
    configure_package_config_file("${PROJECT_SOURCE_DIR}/cmake/isolineConfig.cmake.in"
        "${generated}/isolineConfig.cmake"
        INSTALL_DESTINATION "${ISOLINE_INSTALL_PACKAGE_DIR}")
    # A minor release may break the API while Isoline is 0.x; from 1.0 only a major one may.
    set(compatibility SameMajorVersion)
    if(PROJECT_VERSION_MAJOR EQUAL 0)
        set(compatibility SameMinorVersion)
    endif()
    write_basic_package_version_file("${generated}/isolineConfigVersion.cmake"
        VERSION "${PROJECT_VERSION}"
        COMPATIBILITY ${compatibility})
    install(FILES "${generated}/isolineConfig.cmake" "${generated}/isolineConfigVersion.cmake"
                  "${PROJECT_SOURCE_DIR}/cmake/isolineComponentNames.cmake"
                  "${PROJECT_SOURCE_DIR}/cmake/isolineConsumerToolchain.cmake"
        DESTINATION "${ISOLINE_INSTALL_PACKAGE_DIR}"
        COMPONENT ${ISOLINE_INSTALL_COMPONENT})
    message(STATUS "[isoline] install: the package isoline ${PROJECT_VERSION} (${compatibility}), "
                   "needing core-cpp ${ISOLINE_INSTALL_CORE_CPP_VERSION}")
endfunction()

if(ISOLINE_INSTALL)
    include(CMakePackageConfigHelpers)
    isoline_install()
endif()

# --- the tests that install the package ------------------------------------------------------------

# Where the install tests are registered: (condition, reason) rows, every one of which must hold. The
# reason of the first that does not is printed, so a lane without them says why.
set(ISOLINE_INSTALL_TEST_CONDITIONS
    "ISOLINE_TESTING"            "the tests are off (ISOLINE_TESTING)"
    "ISOLINE_INSTALL"            "nothing is installed (ISOLINE_INSTALL is OFF)"
    "NOT CMAKE_CROSSCOMPILING"   "a cross build's consumer cannot run on the build host"
    "NOT ISOLINE_SANITIZE"       "a sanitized build is never installed (ISOLINE_SANITIZE)"
    "NOT ISOLINE_COVERAGE"       "a coverage build is never installed (ISOLINE_COVERAGE)"
    "CMAKE_GENERATOR MATCHES \"^Ninja\""
                                 "they are proven with the Ninja generators only (${CMAKE_GENERATOR})")

# Where the install tests of a mode that imports are registered, beyond the rows above: (condition,
# reason) rows, every one of which must hold. The reason of the first that does not is printed.
set(ISOLINE_INSTALL_MODULE_TEST_CONDITIONS
    "ISOLINE_MODULES"              "the module is off (ISOLINE_MODULES)"
    "ISOLINE_MODULES_INSTALLABLE"  "the generator ${CMAKE_GENERATOR} cannot install C++ modules")

# The install tests are the consumer's modes: one CTest per row of ISOLINE_CONSUMER_MODES, which
# tests/cmake/check-install.cmake and tests/consumer read too.
include("${PROJECT_SOURCE_DIR}/tests/consumer/ConsumerModes.cmake")

# The component naming rule (cmake/isolineComponentNames.cmake), in script mode: pure, but registered
# with the install tests it serves, so a lane has either all of them or none.
set(ISOLINE_INSTALL_COMPONENT_NAMES_TEST install-component-names)

## @brief Sets @p outVar to the reason of the first row of the (condition, reason) table @p conditions
## (ISOLINE_INSTALL_TEST_CONDITIONS, ISOLINE_INSTALL_MODULE_TEST_CONDITIONS) whose condition does not
## hold, or to "" if every one holds.
function(isoline_install_tests_refusal conditions outVar)
    set(refusal "")
    set(rows ${${conditions}})
    while(rows)
        list(POP_FRONT rows condition reason)
        cmake_language(EVAL CODE "if(${condition})\nset(holds ON)\nelse()\nset(holds OFF)\nendif()")
        if(NOT holds)
            set(refusal "${reason}")
            break()
        endif()
    endwhile()
    set(${outVar} "${refusal}" PARENT_SCOPE)
endfunction()

## @brief Registers each row of ISOLINE_CONSUMER_MODES as a CTest of label `install` (and `modules`, for a
## mode that imports), or, where a row of ISOLINE_INSTALL_TEST_CONDITIONS refuses, none and says why. A
## mode that imports is registered where ISOLINE_INSTALL_MODULE_TEST_CONDITIONS hold, too.
function(isoline_add_install_tests)
    isoline_install_tests_refusal(ISOLINE_INSTALL_TEST_CONDITIONS refusal)
    if(refusal)
        # A parent project's configure log is the parent's: it did not ask for Isoline's tests.
        if(PROJECT_IS_TOP_LEVEL)
            message(STATUS "[isoline] install tests: not registered: ${refusal}")
        endif()
        return()
    endif()
    isoline_install_tests_refusal(ISOLINE_INSTALL_MODULE_TEST_CONDITIONS moduleRefusal)
    add_test(NAME ${ISOLINE_INSTALL_COMPONENT_NAMES_TEST}
             COMMAND "${CMAKE_COMMAND}" -P "${PROJECT_SOURCE_DIR}/tests/cmake/install-component-names.cmake")
    set_tests_properties(${ISOLINE_INSTALL_COMPONENT_NAMES_TEST} PROPERTIES LABELS install TIMEOUT 60)
    # What the package asks of its consumer's toolchain (cmake/isolineConsumerToolchain.cmake), likewise pure.
    add_test(NAME install-consumer-toolchain
             COMMAND "${CMAKE_COMMAND}" -P "${PROJECT_SOURCE_DIR}/tests/cmake/install-consumer-toolchain.cmake")
    set_tests_properties(install-consumer-toolchain PROPERTIES LABELS install TIMEOUT 60)
    # The build's own development flags, which no installed target may carry (check-install step 4).
    set(forbiddenOptionsFile "${ISOLINE_FORBIDDEN_OPTIONS_FILE}")
    set(isExpectingImportStd OFF)
    if(ISOLINE_EXPECT_IMPORT_STD)
        set(isExpectingImportStd ON)
    endif()
    get_property(isMultiConfig GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
    if(NOT isMultiConfig)
        set(isMultiConfig OFF)
    endif()
    # The toolchain goes to every project check-install configures. Of the std module's description it is
    # the cache entry, the one a lane sets to override CMake's (CMakeCXXCompiler.cmake shadows it with a
    # variable of CMake's own finding, which a consumer finds for itself).
    set(toolchainArguments
        "-DBUILD_DIR=${PROJECT_BINARY_DIR}"
        "-DSOURCE_DIR=${PROJECT_SOURCE_DIR}"
        "-DCXX_COMPILER=${CMAKE_CXX_COMPILER}"
        "-DCXX_FLAGS=${CMAKE_CXX_FLAGS}"
        "-DSTDLIB_MODULES_JSON=$CACHE{CMAKE_CXX_STDLIB_MODULES_JSON}"
        "-DGENERATOR=${CMAKE_GENERATOR}"
        "-DGENERATOR_PLATFORM=${CMAKE_GENERATOR_PLATFORM}"
        "-DGENERATOR_TOOLSET=${CMAKE_GENERATOR_TOOLSET}"
        "-DGENERATOR_IS_MULTI_CONFIG=${isMultiConfig}"
        "-DMAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}"
        "-DMT=${CMAKE_MT}"
        "-DAR=${CMAKE_AR}"
        "-DRANLIB=${CMAKE_RANLIB}"
        "-DCONFIG=$<CONFIG>")
    # core-cpp, built and installed once into a prefix every mode copies (check-install's CORE_CPP_ONLY):
    # the setup fixture of every mode's test, so a lane builds it once rather than once per mode.
    set(coreCppPrefix "${PROJECT_BINARY_DIR}/install-tests/core-cpp/prefix")
    add_test(NAME install-core-cpp
             COMMAND "${CMAKE_COMMAND}" ${toolchainArguments} "-DPREFIX=${coreCppPrefix}" -DCORE_CPP_ONLY=ON
                     -P "${PROJECT_SOURCE_DIR}/tests/cmake/check-install.cmake")
    # A cold build of core-cpp's base library on a loaded machine.
    set_tests_properties(install-core-cpp PROPERTIES LABELS install TIMEOUT 900
                                                     FIXTURES_SETUP isoline-install-core-cpp)
    set(testCount 2)
    isoline_consumer_mode("" every)
    foreach(mode IN LISTS everyKnownModes)
        isoline_consumer_mode("${mode}" consumer)
        # A mode that uses a component this build installs no library for (the C API's, without
        # ISOLINE_C_API) has nothing to consume.
        set(missingComponent "")
        foreach(packageComponent IN LISTS consumerPackageComponents)
            set(isInstalled OFF)
            set(rows ${ISOLINE_INSTALL_LIBRARIES})
            while(rows)
                list(POP_FRONT rows rowLibrary rowExportName rowExportSet rowComponent rowIsExcludedFromAll)
                if(rowExportName STREQUAL packageComponent)
                    set(isInstalled ON)
                endif()
            endwhile()
            if(NOT isInstalled AND NOT missingComponent)
                set(missingComponent "${packageComponent}")
            endif()
        endforeach()
        if(missingComponent)
            message(STATUS "[isoline] install tests: ${consumerTest} not registered: this build installs no "
                           "component ${missingComponent}")
            continue()
        endif()
        set(labels install)
        if(consumerImportsIsoline OR consumerImportsStd)
            if(moduleRefusal)
                message(STATUS "[isoline] install tests: ${consumerTest} not registered: ${moduleRefusal}")
                continue()
            endif()
            list(APPEND labels modules)
        endif()
        add_test(NAME ${consumerTest}
                 COMMAND "${CMAKE_COMMAND}" ${toolchainArguments}
                         "-DPREFIX=${PROJECT_BINARY_DIR}/install-tests/${mode}/prefix"
                         "-DCORE_CPP_PREFIX=${coreCppPrefix}"
                         "-DCONSUMER_SOURCE=${PROJECT_SOURCE_DIR}/tests/consumer"
                         "-DMODE=${mode}"
                         "-DEXPECT_IMPORT_STD=${isExpectingImportStd}"
                         "-DFORBIDDEN_OPTIONS_FILE=${forbiddenOptionsFile}"
                         -P "${PROJECT_SOURCE_DIR}/tests/cmake/check-install.cmake")
        # It builds a consumer of its own: measured at 2 to 8 seconds with a warm compiler cache, so the
        # bound leaves room for a cold one on a loaded machine. Exit 77 is a mode whose toolchain lacks what
        # it needs (import std), on a lane that does not expect it.
        set_tests_properties(${consumerTest} PROPERTIES LABELS "${labels}" TIMEOUT 900 SKIP_RETURN_CODE 77
                                                       FIXTURES_REQUIRED isoline-install-core-cpp)
        math(EXPR testCount "${testCount} + 1")
    endforeach()
    message(STATUS "[isoline] install tests: ${testCount} registered")
endfunction()

# The build's own options, as isoline_apply_build_options() recorded them (ISOLINE_BUILD_OPTIONS): what no
# installed target may carry, which check-install step 4 reads. Written whatever the install tests' rows say,
# for install-forbidden-options. Included last, so every target has been given its options.
set(ISOLINE_FORBIDDEN_OPTIONS_FILE "${PROJECT_BINARY_DIR}/install-tests/forbidden-options.txt")
get_property(isolineBuildOptions GLOBAL PROPERTY ISOLINE_BUILD_OPTIONS)
list(REMOVE_DUPLICATES isolineBuildOptions)
list(JOIN isolineBuildOptions "\n" isolineForbiddenLines)
file(CONFIGURE OUTPUT "${ISOLINE_FORBIDDEN_OPTIONS_FILE}" CONTENT "${isolineForbiddenLines}\n" @ONLY)

## @brief Registers install-forbidden-options (label lint): every option an installed target (a row of
## ISOLINE_INSTALL_LIBRARIES, and its module target) compiles or links with is in the forbidden list, so the
## record isoline_apply_build_options() keeps covers every option of the installed targets, and a flag
## added beside it (target_compile_options() on an installed library) fails here instead of escaping
## check-install's leak check.
function(isoline_add_forbidden_options_test)
    if(NOT ISOLINE_TESTING OR NOT ISOLINE_INSTALL)
        return()
    endif()
    set(content "")
    set(rows ${ISOLINE_INSTALL_LIBRARIES})
    while(rows)
        list(POP_FRONT rows library exportName exportSet component isExcludedFromAll)
        foreach(target IN ITEMS ${library} ${library}_module)
            if(TARGET ${target})
                string(APPEND content "${target}\t$<JOIN:$<TARGET_PROPERTY:${target},COMPILE_OPTIONS>,\t>"
                                      "\t$<JOIN:$<TARGET_PROPERTY:${target},LINK_OPTIONS>,\t>\n")
            endif()
        endforeach()
    endwhile()
    set(appliedFile "${PROJECT_BINARY_DIR}/install-tests/installed-target-options-$<CONFIG>.txt")
    file(GENERATE OUTPUT "${appliedFile}" CONTENT "${content}")
    add_test(NAME install-forbidden-options
             COMMAND "${CMAKE_COMMAND}" "-DAPPLIED_FILE=${appliedFile}"
                     "-DFORBIDDEN_OPTIONS_FILE=${ISOLINE_FORBIDDEN_OPTIONS_FILE}"
                     -P "${PROJECT_SOURCE_DIR}/tests/cmake/check-forbidden-options.cmake")
    set_tests_properties(install-forbidden-options PROPERTIES LABELS lint TIMEOUT 60)
endfunction()

isoline_add_forbidden_options_test()
isoline_add_install_tests()
