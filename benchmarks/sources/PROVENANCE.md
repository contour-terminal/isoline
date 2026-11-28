# Provenance of the benchmark guests

Every `shootout-*.c` here is Sightglass's `benchmarks/shootout/src/<kernel>.c` at commit
f0d2040f1b4c521be5b763241f5c3b1146efff97, adapted to `run(iterations)` and the shim. Each kernel's licence
chain, from the Sightglass history:

1. **Written.** Frank Denis added every kernel in Sightglass's initial import (eb97061395, 2019-02-05),
   except seqhash, which he added in 4796febee7 (2019-04-03). The repository was then licensed
   Apache-2.0 or MIT (its `LICENSE-APACHE` and `LICENSE-MIT`).
2. **Relicensed in Sightglass.** Nick Fitzgerald added The Computer Language Benchmarks Game's 3-clause BSD
   licence as `LICENSE.md` beside every shootout kernel (51af9b5bac, 2021-01-28). The directory still carries it
   (36748a7d09 moved it in 2022, 76769a5e92 in 2023 into `benchmarks/shootout/src/LICENSE.md`). That is how
   Sightglass distributes these files, so each one's SPDX header says `BSD-3-Clause`.
3. **Reproduced code.** Where a kernel reproduces a function of libsodium or libhydrogen (Copyright (c)
   2013-2026 Frank Denis, ISC), its header adds `ISC` and names the function. Here, "reproduces" means the
   function's code itself, not code written in the same style.

| guest | first commit | author | licence | reproduces (ISC) |
|---|---|---|---|---|
| shootout-ackermann | eb97061395, 2019-02-05 | Frank Denis | BSD-3-Clause | – |
| shootout-base64 | eb97061395, 2019-02-05 | Frank Denis | BSD-3-Clause AND ISC | libsodium's `sodium_bin2base64()`, `sodium_base642bin()` (`sodium/codecs.c`) |
| shootout-ctype | eb97061395, 2019-02-05 | Frank Denis | BSD-3-Clause | – |
| shootout-ed25519 | eb97061395, 2019-02-05 | Frank Denis | BSD-3-Clause AND ISC | libsodium's `ge25519_scalarmult()` and the `fe25519` field arithmetic (`crypto_core/ed25519/ref10`) |
| shootout-fib2 | eb97061395, 2019-02-05 | Frank Denis | BSD-3-Clause | – |
| shootout-gimli | eb97061395, 2019-02-05 | Frank Denis | BSD-3-Clause AND ISC | libhydrogen's `gimli_core()` (`impl/gimli-core/portable.h`) |
| shootout-heapsort | eb97061395, 2019-02-05 | Frank Denis | BSD-3-Clause | – |
| shootout-keccak | eb97061395, 2019-02-05 | Frank Denis | BSD-3-Clause | – |
| shootout-matrix | eb97061395, 2019-02-05 | Frank Denis | BSD-3-Clause | – |
| shootout-memmove | eb97061395, 2019-02-05 | Frank Denis | BSD-3-Clause | – |
| shootout-minicsv | eb97061395, 2019-02-05 | Frank Denis | BSD-3-Clause | – |
| shootout-nestedloop | eb97061395, 2019-02-05 | Frank Denis | BSD-3-Clause | – |
| shootout-random | eb97061395, 2019-02-05 | Frank Denis | BSD-3-Clause | – |
| shootout-ratelimit | eb97061395, 2019-02-05 | Frank Denis | BSD-3-Clause | – |
| shootout-seqhash | 4796febee7, 2019-04-03 | Frank Denis | BSD-3-Clause | – |
| shootout-sieve | eb97061395, 2019-02-05 | Frank Denis | BSD-3-Clause | – |
| shootout-switch | eb97061395, 2019-02-05 | Frank Denis | BSD-3-Clause | – |
| shootout-xblabla20 | eb97061395, 2019-02-05 | Frank Denis | BSD-3-Clause | – |
| shootout-xchacha20 | eb97061395, 2019-02-05 | Frank Denis | BSD-3-Clause AND ISC | libsodium's ChaCha20 and HChaCha20 cores (`crypto_stream/chacha20/ref`, `crypto_core/hchacha20`) |

The micro-kernels (`micro-*.c`) and the shim are Isoline's, under Apache-2.0.
