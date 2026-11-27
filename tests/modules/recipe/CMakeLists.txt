# SPDX-License-Identifier: Apache-2.0
#
# The recipe each later public library follows (docs/modules.md, "Adding a module"), proven by a fixture:
# a library of its own with a HEADERS file set, one isoline_add_module() row, a generated interface unit
# that re-exports module isoline (IMPORTS), one row of ISOLINE_INSTALL_LIBRARIES
# (cmake/IsolineInstallRows.cmake), and a consumer that imports the module alone. A library of the testing
# build: it installs under its own component, isoline-test-fixture, never in a plain `cmake --install`.

add_library(isoline_recipe_fixture STATIC RecipeFixture.cpp)
add_library(isoline::recipe_fixture ALIAS isoline_recipe_fixture)
target_sources(isoline_recipe_fixture
    PUBLIC FILE_SET HEADERS
        BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}"
        FILES isoline/recipe_fixture/RecipeFixture.hpp)
target_compile_features(isoline_recipe_fixture PUBLIC cxx_std_23)
# Header-based, as isoline_core is: its module is a target of its own, isoline_recipe_fixture_module.
set_target_properties(isoline_recipe_fixture PROPERTIES CXX_SCAN_FOR_MODULES OFF)
target_link_libraries(isoline_recipe_fixture PUBLIC isoline_core)
isoline_apply_target_policy(isoline_recipe_fixture)

# The row. The library's HEADER_SET is the module's surface; IMPORTS re-exports module isoline, and links
# its module target PUBLIC, so an importer of isoline.recipe_fixture names isoline::api's types too.
isoline_add_module(
    NAME isoline.recipe_fixture
    LIBRARY isoline_recipe_fixture
    SOURCE isoline.recipe_fixture.cppm
    NAMESPACES isoline::recipe_fixture
    IMPORTS isoline)

# The consumer: imports isoline.recipe_fixture, and nothing else of Isoline.
add_executable(isoline_recipe_module_test RecipeConsumer_test.cpp)
# The add module's bytes, which a consumer that only imports may include (tests/consumer/AddModule.hpp).
target_include_directories(isoline_recipe_module_test PRIVATE "${PROJECT_SOURCE_DIR}/tests/consumer")
target_link_libraries(isoline_recipe_module_test PRIVATE Catch2::Catch2WithMain)
isoline_add_module_consumer(isoline_recipe_module_test isoline.recipe_fixture)
isoline_apply_target_policy(isoline_recipe_module_test TEST)
# E2's import-reexporting lift is isoline_add_module_consumer()'s, read from the row's IMPORTS.
add_test(NAME isoline_recipe_module_test COMMAND isoline_recipe_module_test)
set_tests_properties(isoline_recipe_module_test PROPERTIES LABELS "unit;modules" TIMEOUT 300)
