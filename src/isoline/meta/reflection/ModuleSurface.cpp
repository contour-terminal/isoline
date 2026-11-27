// SPDX-License-Identifier: Apache-2.0
//
// The reflected surface of module isoline (meta/reflection/ModuleSurface.hpp), printed one qualified name per
// line, sorted. The test meta-module-surface.reflection compares it with what the export lint lists
// (scripts/compare-module-surface.py): the lint reads the header set through Clang, this program through GCC,
// so a name only one of them sees is a declaration only one compiler compiles. The umbrella CMake generates
// includes exactly the header set, the module's surface (isoline_add_reflection_check(... HEADER_SETS_ONLY));
// the namespace rows are the row's NAMESPACES (isoline_add_module_surface_reflection()).

#include <isoline/meta/reflection/ModuleSurface.hpp>

#include <cstdio>

#include <isoline/meta/reflection/ModuleSurfaceUmbrella.hpp>

namespace
{

/// The row's public namespaces, generated from its NAMESPACES.
constexpr isoline::meta::reflection::SurfaceNamespace SurfaceNamespaces[] = {
#include <isoline/meta/reflection/ModuleSurfaceNamespaces.def>
};

/// Their members' names, computed at compile time.
constexpr auto SurfaceNames = isoline::meta::reflection::staticSurfaceNamesOf(SurfaceNamespaces);

} // namespace

int main()
{
    for (auto const* name: SurfaceNames)
        std::puts(name);
    return 0;
}
