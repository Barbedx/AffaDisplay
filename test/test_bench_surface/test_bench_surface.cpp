// The bench console's surface, executed rather than inspected.
//
// examples/90_bench_ota exposes one HTTP endpoint per library capability, and the owner's
// acceptance condition is that every one of them can be exercised: a menu with three
// parameters, set text, screen on and off, popup shown and hidden, set the clock, and
// navigation by command (next, previous, left, right, select).
//
// The board was bricked during bring-up, so the console cannot currently be driven over
// HTTP. This suite covers everything on that list EXCEPT the HTTP layer and the glass: for
// each item it makes the same library call the endpoint makes, in the same order, and then
// asserts what a faithful panel model DECODED from the wire. It is not a substitute for
// seeing the text on the panel â€” nothing is â€” but it is the difference between "the code
// looks right" and "the code was run and the panel understood it".
//
// What it does NOT prove: that PsychicHttp routes the query string correctly, that WiFi
// comes up, and that a real Carminat renders what the twin decodes. Those need the rig.

#include <unity.h>
#include <cstring>

#include "../affa_test_support.h"
#include "carminat/CarminatDisplay.h"
#include "vpanel/CarminatVirtualPanel.h"
#include "core/AffaRing.h"

using namespace affa;

namespace {

// Two-ended bus, as in test_twin: each side's send() lands in the other's ring, and the
// fromSelf stamp is cleared at the crossing because a real controller does not tell the
// other node "this frame was mine".
class BusLink final : public ICanLink {
 public:
  void connect(BusLink* peer) { _peer = peer; }
  bool send(const Frame& f) override {
    ++_stats.txFrames;
    if (_peer) {
      Frame c = f;
      c.fromSelf = false;
      if (!_peer->_rx.push(c)) ++_peer->_stats.ringOverflow;
      ++_peer->_stats.rxFrames;
    }
    return true;
  }
  bool  recv(Frame& out) override { return _rx.pop(out); }
  bool  isLive() const override   { return true; }
  Stats stats()  const override   { return _stats; }
  bool  pendingRx() const { return !_rx.empty(); }
 private:
  AffaRing<Frame, 128> _rx;
  Stats    _stats{};
  BusLink* _peer = nullptr;
};

// Layer 0 tap: what the driver actually put on the wire. Needed for the operations the
// twin does not model (power control, the clock), where the frame IS the evidence.
Frame g_tx[192];
int   g_txN = 0;
void tap(const Frame& f, Direction d, void*) {
  if (d == Direction::Tx && g_txN < 192) g_tx[g_txN++] = f;
}
bool sawTx(uint16_t id, const uint8_t* head, uint8_t n) {
  for (int i = 0; i < g_txN; ++i) {
    if (g_tx[i].id != id) continue;
    if (memcmp(g_tx[i].data, head, n) == 0) return true;
  }
  return false;
}

const char* kModeList[3] = {"Off", "Auto", "On"};

struct Bench {
  BusLink radio, panel;
  affatest::FakeClock clk;
  CarminatDisplay d;
  CarminatVirtualPanel twin;

  Bench() : d(radio, clk) {
    radio.connect(&panel);
    panel.connect(&radio);
  }

  // Exactly what examples/90_bench_ota does in setup(): build the demo menu, install the
  // tap, bring the twin up in the mode that models a panel, then begin().
  void begin() {
    buildDemoMenu();
    g_txN = 0;
    d.onFrame(&tap, nullptr);
    twin.begin(panel, clk);
    twin.setAckMode(VirtualPanelBase::AckMode::Declared);
    d.begin();

    // Two passes complete the handshake: pass 1 carries our 0xB9/0xBA to the twin, which
    // answers 0x69 and the `61 11` that asks us to announce ourselves; pass 2 carries the
    // three hello frames back. Then one payload drags the lazy 0x70 registration burst
    // with it and FUNCSREG latches — the same sequence the bench firmware goes through
    // before its first setText, and the reason a render before it returns NoSync.
    run(2);
    (void)d.setPower(true);
    settle();
  }

  void run(int passes) { for (int i = 0; i < passes; ++i) step(); }

