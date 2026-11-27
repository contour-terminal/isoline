// SPDX-License-Identifier: Apache-2.0
#pragma once

// The machine layer's target seam. For now it holds what code is generated for, TargetDescription,
// which lives in its own data-only header so that what names a target (CompileConfig, an artifact's
// key) needs nothing of the backend; the ITarget interface joins it with the machine layer (P5 Task 9).
#include <isoline/compiler/machine/TargetDescription.hpp>
