// The panel class and its TWIN on one bus, with no hardware anywhere.
//
// This is the suite that asserts SEMANTICS rather than bytes: "the panel shows header
// 'Main Menu', row 0 'Bright: 50%'". A byte comparison passes for the wrong reason (two
// bugs cancelling) and fails for no reason (a harmless padding change); this fails exactly
// when the driver would have put the wrong thing on the glass.
//
// THE TWIN IS AN INDEPENDENT WITNESS. Its offsets come from docs/WIRE-SPEC.md, not from our
// encoder. If it and a render call ever disagree by a byte, that is a finding about one of
// the two and the way to settle it is a capture — never by moving a constant in proto/.
//
// AckMode::Declared IS THE ONE THAT MODELS A PANEL. The default, Done, terminates every
// multi-frame transfer after frame 0 (because the transmit FSM correctly treats "DONE while
// bytes remain" as SUCCESS), so a twin in that mode sees 8 bytes of a 96-byte screen.

#include "../affa_test_support.h"

#include "carminat/CarminatDisplay.h"
#include "updatelist/UpdateListDisplay.h"
#include "vpanel/CarminatVirtualPanel.h"
#include "vpanel/UpdateListSegVirtualPanel.h"
#include "core/AffaRing.h"

using namespace affa;

namespace {

// ---------------------------------------------------------------------------
// A two-ended bus. Each side's send() lands in the OTHER side's receive ring, which is
// what a real CAN bus does and what LoopbackLink deliberately does not.
// ---------------------------------------------------------------------------
class BusLink final : public ICanLink {
 public:
  void connect(BusLink* peer) { _peer = peer; }

  bool send(const Frame& f) override {
    ++_stats.txFrames;
    if (_peer) {
      Frame c = f;
      // A real controller does not tell the other node "this was mine". Clearing the stamp
      // at the crossing is what makes the two ends behave like two nodes rather than like
      // one object talking to itself.
      c.fromSelf = false;
      if (!_peer->_rx.push(c)) ++_peer->_stats.ringOverflow;
      ++_peer->_stats.rxFrames;
    }
    return true;
  }
  bool  recv(Frame& out) override { return _rx.pop(out); }
  bool  isLive() const override   { return true; }
  Stats stats()  const override   { return _stats; }

  bool pendingRx() const { return !_rx.empty(); }

 private:
  AffaRing<Frame, 128> _rx;
  Stats    _stats{};
  BusLink* _peer = nullptr;
};

// ---------------------------------------------------------------------------
// Bench: driver on one end, twin on the other
// ---------------------------------------------------------------------------
template <class Driver, class Twin>
struct Bench {
  BusLink radioSide, panelSide;
  affatest::FakeClock clk;
  Driver d;
  Twin   twin;

  Bench() : d(radioSide, clk) {
    radioSide.connect(&panelSide);
    panelSide.connect(&radioSide);
  }

  void begin() {
    twin.begin(panelSide, clk);
    twin.setAckMode(VirtualPanelBase::AckMode::Declared);
    d.begin();
  }

  // One pass of the whole system: the driver polls, then everything it transmitted is fed
  // to the twin, whose answers land in the driver's ring for the next pass.
  void step() {
    d.poll();
    Frame f;
    while (panelSide.recv(f)) twin.onFrame(f);
  }

  void run(int passes) { for (int i = 0; i < passes; ++i) step(); }

  // Runs until the bus is quiet, with a bounded, loud failure.
  void settle(int maxPasses = 300) {
    for (int i = 0; i < maxPasses; ++i) {
      if (!d.busy() && !panelSide.pendingRx() && !radioSide.pendingRx()) return;
      step();
    }
    TEST_FAIL_MESSAGE("the bench never went quiet");
  }

