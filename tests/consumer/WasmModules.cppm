// SPDX-License-Identifier: Apache-2.0
//
// The consumer's test modules (AddModule.hpp) as a C++ module of the consumer's own, so that
// ImportConsumer.cpp reaches them without an #include: its import-std mode includes nothing, since a
// standard header included after `import std;` may redefine what the std module holds. The header stays
// the one copy of the bytes; this unit re-exports them.

module;

#include "AddModule.hpp"

export module isoline_consumer.wasm_modules;

export namespace isoline::consumer
{
using isoline::consumer::AddModule;
using isoline::consumer::DoubleImportModule;
} // namespace isoline::consumer