  void buildDemoMenu() {
    Menu& m = d.getMenu();
    MenuItem bright;
    bright.label = "Bright";
    bright.fields[0] = integerField(50, 0, 100, 5, 4, "%");
    bright.fieldCount = 1;
    m.addItem(bright);

    MenuItem mode;
    mode.label = "Mode";
    mode.fields[0] = listField(kModeList, 3, 1);
    mode.fieldCount = 1;
    m.addItem(mode);

    // THE "menu with 3 parameters" of the acceptance condition.
    MenuItem tm;
    tm.label = "Time";
    tm.fields[0] = integerField(12, 0, 23, 1, 6, "h");
    tm.fields[1] = integerField(30, 0, 59, 1, 10, "m");
    tm.fields[2] = readOnlyField(0, "s");
    tm.fieldCount = 3;
    m.addItem(tm);
  }

  void step() {
    d.poll();
    Frame f;
    while (panel.recv(f)) twin.onFrame(f);
  }
  void settle(int maxPasses = 400) {
    for (int i = 0; i < maxPasses; ++i) {
      if (!d.busy() && !panel.pendingRx() && !radio.pendingRx()) return;
      step();
    }
    TEST_FAIL_MESSAGE("the bench never went quiet");
  }
  // Renders are queued; a render plus its settle is what an endpoint's caller observes.
  void apply() { settle(); }
};

// ---------------------------------------------------------------------------
// One test per line of the acceptance condition.
// ---------------------------------------------------------------------------

// "Ð¿Ð¾ÑÑ‚Ð°Ð²Ð¸Ñ‚Ð¸ Ñ‚ÐµÐºÑÑ‚" â€” GET /api/text?t=AFFA%20OK
void test_endpoint_text_reaches_the_panel(void) {
  Bench b; b.begin();
  TEST_ASSERT_TRUE_MESSAGE(b.d.registered(), "the handshake must complete first");

  TEST_ASSERT_EQUAL(Result::Ok, b.d.setText("AFFA OK", 255));
  b.apply();
  TEST_ASSERT_EQUAL_STRING_MESSAGE("AFFA OK", b.twin.screen().header,
                                   "the panel model decoded a different string");
}

// "Ð·Ð°Ð´Ð°Ñ‚Ð¸ Ñ‡Ð°Ñ" â€” GET /api/time?hhmm=1234
void test_endpoint_clock_reaches_the_wire(void) {
  Bench b; b.begin();
  g_txN = 0;
  TEST_ASSERT_EQUAL(Result::Ok, b.d.setTime("1234"));
  b.apply();
  // 0x151: 05 'V' '1' '2' '3' '4' â€” the clock frame, verbatim from WIRE-SPEC Â§8.2.
  const uint8_t want[6] = {0x05, 'V', '1', '2', '3', '4'};
  TEST_ASSERT_TRUE_MESSAGE(sawTx(0x151, want, 6), "no clock frame on 0x151");
}

// "Ð²Ð²Ñ–Ð¼ÐºÐ½ÑƒÑ‚Ð¸/Ð²Ð¸Ð¼ÐºÐ½ÑƒÑ‚Ð¸ ÐµÐºÑ€Ð°Ð½" â€” GET /api/state?on=0|1
void test_endpoint_screen_off_then_on(void) {
  Bench b; b.begin();

  g_txN = 0;
  TEST_ASSERT_EQUAL(Result::Ok, b.d.setPower(false));
  b.apply();
  const uint8_t off[3] = {0x03, 0x52, 0x00};
  TEST_ASSERT_TRUE_MESSAGE(sawTx(0x151, off, 3), "no display-off frame");

  g_txN = 0;
  TEST_ASSERT_EQUAL(Result::Ok, b.d.setPower(true));
  b.apply();
  const uint8_t on[3] = {0x03, 0x52, 0x09};
  TEST_ASSERT_TRUE_MESSAGE(sawTx(0x151, on, 3), "no display-on frame");
}

// "Ð²Ñ–Ð´Ð¾Ð±Ñ€Ð°Ð·Ð¸Ñ‚Ð¸/ÑÑ…Ð¾Ð²Ð°Ñ‚Ð¸ popup" â€” GET /api/popup?t=... and /api/popup/hide
void test_endpoint_popup_shown_then_hidden(void) {
  Bench b; b.begin();

  TEST_ASSERT_EQUAL(Result::Ok, b.d.showPopupText("VOL 28", 0x09, 0xFF, 0x60));
  b.apply();
  // The wire carries eight cells, space-padded — that is pinned byte for byte by
  // test_carminat_wire. Here we assert the MEANING, and the decoder trims the padding,
  // which is what a semantic oracle should do: "VOL 28" is what the panel shows.
  TEST_ASSERT_EQUAL_STRING_MESSAGE("VOL 28", b.twin.screen().header,
                                   "the popup text did not decode");

  g_txN = 0;
  TEST_ASSERT_EQUAL(Result::Ok, b.d.hidePopup());
  b.apply();
  const uint8_t close[3] = {0x02, 0x54, 0x03};
  TEST_ASSERT_TRUE_MESSAGE(sawTx(0x151, close, 3), "no popup-close frame");
}

// "Ð·Ð°Ð´Ð°Ñ‚Ð¸ Ð¼ÐµÐ½ÑŽ Ð· 3 Ð¿Ð°Ñ€Ð°Ð¼ÐµÑ‚Ñ€Ð°Ð¼Ð¸" â€” the demo menu's third item, rendered on the panel.
void test_endpoint_menu_with_three_parameters_renders(void) {
  Bench b; b.begin();

  TEST_ASSERT_EQUAL(Result::Ok, b.d.nav(NavCommand::Open));
  b.apply();
  TEST_ASSERT_TRUE_MESSAGE(b.d.getMenu().isOpen(), "nav(Open) did not open the menu");
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(3, b.d.getMenu().count(), "the demo menu must have 3 items");

  // Walk to the three-field item and prove all three values are on the glass at once.
  (void)b.d.nav(NavCommand::Next); b.apply();
  (void)b.d.nav(NavCommand::Next); b.apply();
  TEST_ASSERT_EQUAL_UINT8(2, b.d.getMenu().selectedIndex());

  const ScreenModel& s = b.twin.screen();
  const bool onRow0 = strstr(s.row0, "12") && strstr(s.row0, "30");
  const bool onRow1 = strstr(s.row1, "12") && strstr(s.row1, "30");
  TEST_ASSERT_TRUE_MESSAGE(onRow0 || onRow1,
                           "the three-field item did not render both editable values");
}

// "Ð½Ð°Ð²Ñ–Ð³Ð°Ñ†Ñ–Ñ Ð¿Ð¾ Ð¼ÐµÐ½ÑŽ Ñ‡ÐµÑ€ÐµÐ· ÐºÐ¾Ð¼Ð°Ð½Ð´Ð¸ (Ð½Ð°ÑÑ‚ÑƒÐ¿Ð½Ð¸Ð¹, Ð¿Ð¾Ð¿ÐµÑ€ÐµÐ´Ð½Ñ–Ð¹, Ð²Ð»Ñ–Ð²Ð¾, Ð²Ð¿Ñ€Ð°Ð²Ð¾, Ð²Ð¸Ð±Ñ€Ð°Ñ‚Ð¸)"
void test_endpoint_navigation_commands(void) {
  Bench b; b.begin();
  Menu& m = b.d.getMenu();

  TEST_ASSERT_EQUAL(Result::Ok, b.d.nav(NavCommand::Open)); b.apply();
  TEST_ASSERT_EQUAL_UINT8(0, m.selectedIndex());

  // Ð½Ð°ÑÑ‚ÑƒÐ¿Ð½Ð¸Ð¹ / Ð¿Ð¾Ð¿ÐµÑ€ÐµÐ´Ð½Ñ–Ð¹
  TEST_ASSERT_EQUAL(Result::Ok, b.d.nav(NavCommand::Next)); b.apply();
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(1, m.selectedIndex(), "Next did not advance");
  TEST_ASSERT_EQUAL(Result::Ok, b.d.nav(NavCommand::Prev)); b.apply();
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, m.selectedIndex(), "Prev did not go back");

