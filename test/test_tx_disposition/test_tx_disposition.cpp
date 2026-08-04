// Disposition-aware TX: a controller that is alive but temporarily locally full is not a
// wire failure. These tests pin the two places that used to lie about that distinction:
// the payload byte cursor and Carminat's raw opening exchange.

#include <unity.h>

#include "../affa_test_support.h"
#include "carminat/CarminatDisplay.h"
#include "core/AffaRing.h"

using namespace affa;
using affatest::FakeClock;
using affatest::drain;
using affatest::drainCount;
using affatest::panelSyncRequest;
using affatest::panelSyncStart;
using affatest::pumpUntilIdle;

namespace {

// A direct-TWAI-shaped test seam: Accepted frames are retained, Busy frames are not. The
// old bool send() remains implemented to prove that adding trySend() did not remove the
// legacy virtual contract; production links may override only the richer method.
class BusyLink final : public ICanLink {
 public:
  bool send(const Frame& f) override { return trySend(f) == TxDisposition::Accepted; }

  TxDisposition trySend(const Frame& f) override {
    ++offers;
    if (busyOnOffer != 0 && offers == busyOnOffer) return TxDisposition::Busy;
    if (busyOffers != 0) { --busyOffers; return TxDisposition::Busy; }
    if (!_sent.push(f)) return TxDisposition::Busy;
    return TxDisposition::Accepted;
  }

  bool recv(Frame& out) override { return _rx.pop(out); }
  bool isLive() const override { return live; }

  void inject(const Frame& f) { _rx.push(f); }
  bool takeSent(Frame& f) { return _sent.pop(f); }
  uint32_t sentCount() const { return _sent.size(); }

  bool live = true;
  uint32_t offers = 0;
  uint32_t busyOffers = 0;
  uint32_t busyOnOffer = 0;

 private:
  AffaRing<Frame, 128> _rx;
  AffaRing<Frame, 128> _sent;
};

struct Rig {
  BusyLink link;
  FakeClock clock;
  CarminatDisplay display;
  Rig() : display(link, clock) {}

  void authorize(bool selfAck = false) {
    display.begin();
    // The 0x70 registrations leave with the final hello frame on this family, so a rig that
    // wants them acknowledged must arm the emulator before the opening, not after.
    display.setSelfAck(selfAck);
    affatest::completeCarminatAuth(display, link, clock);
    TEST_ASSERT_TRUE_MESSAGE(display.synced(), "accepted hello authorizes Carminat");
    drain(link);
  }

  void registerFunctions() {
    TEST_ASSERT_EQUAL(static_cast<uint8_t>(Result::Ok),
                      static_cast<uint8_t>(display.setPower(true)));
    affatest::settleCarminatRegistration(display, clock);
    pumpUntilIdle(display);
    TEST_ASSERT_TRUE(display.registered());
    display.setSelfAck(false);
    drain(link);
  }
};

}  // namespace

// A busy offer must leave a payload not-started: abortPending() may still remove it, and a
// later accepted first frame must contain the original first payload bytes rather than a
// stale cursor's continuation.
void test_busy_offer_never_commits_payload_bytes_or_started_state(void) {
  Rig r;
  r.authorize(/*selfAck=*/true);
  r.registerFunctions();

  TEST_ASSERT_EQUAL(static_cast<uint8_t>(Result::Ok),
                    static_cast<uint8_t>(r.display.showMenu("ONE", "TWO", "THREE")));
  r.link.busyOffers = 1;
  r.display.poll();
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, r.link.sentCount(), "Busy frame is not observable");
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(1, r.display.abortPending(),
                                  "Busy did not falsely start the payload");

  TEST_ASSERT_EQUAL(static_cast<uint8_t>(Result::Ok),
                    static_cast<uint8_t>(r.display.showMenu("ONE", "TWO", "THREE")));
  r.link.busyOffers = 1;
  r.display.poll();
  TEST_ASSERT_EQUAL_UINT32(0, r.link.sentCount());

  r.clock.advance(AFFA_TX_RETRY_MS);
  r.display.poll();
  Frame first;
  // The clock advance also reaches the profile's 500 ms heartbeat deadline.  It is not part
  // of the payload assertion, so discard that legal B9 and take the retry's first 0x151.
  do {
    TEST_ASSERT_TRUE_MESSAGE(r.link.takeSent(first), "accepted retry emits a first frame");
  } while (first.id == 0x3AF && first.data[0] == 0xB9);
  TEST_ASSERT_EQUAL_HEX32(0x151, first.id);
  TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x10, first.data[0],
                                 "retry starts from payload byte zero, not byte eight");
  TEST_ASSERT_EQUAL_HEX8(0x5A, first.data[1]);
}

