// A full navigation script over a three-item demo menu, one item carrying THREE fields,
// asserted on the EMITTED FRAMES at every step.
//
// The two frame shapes are the whole reason the menu lives in the library: a full redraw is
// one 96-byte ISO-TP payload, a highlight move is ONE frame — about 1/14th of the bus time
// — and deciding which of the two a key produces is reasoning about the wire, not about the
// application. So every step below asserts the FRAME COUNT as well as the content: a menu
// that redrew the whole screen for a highlight move would still look right on the glass and
// would still be a defect.
//
// The payload is decoded back with affa::screen, which is an INDEPENDENT WITNESS built from
// docs/WIRE-SPEC.md rather than from our builder.

#include "../affa_test_support.h"

#include "carminat/CarminatDisplay.h"
#include "carminat/IPage.h"
#include "proto/IsoTp.h"
#include "proto/ScreenDecode.h"

using namespace affa;
using affatest::drain;
using affatest::pumpUntilIdle;

namespace {

const char* const kModes[] = {"Off", "Low", "High"};

// What one navigation step put on the wire.
struct Step {
  int      frames     = 0;   // total CAN frames
  int      redraws    = 0;   // 0x10 .. 0x21 first frames — a full 96-byte screen
  int      highlights = 0;   // `07 29 01 <rowTag>` single frames
  uint8_t  row        = 0;   // the last highlight's row tag: 0x7E top, 0x7F bottom
  bool     decoded    = false;
  ScreenModel screen;
};

template <class L>
Step capture(L& link) {
  Step s;
  Frame store[64];
  Frame f;
  int n = 0;
  while (link.takeSent(f)) {
    TEST_ASSERT_LESS_THAN_MESSAGE(64, n, "more frames than the capture buffer holds");
    store[n++] = f;
  }
  s.frames = n;

  isotp::Reassembler ra;
  for (int i = 0; i < n; ++i) {
    const Frame& g = store[i];
    if (g.len >= 4 && g.data[0] == screen::kHighlightSf &&
        g.data[1] == screen::kHighlightCmd && g.data[2] == screen::kHighlightArg) {
      ++s.highlights;
      s.row = g.data[3];
      continue;
    }
    if (g.data[0] == 0x10 && g.data[2] == screen::kMenuCmd) ++s.redraws;
    ra.onFrame(g);
  }
  if (ra.len() >= screen::kMenuHwMinLen) {
    screen::menu(ra.buffer(), ra.len(), s.screen);
    s.decoded = true;
  }
  return s;
}

struct Rig {
  LoopbackLink<256> link;
  affatest::FakeClock clk;
  CarminatDisplay d;
  Rig() : d(link, clk) {}

  void up() {
    d.begin();
    // Self-ACK BEFORE the opening: the 0x70 registrations now leave with the final hello.
    d.setSelfAck(true);
    affatest::completeCarminatAuth(d, link, clk);
    (void)d.setPower(true);                       // latch FUNCSREG out of the way
    affatest::settleCarminatRegistration(d, clk);
    pumpUntilIdle(d);
    TEST_ASSERT_TRUE(d.registered());
    drain(link);
  }

  void addDemoItems() {
    MenuItem a{};
    a.label       = "Bright";
    a.fields[0]   = integerField(50, 0, 100, 5, 10, "%");
    a.fields[1]   = listField(kModes, 3, 0);
    a.fields[2]   = readOnlyField(12, "V");
    a.fieldCount  = 3;
    TEST_ASSERT_EQUAL_INT(0, d.getMenu().addItem(a));

    MenuItem b{};
    b.label = "Second";
    TEST_ASSERT_EQUAL_INT(1, d.getMenu().addItem(b));

    MenuItem c{};
    c.label = "Third";
    TEST_ASSERT_EQUAL_INT(2, d.getMenu().addItem(c));
  }

