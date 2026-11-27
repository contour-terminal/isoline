// The header's own fixture: exempt.
struct Positive
{
    int twice(int value) const noexcept ISOLINE_PRE(value > 0);
};
