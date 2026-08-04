// Shared harness for every host suite under test/.
//
// Header-only ON PURPOSE. PlatformIO compiles each test_* folder as its own program, and a
// .cpp in the test root would be linked into all of them; a header is included by the ones
// that want it and costs the others nothing.
//
// The rig is the one test_core established: LoopbackLink + a FakeClock you advance by hand.
// Nothing here sleeps, allocates or touches Arduino.
#pragma once

#include <unity.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>

#include "AffaConfig.h"
#include "core/AffaTypes.h"
#include "core/AffaConstants.h"
#include "core/IClock.h"
#include "core/ICanLink.h"
#include "link/LoopbackLink.h"
#if AFFA_PANEL_CARMINAT
#  include "carminat/CarminatConstants.h"
#endif

namespace affatest {

// ---------------------------------------------------------------------------
// Clock
// ---------------------------------------------------------------------------

struct FakeClock final : affa::IClock {
  uint32_t t = 0;
  uint32_t millis() const override { return t; }
  void advance(uint32_t ms) { t += ms; }
};

// ---------------------------------------------------------------------------
// Frame construction
// ---------------------------------------------------------------------------

inline affa::Frame mk(uint32_t id, std::initializer_list<uint8_t> bytes, uint8_t len = 8) {
  affa::Frame f;
  f.id  = id;
  f.len = len;
  uint8_t i = 0;
  for (uint8_t b : bytes) {
    if (i < 8) f.data[i++] = b;
  }
  return f;
}

// The AFFA3 NAV panel's GOOD `61 11 00 A3 A3 …` authorization request. The panel begins
// a session; the ESP never probes it into existence. `61 11 01` is the bootstrap / START
// request and is covered by a separate test.
inline affa::Frame panelSyncRequest() {
  return mk(0x3CF, {0x61, 0x11, 0x00, 0xA3, 0xA3, 0xA3, 0xA3, 0xA3});
}
// The legacy bootstrap / START request. It receives hello and a one-shot control pair,
// but never authorizes registration or rendering; only panelSyncRequest() does that.
inline affa::Frame panelSyncStart() {
  return mk(0x3CF, {0x61, 0x11, 0x01, 0xA3, 0xA3, 0xA3, 0xA3, 0xA3});
}
inline affa::Frame panelPeerAlive() {
  return mk(0x3CF, {0x69, 0xA3, 0xA3, 0xA3, 0xA3, 0xA3, 0xA3, 0xA3});
}

#if AFFA_PANEL_CARMINAT
// Drives the captured AFFA3 opening exactly as the real panel sees it: full 61 11 00,
// then B0 #1 at +31 ms and the remaining B0s at +31 ms each.  Delays are protocol timing,
// not an implementation detail, so host tests must advance their fake clock rather than
// relying on a number of poll() calls.
template <class D, class L>
inline void completeCarminatAuth(D& d, L& l, FakeClock& clk) {
  l.inject(panelSyncRequest());
  d.poll();
  clk.advance(affa::carminat::kHelloFirstDelayMs);
  d.poll();                                     // B0 #1
  // THE DISPLAY REGISTERS ITS OWN CHANNEL FIRST, and the harness has to model that or it is
  // not modelling this panel. Measured 4/4: `1C1 70` lands 0.81-1.55 ms after B0#1 — between
  // the first and second announce frames — and our `151 70` follows ~61 ms later. Without it
  // the library correctly refuses to register, and every rig built on this helper stalls.
  l.inject(mk(0x1C1, {0x70, 0xA3, 0xA3, 0xA3, 0xA3, 0xA3, 0xA3, 0xA3}));
  clk.advance(affa::carminat::kHelloFrameGapMs);
  d.poll();                                     // 5C1 74 reflex, then B0 #2
  clk.advance(affa::carminat::kHelloFrameGapMs);
  d.poll();                                     // B0 #3
  d.poll();                                     // and the 0x70 probes it now permits
}

// A self-ACK rig has to let both 0x70 registrations complete before the profile's measured
// 400 ms post-registration quiet period can end.  Keep this in the shared harness so every
// Carminat builder test starts on a genuinely usable session, not a zero-time shortcut.
template <class D>
inline void settleCarminatRegistration(D& d, FakeClock& clk) {
  for (uint8_t i = 0; i < 8 && !d.registered(); ++i) d.poll();
  TEST_ASSERT_TRUE_MESSAGE(d.registered(), "Carminat registrations must complete first");
  clk.advance(affa::carminat::kPayloadAfterRegistrationMs);
  d.poll();
}
#endif

// ---------------------------------------------------------------------------
// Link helpers
// ---------------------------------------------------------------------------

template <class L>
inline void drain(L& l) {
  affa::Frame f;
  while (l.takeSent(f)) {
  }
}

template <class L>
inline uint32_t drainCount(L& l) {
  affa::Frame f;
  uint32_t n = 0;
  while (l.takeSent(f)) ++n;
  return n;
}

// Pops one frame and asserts it byte for byte against `want`.
template <class L>
inline void expectFrame(L& l, const affa::Frame& want, const char* what) {
  affa::Frame got;
  TEST_ASSERT_TRUE_MESSAGE(l.takeSent(got), what);
  TEST_ASSERT_EQUAL_HEX32_MESSAGE(want.id, got.id, what);
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(want.len, got.len, what);
  TEST_ASSERT_EQUAL_HEX8_ARRAY_MESSAGE(want.data, got.data, 8, what);
}

// Pops exactly `n` frames, asserts each byte for byte, then asserts the link is empty.
// "Exactly" is the point: a golden vector that only checks a prefix would pass against a
// builder that emitted one frame too many, which is precisely the showMenu ACK-model trap.
template <class L>
inline void expectFrames(L& l, const affa::Frame* want, size_t n, const char* what) {
  char msg[160];
  for (size_t i = 0; i < n; ++i) {
    affa::Frame got;
    std::snprintf(msg, sizeof(msg), "%s: frame %u of %u", what, unsigned(i), unsigned(n));
    TEST_ASSERT_TRUE_MESSAGE(l.takeSent(got), msg);
    TEST_ASSERT_EQUAL_HEX32_MESSAGE(want[i].id, got.id, msg);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(want[i].len, got.len, msg);
    TEST_ASSERT_EQUAL_HEX8_ARRAY_MESSAGE(want[i].data, got.data, 8, msg);
  }
  affa::Frame extra;
  std::snprintf(msg, sizeof(msg), "%s: expected EXACTLY %u frames", what, unsigned(n));
  TEST_ASSERT_FALSE_MESSAGE(l.takeSent(extra), msg);
}

// ---------------------------------------------------------------------------
// Pumping
// ---------------------------------------------------------------------------

template <class D>
inline void pump(D& d, int polls) {
  for (int i = 0; i < polls; ++i) d.poll();
}

// Polls until the transmit queue is empty. It is a bounded loop with a loud failure: a
// test that silently stopped pumping would assert against a half-transmitted message.
template <class D>
inline void pumpUntilIdle(D& d, int maxPolls = 400) {
  for (int i = 0; i < maxPolls && d.busy(); ++i) d.poll();
  TEST_ASSERT_FALSE_MESSAGE(d.busy(), "transmit queue never drained");
}

}  // namespace affatest

// Result comparison that prints the two enumerators' numeric values rather than "0 != 7".
#define ASSERT_RESULT(want, got)                                            \
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(static_cast<uint8_t>(affa::Result::want), \
                                  static_cast<uint8_t>(got), "Result::" #want " expected")
