# SPDX-License-Identifier: Apache-2.0
#
# How a component of the isoline package is named, in one place for both sides: the build
# (cmake/IsolineInstall.cmake) names each library's export set after its component, and the installed
# isolineConfig.cmake, beside which this file is installed, finds the components by those file names.
#
#   component recipe_fixture <-> export set isolineRecipeFixtureTargets
#                            <-> file isolineRecipeFixtureTargets.cmake
#
# Pure: no target, no cache entry, so it is testable in script mode
# (tests/cmake/install-component-names.cmake).

include_guard(GLOBAL)

## @brief Sets @p outVar to the export set of the component @p component: isoline<Component>Targets,
## each `_`-separated word capitalized.
function(isoline_component_export_set component outVar)
    string(REPLACE "_" ";" words "${component}")
    set(camelCase "")
    foreach(word IN LISTS words)
        string(SUBSTRING "${word}" 0 1 first)
        string(SUBSTRING "${word}" 1 -1 rest)
        string(TOUPPER "${first}" first)
        string(APPEND camelCase "${first}${rest}")
    endforeach()
    set(${outVar} "isoline${camelCase}Targets" PARENT_SCOPE)
endfunction()

## @brief Sets @p outVar to the component whose export set is @p exportSet (isolineRecipeFixtureTargets
## is recipe_fixture), or to "" if @p exportSet is not of the form isoline<Component>Targets.
function(isoline_component_of_export_set exportSet outVar)
    set(component "")
    if(exportSet MATCHES "^isoline(.+)Targets$")
        # CamelCase to snake_case: RecipeFixture is recipe_fixture.
        string(REGEX REPLACE "([a-z0-9])([A-Z])" "\\1_\\2" component "${CMAKE_MATCH_1}")
        string(TOLOWER "${component}" component)
    endif()
    set(${outVar} "${component}" PARENT_SCOPE)
endfunction()

## @brief Sets @p outVar to why @p component cannot name a component, or to "" if it can: its export set
## must name it back, or the installed config would find it under another name (a_b would be ab).
function(isoline_component_name_refusal component outVar)
    isoline_component_export_set("${component}" exportSet)
    isoline_component_of_export_set("${exportSet}" roundTrip)
    set(refusal "")
    if(NOT roundTrip STREQUAL component)
        string(CONCAT refusal "its export set ${exportSet} names the component '${roundTrip}', not "
                              "'${component}': use lower-case words that each start with a letter, "
                              "joined by '_'")
    endif()
    set(${outVar} "${refusal}" PARENT_SCOPE)
endfunction()
