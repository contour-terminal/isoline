#pragma once

namespace isoline::api
{
enum Kind
{
    KindFunction,
    KindMemory,
};

enum class Scoped
{
    Hidden,
};

enum
{
    AnonymousConstant = 1,
};
} // namespace isoline::api