  // Runs the clock far enough forward for the driver to declare peer loss.
  //
  // It takes MORE THAN ONE heartbeat tick, and that is the FSM being correct rather than
  // slow: a PeerAlive flag latched by the twin's last 0x69 is still pending, and the first
  // tick after the silence begins consumes it and re-arms the window. Only the tick after
  // that reaches the expiry branch. A test that jumped 5001 ms once and expected a
  // teardown would be asserting that a single missed ping tears the link down, which is
  // exactly what AFFA_PEER_TIMEOUT_MS exists not to do.
  void starve(int rounds = 3) {
    for (int i = 0; i < rounds; ++i) {
      clk.advance(AFFA_PEER_TIMEOUT_MS + AFFA_SYNC_INTERVAL_MS + 1);
      run(2);
    }
  }
};

using CarBench = Bench<CarminatDisplay, CarminatVirtualPanel>;
using SegBench = Bench<UpdateListDisplay, UpdateListSegVirtualPanel>;

int g_keys = 0;
Key g_key = Key::Load;
KeyEdge g_edge = KeyEdge::Click;
void countKey(Key k, KeyEdge e, void*) { ++g_keys; g_key = k; g_edge = e; }

// Layer 0 on the DRIVER's side. The bench feeds every transmitted frame straight into the
// twin, so by the time a test looks at the bus it is already empty — the tap is how a test
// sees what the driver actually put there.
Frame g_tx[128];
int   g_txN = 0;
void  tapTx(const Frame& f, Direction dir, void*) {
  if (dir == Direction::Tx && g_txN < 128) g_tx[g_txN++] = f;
}
bool sawTx(uint32_t id, uint8_t b0) {
  for (int i = 0; i < g_txN; ++i)
    if (g_tx[i].id == id && g_tx[i].data[0] == b0) return true;
  return false;
}

}  // namespace

// ---------------------------------------------------------------------------
// The handshake, with no hardware
// ---------------------------------------------------------------------------

void test_the_handshake_completes_to_registered_against_the_twin(void) {
  CarBench b;
  b.begin();
  TEST_ASSERT_FALSE(b.d.synced());
  TEST_ASSERT_FALSE(b.twin.synced());

  // Pass 1: the radio's 0xB9 heartbeat and 0xBA request reach the twin, which answers with
  // its 0x69 ping and the `61 11` that asks the radio to announce itself.
  // Pass 2: the radio's three hello frames reach the twin, whose 0x70 is the edge that
  // means "the link is up" on the panel side.
  b.run(2);
  TEST_ASSERT_TRUE_MESSAGE(b.d.synced(), "the driver cleared FAILED");
  TEST_ASSERT_TRUE_MESSAGE(b.twin.synced(), "and the twin saw the announce");
  TEST_ASSERT_GREATER_OR_EQUAL(1u, b.twin.syncRepliesSent());

  // The first payload drags the lazy 0x70 registration burst with it. The twin has to
  // acknowledge the WHOLE function table — a twin that answered only 0x151 would stall the
  // 0x1F1 probe for the full ACK timeout and then fail the payload behind it.
  ASSERT_RESULT(Ok, b.d.setPower(true));
  b.settle();
  TEST_ASSERT_TRUE_MESSAGE(b.d.registered(), "FUNCSREG latched with no hardware present");
  TEST_ASSERT_GREATER_OR_EQUAL(3u, b.twin.acksSent());
}

void test_the_twins_ping_holds_the_peer_watchdog_open(void) {
  // A real panel pings on a timer of its own; the twin is FED frames and never polled, so
  // it hangs its 0x69 off the radio's heartbeat. Same cadence, same effect on the driver's
  // 5000 ms wall-clock watchdog.
  CarBench b;
  b.begin();
  b.run(2);
  TEST_ASSERT_TRUE(b.d.synced());

  for (int s = 0; s < 20; ++s) {
    b.clk.advance(1000);
    b.run(2);
    TEST_ASSERT_TRUE_MESSAGE(b.d.synced(), "twenty seconds of twin pings must hold it up");
  }

  // And when the twin stops answering, the watchdog still fires on the clock.
  b.twin.setEmulate(false);            // PASSIVE: decodes, transmits nothing
  b.starve();
  TEST_ASSERT_FALSE_MESSAGE(b.d.synced(), "a silent panel is still declared lost");
  TEST_ASSERT_FALSE_MESSAGE(b.d.registered(), "and FUNCSREG goes with it");
}

