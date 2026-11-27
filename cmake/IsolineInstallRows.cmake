# SPDX-License-Identifier: Apache-2.0
#
# What Isoline installs, as data, and the one install rule that must run before cmake/IsolineInstall.cmake:
# a module target's. Included by cmake/IsolineModules.cmake, before any library target exists, so that
# isoline_add_module() can install the module target it creates in the directory that creates it, and by
# cmake/IsolineInstall.cmake, which installs everything else once every target exists.

include_guard(GLOBAL)
include(GNUInstallDirs)

# The installed libraries: (library, export name, export set, install component, EXCLUDE_FROM_ALL).
# A library is a row. Its export name is what a consumer spells after isoline::, and its export set
# is isoline<ExportName>Targets, the file isolineConfig.cmake finds the component `<export name>` by.
# EXCLUDE_FROM_ALL ON installs it only by its component, never in a plain `cmake --install`. A library's
# module target (isoline_add_module()) installs beside it, in its export set and component. What a
# component needs loaded before it is derived, not listed: the rows' libraries its library and module
# target link (a module row's IMPORTS link the imported modules' targets), written beside its targets file
# as isoline<ExportName>Requires.cmake, which isolineConfig.cmake reads.
set(ISOLINE_INSTALL_LIBRARIES
    "isoline_core"   "core"   "isolineCoreTargets"   "isoline"   OFF
    "isoline_text"   "text"   "isolineTextTargets"   "isoline"   OFF)

# The debug information of an installed library's objects on MSVC: embedded in them (/Z7), never in a PDB
# beside the build (/Zi), which a static library's consumer links against by its build path. A prefix that
# is moved (or packaged) then links its Debug libraries without warning LNK4099 ("PDB was not found"). An
# empty value, as in Release, keeps the toolchain's default (none).
set(ISOLINE_INSTALL_MSVC_DEBUG_INFORMATION_FORMAT "$<$<CONFIG:Debug,RelWithDebInfo>:Embedded>")

# The package's directory, and beneath it the installed interface units with the module information
# CMake writes for them (D11).
set(ISOLINE_INSTALL_PACKAGE_DIR "${CMAKE_INSTALL_LIBDIR}/cmake/isoline")
set(ISOLINE_INSTALL_MODULE_DIR "${ISOLINE_INSTALL_PACKAGE_DIR}/modules")

## @brief Looks up the row of ISOLINE_INSTALL_LIBRARIES for @p library.
##
## Sets <outPrefix>Found to ON or OFF and, where found, <outPrefix>ExportName, <outPrefix>ExportSet,
## <outPrefix>Component and <outPrefix>IsExcludedFromAll.
function(isoline_install_library_row library outPrefix)
    set(rows ${ISOLINE_INSTALL_LIBRARIES})
    while(rows)
        list(POP_FRONT rows rowLibrary exportName exportSet component isExcludedFromAll)
        if(rowLibrary STREQUAL library)
            set(${outPrefix}Found ON PARENT_SCOPE)
            set(${outPrefix}ExportName "${exportName}" PARENT_SCOPE)
            set(${outPrefix}ExportSet "${exportSet}" PARENT_SCOPE)
            set(${outPrefix}Component "${component}" PARENT_SCOPE)
            set(${outPrefix}IsExcludedFromAll ${isExcludedFromAll} PARENT_SCOPE)
            return()
        endif()
    endwhile()
    set(${outPrefix}Found OFF PARENT_SCOPE)
endfunction()

## @brief Installs @p moduleTarget, the module target of @p library, into @p library's export set and
## component: its interface unit as source, in its CXX_MODULES file set, never a BMI (D11). Where the
## generator cannot install module information it installs nothing and says why; without ISOLINE_INSTALL,
## nothing either. Sets the global property ISOLINE_INSTALL_MODULES_OF_<export set>, which
## cmake/IsolineInstall.cmake reads to install the export set's module information.
##
## Call it in the directory that creates @p moduleTarget (isoline_add_module() does). CMake 4.3 writes an
## export's IMPORTED_CXX_MODULES_<CONFIG> relative to the install prefix only for a target installed in
## its own directory; installed from another one (the top level, as the other rows are), the installed
## target-<name>-<config>.cmake names the interface unit by its absolute source path instead (measured,
## 2026-11-27), and the package is no longer relocatable. check-install proves it is.
function(isoline_install_module_target moduleTarget library)
    if(NOT ISOLINE_INSTALL)
        return()
    endif()
    isoline_install_library_row(${library} row)
    if(NOT rowFound)
        message(FATAL_ERROR "[isoline] ${moduleTarget}: its library ${library} has no row in "
                            "ISOLINE_INSTALL_LIBRARIES (cmake/IsolineInstallRows.cmake)")
    endif()
    if(NOT ISOLINE_MODULES_INSTALLABLE)
        # D11: a Visual Studio generator can neither install module information nor build a BMI for an
        # imported module target (cmake-cxxmodules(7)); its in-tree module builds are unaffected.
        message(STATUS "[isoline] install: ${moduleTarget} is not installed: the generator ${CMAKE_GENERATOR} "
                       "cannot install C++ module information (cmake-cxxmodules(7))")
        return()
    endif()
    set(exclusion "")
    if(rowIsExcludedFromAll)
        set(exclusion EXCLUDE_FROM_ALL)
    endif()
    set_target_properties(${moduleTarget} PROPERTIES
        MSVC_DEBUG_INFORMATION_FORMAT "${ISOLINE_INSTALL_MSVC_DEBUG_INFORMATION_FORMAT}")
    install(TARGETS ${moduleTarget}
        EXPORT ${rowExportSet}
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT ${rowComponent} ${exclusion}
        FILE_SET CXX_MODULES DESTINATION "${ISOLINE_INSTALL_MODULE_DIR}" COMPONENT ${rowComponent} ${exclusion})
    set_property(GLOBAL PROPERTY ISOLINE_INSTALL_MODULES_OF_${rowExportSet} ON)
endfunction()
