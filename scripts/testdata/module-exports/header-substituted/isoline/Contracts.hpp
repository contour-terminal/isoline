#pragma once

// What the build compiles: a compiler without contracts stops here, and the lint reads the substitute.
#error "the lint read the substituted header"

#define ISOLINE_PRE(condition) pre(condition)
