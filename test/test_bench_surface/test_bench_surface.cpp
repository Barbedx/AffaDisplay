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
// seeing the text on the panel — nothing is — but it is the difference between "the code
// looks right" and "the code was run and the panel understood it".
//
// What it does NOT prove: that PsychicHttp routes the query string correctly, that WiFi
// comes up, and that a real Carminat renders what this suite decodes. Those need the rig.
//
// THE ORACLE IS LOCAL AND IT READS THE WIRE. It used to be CarminatVirtualPanel, a panel
// twin that shipped as library surface; the twins are gone (they were application-shaped
// code in src/), and what replaces them is the thirty lines of decodeTx() below, built on
// the two things the library still publishes for decoding somebody else's traffic:
// isotp::Reassembler and affa::screen. The independence that made the twin worth having
// survives — this decoder still disagrees with the encoder rather than sharing code with
// it — and it now reads the frames the driver ACTUALLY transmitted, one layer closer to
// the glass than a model of a panel that consumed them.

#include <unity.h>
#include <cstring>

#include "../affa_test_support.h"
#include "carminat/CarminatDisplay.h"
#include "proto/IsoTp.h"
#include "proto/ScreenDecode.h"

using namespace affa;

namespace {

// Layer 0 tap: what the driver actually put on the wire. It is the evidence for the
// operations with no screen to decode (power control, the clock) AND the input to the
// semantic oracle below.
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

// Replay every captured 0x151 frame through a fresh reassembler and decode each completed
// message, last one winning — which is what the panel's glass shows. Transcribed from the
// deleted CarminatVirtualPanel::decode(), including the two traps it recorded:
//
//   * the highlight is a STANDALONE single frame, and screen::frame() carries the full
//     `07 29 01` guard because 0x151 also carries `03 52 …`, `05 56 …` and `02 54 03`.
//   * EVERY multi-frame message goes through the reassembler, including first frames whose
//     command we do not decode. Returning early on an unrecognised first frame leaves the
//     previous message active, and its continuations then append to THAT buffer.
ScreenModel decodeTx() {
  ScreenModel s{};
  isotp::Reassembler asmb;
  for (int i = 0; i < g_txN; ++i) {
    const Frame& f = g_tx[i];
    if (f.id != carminat::kIdSetText || f.len == 0) continue;
    if (screen::frame(f, s)) continue;
    if (!asmb.onFrame(f)) continue;

    const uint8_t* p = asmb.buffer();
    const uint8_t  n = asmb.len();
    if (n < 4) continue;                  // p[2] is the command, p[3] its first operand
    switch (p[2]) {
      // Mode 0x05 is the FULLSCREEN variant; its payload has a different layout entirely
      // and decoding it with the menu offsets would produce a confident wrong screen.
      case screen::kMenuCmd:
        if (p[3] == screen::kMenuModeWin) screen::menu(p, n, s);
        break;
      case screen::kWinTextCmdFull:
      case screen::kWinTextCmdWindow:
        screen::windowText(p, n, s);
        break;
      case screen::kInfoCmd:
        screen::infoRow(p, n, s);
        break;
      default:
        break;                            // unmodelled: decoded as nothing, not as a guess
    }
  }
  return s;
}

const char* kModeList[3] = {"Off", "Auto", "On"};

struct Bench {
  affa::LoopbackLink<128> link;
  affatest::FakeClock clk;
  CarminatDisplay d;

  Bench() : d(link, clk) {}

