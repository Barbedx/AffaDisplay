// UpdateList (AFFA2) wire vectors: both setText encodings, setPower's `04 52` form, the
// 0x81 filler everywhere, and the identifier arithmetic that is wrong by eye —
// 0x0A9 | 0x400 == 0x4A9, NOT 0x5A9.
//
// The 0x81 filler is asserted on OUR OWN transmissions only. Nothing here matches a
// RECEIVED filler byte, and nothing anywhere may: it is per-node (our bench panel pads
// 0xA3, an OEM cluster 0x84, the OEM radio 0xFF), which is why every injected frame below
// pads with 0xA3 on purpose.

#include "../affa_test_support.h"

#include "updatelist/UpdateListDisplay.h"
#include "updatelist/UpdateListMenuDisplay.h"
#include "updatelist/UpdateListConstants.h"

using namespace affa;
using affatest::mk;
using affatest::drain;
using affatest::expectFrames;
using affatest::pump;
using affatest::pumpUntilIdle;

namespace {

// ---------------------------------------------------------------------------
// Golden vectors — docs/WIRE-SPEC.md §9
// ---------------------------------------------------------------------------

const Frame kRegister[] = {
    {0x121, 8, {0x70, 0x81, 0x81, 0x81, 0x81, 0x81, 0x81, 0x81}, false},
    {0x1B1, 8, {0x70, 0x81, 0x81, 0x81, 0x81, 0x81, 0x81, 0x81}, false},
};

const Frame kHello[] = {
    {0x3DF, 8, {0x70, 0x1A, 0x11, 0x00, 0x00, 0x00, 0x00, 0x01}, false},
};

const Frame kAlive =
    {0x3DF, 8, {0x79, 0x00, 0x81, 0x81, 0x81, 0x81, 0x81, 0x81}, false};
const Frame kSyncRequest =
    {0x3DF, 8, {0x7A, 0x01, 0x81, 0x81, 0x81, 0x81, 0x81, 0x81}, false};

// setText("AUX", 255): digit > 9 -> chan 0x7A. Four frames, exactly full, last PCI 0x23.
const Frame kSegAux[] = {
    {0x121, 8, {0x10, 0x19, 0x76, 0x7A, 0x01, 0x41, 0x55, 0x58}, false},
    {0x121, 8, {0x21, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x41}, false},
    {0x121, 8, {0x22, 0x55, 0x58, 0x00, 0x00, 0x00, 0x00, 0x00}, false},
    {0x121, 8, {0x23, 0x00, 0x00, 0x00, 0x00, 0x00, 0x81, 0x81}, false},
};

// setText("HELLO", 3): only data[3] changes, to 0x70 + 3.
const Frame kSegHelloChan3[] = {
    {0x121, 8, {0x10, 0x19, 0x76, 0x73, 0x01, 'H', 'E', 'L'}, false},
    {0x121, 8, {0x21, 'L', 'O', 0x00, 0x00, 0x00, 0x10, 'H'}, false},
    {0x121, 8, {0x22, 'E', 'L', 'L', 'O', 0x00, 0x00, 0x00}, false},
    {0x121, 8, {0x23, 0x00, 0x00, 0x00, 0x00, 0x00, 0x81, 0x81}, false},
};

// LCD setText("MENU"): the 0x7F text-plus-icons form. Five frames, last PCI 0x24 carrying
// one payload byte and six 0x81 of TRANSPORT filler.
const Frame kLcdMenu[] = {
    {0x121, 8, {0x10, 0x1C, 0x7F, 0x55, 0x55, 0xFF, 0x60, 0x03}, false},
    {0x121, 8, {0x21, 0x4D, 0x45, 0x4E, 0x55, 0x00, 0x00, 0x00}, false},
    {0x121, 8, {0x22, 0x00, 0x10, 0x4D, 0x45, 0x4E, 0x55, 0x00}, false},
    {0x121, 8, {0x23, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, false},
    {0x121, 8, {0x24, 0x00, 0x81, 0x81, 0x81, 0x81, 0x81, 0x81}, false},
};

const Frame kSetStateEnable[] = {
    {0x1B1, 8, {0x04, 0x52, 0x02, 0xFF, 0xFF, 0x81, 0x81, 0x81}, false},
};
const Frame kSetStateDisable[] = {
    {0x1B1, 8, {0x04, 0x52, 0x00, 0xFF, 0xFF, 0x81, 0x81, 0x81}, false},
};

const Frame kAckToKeyId[] = {
    {0x4A9, 8, {0x74, 0x81, 0x81, 0x81, 0x81, 0x81, 0x81, 0x81}, false},
};

// ---------------------------------------------------------------------------
// Rigs
// ---------------------------------------------------------------------------

template <class Panel>
struct Rig {
  LoopbackLink<256> link;
  affatest::FakeClock clk;
  Panel d;

  Rig() : d(link, clk) {}

  // THE OPENING, AS THIS FAMILY NOW RUNS IT. Two frames from the panel, not one.
  //
  // The request draws the single hello frame immediately — that is this family's one
  // remaining difference from Carminat (SyncProfile::helloRequiresAnnounce is false, the
  // reference answered from inside recv()). But registration is no longer lazy and no
  // longer ours to start whenever we like: the panel opens its OWN channel first, `0A9 70`,
  // and only that unlocks our `121`/`1B1` probes.
  //
  // A rig that injects only the request is not modelling this panel any more. It was not
  // modelling the Carminat one either, before the same gate landed there and every rig
  // built on the old helper stalled — which is exactly the failure a real panel would show.
  void sync() {
    d.begin();
    // The panel pads with 0xA3. We never look at it, and this injection exists to make
    // sure of that.
    link.inject(affatest::panelSyncRequest());
    d.poll();
    TEST_ASSERT_TRUE(d.synced());
  }

  // The panel's own channel registration. [REF] the reference driver acknowledged `0A9 70`
  // and gated nothing on it; we gate on it, because the one real OEM bus using these
  // function ids registers in that order.
  void peerChannel() {
    link.inject(mk(0x0A9, {0x70, 0xA3, 0xA3, 0xA3, 0xA3, 0xA3, 0xA3, 0xA3}));
    d.poll();
  }

  void up() {
    d.begin();
    d.setSelfAck(true);          // armed BEFORE the opening: the 0x70 probes now leave
                                 // during it, and a self-ACK latched after the frame has
                                 // been offered never arrives
    link.inject(affatest::panelSyncRequest());
    d.poll();
    TEST_ASSERT_TRUE(d.synced());
    peerChannel();
    pumpUntilIdle(d);
    TEST_ASSERT_TRUE(d.registered());
    drain(link);
  }
};

using SegRig = Rig<UpdateListDisplay>;
using LcdRig = Rig<UpdateListMenuDisplay>;

int g_keys = 0;
Key g_lastKey = Key::Load;
KeyEdge g_lastEdge = KeyEdge::Click;
void countKey(Key k, KeyEdge e, void*) { ++g_keys; g_lastKey = k; g_lastEdge = e; }

}  // namespace

// ---------------------------------------------------------------------------
// Identifier arithmetic
// ---------------------------------------------------------------------------

void test_key_ack_id_is_computed_not_tabulated(void) {
  // 0x0A9 is the ONE identifier in either family's table with bit 8 already clear, so the
  // "add 0x400 to the leading digit" shortcut that works everywhere else is wrong here.
  TEST_ASSERT_EQUAL_HEX16_MESSAGE(0x4A9, updatelist::kIdKeyPressed | kReplyFlag,
                                  "0x0A9 | 0x400 is 0x4A9, not 0x5A9");
  TEST_ASSERT_EQUAL_HEX16(0x4A9, updatelist::kAckIdKeyPressed);

  // And the base must actually put it there. A hard-coded 0x5A9 would be answered by
  // nothing at all, silently.
  SegRig r;
  r.up();
  r.link.inject(mk(0x0A9, {0x70, 0xA3, 0xA3, 0xA3, 0xA3, 0xA3, 0xA3, 0xA3}));
  r.d.poll();
  expectFrames(r.link, kAckToKeyId, 1, "RX 0x0A9 -> TX 0x4A9 74 + 0x81 filler");
}

// ---------------------------------------------------------------------------
// Sync + registration
// ---------------------------------------------------------------------------

void test_updatelist_sync_frames_are_byte_exact(void) {
  // THE BYTES ARE UNCHANGED AND THE TIMING IS NOT, which is the whole of what moving this
  // family onto the shared rules did. This test used to open with
  //
  //     begin(); poll();  ->  79 00 81.. then 7A 01 81..   on the very first pass
  //
  // because the profile had no authorization gate and treated FAILED as permission to
  // transmit. That is the reference driver's every-second probe into an empty bus, which is
  // precisely the storm `waitForPanel` was added to kill on the other family. A panel-led
  // opening starts silent.
  SegRig r;
  r.d.begin();
  r.d.poll();
  Frame f;
  TEST_ASSERT_FALSE_MESSAGE(r.link.takeSent(f),
                            "a panel-led opening says NOTHING before the panel does");

  // Silence on both sides is a deadlock, so the announce still comes — slowly, and it is a
  // bare request with no alive in front of it, exactly as on Carminat.
  r.clk.advance(updatelist::kSync.announceWhenSilentMs);
  r.d.poll();
  affatest::expectFrame(r.link, kSyncRequest,
                        "UpdateList request: 7A 01 — a REAL argument, not filler");
  TEST_ASSERT_FALSE_MESSAGE(r.link.takeSent(f), "and no 79 rides in front of it");

  // The panel's request draws the hello IMMEDIATELY — the one thing this family still does
  // differently. Carminat's burst answers the panel's SECOND ask; this one answers the
  // first, as its reference did from inside recv().
  r.link.inject(affatest::panelSyncRequest());
  r.d.poll();
  expectFrames(r.link, kHello, 1, "UpdateList hello is a single frame, on the FIRST request");

  // And the 1 Hz alive is still byte-exact when it finally starts — after registration,
  // like Carminat's, rather than from boot.
  r.d.setSelfAck(true);
  r.peerChannel();
  pumpUntilIdle(r.d);
  TEST_ASSERT_TRUE_MESSAGE(r.d.registered(), "the opening registers without a render");
  drain(r.link);
  r.clk.advance(AFFA_SYNC_INTERVAL_MS);
  r.d.poll();
  affatest::expectFrame(r.link, kAlive, "UpdateList 1 Hz alive: 79 00 81 x6");
}

void test_registration_walks_the_function_table_with_0x81_filler(void) {
  // SAME TWO FRAMES, SAME ORDER, SAME FILLER — AND NO LONGER TRIGGERED BY A RENDER.
  //
  // This used to read `sync(); setPower(true); pumpUntilIdle()` and assert
  // {0x121, 0x1B1, payload}, because registration was lazy: the first render dragged it
  // along. It is part of the opening now, so a build that never renders still registers —
  // and a panel that never opens its own `0A9` channel correctly gets nothing.
  SegRig r;
  r.d.begin();
  r.d.setSelfAck(true);
  r.link.inject(affatest::panelSyncRequest());
  r.d.poll();
  drain(r.link);                      // the hello

  Frame f;
  TEST_ASSERT_FALSE_MESSAGE(r.link.takeSent(f),
                            "no 0x70 probe may precede the panel's own 0A9 registration");
  TEST_ASSERT_FALSE(r.d.registered());

  r.peerChannel();
  pumpUntilIdle(r.d);
  // OUR REFLEX FIRST, THEN OUR PROBES — the same order Carminat shows, where the `5C1 74`
  // that answers the panel's `1C1 70` precedes our `151 70` by ~61 ms. The reflex leaves
  // from the receive pump, so it is always ahead of anything the sync pump schedules.
  affatest::expectFrame(r.link, kAckToKeyId[0],
                        "the 4A9 74 reflex answers the panel's own channel");
  affatest::expectFrame(r.link, kRegister[0], "registration walks 0x121 first...");
  affatest::expectFrame(r.link, kRegister[1], "...then 0x1B1, both with 0x81 filler");
  TEST_ASSERT_TRUE_MESSAGE(r.d.registered(),
                           "the opening completes registration with no application help");

  // AND THEN THE LIBRARY LIGHTS THE GLASS ITSELF — this family's `04 52 02 FF FF`, not
  // Carminat's `03 52 09`, from the same rule and the same place in the opening. No
  // application asked for it, which is the point: a panel that is not on ACKs a screen it
  // never lights, and this build never calls setPower() at all.
  expectFrames(r.link, kSetStateEnable, 1, "the opening ends by powering the panel on");
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(static_cast<uint8_t>(affa::Phase::Ready),
                                  static_cast<uint8_t>(r.d.phase()),
                                  "and only then is the phase Ready");
}

// ---------------------------------------------------------------------------
// setPower
// ---------------------------------------------------------------------------

void test_setPower_declares_0x04_and_pads_with_0x81(void) {
  // `04 52 <state> FF FF` — the self-consistent length byte of the two families. Carminat
  // spells the same command `03 52 …`. DO NOT UNIFY THEM.
  SegRig r;
  r.up();
  ASSERT_RESULT(Ok, r.d.setPower(true));
  pumpUntilIdle(r.d);
  expectFrames(r.link, kSetStateEnable, 1, "setPower(true) on 0x1B1");

  ASSERT_RESULT(Ok, r.d.setPower(false));
  pumpUntilIdle(r.d);
  expectFrames(r.link, kSetStateDisable, 1, "setPower(false) on 0x1B1");
}

// ---------------------------------------------------------------------------
// setText — both encodings
// ---------------------------------------------------------------------------

void test_segment_setText_is_four_frames(void) {
  SegRig r;
  r.up();
  ASSERT_RESULT(Ok, r.d.setText("AUX", 255));
  pumpUntilIdle(r.d);
  expectFrames(r.link, kSegAux, 4, "segment setText(\"AUX\") — 29 bytes, last PCI 0x23");
}

void test_segment_setText_channel_byte_follows_the_digit(void) {
  SegRig r;
  r.up();
  ASSERT_RESULT(Ok, r.d.setText("HELLO", 3));
  pumpUntilIdle(r.d);
  expectFrames(r.link, kSegHelloChan3, 4, "digit 3 -> chan 0x73");

  // digit 0 -> 0x70, digit 9 -> 0x79, anything above -> 0x7A.
  const uint8_t kDigits[] = {0, 9, 10, 255};
  const uint8_t kChan[]   = {0x70, 0x79, 0x7A, 0x7A};
  for (uint8_t i = 0; i < 4; ++i) {
    (void)r.d.setText("X", kDigits[i]);
    pumpUntilIdle(r.d);
    Frame f;
    TEST_ASSERT_TRUE(r.link.takeSent(f));
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(kChan[i], f.data[3], "channel byte");
    drain(r.link);
  }
}

void test_segment_setText_pads_both_fields_with_NUL(void) {
  // NUL, not space. Every capture and both golden vectors show 0x00 here; finding #9's
  // "emit the OEM space form" is specific to Carminat's showInfoMenu.
  SegRig r;
  r.up();
  (void)r.d.setText("AUX", 255);
  pumpUntilIdle(r.d);
  Frame f0, f1;
  TEST_ASSERT_TRUE(r.link.takeSent(f0));
  TEST_ASSERT_TRUE(r.link.takeSent(f1));
  TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00, f1.data[1], "old text field is NUL-padded");
  drain(r.link);
}