// ---------------------------------------------------------------------------
// Text
// ---------------------------------------------------------------------------

void test_setText_shows_up_in_the_twins_screen_model(void) {
  CarBench b;
  b.begin();
  b.run(2);
  ASSERT_RESULT(Ok, b.d.setPower(true));
  b.settle();

  ASSERT_RESULT(Ok, b.d.setText("HELLO"));
  b.settle();
  ASSERT_RESULT(Ok, b.d.lastResult());

  TEST_ASSERT_TRUE(b.twin.screen().mode == ScreenModel::Mode::Menu);
  TEST_ASSERT_EQUAL_STRING_MESSAGE("HELLO", b.twin.screen().header,
                                   "the windowed text lands in the header field");
}

// ---------------------------------------------------------------------------
// The menu screen
// ---------------------------------------------------------------------------

void test_showMenu_decodes_to_three_strings_a_scroll_byte_and_a_highlight(void) {
  CarBench b;
  b.begin();
  b.run(2);
  ASSERT_RESULT(Ok, b.d.setPower(true));
  b.settle();

  ASSERT_RESULT(Ok, b.d.showMenu("Main Menu", "Voltage:0V", "Boost:0mbar", 0x0B));
  b.settle();
  ASSERT_RESULT(Ok, b.d.lastResult());

  const ScreenModel& s = b.twin.screen();
  TEST_ASSERT_TRUE(s.mode == ScreenModel::Mode::Menu);
  TEST_ASSERT_EQUAL_STRING("Main Menu", s.header);
  TEST_ASSERT_EQUAL_STRING("Voltage:0V", s.row0);
  TEST_ASSERT_EQUAL_STRING("Boost:0mbar", s.row1);
  TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x0B, s.scroll, "down-arrow only");
  TEST_ASSERT_EQUAL_HEX8(0x7E, s.row0Id);
  TEST_ASSERT_EQUAL_HEX8(0x7F, s.row1Id);
  TEST_ASSERT_EQUAL_INT16_MESSAGE(-1, s.sel, "a fresh screen clears the highlight");

  // The highlight is a DIFFERENT single frame, and it carries the row TAG rather than an
  // index because that is what `07 29 01 <row>` puts on the wire.
  ASSERT_RESULT(Ok, b.d.highlightItem(1));
  b.settle();
  TEST_ASSERT_EQUAL_INT16_MESSAGE(0x7F, b.twin.screen().sel, "the bottom row is selected");
  TEST_ASSERT_EQUAL_STRING_MESSAGE("Main Menu", b.twin.screen().header,
                                   "and the screen underneath is untouched");
}

void test_a_real_menu_render_reaches_the_twin_as_the_application_wrote_it(void) {
  // The whole stack at once: the application fills a Menu, a key opens it, and the TWIN is
  // asked what the panel is showing.
  CarBench b;
  b.begin();
  b.run(2);
  ASSERT_RESULT(Ok, b.d.setPower(true));
  b.settle();

  static const char* const kModes[] = {"Off", "On"};
  MenuItem a{};
  a.label      = "Bright";
  a.fields[0]  = integerField(50, 0, 100, 5, 10, "%");
  a.fieldCount = 1;
  b.d.getMenu().addItem(a);

  MenuItem c{};
  c.label      = "Lights";
  c.fields[0]  = listField(kModes, 2, 1);
  c.fieldCount = 1;
  b.d.getMenu().addItem(c);

  ASSERT_RESULT(Ok, b.d.nav(NavCommand::Open));
  b.settle();

  const ScreenModel& s = b.twin.screen();
  TEST_ASSERT_EQUAL_STRING("Main Menu", s.header);
  TEST_ASSERT_EQUAL_STRING("Bright: 50%", s.row0);
  TEST_ASSERT_EQUAL_STRING("Lights: On", s.row1);
  TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00, s.scroll, "two items: no arrows");
  TEST_ASSERT_EQUAL_INT16_MESSAGE(0x7E, s.sel, "the top row is highlighted");

  // Move the selection: one frame, and only the highlight changes.
  ASSERT_RESULT(Ok, b.d.nav(NavCommand::Next));
  b.settle();
  TEST_ASSERT_EQUAL_INT16(0x7F, b.twin.screen().sel);
  TEST_ASSERT_EQUAL_STRING_MESSAGE("Bright: 50%", b.twin.screen().row0,
                                   "a highlight move does not redraw the rows");
}