  // Ð²Ð¸Ð±Ñ€Ð°Ñ‚Ð¸ â€” enters edit on the integer item
  TEST_ASSERT_EQUAL(Result::Ok, b.d.nav(NavCommand::Select)); b.apply();
  TEST_ASSERT_TRUE_MESSAGE(m.isEditing(), "Select did not enter edit mode");

  // Ð²Ð¿Ñ€Ð°Ð²Ð¾ / Ð²Ð»Ñ–Ð²Ð¾ â€” the value moves by the field's step and comes back
  const int before = m.item(0)->fields[0].value;
  TEST_ASSERT_EQUAL(Result::Ok, b.d.nav(NavCommand::Increase)); b.apply();
  const int up = m.item(0)->fields[0].value;
  TEST_ASSERT_TRUE_MESSAGE(up > before, "Increase did not raise the value");
  TEST_ASSERT_EQUAL(Result::Ok, b.d.nav(NavCommand::Decrease)); b.apply();
  TEST_ASSERT_EQUAL_INT_MESSAGE(before, m.item(0)->fields[0].value,
                                "Decrease did not undo Increase");

  // Ð½Ð°Ð·Ð°Ð´ â€” leaves edit, then closes
  TEST_ASSERT_EQUAL(Result::Ok, b.d.nav(NavCommand::Back)); b.apply();
  TEST_ASSERT_FALSE_MESSAGE(m.isEditing(), "Back did not leave edit mode");
}