  // What examples/90_bench_ota does in setup(): build the demo menu, install the tap, then
  // begin() and complete the handshake.
  void begin() {
    buildDemoMenu();
    g_txN = 0;
    d.onFrame(&tap, nullptr);
    d.begin();

    // The panel's `61 11` is what clears FAILED; setSelfAck then supplies the per-frame
    // ACK that would otherwise come from the panel, so the lazy 0x70 registration burst
    // completes and FUNCSREG latches. Same sequence the bench firmware goes through before
    // its first setText, and the reason a render before it returns NoSync.
    d.setSelfAck(true);
    affatest::completeCarminatAuth(d, link, clk);
    (void)d.setPower(true);
    affatest::settleCarminatRegistration(d, clk);
    settle();
    TEST_ASSERT_TRUE_MESSAGE(d.registered(), "FUNCSREG must latch before the bench tests");
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

  void step() { d.poll(); }
  void settle(int maxPasses = 400) {
    for (int i = 0; i < maxPasses; ++i) {
      if (!d.busy()) return;
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

// "поставити текст" — GET /api/text?t=AFFA%20OK
void test_endpoint_text_reaches_the_panel(void) {
  Bench b; b.begin();
  TEST_ASSERT_TRUE_MESSAGE(b.d.registered(), "the handshake must complete first");

  g_txN = 0;
  TEST_ASSERT_EQUAL(Result::Ok, b.d.setText("AFFA OK", 255));
  b.apply();
  TEST_ASSERT_EQUAL_STRING_MESSAGE("AFFA OK", decodeTx().header,
                                   "the wire decoded to a different string");
}

// "задати час" — GET /api/time?hhmm=1234
void test_endpoint_clock_reaches_the_wire(void) {
  Bench b; b.begin();
  g_txN = 0;
  TEST_ASSERT_EQUAL(Result::Ok, b.d.setTime("1234"));
  b.apply();
  // 0x151: 05 'V' '1' '2' '3' '4' — the clock frame, verbatim from WIRE-SPEC §8.2.
  const uint8_t want[6] = {0x05, 'V', '1', '2', '3', '4'};
  TEST_ASSERT_TRUE_MESSAGE(sawTx(0x151, want, 6), "no clock frame on 0x151");
}

// "ввімкнути/вимкнути екран" — GET /api/state?on=0|1
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

// "відобразити/сховати popup" — GET /api/popup?t=... and /api/popup/hide
void test_endpoint_popup_shown_then_hidden(void) {
  Bench b; b.begin();

  g_txN = 0;
  TEST_ASSERT_EQUAL(Result::Ok, b.d.showPopupText("VOL 28", 0x09, 0xFF, 0x60));
  b.apply();
  // The wire carries eight cells, space-padded — that is pinned byte for byte by
  // test_carminat_wire. Here we assert the MEANING, and the decoder trims the padding,
  // which is what a semantic oracle should do: "VOL 28" is what the panel shows.
  TEST_ASSERT_EQUAL_STRING_MESSAGE("VOL 28", decodeTx().header,
                                   "the popup text did not decode");

  g_txN = 0;
  TEST_ASSERT_EQUAL(Result::Ok, b.d.hidePopup());
  b.apply();
  const uint8_t close[3] = {0x02, 0x54, 0x03};
  TEST_ASSERT_TRUE_MESSAGE(sawTx(0x151, close, 3), "no popup-close frame");
}

// "задати меню з 3 параметрами" — the demo menu's third item, rendered on the panel.
void test_endpoint_menu_with_three_parameters_renders(void) {
  Bench b; b.begin();

  TEST_ASSERT_EQUAL(Result::Ok, b.d.nav(NavCommand::Open));
  b.apply();
  TEST_ASSERT_TRUE_MESSAGE(b.d.getMenu().isOpen(), "nav(Open) did not open the menu");
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(3, b.d.getMenu().count(), "the demo menu must have 3 items");

  // Walk to the three-field item and prove all three values are on the glass at once.
  (void)b.d.nav(NavCommand::Next); b.apply();
  g_txN = 0;
  (void)b.d.nav(NavCommand::Next); b.apply();
  TEST_ASSERT_EQUAL_UINT8(2, b.d.getMenu().selectedIndex());

  const ScreenModel s = decodeTx();
  const bool onRow0 = strstr(s.row0, "12") && strstr(s.row0, "30");
  const bool onRow1 = strstr(s.row1, "12") && strstr(s.row1, "30");
  TEST_ASSERT_TRUE_MESSAGE(onRow0 || onRow1,
                           "the three-field item did not render both editable values");
}

// "навігація по меню через команди (наступний, попередній, вліво, вправо, вибрати)"
void test_endpoint_navigation_commands(void) {
  Bench b; b.begin();
  Menu& m = b.d.getMenu();

  TEST_ASSERT_EQUAL(Result::Ok, b.d.nav(NavCommand::Open)); b.apply();
  TEST_ASSERT_EQUAL_UINT8(0, m.selectedIndex());

  // наступний / попередній
  TEST_ASSERT_EQUAL(Result::Ok, b.d.nav(NavCommand::Next)); b.apply();
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(1, m.selectedIndex(), "Next did not advance");
  TEST_ASSERT_EQUAL(Result::Ok, b.d.nav(NavCommand::Prev)); b.apply();
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, m.selectedIndex(), "Prev did not go back");

  // вибрати — enters edit on the integer item
  TEST_ASSERT_EQUAL(Result::Ok, b.d.nav(NavCommand::Select)); b.apply();
  TEST_ASSERT_TRUE_MESSAGE(m.isEditing(), "Select did not enter edit mode");

  // вправо / вліво — the value moves by the field's step and comes back
  const int before = m.item(0)->fields[0].value;
  TEST_ASSERT_EQUAL(Result::Ok, b.d.nav(NavCommand::Increase)); b.apply();
  const int up = m.item(0)->fields[0].value;
  TEST_ASSERT_TRUE_MESSAGE(up > before, "Increase did not raise the value");
  TEST_ASSERT_EQUAL(Result::Ok, b.d.nav(NavCommand::Decrease)); b.apply();
  TEST_ASSERT_EQUAL_INT_MESSAGE(before, m.item(0)->fields[0].value,
                                "Decrease did not undo Increase");

  // назад — leaves edit, then closes
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