// ---------------------------------------------------------------------------
// A key pressed ON THE TWIN
// ---------------------------------------------------------------------------

void test_a_key_pressed_on_the_twin_arrives_as_an_affa_Key(void) {
  CarBench b;
  b.begin();
  b.run(2);
  b.d.onKey(&countKey, nullptr);

  g_keys = 0;
  TEST_ASSERT_TRUE(b.twin.transmitKey(static_cast<uint16_t>(Key::Pause), false));
  b.run(2);
  TEST_ASSERT_EQUAL_INT(1, g_keys);
  TEST_ASSERT_EQUAL_HEX16(static_cast<uint16_t>(Key::Pause), static_cast<uint16_t>(g_key));
  TEST_ASSERT_TRUE(g_edge == KeyEdge::Click);

  // A held non-wheel key.
  g_keys = 0;
  TEST_ASSERT_TRUE(b.twin.transmitKey(static_cast<uint16_t>(Key::Load), true));
  b.run(2);
  TEST_ASSERT_EQUAL_INT(1, g_keys);
  TEST_ASSERT_EQUAL_HEX16(static_cast<uint16_t>(Key::Load), static_cast<uint16_t>(g_key));
  TEST_ASSERT_TRUE(g_edge == KeyEdge::Hold);

  // The detents are EXEMPT from hold masking on the encoder side too, so a "held" detent
  // transmitted by the twin is byte-identical to a clicked one and arrives as a click.
  g_keys = 0;
  TEST_ASSERT_TRUE(b.twin.transmitKey(static_cast<uint16_t>(Key::RollDown), true));
  b.run(2);
  TEST_ASSERT_EQUAL_INT(1, g_keys);
  TEST_ASSERT_EQUAL_HEX16_MESSAGE(static_cast<uint16_t>(Key::RollDown),
                                  static_cast<uint16_t>(g_key),
                                  "masking would have reported a wheel-DOWN as a wheel-UP");
  TEST_ASSERT_TRUE_MESSAGE(g_edge == KeyEdge::Click, "a held detent has no wire form");
}

// ---------------------------------------------------------------------------
// The ACK model is not an implementation detail
// ---------------------------------------------------------------------------

void test_ack_mode_Done_cannot_model_a_panel(void) {
  // Kept as an executable warning rather than a bug report. With AckMode::Done — which is
  // the API.md default — the twin answers DONE to frame 0, the transmit FSM correctly
  // reports SUCCESS, and the twin has seen 8 bytes of a 96-byte screen. Any test that used
  // the default and asserted on screen() would be asserting on nothing.
  CarBench b;
  b.begin();
  b.run(2);
  ASSERT_RESULT(Ok, b.d.setPower(true));
  b.settle();

  b.twin.setAckMode(VirtualPanelBase::AckMode::Done);
  ASSERT_RESULT(Ok, b.d.showMenu("Main Menu", "Voltage:0V", "Boost:0mbar", 0x0B));
  b.settle();
  ASSERT_RESULT(Ok, b.d.lastResult());
  TEST_ASSERT_TRUE_MESSAGE(b.twin.screen().mode == ScreenModel::Mode::None,
                           "8 of 96 bytes is not a screen, and must not decode as one");

  // Declared reproduces the hardware frame counts without being told them.
  b.twin.setAckMode(VirtualPanelBase::AckMode::Declared);
  ASSERT_RESULT(Ok, b.d.showMenu("Main Menu", "Voltage:0V", "Boost:0mbar", 0x0B));
  b.settle();
  TEST_ASSERT_EQUAL_STRING("Main Menu", b.twin.screen().header);
}