// A full display-originated 61 11 00 is necessary but not sufficient: the exact hello trio
// also has to be accepted by the local controller before registration/rendering is released.
void test_busy_hello_does_not_authorize_until_the_full_burst_is_accepted(void) {
  Rig r;
  r.display.begin();
  r.link.busyOffers = 1;
  r.link.inject(panelSyncRequest());
  r.display.poll();
  TEST_ASSERT_FALSE_MESSAGE(r.display.synced(), "the 31 ms hello deadline keeps auth closed");
  TEST_ASSERT_EQUAL_UINT32(0, r.link.sentCount());

  r.clock.advance(carminat::kHelloFirstDelayMs);
  r.display.poll();
  TEST_ASSERT_FALSE_MESSAGE(r.display.synced(), "a busy first B0 keeps auth closed");
  TEST_ASSERT_EQUAL_UINT32(0, r.link.sentCount());

  r.clock.advance(AFFA_TX_RETRY_MS);
  r.display.poll();
  TEST_ASSERT_FALSE_MESSAGE(r.display.synced(), "B0 #1 alone is not authorization");

  // The display opens its OWN channel here, in the measured gap between B0#1 and B0#2
  // ([CAP] 0.81-1.55 ms behind it, 4/4), and the `5C1 74 00 …` reflex answers it inside the
  // same poll. That reflex is not authorization — but it IS what licenses our own 0x70
  // probe when B0#3 finally lands, so a busy-link test that omitted it would be measuring a
  // registration that was suppressed for the wrong reason.
  r.link.inject(affatest::mk(0x1C1, {0x70, 0xA3, 0xA3, 0xA3, 0xA3, 0xA3, 0xA3, 0xA3}));
  r.clock.advance(carminat::kHelloFrameGapMs);
  r.display.poll();
  TEST_ASSERT_FALSE_MESSAGE(r.display.synced(),
                            "B0 #2 is not authorization, and neither is the 5C1 reflex");
  r.clock.advance(carminat::kHelloFrameGapMs);
  r.display.poll();
  TEST_ASSERT_TRUE_MESSAGE(r.display.synced(), "only the accepted full hello opens auth");
  // FIVE, not three: the three B0 frames, the 5C1 74 that answers the display's own channel
  // registration, and the first 0x70 registration probe, which the OEM radio puts on the
  // wire 0.10-0.59 ms after B0#3 with no application involvement at all
  // (carminat::kSync.registerAfterHello). The second probe follows once this one is
  // acknowledged, which nothing does on this deliberately unhelpful link.
  TEST_ASSERT_EQUAL_UINT32(5, drainCount(r.link));
}

// The one-shot START announce is not issued just because it was scheduled. A Busy BA leaves
// it pending; the next permitted retry sends it, and a later 01 does not repeat it after that
// successful issue.
//
// RENAMED from ..._start_pair_..._after_b9_and_ba: the bootstrap is a bare BA now.
// SyncProfile::bootstrapAliveFrame is false on this family — [CAP] the reattach capture
// "aknowledge offed display.csv" at 84945066 opens with one unprompted `3AF BA` and no B9.
// The property being pinned is unchanged and is now sharper, because "issued" can no longer
// be half-true: there is exactly one frame, and it is either out or it is not.
void test_busy_start_announce_is_retried_and_only_marked_issued_after_ba(void) {
  Rig r;
  r.display.begin();
  r.link.busyOnOffer = 1;  // the first bootstrap BA is locally busy
  r.link.inject(panelSyncStart());
  r.display.poll();
  TEST_ASSERT_FALSE(r.display.synced());
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, r.link.sentCount(), "busy BA does not leave the link");
  drain(r.link);

  r.clock.advance(AFFA_TX_RETRY_MS);
  r.display.poll();
  Frame ba;
  TEST_ASSERT_TRUE_MESSAGE(r.link.takeSent(ba), "the pending BA retries once permitted");
  TEST_ASSERT_EQUAL_HEX32(0x3AF, ba.id);
  TEST_ASSERT_EQUAL_HEX8(0xBA, ba.data[0]);
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, r.link.sentCount(),
                                   "and the retry does not grow a B9 alongside it");

  r.link.inject(panelSyncStart());
  r.display.poll();
  // A repeat remains strict-discovery only: it never sends hello or a second BA.
  TEST_ASSERT_EQUAL_UINT32(0, drainCount(r.link));
}

// The display's own 1C1 registration must not be lost merely because the controller is
// locally full between B0 fragments.  Its 5C1 reply gets one bounded retry and then wins the
// next transmit slot ahead of B0#2, matching the captured interleaving.
void test_busy_panel_control_ack_retries_before_the_next_b0(void) {
  Rig r;
  r.display.begin();
  r.link.inject(panelSyncRequest());
  r.display.poll();
  r.clock.advance(carminat::kHelloFirstDelayMs);
  r.display.poll();                            // B0 #1
  drain(r.link);

  r.link.busyOffers = 1;
  r.link.inject(affatest::mk(0x1C1, {0x70, 0xA3, 0xA3, 0xA3,
                                      0xA3, 0xA3, 0xA3, 0xA3}));
  r.display.poll();                            // 5C1 offer is Busy; B0 #2 stays held
  TEST_ASSERT_EQUAL_UINT32(0, r.link.sentCount());
  TEST_ASSERT_FALSE(r.display.synced());

  r.clock.advance(AFFA_TX_RETRY_MS);
  r.display.poll();
  Frame ack, b0;
  TEST_ASSERT_TRUE(r.link.takeSent(ack));
  TEST_ASSERT_TRUE(r.link.takeSent(b0));
  TEST_ASSERT_EQUAL_HEX32(0x5C1, ack.id);
  TEST_ASSERT_EQUAL_HEX8(0x74, ack.data[0]);
  TEST_ASSERT_EQUAL_HEX32(0x3AF, b0.id);
  TEST_ASSERT_EQUAL_HEX8(0xB0, b0.data[0]);
  TEST_ASSERT_EQUAL_UINT32(0, r.link.sentCount());

  r.clock.advance(carminat::kHelloFrameGapMs);
  r.display.poll();                            // B0 #3 completes the opening
  TEST_ASSERT_TRUE(r.display.synced());
}

void setUp(void) {}
void tearDown(void) {}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_busy_offer_never_commits_payload_bytes_or_started_state);
  RUN_TEST(test_busy_hello_does_not_authorize_until_the_full_burst_is_accepted);
  RUN_TEST(test_busy_start_announce_is_retried_and_only_marked_issued_after_ba);
  RUN_TEST(test_busy_panel_control_ack_retries_before_the_next_b0);
  return UNITY_END();
}
