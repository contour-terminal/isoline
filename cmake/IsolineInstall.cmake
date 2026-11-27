# SPDX-License-Identifier: Apache-2.0
#
# Isoline as an installed CMake package, and the tests that install it.
#
#   find_package(isoline 0.1 REQUIRED COMPONENTS core)
#   target_link_libraries(app PRIVATE isoline::core)
#
# Included last by the top-level CMakeLists.txt, once every library target exists.

include_guard(GLOBAL)
include("${CMAKE_CURRENT_LIST_DIR}/isolineComponentNames.cmake")

# --- the package -----------------------------------------------------------------------------------

# The installed libraries: (library, export name, export set, install component, EXCLUDE_FROM_ALL).
# A library is a row. Its export name is what a consumer spells after isoline::, and its export set
# is isoline<ExportName>Targets, the file isolineConfig.cmake finds the component `<export name>` by.
# EXCLUDE_FROM_ALL ON installs it only by its component, never in a plain `cmake --install`.
set(ISOLINE_INSTALL_LIBRARIES
    "isoline_core"   "core"   "isolineCoreTargets"   "isoline"   OFF)

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

## @brief Installs one row of ISOLINE_INSTALL_LIBRARIES: the library with its HEADERS file set, and
## its export set as isoline<ExportName>Targets.cmake.
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
    set_target_properties(${library} PROPERTIES EXPORT_NAME "${exportName}")
    install(TARGETS ${library}
        EXPORT ${exportSet}
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT ${component} ${exclusion}
        LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT ${component} ${exclusion}
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT ${component} ${exclusion}
        FILE_SET HEADERS DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}" COMPONENT ${component} ${exclusion})
    install(EXPORT ${exportSet}
        NAMESPACE isoline::
        DESTINATION "${ISOLINE_INSTALL_PACKAGE_DIR}"
        COMPONENT ${component}
        ${exclusion})
endfunction()

## @brief Installs every row of ISOLINE_INSTALL_LIBRARIES and the package's config and version files.
function(isoline_install)
    set(rows ${ISOLINE_INSTALL_LIBRARIES})
    while(rows)
        list(POP_FRONT rows library exportName exportSet component isExcludedFromAll)
        isoline_install_library(${library} ${exportName} ${exportSet} ${component} ${isExcludedFromAll})
    endwhile()

    isoline_install_core_cpp_version(ISOLINE_INSTALL_CORE_CPP_VERSION)
    set(generated "${PROJECT_BINARY_DIR}/isoline-package")
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
        DESTINATION "${ISOLINE_INSTALL_PACKAGE_DIR}"
        COMPONENT ${ISOLINE_INSTALL_COMPONENT})
    message(STATUS "[isoline] install: the package isoline ${PROJECT_VERSION} (${compatibility}), "
                   "needing core-cpp ${ISOLINE_INSTALL_CORE_CPP_VERSION}")
endfunction()

if(ISOLINE_INSTALL)
    include(GNUInstallDirs)
    include(CMakePackageConfigHelpers)
    set(ISOLINE_INSTALL_PACKAGE_DIR "${CMAKE_INSTALL_LIBDIR}/cmake/isoline")
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

# The install tests: (name, consumer mode). A test is a row; tests/cmake/check-install.cmake knows
# each mode.
set(ISOLINE_INSTALL_TESTS
    "install-include-consumer"   "include")

# The component naming rule (cmake/isolineComponentNames.cmake), in script mode: pure, but registered
# with the install tests it serves, so a lane has either all of them or none.
set(ISOLINE_INSTALL_COMPONENT_NAMES_TEST install-component-names)

## @brief Sets @p outVar to the reason of the first row of ISOLINE_INSTALL_TEST_CONDITIONS whose
## condition does not hold, or to "" if every one holds.
function(isoline_install_tests_refusal outVar)
    set(refusal "")
    set(rows ${ISOLINE_INSTALL_TEST_CONDITIONS})
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

## @brief Registers each row of ISOLINE_INSTALL_TESTS as a CTest of label `install`, or, where a row of
## ISOLINE_INSTALL_TEST_CONDITIONS refuses, none and says why.
function(isoline_add_install_tests)
    isoline_install_tests_refusal(refusal)
    if(refusal)
        # A parent project's configure log is the parent's: it did not ask for Isoline's tests.
        if(PROJECT_IS_TOP_LEVEL)
            message(STATUS "[isoline] install tests: not registered: ${refusal}")
        endif()
        return()
    endif()
    add_test(NAME ${ISOLINE_INSTALL_COMPONENT_NAMES_TEST}
             COMMAND "${CMAKE_COMMAND}" -P "${PROJECT_SOURCE_DIR}/tests/cmake/install-component-names.cmake")
    set_tests_properties(${ISOLINE_INSTALL_COMPONENT_NAMES_TEST} PROPERTIES LABELS install TIMEOUT 60)
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
    set(rows ${ISOLINE_INSTALL_TESTS})
    while(rows)
        list(POP_FRONT rows name mode)
        add_test(NAME ${name}
                 COMMAND "${CMAKE_COMMAND}"
                         "-DBUILD_DIR=${PROJECT_BINARY_DIR}"
                         "-DSOURCE_DIR=${PROJECT_SOURCE_DIR}"
                         "-DPREFIX=${PROJECT_BINARY_DIR}/install-tests/${mode}/prefix"
                         "-DCONSUMER_SOURCE=${PROJECT_SOURCE_DIR}/tests/consumer"
                         "-DMODE=${mode}"
                         "-DCXX_COMPILER=${CMAKE_CXX_COMPILER}"
                         "-DGENERATOR=${CMAKE_GENERATOR}"
                         "-DGENERATOR_PLATFORM=${CMAKE_GENERATOR_PLATFORM}"
                         "-DGENERATOR_TOOLSET=${CMAKE_GENERATOR_TOOLSET}"
                         "-DGENERATOR_IS_MULTI_CONFIG=${isMultiConfig}"
                         "-DMAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}"
                         "-DMT=${CMAKE_MT}"
                         "-DCONFIG=$<CONFIG>"
                         "-DEXPECT_IMPORT_STD=${isExpectingImportStd}"
                         "-DFORBIDDEN_OPTIONS_FILE=${forbiddenOptionsFile}"
                         -P "${PROJECT_SOURCE_DIR}/tests/cmake/check-install.cmake")
        # It builds core-cpp and a consumer of its own: measured at 2 to 8 seconds with a warm compiler
        # cache, so the bound leaves room for a cold one on a loaded machine.
        set_tests_properties(${name} PROPERTIES LABELS install TIMEOUT 900)
    endwhile()
    list(LENGTH ISOLINE_INSTALL_TESTS cellCount)
    math(EXPR testCount "${cellCount} / 2 + 1")
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