void test_a_passive_twin_decodes_without_transmitting(void) {
  // PASSIVE is how you run a twin ALONGSIDE a real panel on a live bus to see what the
  // panel sees. Two ACKers on one bus is the failure this switch prevents.
  CarBench b;
  b.begin();
  b.run(2);
  ASSERT_RESULT(Ok, b.d.setPower(true));
  b.settle();

  const uint32_t acksBefore = b.twin.acksSent();
  b.twin.setEmulate(false);
  b.d.setSelfAck(true);                 // something has to drive the transfer out
  ASSERT_RESULT(Ok, b.d.setText("QUIET"));
  b.settle();

  TEST_ASSERT_EQUAL_UINT32_MESSAGE(acksBefore, b.twin.acksSent(),
                                   "a passive twin transmits nothing at all");
  TEST_ASSERT_EQUAL_STRING_MESSAGE("QUIET", b.twin.screen().header,
                                   "and still decodes the screen");
}

void test_a_resync_clears_everything_the_panel_latches(void) {
  // A resync means the panel was power-cycled or lost the link, so the screen, the
  // half-assembled transfer and the differential icon latch are ALL unknown.
  CarBench b;
  b.begin();
  b.run(2);
  ASSERT_RESULT(Ok, b.d.setPower(true));
  b.settle();
  ASSERT_RESULT(Ok, b.d.setText("HELLO"));
  b.settle();
  TEST_ASSERT_EQUAL_STRING("HELLO", b.twin.screen().header);
  TEST_ASSERT_FALSE_MESSAGE(b.twin.iconsKnown(),
                            "Carminat never sends a 0x7F, so the latch stays unknown");

  // Power-cycle the panel. begin() resets the MODEL but not the profile, because a panel
  // that was just powered on has an empty screen and no latched icons.
  b.twin.begin(b.panelSide, b.clk);
  b.twin.setAckMode(VirtualPanelBase::AckMode::Declared);
  TEST_ASSERT_TRUE(b.twin.screen().mode == ScreenModel::Mode::None);
  TEST_ASSERT_FALSE(b.twin.synced());

  // WORTH KNOWING, and this is where you find it out: resetting the twin's model does NOT
  // by itself tell the radio anything. The twin still answers the 0xB9 heartbeat with its
  // 0x69 ping, so from the driver's side the link is perfectly healthy while the panel has
  // in fact forgotten everything. The handshake only restarts when the panel goes quiet
  // (below) or asks with `61 11` — which a real panel does on power-up and this twin, being
  // fed frames rather than polled, cannot initiate.
  b.twin.setEmulate(false);
  b.starve();
  TEST_ASSERT_FALSE_MESSAGE(b.d.synced(), "silence is what the driver actually reacts to");
  TEST_ASSERT_FALSE_MESSAGE(b.d.registered(), "and FUNCSREG is dropped with it");

  // The panel comes back. FAILED re-arms the 0xBA request, the twin answers `61 11`, the
  // driver announces, and the twin's 0x70 edge brings it back up — with no application
  // involvement anywhere.
  b.twin.setEmulate(true);
  for (int i = 0; i < 4; ++i) {
    b.clk.advance(AFFA_SYNC_INTERVAL_MS);
    b.run(3);
  }
  TEST_ASSERT_TRUE_MESSAGE(b.d.synced(), "the handshake restarts by itself");
  TEST_ASSERT_TRUE_MESSAGE(b.twin.synced(), "and the twin sees the announce again");
  TEST_ASSERT_TRUE_MESSAGE(b.twin.screen().mode == ScreenModel::Mode::None,
                           "nothing is latched across a resync");
  TEST_ASSERT_FALSE(b.twin.iconsKnown());

  // And the first send after the resync registers the functions again, because the panel
  // forgot us too.
  TEST_ASSERT_FALSE(b.d.registered());
  ASSERT_RESULT(Ok, b.d.setText("BACK"));
  b.settle();
  TEST_ASSERT_TRUE(b.d.registered());
  TEST_ASSERT_EQUAL_STRING("BACK", b.twin.screen().header);
}

