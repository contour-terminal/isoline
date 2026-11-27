// A check of a C caller's argument, which must be an error value.
void wasm_valtype_delete(void* type) noexcept
{
    ISOLINE_PRECONDITION(type != nullptr);
}

void wasm_valtype_copy(void* type) noexcept
{
    ISOLINE_INVARIANT(type != nullptr);
}