  // Runs one navigation command and returns what it put on the wire.
  Step step(NavCommand c) {
    ASSERT_RESULT(Ok, d.nav(c));
    pumpUntilIdle(d);
    return capture(link);
  }
};

void expectRedraw(const Step& s, const char* header, const char* row0, const char* row1,
                  uint8_t scroll, uint8_t row, const char* what) {
  TEST_ASSERT_EQUAL_INT_MESSAGE(15, s.frames, what);        // 14 payload + 1 highlight
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, s.redraws, what);
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, s.highlights, what);
  TEST_ASSERT_TRUE_MESSAGE(s.decoded, what);
  TEST_ASSERT_EQUAL_STRING_MESSAGE(header, s.screen.header, what);
  TEST_ASSERT_EQUAL_STRING_MESSAGE(row0, s.screen.row0, what);
  TEST_ASSERT_EQUAL_STRING_MESSAGE(row1, s.screen.row1, what);
  TEST_ASSERT_EQUAL_HEX8_MESSAGE(scroll, s.screen.scroll, what);
  TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x7E, s.screen.row0Id, what);
  TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x7F, s.screen.row1Id, what);
  TEST_ASSERT_EQUAL_HEX8_MESSAGE(row, s.row, what);
}

void expectHighlightOnly(const Step& s, uint8_t row, const char* what) {
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, s.frames, what);
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, s.redraws, what);
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, s.highlights, what);
  TEST_ASSERT_EQUAL_HEX8_MESSAGE(row, s.row, what);
}

void expectNothing(const Step& s, const char* what) {
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, s.frames, what);
}

int g_closes = 0;
void countClose(void*) { ++g_closes; }

}  // namespace

// ---------------------------------------------------------------------------
// Open
// ---------------------------------------------------------------------------

void test_open_renders_header_rows_highlight_and_scroll_byte(void) {
  Rig r;
  r.up();
  r.addDemoItems();

  const Step s = r.step(NavCommand::Open);
  // Header from the Menu's constructor, row 0 rendered from the item's label and its three
  // fields, row 1 the next item, the down-arrow because there is more list below, and the
  // highlight on the TOP row.
  expectRedraw(s, "Main Menu", "Bright: 50% Off 12V", "Second", 0x0B, 0x7E, "nav(Open)");
  TEST_ASSERT_TRUE(r.d.getMenu().isOpen());
  TEST_ASSERT_EQUAL_UINT8(0, r.d.getMenu().selectedIndex());
  TEST_ASSERT_EQUAL_UINT8(0, r.d.getMenu().selectedRow());
}

void test_open_on_an_empty_menu_renders_nothing_and_says_so(void) {
  // An open menu with nothing in it renders nothing and then swallows the wheel and Load,
  // which reads as a dead panel. Refusing is the whole point.
  Rig r;
  r.up();
  ASSERT_RESULT(NotSupported, r.d.nav(NavCommand::Open));
  pumpUntilIdle(r.d);
  TEST_ASSERT_FALSE(r.d.getMenu().isOpen());
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, r.link.sentCount(), "an empty menu draws nothing");
}

// ---------------------------------------------------------------------------
// Next / Prev
// ---------------------------------------------------------------------------

