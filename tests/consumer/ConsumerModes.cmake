# SPDX-License-Identifier: Apache-2.0
#
# The installed consumer's modes, as one table: cmake/IsolineInstall.cmake registers a CTest per row,
# tests/cmake/check-install.cmake installs the package for it and judges the run, and
# tests/consumer/CMakeLists.txt builds it. A mode is a row here, nothing else. Pure data, and the lookup
# below reads nothing but this table, so all three (two of them in script mode) include it.
#
# (mode, CTest name, lowest CMake, source, uses, install components)
#  - uses: what the consumer's source uses, |-separated: rows of ISOLINE_CONSUMER_USES, a library it
#    includes or a module it imports, and std for `import std;`. A mode that imports is scanned for module
#    dependencies and needs a build with ISOLINE_MODULES; one that imports std needs a toolchain with a std
#    module, too (D9). A mode that imports nothing must not be scanned.
#  - source: a `.c` source makes a project of C alone (`project(... LANGUAGES C)`), which asks the package
#    for its C interface and must be given no C++ toolchain; any other source, a C++ project.
#  - install components: the components check-install installs into the prefix, |-separated.
#  - The import modes need CMake 4.3, the modules floor (D8); the header modes run on the header lanes'
#    CMake.
set(ISOLINE_CONSUMER_MODES
    "include"        "install-include-consumer"        "3.25" "IncludeConsumer.cpp" "core"
        "isoline"
    "include-text"   "install-include-text-consumer"   "3.25" "TextConsumer.cpp"    "text"
        "isoline"
    "import"         "install-import-consumer"         "4.3"  "ImportConsumer.cpp"  "isoline"
        "isoline"
    "import-std"     "install-import-std-consumer"     "4.3"  "ImportConsumer.cpp"  "std|isoline"
        "isoline"
    "import-fixture" "install-import-fixture-consumer" "4.3"  "FixtureConsumer.cpp" "isoline.recipe_fixture"
        "isoline|isoline-test-fixture"
    "c"              "install-c-consumer"              "3.25" "CConsumer.cpp"       "c"
        "isoline|isoline-c"
    "c-only"         "install-c-only-consumer"         "3.25" "CConsumer.c"         "c"
        "isoline|isoline-c")

# What a mode may use: (name, how: include or import, the package component that holds it, the target the
# consumer links). The consumer asks find_package() for the components of what it uses, alone: the package
# loads what they need beneath them.
set(ISOLINE_CONSUMER_USES
    "core"                   "include" "core"           "isoline::core"
    "text"                   "include" "text"           "isoline::text"
    "isoline"                "import"  "core"           "isoline::core_module"
    "isoline.recipe_fixture" "import"  "recipe_fixture" "isoline::recipe_fixture_module"
    "c"                      "include" "c"              "isoline::c")

## @brief Looks up the row of ISOLINE_CONSUMER_MODES for @p mode.
##
## Sets <outPrefix>Found to ON or OFF and, where found, <outPrefix>Test, <outPrefix>MinimumCMake,
## <outPrefix>Source, <outPrefix>Components (the install components, a list), <outPrefix>PackageComponents
## (what the consumer asks find_package() for, a list), <outPrefix>LinkTargets (a list), and
## <outPrefix>ImportsIsoline (a module of the package) and <outPrefix>ImportsStd (ON or OFF). Sets
## <outPrefix>KnownModes to every mode of the table, either way. A mode that uses neither std nor a row of
## ISOLINE_CONSUMER_USES is a fatal error.
function(isoline_consumer_mode mode outPrefix)
    set(knownModes "")
    set(found OFF)
    set(rows "${ISOLINE_CONSUMER_MODES}")
    while(rows)
        list(POP_FRONT rows rowMode rowTest rowMinimumCMake rowSource rowUses rowComponents)
        list(APPEND knownModes "${rowMode}")
        if(NOT rowMode STREQUAL mode)
            continue()
        endif()
        set(found ON)
        set(importsIsoline OFF)
        set(importsStd OFF)
        set(packageComponents "")
        set(linkTargets "")
        string(REPLACE "|" ";" uses "${rowUses}")
        foreach(used IN LISTS uses)
            if(used STREQUAL "std")
                set(importsStd ON)
                continue()
            endif()
            set(isKnown OFF)
            set(useRows "${ISOLINE_CONSUMER_USES}")
            while(useRows)
                list(POP_FRONT useRows rowName rowHow rowPackageComponent rowTarget)
                if(rowName STREQUAL used)
                    set(isKnown ON)
                    list(APPEND packageComponents "${rowPackageComponent}")
                    list(APPEND linkTargets "${rowTarget}")
                    if(rowHow STREQUAL "import")
                        set(importsIsoline ON)
                    endif()
                endif()
            endwhile()
            if(NOT isKnown)
                message(FATAL_ERROR "ISOLINE_CONSUMER_MODES: mode ${mode} uses ${used}, which is neither std "
                                    "nor a row of ISOLINE_CONSUMER_USES")
            endif()
        endforeach()
        list(REMOVE_DUPLICATES packageComponents)
        string(REPLACE "|" ";" components "${rowComponents}")
        set(${outPrefix}Test "${rowTest}" PARENT_SCOPE)
        set(${outPrefix}MinimumCMake "${rowMinimumCMake}" PARENT_SCOPE)
        set(${outPrefix}Source "${rowSource}" PARENT_SCOPE)
        if(rowSource MATCHES "[.]c$")
            set(${outPrefix}Language C PARENT_SCOPE)
        else()
            set(${outPrefix}Language CXX PARENT_SCOPE)
        endif()
        set(${outPrefix}Components "${components}" PARENT_SCOPE)
        set(${outPrefix}PackageComponents "${packageComponents}" PARENT_SCOPE)
        set(${outPrefix}LinkTargets "${linkTargets}" PARENT_SCOPE)
        set(${outPrefix}ImportsIsoline ${importsIsoline} PARENT_SCOPE)
        set(${outPrefix}ImportsStd ${importsStd} PARENT_SCOPE)
    endwhile()
    set(${outPrefix}Found ${found} PARENT_SCOPE)
    set(${outPrefix}KnownModes "${knownModes}" PARENT_SCOPE)
endfunction()