void test_lcd_setText_is_five_frames(void) {
  LcdRig r;
  r.up();
  ASSERT_RESULT(Ok, r.d.setText("MENU"));
  pumpUntilIdle(r.d);
  expectFrames(r.link, kLcdMenu, 5, "LCD setText(\"MENU\") — 30 bytes, last PCI 0x24");
}

// The two panels show DIFFERENT NUMBERS OF CELLS, so they must not scroll the same window.
// Both encodings carry an 8-cell "old" and a 12-cell "new" field and the panel renders
// `new`; the segment glass is 8 characters wide, the LCD renders all 12. Inheriting the
// segment's 8-wide marquee would leave four LCD cells permanently blank — visible on the
// glass and invisible in a frame count, which is why it is asserted on the TEXT.
void test_the_lcd_scrolls_a_wider_window_than_the_segment_panel(void) {
  // 20 characters, so both windows are full and neither has wrapped yet at position 0.
  const char* kLong = "ABCDEFGHIJKLMNOPQRST";

  SegRig s;
  s.up();
  drain(s.link);
  s.d.setScrollText(kLong);
  s.d.setScrollActive(true);
  s.d.poll();
  pumpUntilIdle(s.d);
  Frame sf0, sf1, sf2, sf3;
  TEST_ASSERT_TRUE(s.link.takeSent(sf0));
  TEST_ASSERT_TRUE(s.link.takeSent(sf1));
  TEST_ASSERT_TRUE(s.link.takeSent(sf2));
  TEST_ASSERT_TRUE(s.link.takeSent(sf3));
  // Segment new text is payload[14..25]. Frame 0 carries payload[0..7] and each
  // continuation seven more, so payload[14] is frame 1 (PCI 0x21) data[7].
  TEST_ASSERT_EQUAL_HEX8_MESSAGE('A', sf1.data[7], "segment new-text field starts at 'A'");
  // Cell 8 is payload[22] = frame 3 (PCI 0x23) data[1]. The 8-wide window has run out, so
  // this is the NUL padding — the very thing the LCD must NOT have.
  TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00, sf3.data[1],
      "segment scrolls 8 cells, so cell 9 is padding");

  LcdRig l;
  l.up();
  drain(l.link);
  l.d.setScrollText(kLong);
  l.d.setScrollActive(true);
  l.d.poll();
  pumpUntilIdle(l.d);
  Frame lf0, lf1, lf2, lf3;
  TEST_ASSERT_TRUE(l.link.takeSent(lf0));
  TEST_ASSERT_TRUE(l.link.takeSent(lf1));
  TEST_ASSERT_TRUE(l.link.takeSent(lf2));
  TEST_ASSERT_TRUE(l.link.takeSent(lf3));
  // LCD new text is payload[17..28]; payload[17] is frame 2 (PCI 0x22) data[3].
  TEST_ASSERT_EQUAL_HEX8_MESSAGE('A', lf2.data[3], "LCD new-text field starts at 'A'");
  // Cell 8 is payload[25] = frame 3 (PCI 0x23) data[4]. THE ASSERTION THAT MATTERS: the
  // LCD is still emitting real characters exactly where the segment panel had padding.
  TEST_ASSERT_EQUAL_HEX8_MESSAGE('I', lf3.data[4],
      "LCD must fill all 12 cells — an 8-wide window would leave NUL here");
}