void test_next_and_prev_move_the_selection_and_scroll_the_window(void) {
  Rig r;
  r.up();
  r.addDemoItems();
  r.step(NavCommand::Open);

  // Inside the visible window: ONE frame. This is the distinction the whole design exists
  // for — a redraw here would be ~14x the bus time for the same picture.
  Step s = r.step(NavCommand::Next);
  expectHighlightOnly(s, 0x7F, "Next 0 -> 1 stays inside the window");
  TEST_ASSERT_EQUAL_UINT8(1, r.d.getMenu().selectedIndex());

  // The window must scroll: full redraw, and the arrow flips to "up only" because the
  // selection is now the last item.
  s = r.step(NavCommand::Next);
  expectRedraw(s, "Main Menu", "Second", "Third", 0x07, 0x7F, "Next 1 -> 2 scrolls");
  TEST_ASSERT_EQUAL_UINT8(2, r.d.getMenu().selectedIndex());

  // THE LAST ENTRY IS REACHABLE and does not wrap.
  s = r.step(NavCommand::Next);
  expectNothing(s, "Next at the end of the list does nothing at all");
  TEST_ASSERT_EQUAL_UINT8(2, r.d.getMenu().selectedIndex());

  s = r.step(NavCommand::Prev);
  expectHighlightOnly(s, 0x7E, "Prev 2 -> 1 stays inside the window");

  s = r.step(NavCommand::Prev);
  expectRedraw(s, "Main Menu", "Bright: 50% Off 12V", "Second", 0x0B, 0x7E,
               "Prev 1 -> 0 scrolls back");

  // THE FIRST ENTRY IS REACHABLE and does not wrap either.
  s = r.step(NavCommand::Prev);
  expectNothing(s, "Prev at the start of the list does nothing at all");
  TEST_ASSERT_EQUAL_UINT8(0, r.d.getMenu().selectedIndex());
}

// ---------------------------------------------------------------------------
// Select: enter edit, then walk the fields
// ---------------------------------------------------------------------------

void test_select_enters_edit_then_advances_field_0_1_2_then_exits(void) {
  Rig r;
  r.up();
  r.addDemoItems();
  r.step(NavCommand::Open);

  // Enter edit: the row now carries the `*` marker and the live field is wrapped in <>.
  Step s = r.step(NavCommand::Select);
  expectRedraw(s, "Main Menu", "*Bright: <50%> Off 12V", "Second", 0x0B, 0x7E,
               "Select enters edit on field 0");
  TEST_ASSERT_TRUE(r.d.getMenu().isEditing());

  s = r.step(NavCommand::Select);
  expectRedraw(s, "Main Menu", "*Bright: 50% <Off> 12V", "Second", 0x0B, 0x7E,
               "Select advances 0 -> 1");

  s = r.step(NavCommand::Select);
  expectRedraw(s, "Main Menu", "*Bright: 50% Off <12V>", "Second", 0x0B, 0x7E,
               "Select advances 1 -> 2");

  // The last field EXITS edit mode; it does not wrap back to field 0.
  s = r.step(NavCommand::Select);
  expectRedraw(s, "Main Menu", "Bright: 50% Off 12V", "Second", 0x0B, 0x7E,
               "Select on the last field leaves edit mode");
  TEST_ASSERT_FALSE(r.d.getMenu().isEditing());
}

void test_select_on_an_item_with_no_fields_draws_nothing(void) {
  Rig r;
  r.up();
  r.addDemoItems();
  r.step(NavCommand::Open);
  r.step(NavCommand::Next);
  drain(r.link);

  const Step s = r.step(NavCommand::Select);
  expectNothing(s, "an item with no fields has nothing to edit");
  TEST_ASSERT_FALSE(r.d.getMenu().isEditing());
}

// ---------------------------------------------------------------------------
// Editing values
// ---------------------------------------------------------------------------

void test_the_first_and_last_list_entries_are_both_reachable(void) {
  // The extracted clamp wrote `if (newIndex >= size-1) newIndex = size-1`, which cannot
  // express "one past the end" and "the end" differently. The bound is `> count-1`.
  Rig r;
  r.up();
  r.addDemoItems();
  r.step(NavCommand::Open);
  r.step(NavCommand::Select);       // edit field 0
  r.step(NavCommand::Select);       // edit field 1, the list

  Step s = r.step(NavCommand::Next);
  expectRedraw(s, "Main Menu", "*Bright: 50% <Low> 12V", "Second", 0x0B, 0x7E, "list 0 -> 1");

  s = r.step(NavCommand::Next);
  expectRedraw(s, "Main Menu", "*Bright: 50% <High> 12V", "Second", 0x0B, 0x7E,
               "list 1 -> 2: THE LAST ENTRY IS REACHABLE");

  s = r.step(NavCommand::Next);
  expectNothing(s, "no change, no frames — clamped at the last entry");

  s = r.step(NavCommand::Prev);
  expectRedraw(s, "Main Menu", "*Bright: 50% <Low> 12V", "Second", 0x0B, 0x7E, "list 2 -> 1");

  s = r.step(NavCommand::Prev);
  expectRedraw(s, "Main Menu", "*Bright: 50% <Off> 12V", "Second", 0x0B, 0x7E,
               "list 1 -> 0: THE FIRST ENTRY IS REACHABLE");

  s = r.step(NavCommand::Prev);
  expectNothing(s, "no change, no frames — clamped at the first entry");
}

