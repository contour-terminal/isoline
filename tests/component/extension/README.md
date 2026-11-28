# The extension world

A component world shaped like Contour's extensions, and two guests of it, prebuilt: the template that
Contour's extension host (P9) follows to host its extensions as components. The host side is
`src/isoline/component/api/Extension_test.cpp`, written only against `component::api` and its typed
bindings; `docs/embedding.md` walks through it.

| File | What it is |
|---|---|
| `extension.wit` | The world `isoline-test:extension/extension@0.1.0`: Contour's `terminal-extensions:statusline@0.1.0` (`types`, `host`, the exports `load`, `update`, `render`), a host-owned `surface` resource, and a `control` interface of the shapes of `terminal-extensions:control@0.1.0`. |
| `extension.wat` | A guest written by hand in the component text format: two core modules (the memory and its bump allocator, and the code), `cabi_realloc`, post-returns, UTF-8. |
| `guest.c` | A guest in C, on the bindings `wit-bindgen c` generates, with its own bump allocator and no libc. |
| `build-guests.sh` | Rebuilds both `.wasm` files from their sources; `--check` compares instead. |
| `extension.wasm`, `guest.wasm` | The prebuilt guests the tests load. |

Both guests behave alike, so one host drives both:

- `load(config)` first reads the configuration's `mode` entry (below). It logs `load`, reads `host.state`
  (logging the active tab and each user variable), runs the command `status` with the configuration as its
  arguments (logging its output, or the name of its error), asks for a frame after 250 ms, and returns the
  event kinds `title` and `cwd`.
- `update(event)` keeps a `title` event's title and asks for a render; every other event asks for none.
- `render(cols, rows)` returns `[title]`, the title cut to `cols - 2` bytes.
- `panes(filter)` calls `control.list-panes(filter)` and returns its answer unchanged.
- `paint(bytes)` opens a surface, writes `bytes` to it three times, reads its size and drops it; then
  opens a second surface and presents it to the host. It returns the first surface's size.

The `mode` makes a guest misbehave, as a hostile or broken extension would:

| `mode` | What the guest does |
|---|---|
| `normal` | All of the above (also without a `mode` entry). |
| `hostile` | `render` returns a `list<u8>` of 2^28 − 1 bytes its memory does not hold. |
| `spin` | `render` logs `spinning` and never returns. |
| `large` | `render` returns 4 MiB its memory holds. |
| `trap` | `render` traps (`unreachable`). |
| `hoard` | `paint` opens a surface, writes once, reads its size and keeps it, never dropping it. |
| `bad-realloc` | From `host.state` on, `cabi_realloc` hands out a block past the memory's end. |
| `spin-load` | `load` logs `spinning` and never returns. |

## Building

No build of Isoline runs `build-guests.sh`, and no test needs its tools: both outputs are committed. After
changing a source, rebuild them with the versions `scripts/tools.lock` pins, and update the digests below:

```sh
WASM_TOOLS="$(python3 -I scripts/fetch-tools.py --tool wasm-tools)" \
WIT_BINDGEN="$(python3 -I scripts/fetch-tools.py --tool wit-bindgen)" \
WASI_SDK="$(dirname "$(dirname "$(python3 -I scripts/fetch-tools.py --tool wasi-sdk)")")" \
    tests/component/extension/build-guests.sh
```

The C guest is built with wasi-sdk's clang for `wasm32-unknown-unknown` with `-nostdlib`, its bindings'
two C library headers stood in for by declarations of what `guest.c` defines, and componentized by
`wasm-tools component new`, which adds wit-component's shim and fixup modules.

| File | Built by | SHA-256 |
|---|---|---|
| `extension.wasm` | `wasm-tools parse` 1.263.0 | `5710b957fde570a0b2b94a76f779b1a16c82e334dae12eb08e85feb3069f07fd` |
| `guest.wasm` | `wit-bindgen c` 0.63.0, wasi-sdk 34.0, `wasm-tools component new` 1.263.0 | `8b2b01e98ad487034fc8775fbddec18276fbb1b6bc0b2e3f1c887a8131a4cde4` |
