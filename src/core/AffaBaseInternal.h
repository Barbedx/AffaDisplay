// Shared between the four translation units that implement AffaDisplayBase. NOT a public
// header — nothing outside src/core/ may include it, and nothing in here is part of the API.
//
// It exists because step 7 of docs/REFACTOR-PLAN.md split a 2156-line file into four, and
// these constants were in an anonymous namespace that three of them still need. Copying them
// per file would be four places for a tuned retry budget to drift apart, which is the exact
// failure mode this library was written to stop happening.
#pragma once
#include <cstdint>

namespace affa {
namespace basedetail {

// The log tag every AFFA_LOG* call in the base passes.
inline constexpr const char* kTag = "AFFA";

// A permanently undeliverable announce costs at most one offer plus this many retries. See
// AffaDisplayBase::pumpUnauthControl().
inline constexpr uint8_t kBootstrapBusyRetries = 1;

// A lost 5C1 costs the whole session, so this one is retried harder than ordinary control
// traffic — and at most two may ever be owed at once (§4.2, one ACK per received frame).
inline constexpr uint8_t kGenericAckBusyRetries = 3;
inline constexpr uint8_t kGenericAckMaxOwed     = 2;

}  // namespace basedetail
}  // namespace affa