void test_a_hold_edge_takes_the_coarse_step(void) {
  // Field::stepMultiplier is reachable ONLY through nav()/pressKey with a source that
  // includes Local: a held detent has no wire representation, so the panel wheel physically
  // cannot produce this.
  Rig r;
  r.up();
  r.addDemoItems();
  r.step(NavCommand::Open);
  r.step(NavCommand::Select);       // edit field 0: value 50, step 5, multiplier 10

  Step s = r.step(NavCommand::Next);
  expectRedraw(s, "Main Menu", "*Bright: <55%> Off 12V", "Second", 0x0B, 0x7E,
               "a click is one step");

  s = r.step(NavCommand::Increase);
  expectRedraw(s, "Main Menu", "*Bright: <100%> Off 12V", "Second", 0x0B, 0x7E,
               "a hold is step * stepMultiplier, clamped at max");

  s = r.step(NavCommand::Increase);
  expectNothing(s, "already at max: no change, no frames");
}

void test_a_read_only_field_is_rendered_and_never_edited(void) {
  Rig r;
  r.up();
  r.addDemoItems();
  r.step(NavCommand::Open);
  r.step(NavCommand::Select);
  r.step(NavCommand::Select);
  r.step(NavCommand::Select);       // edit field 2, the read-only live reading

  const Step s = r.step(NavCommand::Next);
  expectNothing(s, "a readOnly field ignores the wheel entirely");
}

// ---------------------------------------------------------------------------
// Back
// ---------------------------------------------------------------------------

void test_back_closes_the_menu_and_restores_the_source_banner(void) {
  Rig r;
  r.up();
  r.addDemoItems();
  r.step(NavCommand::Open);
  r.step(NavCommand::Select);       // leave it in edit mode, which close() must clear
  drain(r.link);

  ASSERT_RESULT(Ok, r.d.nav(NavCommand::Back));
  pumpUntilIdle(r.d);
  TEST_ASSERT_FALSE(r.d.getMenu().isOpen());
  TEST_ASSERT_FALSE_MESSAGE(r.d.getMenu().isEditing(),
                            "reopening must not resume in edit mode on a stale field");

  // The OEM convention on close: back to the source banner. It ships as a DEFAULT because
  // it is the convention, and Menu::onClose() replaces it because it is policy.
  static const Frame kRenault[] = {
      {0x151, 8, {0x10, 0x0E, 0x77, 0x55, 0x55, 0xFF, 0x60, 0x01}, false},
      {0x151, 8, {0x21, 0x52, 0x45, 0x4E, 0x41, 0x55, 0x4C, 0x54}, false},
      {0x151, 8, {0x22, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, false},
  };
  affatest::expectFrames(r.link, kRenault, 3, "close -> setText(\"RENAULT\")");
}

void test_clear_closes_silently_without_firing_the_close_callback(void) {
  // Tearing the content down is the APPLICATION replacing the menu, not the user leaving,
  // so it must not transmit a banner over whatever the application is about to draw.
  Rig r;
  r.up();
  r.addDemoItems();
  r.d.getMenu().onClose(&countClose, nullptr);
  g_closes = 0;
  r.step(NavCommand::Open);

  r.d.getMenu().clear();
  pumpUntilIdle(r.d);
  TEST_ASSERT_FALSE(r.d.getMenu().isOpen());
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, g_closes, "clear() does not fire CloseCb");
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, r.link.sentCount(), "and puts nothing on the wire");
}