void test_the_two_encodings_declare_different_lengths(void) {
  // 0x19 covers data[2..26], 0x1C covers data[2..29]. It is NOT "0x19 + 3" by coincidence:
  // the two encodings carry different amounts of content, and both length bytes are right.
  TEST_ASSERT_EQUAL_HEX8(0x19, updatelist::kSegFfDl);
  TEST_ASSERT_EQUAL_HEX8(0x1C, updatelist::kLcdFfDl);
  TEST_ASSERT_EQUAL_UINT8(29, updatelist::kSegPayload);
  TEST_ASSERT_EQUAL_UINT8(30, updatelist::kLcdPayload);
}

// ---------------------------------------------------------------------------
// Key channel
// ---------------------------------------------------------------------------

void test_a_malformed_key_frame_is_not_acknowledged(void) {
  // The one family quirk on top of the base's suppression list: `03 <not 89>` gets
  // silence. Everything else on 0x0A9 — including the panel's own 0x70 probe — is ACKed.
  SegRig r;
  r.up();
  r.d.onKey(&countKey, nullptr);
  g_keys = 0;

  r.link.inject(mk(0x0A9, {0x03, 0x88, 0x00, 0x00, 0xA3, 0xA3, 0xA3, 0xA3}));
  r.d.poll();
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, r.link.sentCount(), "no ACK for `03 88`");
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, g_keys, "and no phantom key either");

  // A WELL-FORMED key frame is both acknowledged and delivered.
  r.link.inject(mk(0x0A9, {0x03, 0x89, 0x00, 0x05, 0xA3, 0xA3, 0xA3, 0xA3}));
  r.d.poll();
  expectFrames(r.link, kAckToKeyId, 1, "a valid key frame IS acknowledged, on 0x4A9");
  TEST_ASSERT_EQUAL_INT(1, g_keys);
  TEST_ASSERT_EQUAL_HEX16(0x0005, static_cast<uint16_t>(g_lastKey));
}

