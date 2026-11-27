#pragma once

// #define ISOLINE_COMMENTED 1
/*
#define ISOLINE_BLOCK_COMMENTED 1
*/

namespace isoline::api
{
enum class Opcode
{
#define ISOLINE_OPCODE(Name, Code) Name,
    ISOLINE_OPCODE(Nop, 0x01) ISOLINE_OPCODE(Drop, 0x1A)
#undef ISOLINE_OPCODE
};

#  define ISOLINE_OPCODE(Name, Code) \
    case Opcode::Name:              \
        return Code;
inline int code(Opcode opcode)
{
    switch (opcode)
    {
        ISOLINE_OPCODE(Nop, 0x01)
        ISOLINE_OPCODE(Drop, 0x1A)
    }
    return 0;
}
#  undef ISOLINE_OPCODE
} // namespace isoline::api