// ---------------------------------------------------------------------------
// Scroll indicator on the short lists the extracted code read past the end of
// ---------------------------------------------------------------------------

void test_a_one_and_two_item_menu_shows_no_arrows(void) {
  // count <= 2 -> 0x00. The extracted code indexed items[top+1] with no bounds check and
  // read past the end of a one-item menu; it would also have returned 0x0B there.
  Rig r;
  r.up();
  MenuItem only{};
  only.label = "Only";
  r.d.getMenu().addItem(only);

  Step s = r.step(NavCommand::Open);
  TEST_ASSERT_TRUE(s.decoded);
  TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00, s.screen.scroll, "one item: no arrows");
  TEST_ASSERT_EQUAL_STRING("Only", s.screen.row0);
  TEST_ASSERT_EQUAL_STRING_MESSAGE("", s.screen.row1, "the bottom row is empty, not garbage");

  MenuItem second{};
  second.label = "Two";
  r.d.getMenu().addItem(second);
  drain(r.link);
  // render() returns void on the model — the panel's verdict lives on the adapter, which is
  // the only layer holding an IPanel. Same assertion, one indirection further out.
  r.d.getMenu().render();
  ASSERT_RESULT(Ok, r.d.menuRenderer().lastResult());
  pumpUntilIdle(r.d);
  s = capture(r.link);
  TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00, s.screen.scroll, "two items: still no arrows");
  TEST_ASSERT_EQUAL_STRING("Two", s.screen.row1);
}

// ---------------------------------------------------------------------------
// The page stack owns every key while it is pushed
// ---------------------------------------------------------------------------

namespace {
struct CountingPage final : IPage {
  int enters = 0, exits = 0, ticks = 0, keys = 0;
  void onEnter() override { ++enters; }
  void onExit()  override { ++exits; }
  void onTick()  override { ++ticks; }
  void handleKey(Key, KeyEdge) override { ++keys; }
};
}  // namespace

void test_an_active_page_owns_every_key_including_the_hotkey(void) {
  Rig r;
  r.up();
  r.addDemoItems();

  CountingPage page;
  r.d.pushPage(&page);
  TEST_ASSERT_EQUAL_INT(1, page.enters);
  drain(r.link);

  // The GESTURE: openMenu() refuses while a page is pushed, which is exactly what makes
  // the base fall through to routeKeyToMenu() and hand the key to the page.
  ASSERT_RESULT(Ok, r.d.pressKey(Key::Load, KeyEdge::Hold, KeySource::Local));
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, page.keys, "hold-Load reached the page, not the menu");
  TEST_ASSERT_FALSE(r.d.getMenu().isOpen());

  // The INTENT: nav(Open) asks openMenu() directly and bypasses key routing altogether, so
  // it reports NotSupported rather than drawing a menu underneath the page — and it is NOT
  // delivered to the page either, because it was never a key.
  ASSERT_RESULT(NotSupported, r.d.nav(NavCommand::Open));
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, page.keys, "nav(Open) is an intent, not a keystroke");
  TEST_ASSERT_FALSE(r.d.getMenu().isOpen());

  ASSERT_RESULT(Ok, r.d.pressKey(Key::RollDown, KeyEdge::Click, KeySource::Local));
  TEST_ASSERT_EQUAL_INT(2, page.keys);

  // onTick is once per poll(), which is NOT a period — a page that needs one compares
  // against its own clock.
  const int before = page.ticks;
  affatest::pump(r.d, 5);
  TEST_ASSERT_EQUAL_INT(before + 5, page.ticks);

  pumpUntilIdle(r.d);
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, r.link.sentCount(), "a page draws nothing by itself");

  r.d.popPage();
  TEST_ASSERT_EQUAL_INT(1, page.exits);
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, r.link.sentCount(),
                                   "the menu was closed, so nothing is re-rendered");
}