// ---------------------------------------------------------------------------
// The AMS banner: the delay(100) loop, unwound
// ---------------------------------------------------------------------------

void test_ams_banner_is_three_renders_100ms_apart(void) {
  // Legacy: `for (i = 0; i < 3; i++) { setText(msg); delay(100); }`. Same three renders,
  // same spacing, no delay — a repeat count plus a deadline advanced from onPoll().
  SegRig r;
  r.up();
  r.d.onKey(&countKey, nullptr);
  g_keys = 0;

  TEST_ASSERT_TRUE(r.d.amsKeysEnabled());
  ASSERT_RESULT(Ok, r.d.pressKey(Key::Load, KeyEdge::Hold, KeySource::Local));
  TEST_ASSERT_FALSE_MESSAGE(r.d.amsKeysEnabled(), "hold-Load toggles AMS forwarding");
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, g_keys, "the gesture belongs to the panel, not the app");

  int banners = 0;
  for (int step = 0; step < 6; ++step) {
    pump(r.d, 24);                       // more than enough to drain one 4-frame render
    Frame f;
    while (r.link.takeSent(f)) {
      if (f.data[0] == 0x10 && f.data[2] == 0x76) ++banners;
    }
    r.clk.advance(100);
  }
  TEST_ASSERT_EQUAL_INT_MESSAGE(3, banners, "exactly three banner renders, 100 ms apart");
}