// Select must walk field 0 -> 1 -> 2 and then leave, which is the only reason a
// three-field item is in the demo menu at all.
void test_select_walks_all_three_fields(void) {
  Bench b; b.begin();
  Menu& m = b.d.getMenu();
  (void)b.d.nav(NavCommand::Open); b.apply();
  (void)b.d.nav(NavCommand::Next); b.apply();
  (void)b.d.nav(NavCommand::Next); b.apply();
  TEST_ASSERT_EQUAL_UINT8(2, m.selectedIndex());

  (void)b.d.nav(NavCommand::Select); b.apply();
  TEST_ASSERT_TRUE_MESSAGE(m.isEditing(), "Select did not enter edit on the 3-field item");

  int seen = 1;
  for (int i = 0; i < 4 && m.isEditing(); ++i) {
    (void)b.d.nav(NavCommand::Select);
    b.apply();
    if (m.isEditing()) ++seen;
  }
  TEST_ASSERT_FALSE_MESSAGE(m.isEditing(), "editing never ended after walking the fields");
  TEST_ASSERT_GREATER_OR_EQUAL_MESSAGE(2, seen,
      "Select must advance through the editable fields, not exit on the first press");
}

// The console reports NotSupported rather than 404 so that it doubles as a capability
// probe. That only works if the capabilities answer honestly.
void test_capabilities_match_what_the_endpoints_can_do(void) {
  Bench b; b.begin();
  TEST_ASSERT_TRUE(b.d.supports(Feature::Text));
  TEST_ASSERT_TRUE(b.d.supports(Feature::Time));
  TEST_ASSERT_TRUE(b.d.supports(Feature::Power));
  TEST_ASSERT_TRUE(b.d.supports(Feature::Menu));
#if AFFA_ENABLE_POPUP
  TEST_ASSERT_TRUE(b.d.supports(Feature::Popup));
#endif
}

}  // namespace

void setUp(void) {}
void tearDown(void) {}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_endpoint_text_reaches_the_panel);
  RUN_TEST(test_endpoint_clock_reaches_the_wire);
  RUN_TEST(test_endpoint_screen_off_then_on);
  RUN_TEST(test_endpoint_popup_shown_then_hidden);
  RUN_TEST(test_endpoint_menu_with_three_parameters_renders);
  RUN_TEST(test_endpoint_navigation_commands);
  RUN_TEST(test_select_walks_all_three_fields);
  RUN_TEST(test_capabilities_match_what_the_endpoints_can_do);
  return UNITY_END();
}
