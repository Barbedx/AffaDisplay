// widget::Marquee — the scrolling window, away from the panel it used to be welded to.
//
// The defect this suite exists to prevent is the one the extracted tickMedia() had: a
// position ACCUMULATED per call rather than derived from the clock. That version scrolled
// at whatever rate the caller happened to poll at, caught up in a burst after a stall, and
// could not be reasoned about at all. Half the tests below are the same assertion from
// different angles — the window at time t does not depend on how you got to t.
//
// The geometry tests are the other half, and they are new: the window was hard-coded to
// this one panel's eight cells, so "does it work at a width that is not 8" had no answer.

#include "../affa_test_support.h"
#include "widget/Marquee.h"

#include <cstring>

using namespace affa;
using affa::widget::Marquee;
using affa::widget::MarqueeGeometry;

namespace {

// Small and prime-ish, so a wrap bug cannot hide behind a round number.
MarqueeGeometry geom(uint8_t width = 8, uint8_t gap = 3, uint32_t stepMs = 400) {
  return MarqueeGeometry{width, gap, stepMs};
}

// The window as a string, for readable assertions.
const char* win(const Marquee& m, uint32_t now) {
  static char buf[64];
  m.window(m.windowAt(now), buf, sizeof(buf));
  return buf;
}

// ---------------------------------------------------------------------------
// The clock is the position
// ---------------------------------------------------------------------------

// THE HEADLINE. Sampling the same instant after one step or after a thousand must give the
// same window: the position is a function of `now`, not of how many times we asked.
void test_the_window_is_a_function_of_the_clock_not_the_call_count(void) {
  Marquee a(geom()), b(geom());
  a.setText("ABCDEFGHIJKL", 0);
  b.setText("ABCDEFGHIJKL", 0);
  a.setActive(true, 0);
  b.setActive(true, 0);

  // `a` is sampled once at 4000 ms; `b` is sampled at every 50 ms in between.
  for (uint32_t t = 0; t <= 4000; t += 50) (void)b.windowAt(t);

  TEST_ASSERT_EQUAL_UINT16_MESSAGE(a.windowAt(4000), b.windowAt(4000),
      "the window must not depend on how often it was sampled");
  TEST_ASSERT_EQUAL_STRING(win(a, 4000), win(b, 4000));
}

// A stalled loop must not produce a catch-up burst: it lands where the clock says, once.
void test_a_stalled_caller_lands_on_the_clock_not_on_a_backlog(void) {
  Marquee m(geom());
  m.setText("ABCDEFGHIJKL", 0);
  m.setActive(true, 0);

  // 10 s of nothing, then one sample. 10000/400 = 25 steps, text+gap = 15 chars.
  TEST_ASSERT_EQUAL_UINT16(static_cast<uint16_t>(25 % 15), m.windowAt(10000));
}

void test_one_step_per_stepMs(void) {
  Marquee m(geom(8, 3, 400));
  m.setText("ABCDEFGHIJKL", 0);
  m.setActive(true, 0);

  TEST_ASSERT_EQUAL_UINT16_MESSAGE(0, m.windowAt(0),     "t=0 is position 0");
  TEST_ASSERT_EQUAL_UINT16_MESSAGE(0, m.windowAt(399),   "one ms short is still 0");
  TEST_ASSERT_EQUAL_UINT16_MESSAGE(1, m.windowAt(400),   "the step lands exactly on 400");
  TEST_ASSERT_EQUAL_UINT16_MESSAGE(2, m.windowAt(800),   "and again");
}

// ---------------------------------------------------------------------------
// Text
// ---------------------------------------------------------------------------

// The one line that lets a marquee scroll at all in a real application: a media player
// re-publishing the current track every second must not pin the window to character 0.
void test_resetting_the_same_text_does_not_restart_the_scroll(void) {
  Marquee m(geom());
  TEST_ASSERT_TRUE(m.setText("ABCDEFGHIJKL", 0));
  m.setActive(true, 0);

  const uint16_t before = m.windowAt(2000);
  TEST_ASSERT_NOT_EQUAL_MESSAGE(0, before, "precondition: it has actually moved");

  TEST_ASSERT_FALSE_MESSAGE(m.setText("ABCDEFGHIJKL", 2000),
                            "identical content must report no change");
  TEST_ASSERT_EQUAL_UINT16_MESSAGE(before, m.windowAt(2000),
                                   "and must not move the window");
}

void test_new_text_restarts_at_zero(void) {
  Marquee m(geom());
  m.setText("ABCDEFGHIJKL", 0);
  m.setActive(true, 0);
  (void)m.windowAt(2000);

  TEST_ASSERT_TRUE(m.setText("ZZZZZZZZZZZZ", 2000));
  TEST_ASSERT_EQUAL_UINT16_MESSAGE(0, m.windowAt(2000), "a new title starts at the start");
}

// Empty is OFF, and length() == 0 is how the panel knows to transmit nothing rather than
// blank the glass on the application's behalf.
void test_empty_text_disables_the_marquee(void) {
  Marquee m(geom());
  m.setText("ABCDEFGHIJKL", 0);
  TEST_ASSERT_NOT_EQUAL(0, m.length());

  m.setText("", 0);
  TEST_ASSERT_EQUAL_UINT16(0, m.length());
  TEST_ASSERT_EQUAL_UINT16(0, m.windowAt(9999));

  m.setText(nullptr, 0);
  TEST_ASSERT_EQUAL_UINT16_MESSAGE(0, m.length(), "nullptr is the same as empty");
}

// The gap is what makes the wrap read as a pause rather than a collision.
void test_the_gap_is_appended_and_scrolls_through(void) {
  Marquee m(geom(4, 3, 100));
  m.setText("ABCD", 0);
  // "ABCD" + 3 spaces = 7
  TEST_ASSERT_EQUAL_UINT16_MESSAGE(7, m.length(), "the gap is part of the scrolled string");

  char buf[8];
  m.window(0, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING("ABCD", buf);
  m.window(4, buf, sizeof(buf));           // three spaces then wrap to 'A'
  TEST_ASSERT_EQUAL_STRING("   A", buf);
  m.window(6, buf, sizeof(buf));           // one space then ABC
  TEST_ASSERT_EQUAL_STRING(" ABC", buf);
}

// ---------------------------------------------------------------------------
// Pause and resume
// ---------------------------------------------------------------------------

void test_pause_freezes_and_resume_continues_from_there(void) {
  Marquee m(geom(8, 3, 400));
  m.setText("ABCDEFGHIJKL", 0);
  m.setActive(true, 0);

  const uint16_t at2s = m.windowAt(2000);
  m.setActive(false, 2000);
  TEST_ASSERT_EQUAL_UINT16_MESSAGE(at2s, m.base(), "pausing freezes the current window");

  // Ten seconds of being paused must not advance anything.
  TEST_ASSERT_EQUAL_UINT16_MESSAGE(at2s, m.base(), "a pause does not advance the marquee");

  m.setActive(true, 12000);
  TEST_ASSERT_EQUAL_UINT16_MESSAGE(at2s, m.windowAt(12000),
      "resuming continues from the frozen window, not from where a free clock would be");
  TEST_ASSERT_EQUAL_UINT16_MESSAGE(static_cast<uint16_t>((at2s + 1) % m.length()),
      m.windowAt(12400), "and then steps normally");
}

void test_setting_the_same_active_state_twice_is_inert(void) {
  Marquee m(geom());
  m.setText("ABCDEFGHIJKL", 0);
  m.setActive(true, 0);
  const uint16_t at2s = m.windowAt(2000);

  m.setActive(true, 2000);      // already active — must not re-base the epoch
  TEST_ASSERT_EQUAL_UINT16(at2s, m.windowAt(2000));
}

// ---------------------------------------------------------------------------
// Geometry — the reason this is a widget and not a panel member
// ---------------------------------------------------------------------------

void test_the_width_is_the_injected_one(void) {
  char buf[64];

  Marquee narrow(geom(4, 0, 100));
  narrow.setText("ABCDEFGH", 0);
  narrow.window(0, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING_MESSAGE("ABCD", buf, "a 4-cell window shows 4 cells");

  Marquee wide(geom(20, 0, 100));
  wide.setText("ABCDEFGH", 0);
  wide.window(0, buf, sizeof(buf));
  // A text that FITS the window renders ONCE, blank-filled — 0.4.1. The previous contract
  // wrapped it modulo the text ("ABCDEFGHABCDEFGHABCD"), which painted the word 2.5 times
  // on the glass and broke RowScreen's "a static row shows the first width characters".
  // The wrap is a LOOP device and a loop needs something hidden to reveal; it applies only
  // when the text is longer than the window.
  TEST_ASSERT_EQUAL_STRING_MESSAGE("ABCDEFGH            ", buf,
                                   "a window wider than the text pads rather than wraps");

  // And a fitting text has no phase: whatever position the clock arithmetic would give,
  // the window is pinned to 0 and renders identically.
  wide.setActive(true, 0);
  TEST_ASSERT_EQUAL_UINT16_MESSAGE(0, wide.windowAt(100000),
                                   "a fitting text holds position 0 for ever");
  wide.window(7, buf, sizeof(buf));   // even a stale pos must not rotate the render
  TEST_ASSERT_EQUAL_STRING_MESSAGE("ABCDEFGH            ", buf,
                                   "window() ignores pos for a fitting text");
}

void test_the_step_rate_is_the_injected_one(void) {
  Marquee fast(geom(8, 0, 10));
  fast.setText("ABCDEFGHIJKL", 0);
  fast.setActive(true, 0);
  TEST_ASSERT_EQUAL_UINT16_MESSAGE(10, fast.windowAt(100), "10 ms a step: 100 ms is 10");

  Marquee slow(geom(8, 0, 1000));
  slow.setText("ABCDEFGHIJKL", 0);
  slow.setActive(true, 0);
  TEST_ASSERT_EQUAL_UINT16_MESSAGE(0, slow.windowAt(100), "1 s a step: 100 ms is 0");
}

// A zero step would divide by zero and a zero width would draw nothing. Sanitised once at
// construction, as MenuGeometry is, rather than defended in every method.
void test_a_degenerate_geometry_is_sanitised_at_construction(void) {
  Marquee m(geom(0, 0, 0));
  TEST_ASSERT_GREATER_THAN_UINT8(0, m.geometry().width);
  TEST_ASSERT_GREATER_THAN_UINT32(0, m.geometry().stepMs);

  m.setText("ABCDEFGH", 0);
  m.setActive(true, 0);
  (void)m.windowAt(1000);       // must not divide by zero

  char buf[8];
  m.window(0, buf, sizeof(buf));
  TEST_ASSERT_TRUE_MESSAGE(std::strlen(buf) >= 1, "a sanitised width still draws something");
}

// The buffer, not the geometry, is the last word: a caller passing a short buffer gets a
// truncated window rather than a smashed stack.
void test_a_short_output_buffer_truncates_rather_than_overruns(void) {
  Marquee m(geom(20, 0, 100));
  m.setText("ABCDEFGHIJKLMNOPQRST", 0);

  char buf[5];
  buf[4] = '\xEE';
  m.window(0, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING("ABCD", buf);
}

}  // namespace

void setUp(void) {}
void tearDown(void) {}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_the_window_is_a_function_of_the_clock_not_the_call_count);
  RUN_TEST(test_a_stalled_caller_lands_on_the_clock_not_on_a_backlog);
  RUN_TEST(test_one_step_per_stepMs);
  RUN_TEST(test_resetting_the_same_text_does_not_restart_the_scroll);
  RUN_TEST(test_new_text_restarts_at_zero);
  RUN_TEST(test_empty_text_disables_the_marquee);
  RUN_TEST(test_the_gap_is_appended_and_scrolls_through);
  RUN_TEST(test_pause_freezes_and_resume_continues_from_there);
  RUN_TEST(test_setting_the_same_active_state_twice_is_inert);
  RUN_TEST(test_the_width_is_the_injected_one);
  RUN_TEST(test_the_step_rate_is_the_injected_one);
  RUN_TEST(test_a_degenerate_geometry_is_sanitised_at_construction);
  RUN_TEST(test_a_short_output_buffer_truncates_rather_than_overruns);
  return UNITY_END();
}
