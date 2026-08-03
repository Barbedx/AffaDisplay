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

  void authorize() {
    display.begin();
    link.inject(panelSyncRequest());
    display.poll();
    TEST_ASSERT_TRUE_MESSAGE(display.synced(), "accepted hello authorizes Carminat");
    drain(link);
  }

  void registerFunctions() {
    display.setSelfAck(true);
    TEST_ASSERT_EQUAL(static_cast<uint8_t>(Result::Ok),
                      static_cast<uint8_t>(display.setPower(true)));
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
  r.authorize();
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
  TEST_ASSERT_TRUE_MESSAGE(r.link.takeSent(first), "accepted retry emits a first frame");
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
  TEST_ASSERT_FALSE_MESSAGE(r.display.synced(), "a busy first hello keeps auth closed");
  TEST_ASSERT_EQUAL_UINT32(0, r.link.sentCount());

  r.clock.advance(AFFA_TX_RETRY_MS);
  r.display.poll();
  TEST_ASSERT_TRUE_MESSAGE(r.display.synced(), "only the accepted full hello opens auth");
  TEST_ASSERT_EQUAL_UINT32(3, drainCount(r.link));
}

// The one-shot START pair is not issued just because it was scheduled. A Busy B9 leaves the
// pair pending; the next permitted retry sends both B9 and BA, and a later 01 does not repeat
// them after that successful issue.
void test_busy_start_pair_is_retried_and_only_marked_issued_after_b9_and_ba(void) {
  Rig r;
  r.display.begin();
  r.link.busyOnOffer = 4;  // hello x3 accepted; the first bootstrap B9 is locally busy
  r.link.inject(panelSyncStart());
  r.display.poll();
  TEST_ASSERT_FALSE(r.display.synced());
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(3, r.link.sentCount(), "only hello left before busy B9");
  drain(r.link);

  r.clock.advance(AFFA_TX_RETRY_MS);
  r.display.poll();
  Frame b9, ba;
  TEST_ASSERT_TRUE(r.link.takeSent(b9));
  TEST_ASSERT_TRUE(r.link.takeSent(ba));
  TEST_ASSERT_EQUAL_HEX8(0xB9, b9.data[0]);
  TEST_ASSERT_EQUAL_HEX8(0xBA, ba.data[0]);
  TEST_ASSERT_EQUAL_UINT32(0, r.link.sentCount());

  r.link.inject(panelSyncStart());
  r.display.poll();
  // The repeat gets its legacy hello, but never a second one-shot B9 + BA pair.
  TEST_ASSERT_EQUAL_UINT32(3, drainCount(r.link));
}

void setUp(void) {}
void tearDown(void) {}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_busy_offer_never_commits_payload_bytes_or_started_state);
  RUN_TEST(test_busy_hello_does_not_authorize_until_the_full_burst_is_accepted);
  RUN_TEST(test_busy_start_pair_is_retried_and_only_marked_issued_after_b9_and_ba);
  return UNITY_END();
}
