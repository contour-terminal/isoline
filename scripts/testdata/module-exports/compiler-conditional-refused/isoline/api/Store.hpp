#pragma once

namespace isoline::api
{
struct Store
{
    int pages;
};

inline constexpr int storeLimit = 16;

int open(Store const& store);

// A comment that says #if defined(__clang__) is no directive, and neither is "#ifdef _MSC_VER" in a literal.
inline constexpr char const* note = "#ifdef _MSC_VER";

#if defined(__GNUC__) && !defined(__clang__)
int gccOnly();
#endif

#  ifdef _MSC_VER
int msvcOnly();
#  elif __GNUC__ >= 15 \
      || defined(__clang__)
#  endif

#if __clang_major__ > 20
#endif

// A macro that hides a compiler's check from the conditional that reads it.
#define ISO_GCC (defined(__GNUC__) && !defined(__clang__))
#if ISO_GCC
int gccOnlyByMacro();
#endif
#undef ISO_GCC

// Names that only contain a compiler's macro are no compiler's.
#if defined(MY__clang__) || defined(X__GNUC__X) || defined(_MSC_VERSION)
#endif

#include <isoline/api/Limits.def>
} // namespace isoline::api
