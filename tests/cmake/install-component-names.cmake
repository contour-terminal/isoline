# SPDX-License-Identifier: Apache-2.0
#
# The isoline package's component naming rule (cmake/isolineComponentNames.cmake), in script mode:
#
#   cmake -P tests/cmake/install-component-names.cmake
#
# Each component name is a row: the export set it must get, or `refused` where the installer must
# refuse it because its export set would name another component back.

cmake_minimum_required(VERSION 3.25)

include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/isolineComponentNames.cmake")

# (component, export set or `refused`)
set(componentNameCases
    "core"             "isolineCoreTargets"
    "recipe_fixture"   "isolineRecipeFixtureTargets"
    "cache2"           "isolineCache2Targets"
    "a_b"              "refused"
    "x_2y"             "refused"
    "Core"             "refused")

set(failures "")
set(cases ${componentNameCases})
while(cases)
    list(POP_FRONT cases component expected)
    isoline_component_name_refusal("${component}" refusal)
    if(expected STREQUAL "refused")
        if(NOT refusal)
            string(APPEND failures "\n  ${component}: accepted, but it must be refused")
        endif()
        continue()
    endif()
    if(refusal)
        string(APPEND failures "\n  ${component}: refused (${refusal}), but it names ${expected}")
        continue()
    endif()
    isoline_component_export_set("${component}" exportSet)
    isoline_component_of_export_set("${exportSet}" roundTrip)
    if(NOT exportSet STREQUAL expected OR NOT roundTrip STREQUAL component)
        string(APPEND failures "\n  ${component}: export set ${exportSet} (expected ${expected}), "
                               "which names '${roundTrip}' back")
    endif()
endwhile()

# Not of the form isoline<Component>Targets: no component at all.
isoline_component_of_export_set("core-cppTargets" foreign)
if(NOT foreign STREQUAL "")
    string(APPEND failures "\n  core-cppTargets: names the component '${foreign}', but it is not isoline's")
endif()

if(failures)
    message(FATAL_ERROR "install-component-names: failed:${failures}")
endif()
message(STATUS "install-component-names: passed")
