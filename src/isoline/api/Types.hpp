// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <isoline/Features.hpp>
#include <isoline/binary/DecodeError.hpp>
#include <isoline/binary/DecodeLimits.hpp>
#include <isoline/binary/Types.hpp>
#include <isoline/interpreter/Dispatch.hpp>
#include <isoline/runtime/Abrupt.hpp>
#include <isoline/runtime/HostFunction.hpp>
#include <isoline/runtime/Instantiate.hpp>
#include <isoline/runtime/Interrupt.hpp>
#include <isoline/runtime/Limits.hpp>
#include <isoline/runtime/LinearMemory.hpp>
#include <isoline/runtime/Store.hpp>
#include <isoline/runtime/Trap.hpp>
#include <isoline/runtime/Value.hpp>
#include <isoline/validate/ValidationError.hpp>

/// The names of the lower layers the embedding API is spoken in, re-exported into isoline::api.
///
/// This list *is* the public subset of the feature vocabulary (isoline/Features.hpp), runtime/, binary/
/// and validate/: an embedder names these
/// through isoline::api, and api headers spell them unqualified. Every other runtime name is internal,
/// installed but not the embedding API; raw access to a runtime::Store is test-only
/// (api/testing/RuntimeAccess.hpp), because it bypasses every check the api makes.
namespace isoline::api
{

// --- Features ---
using isoline::Feature;
using isoline::FeatureSet;
using isoline::Standard;
using isoline::UnmetPrerequisite;

// --- Values ---
using runtime::F32;
using runtime::F64;
using runtime::I32;
using runtime::I64;
using runtime::Ref;
using runtime::RefKind;
using runtime::V128;
using runtime::Value;

// --- Addresses ---
using runtime::ExnAddr;
using runtime::FuncAddr;
using runtime::GlobalAddr;
using runtime::MemAddr;
using runtime::TableAddr;
using runtime::TagAddr;

// --- How execution ends abruptly ---
using runtime::Abrupt;
using runtime::HostFailure;
using runtime::HostFailureKind;
using runtime::MismatchKind;
using runtime::Thrown;
using runtime::Trap;
using runtime::TrapCode;
using runtime::ValueMismatch;

// --- Host functions ---
using runtime::HostContext;
using runtime::IHostFunction;
using runtime::makeHostFunction;

// --- Interruption and polling ---
using runtime::Interrupter;
using runtime::InterruptState;
using runtime::IPollObserver;
using runtime::PollSite;

// --- Memory allocation ---
using runtime::GrowRequest;
using runtime::HeapMemoryAllocator;
using runtime::ILinearMemory;
using runtime::IMemoryAllocator;
using runtime::MemoryRequest;

// --- Limits and refusals ---
using runtime::AllocationFailure;
using runtime::GrowthRecord;
using runtime::Limit;
using runtime::LimitExceeded;
using runtime::Refusal;
using runtime::StoreLimits;

// --- Linking ---
using runtime::LinkError;
using runtime::LinkErrorCode;

// --- Types ---
using binary::ExternKind;
using binary::FuncType;
using binary::GlobalType;
using binary::Limits;
using binary::MemoryType;
using binary::TableType;
using binary::ValueKind;
using binary::ValueType;

// --- The interpreter's dispatches (EngineConfig::dispatch) ---
using interpreter::availableDispatches;
using interpreter::bestDispatch;
using interpreter::Dispatch;
using interpreter::isAvailable;

// --- Loading ---
using binary::DecodeError;
using binary::DecodeLimits;
using validate::ValidationError;

} // namespace isoline::api
