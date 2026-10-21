// How the owner task sleeps: ICanLink::waitRx() and the WakePacer that AffaTask drives with it.
//
// AffaTask itself is FreeRTOS-only and not built here; the decision it takes after every poll
// is this pure function, and the seam it calls is this virtual. Both are pinned.

#include "../affa_test_support.h"

#include "carminat/CarminatDisplay.h"
#include "core/WakePacer.h"

using namespace affa;

namespace {

// A link that can wait, and reports what the test tells it to.
struct WaitingLink final : ICanLink {
  RxWait   next  = RxWait::Timeout;
  uint32_t asked = 0;
  TxDisposition trySend(const Frame&) override { return TxDisposition::Accepted; }
  bool recv(Frame&) override { return false; }
  RxWait waitRx(uint32_t ms) override { asked = ms; return next; }
};

}  // namespace

void test_a_link_that_cannot_wait_says_so(void) {
  LoopbackLink<16> link;
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(RxWait::Unsupported),
                          static_cast<uint8_t>(link.waitRx(2)));
}

void test_the_display_forwards_its_owner_wait_to_the_link(void) {
  WaitingLink link;
  affatest::FakeClock clk;
  CarminatDisplay d(link, clk);
  link.next = RxWait::Ready;
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(RxWait::Ready),
                          static_cast<uint8_t>(d.waitRx(7)));
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(7, link.asked, "the timeout reaches the link unchanged");
}

void test_no_wait_means_the_old_fixed_period(void) {
  WakePacer p;
  for (int i = 0; i < 3; ++i)
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(WakeAction::SleepPeriod),
                            static_cast<uint8_t>(p.after(RxWait::Unsupported)));
}

void test_a_frame_on_a_quiet_bus_is_polled_at_once(void) {
  WakePacer p;
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(WakeAction::PollNow),
                          static_cast<uint8_t>(p.after(RxWait::Timeout)));
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(static_cast<uint8_t>(WakeAction::PollNow),
                                  static_cast<uint8_t>(p.after(RxWait::Ready)),
                                  "the key frame after a quiet period wakes the owner now");
}

void test_a_storm_cannot_hold_the_task_awake(void) {
  WakePacer p;
  int yields = 0;
  for (int i = 0; i < 100; ++i)
    if (p.after(RxWait::Ready) == WakeAction::YieldThenPoll) ++yields;
  TEST_ASSERT_EQUAL_INT_MESSAGE(50, yields, "every second back-to-back early wake yields a tick");
}

// A link-only build has no display to hang a sink on; the sink is the library's, not the
// display's, and the free function sets it.
struct CaptureSink final : ILogSink {
  char tag[16] = {0};
  int  lines   = 0;
  void write(uint8_t, const char* t, const char*) override {
    std::snprintf(tag, sizeof(tag), "%s", t);
    ++lines;
  }
};

void test_the_log_sink_needs_no_display(void) {
  CaptureSink sink;
  setLogSink(&sink);
  AFFA_LOGW("LINK", "driver %s", "up");
  setLogSink(nullptr);
  AFFA_LOGW("LINK", "after the sink is cleared");
  TEST_ASSERT_EQUAL_INT(1, sink.lines);
  TEST_ASSERT_EQUAL_STRING("LINK", sink.tag);
}

void setUp(void) {}
void tearDown(void) {}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_a_link_that_cannot_wait_says_so);
  RUN_TEST(test_the_display_forwards_its_owner_wait_to_the_link);
  RUN_TEST(test_no_wait_means_the_old_fixed_period);
  RUN_TEST(test_a_frame_on_a_quiet_bus_is_polled_at_once);
  RUN_TEST(test_a_storm_cannot_hold_the_task_awake);
  RUN_TEST(test_the_log_sink_needs_no_display);
  return UNITY_END();
}