// ---------------------------------------------------------------------------
// The other family, through its own twin
// ---------------------------------------------------------------------------

void test_the_updatelist_segment_twin_decodes_both_text_fields(void) {
  SegBench b;
  b.begin();
  b.run(2);
  TEST_ASSERT_TRUE_MESSAGE(b.d.synced(), "0x3DF / 0x3CF, one hello frame");
  TEST_ASSERT_TRUE(b.twin.synced());

  ASSERT_RESULT(Ok, b.d.setPower(true));
  b.settle();
  TEST_ASSERT_TRUE_MESSAGE(b.d.registered(), "{0x121, 0x1B1} both acknowledged");

  ASSERT_RESULT(Ok, b.d.setText("AUX", 255));
  b.settle();
  ASSERT_RESULT(Ok, b.d.lastResult());

  // `new` is what the panel shows -> header; `old` -> row0. Both fields carry the same
  // string in this encoding, which is what the capture shows.
  TEST_ASSERT_EQUAL_STRING("AUX", b.twin.screen().header);
  TEST_ASSERT_EQUAL_STRING("AUX", b.twin.screen().row0);

  // Our 8-segment driver only ever emits the 0x76 text-only form, so the icon latch stays
  // unknown — which is the honest answer, not "no icons".
  TEST_ASSERT_FALSE_MESSAGE(b.twin.iconsKnown(),
                            "0x76 carries no icon header; only 0x7F does");
}

void test_a_key_from_the_updatelist_twin_uses_the_0x0A9_channel(void) {
  SegBench b;
  b.begin();
  b.run(2);
  b.d.onKey(&countKey, nullptr);
  b.d.onFrame(&tapTx, nullptr);

  g_keys = 0;
  g_txN  = 0;
  TEST_ASSERT_TRUE(b.twin.transmitKey(static_cast<uint16_t>(Key::VolUp), false));
  b.run(2);
  TEST_ASSERT_EQUAL_INT(1, g_keys);
  TEST_ASSERT_EQUAL_HEX16(static_cast<uint16_t>(Key::VolUp), static_cast<uint16_t>(g_key));

  // And the ACK the driver owes it goes out on the COMPUTED id. 0x0A9 is the one entry in
  // either family's table with bit 8 already clear, so the "leading digit + 4" shortcut
  // that works everywhere else would put this on 0x5A9, where nothing is listening.
  TEST_ASSERT_TRUE_MESSAGE(sawTx(0x4A9, kAckDone), "0x0A9 | 0x400 is 0x4A9, not 0x5A9");
  TEST_ASSERT_FALSE_MESSAGE(sawTx(0x5A9, kAckDone), "and 0x5A9 must never appear");
}

// ---------------------------------------------------------------------------

void setUp(void) {}
void tearDown(void) {}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_the_handshake_completes_to_registered_against_the_twin);
  RUN_TEST(test_the_twins_ping_holds_the_peer_watchdog_open);
  RUN_TEST(test_setText_shows_up_in_the_twins_screen_model);
  RUN_TEST(test_showMenu_decodes_to_three_strings_a_scroll_byte_and_a_highlight);
  RUN_TEST(test_a_real_menu_render_reaches_the_twin_as_the_application_wrote_it);
  RUN_TEST(test_a_key_pressed_on_the_twin_arrives_as_an_affa_Key);
  RUN_TEST(test_ack_mode_Done_cannot_model_a_panel);
  RUN_TEST(test_a_passive_twin_decodes_without_transmitting);
  RUN_TEST(test_a_resync_clears_everything_the_panel_latches);
  RUN_TEST(test_the_updatelist_segment_twin_decodes_both_text_fields);
  RUN_TEST(test_a_key_from_the_updatelist_twin_uses_the_0x0A9_channel);
  return UNITY_END();
}
