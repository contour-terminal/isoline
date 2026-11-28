#pragma once

// What the build compiles: a compiler without contracts stops here, and Clang reads the substitute; the text
// rules read this header too, so its compiler conditional and the macro it leaves defined are named.
#error "the lint read the substituted header"

#define ISOLINE_PRE(condition) pre(condition)
#ifdef __GNUC__
    #define ISOLINE_POST(result, condition) post(result: condition)
#endif
#define ISOLINE_NATIVE_ONLY 1
