#pragma once

// Warns by default (-Wshift-count-overflow): a system header's warning is not the module's.
inline int shifted()
{
    return 1 << 40;
}
