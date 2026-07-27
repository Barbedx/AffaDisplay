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

  void sync() {
    d.begin();
    // The panel pads with 0xA3. We never look at it, and this injection exists to make
    // sure of that.
    link.inject(affatest::panelSyncRequest());
    d.poll();
    TEST_ASSERT_TRUE(d.synced());
  }

  void up() {
    sync();
    d.setSelfAck(true);
    ASSERT_RESULT(Ok, d.setPower(true));
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
  SegRig r;
  r.d.begin();
  r.d.poll();                     // FAILED: heartbeat, then the sync request
  affatest::expectFrame(r.link, kAlive, "UpdateList 1 Hz alive: 79 00 81 x6");
  affatest::expectFrame(r.link, kSyncRequest, "UpdateList request: 7A 01 — a REAL argument");
  Frame f;
  TEST_ASSERT_FALSE_MESSAGE(r.link.takeSent(f), "nothing else leaves on a FAILED poll");

  r.link.inject(affatest::panelSyncRequest());
  r.d.poll();
  // ONE hello frame, not Carminat's three.
  expectFrames(r.link, kHello, 1, "UpdateList hello is a single frame");
}

void test_registration_walks_the_function_table_with_0x81_filler(void) {
  SegRig r;
  r.sync();
  drain(r.link);
  r.d.setSelfAck(true);

  ASSERT_RESULT(Ok, r.d.setPower(true));
  pumpUntilIdle(r.d);

  static const Frame kWant[] = {
      kRegister[0], kRegister[1], kSetStateEnable[0],
  };
  expectFrames(r.link, kWant, 3, "registration {0x121, 0x1B1} then the payload");
  TEST_ASSERT_TRUE(r.d.registered());
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
    r.d.setText("X", kDigits[i]);
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
  r.d.setText("AUX", 255);
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
  RUN_TEST(test_the_two_encodings_declare_different_lengths);
  RUN_TEST(test_a_malformed_key_frame_is_not_acknowledged);
  RUN_TEST(test_ams_banner_is_three_renders_100ms_apart);
  RUN_TEST(test_ams_disabled_suppresses_the_fall_through);
  RUN_TEST(test_unsupported_operations_report_rather_than_no_op);
  return UNITY_END();
}