// ---------------------------------------------------------------------------
// The transport keys reach the application even while the menu is open
// ---------------------------------------------------------------------------
//
// ADDED WITH THE MENU MIGRATION, and it is the one property that migration could plausibly
// have broken. The deleted Menu::handleKey returned false from its `default:` for SrcNext,
// SrcPrev, VolUp, VolDown and Pause, and AffaDisplayBase::routeKey falls through to the
// application's KeyCb on false. MenuModel has no key vocabulary at all, so that `default:`
// now lives in MenuController::routeKey — route every key into the model instead and the
// menu silently eats the radio controls whenever it is open.

namespace {
int  g_appKeys = 0;
Key  g_appKey  = Key::Load;
void countAppKey(Key k, KeyEdge, void*) { ++g_appKeys; g_appKey = k; }
}  // namespace

void test_transport_keys_fall_through_to_the_application_while_the_menu_is_open(void) {
  Rig r;
  r.up();
  r.addDemoItems();
  r.d.onKey(&countAppKey, nullptr);
  r.step(NavCommand::Open);
  TEST_ASSERT_TRUE(r.d.getMenu().isOpen());

  static const Key kTransport[] = {Key::SrcNext, Key::SrcPrev, Key::VolUp,
                                   Key::VolDown, Key::Pause};
  int expected = 0;
  for (const Key k : kTransport) {
    g_appKeys = 0;
    drain(r.link);
    ASSERT_RESULT(Ok, r.d.pressKey(k, KeyEdge::Click, KeySource::Local));
    pumpUntilIdle(r.d);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, g_appKeys,
                                  "the menu has no opinion on this key: it is the app's");
    TEST_ASSERT_EQUAL_HEX16(static_cast<uint16_t>(k), static_cast<uint16_t>(g_appKey));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, r.link.sentCount(),
                                     "and the menu draws nothing for a key it ignored");
    TEST_ASSERT_TRUE_MESSAGE(r.d.getMenu().isOpen(), "nor does it close");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, r.d.getMenu().selectedIndex(),
                                    "nor does the selection move");
    ++expected;
  }
  TEST_ASSERT_EQUAL_INT(5, expected);

  // The counterpart, so the test cannot pass by the KeyCb simply seeing everything: a key the
  // menu DOES have an opinion about is consumed and never reaches the application.
  g_appKeys = 0;
  ASSERT_RESULT(Ok, r.d.pressKey(Key::RollDown, KeyEdge::Click, KeySource::Local));
  pumpUntilIdle(r.d);
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, g_appKeys, "the wheel is the menu's while it is open");
  TEST_ASSERT_EQUAL_UINT8(1, r.d.getMenu().selectedIndex());
  drain(r.link);
}

// ---------------------------------------------------------------------------

void setUp(void) {}
void tearDown(void) {}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_open_renders_header_rows_highlight_and_scroll_byte);
  RUN_TEST(test_open_on_an_empty_menu_renders_nothing_and_says_so);
  RUN_TEST(test_next_and_prev_move_the_selection_and_scroll_the_window);
  RUN_TEST(test_select_enters_edit_then_advances_field_0_1_2_then_exits);
  RUN_TEST(test_select_on_an_item_with_no_fields_draws_nothing);
  RUN_TEST(test_the_first_and_last_list_entries_are_both_reachable);
  RUN_TEST(test_a_hold_edge_takes_the_coarse_step);
  RUN_TEST(test_a_read_only_field_is_rendered_and_never_edited);
  RUN_TEST(test_back_closes_the_menu_and_restores_the_source_banner);
  RUN_TEST(test_clear_closes_silently_without_firing_the_close_callback);
  RUN_TEST(test_a_one_and_two_item_menu_shows_no_arrows);
  RUN_TEST(test_an_active_page_owns_every_key_including_the_hotkey);
  RUN_TEST(test_transport_keys_fall_through_to_the_application_while_the_menu_is_open);
  return UNITY_END();
}
