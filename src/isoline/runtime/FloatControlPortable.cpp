// SPDX-License-Identifier: Apache-2.0
// The portable row: no control register Isoline knows. CMake compiles it for every target that is
// neither x86-64 nor arm64 built by GCC or Clang (MSVC on arm64 included), and for a build for several
// architectures at once (a universal macOS build). Only the rounding mode, which <cfenv> reaches, is
// pinned (establishFloatDefaults()); flushing subnormals to zero cannot be controlled here.
#include <isoline/runtime/FloatEnvironment.hpp>

namespace isoline::runtime
{

namespace
{
    constexpr auto Layout = FloatControlLayout {};
} // namespace

FloatControlLayout const& hostFloatControlLayout() noexcept
{
    return Layout;
}

FloatControlWord readFloatControl() noexcept
{
    return 0;
}

void writeFloatControl(FloatControlWord /*word*/) noexcept
{
}

} // namespace isoline::runtime