void test_ams_disabled_suppresses_the_fall_through(void) {
  // The extracted semantic, and the whole point of the switch. Layer 0 and Layer 1 still
  // see the raw frames; only the decoded fall-through is suppressed.
  SegRig r;
  r.up();
  r.d.onKey(&countKey, nullptr);

  g_keys = 0;
  r.link.inject(mk(0x0A9, {0x03, 0x89, 0x00, 0x05, 0xA3, 0xA3, 0xA3, 0xA3}));
  r.d.poll();
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, g_keys, "forwarding is ON by default");

  r.d.setAmsKeysEnabled(false);
  g_keys = 0;
  r.link.inject(mk(0x0A9, {0x03, 0x89, 0x00, 0x05, 0xA3, 0xA3, 0xA3, 0xA3}));
  r.d.poll();
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, g_keys, "disabled means the key does not reach KeyCb");
}

// ---------------------------------------------------------------------------
// Capability honesty
// ---------------------------------------------------------------------------

void test_unsupported_operations_report_rather_than_no_op(void) {
  // The extracted setTime()/showMenu() returned NoError while putting nothing on the wire.
  SegRig r;
  r.up();
  ASSERT_RESULT(NotSupported, r.d.setTime("1234"));
  ASSERT_RESULT(NotSupported, r.d.showMenu("a", "b", "c", 0x0B));
  ASSERT_RESULT(NotSupported, r.d.showPopupText("x", 0x09, 0xFF, 0x60));
  TEST_ASSERT_FALSE(r.d.supports(Feature::Time));
  TEST_ASSERT_FALSE(r.d.supports(Feature::Menu));
  TEST_ASSERT_TRUE(r.d.supports(Feature::Text));
  TEST_ASSERT_TRUE(r.d.supports(Feature::Power));
  TEST_ASSERT_TRUE(r.d.supports(Feature::KeyTx));
  pumpUntilIdle(r.d);
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, r.link.sentCount(), "a refusal sends nothing");
}

// ---------------------------------------------------------------------------

void setUp(void) {}
void tearDown(void) {}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_key_ack_id_is_computed_not_tabulated);
  RUN_TEST(test_updatelist_sync_frames_are_byte_exact);
  RUN_TEST(test_registration_walks_the_function_table_with_0x81_filler);
  RUN_TEST(test_setPower_declares_0x04_and_pads_with_0x81);
  RUN_TEST(test_segment_setText_is_four_frames);
  RUN_TEST(test_segment_setText_channel_byte_follows_the_digit);
  RUN_TEST(test_segment_setText_pads_both_fields_with_NUL);
  RUN_TEST(test_lcd_setText_is_five_frames);
  RUN_TEST(test_the_lcd_scrolls_a_wider_window_than_the_segment_panel);
  RUN_TEST(test_the_two_encodings_declare_different_lengths);
  RUN_TEST(test_a_malformed_key_frame_is_not_acknowledged);
  RUN_TEST(test_ams_banner_is_three_renders_100ms_apart);
  RUN_TEST(test_ams_disabled_suppresses_the_fall_through);
  RUN_TEST(test_unsupported_operations_report_rather_than_no_op);
  return UNITY_END();
}
