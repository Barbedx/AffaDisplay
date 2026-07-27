// Every Carminat frame builder against the golden vectors in docs/WIRE-SPEC.md, byte for
// byte, including the ISO-TP chunking, the INCONSISTENT declared length bytes and the 0x00
// padding.
//
// The vectors below are transcribed from docs/WIRE-SPEC.md "Golden vectors". Where a
// vector is [CAP-VERBATIM] the comment says so — those bytes were observed on a real bus
// and are not negotiable.
//
// TWO ACK MODELS, AND showMenu IS THE ONE THAT SEES THE DIFFERENCE. A real panel ends the
// transfer as soon as it holds the DECLARED FF_DL, so showMenu is 13 frames ending at PCI
// 0x2C; through the bench self-ACK emulator all 14 go out and the last PCI is 0x2D. A bare
// 14 is a bug, and so is a bare 13.

#include "../affa_test_support.h"

#include "carminat/CarminatDisplay.h"
#include "carminat/CarminatConstants.h"

using namespace affa;
using affatest::mk;
using affatest::drain;
using affatest::expectFrames;
using affatest::pumpUntilIdle;

namespace {

// ---------------------------------------------------------------------------
// Golden vectors — docs/WIRE-SPEC.md
// ---------------------------------------------------------------------------

// [CAP-VERBATIM] logs/device-monitor-260616-235529.log
const Frame kSetTextRenault[] = {
    {0x151, 8, {0x10, 0x0E, 0x77, 0x55, 0x55, 0xFF, 0x60, 0x01}, false},
    {0x151, 8, {0x21, 0x52, 0x45, 0x4E, 0x41, 0x55, 0x4C, 0x54}, false},
    {0x151, 8, {0x22, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, false},
};

const Frame kSetTextHello[] = {
    {0x151, 8, {0x10, 0x0E, 0x77, 0x55, 0x55, 0xFF, 0x60, 0x01}, false},
    {0x151, 8, {0x21, 'H', 'E', 'L', 'L', 'O', 0x00, 0x00}, false},
    {0x151, 8, {0x22, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, false},
};

const Frame kSetTime1234[] = {
    {0x151, 8, {0x05, 0x56, 0x31, 0x32, 0x33, 0x34, 0x00, 0x00}, false},
};

// [CAP-VERBATIM] @TX 151 03 52 09 FF FF 00 00 00 (x42) / 03 52 00 .. (x11)
const Frame kSetStateEnable[] = {
    {0x151, 8, {0x03, 0x52, 0x09, 0xFF, 0xFF, 0x00, 0x00, 0x00}, false},
};
const Frame kSetStateDisable[] = {
    {0x151, 8, {0x03, 0x52, 0x00, 0xFF, 0xFF, 0x00, 0x00, 0x00}, false},
};

// [CAP-VERBATIM] @TX 151 07 29 01 7E .. (x11) / 07 29 01 7F .. (x12)
const Frame kHighlightTop[] = {
    {0x151, 8, {0x07, 0x29, 0x01, 0x7E, 0x80, 0x00, 0x00, 0x00}, false},
};
const Frame kHighlightBottom[] = {
    {0x151, 8, {0x07, 0x29, 0x01, 0x7F, 0x80, 0x00, 0x00, 0x00}, false},
};

// showMenu("Main Menu", "Voltage:0V", "Boost:0mbar", 0x0B).
// THE LAST ENTRY IS EMULATOR-ONLY. Against a panel the transfer ends at PCI 0x2C because
// the declared FF_DL 0x5A = 90 is satisfied by 6 + 12*7 exactly.
const Frame kShowMenuMain[] = {
    {0x151, 8, {0x10, 0x5A, 0x21, 0x01, 0x7E, 0x80, 0x00, 0x00}, false},  // [0..7]
    {0x151, 8, {0x21, 0x82, 0xFF, 0x0B, 0x4D, 0x61, 0x69, 0x6E}, false},  // [8..14]
    {0x151, 8, {0x22, 0x20, 0x4D, 0x65, 0x6E, 0x75, 0x00, 0x00}, false},  // [15..21]
    {0x151, 8, {0x23, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, false},  // [22..28]
    {0x151, 8, {0x24, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, false},  // [29..35]
    {0x151, 8, {0x25, 0x00, 0x00, 0x7E, 0x56, 0x6F, 0x6C, 0x74}, false},  // [36..42]
    {0x151, 8, {0x26, 0x61, 0x67, 0x65, 0x3A, 0x30, 0x56, 0x00}, false},  // [43..49]
    {0x151, 8, {0x27, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, false},  // [50..56]
    {0x151, 8, {0x28, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, false},  // [57..63]
    {0x151, 8, {0x29, 0x01, 0x7F, 0x42, 0x6F, 0x6F, 0x73, 0x74}, false},  // [64..70]
    {0x151, 8, {0x2A, 0x3A, 0x30, 0x6D, 0x62, 0x61, 0x72, 0x00}, false},  // [71..77]
    {0x151, 8, {0x2B, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, false},  // [78..84]
    {0x151, 8, {0x2C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, false},  // [85..91] HW END
    {0x151, 8, {0x2D, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, false},  // [92..95] EMU
};
constexpr size_t kShowMenuMain_HW  = 13;   // a panel DONEs at the declared FF_DL
constexpr size_t kShowMenuMain_EMU = 14;   // the bench self-ACK emulator runs to the end

// [CAP-VERBATIM] logs/device-monitor-260616-230730.log — 13 frames, ends at 0x2C.
const Frame kShowMenuCaptured[] = {
    {0x151, 8, {0x10, 0x5A, 0x21, 0x01, 0x7E, 0x80, 0x00, 0x00}, false},
    {0x151, 8, {0x21, 0x82, 0xFF, 0x00, 0x4D, 0x65, 0x67, 0x61}, false},
    {0x151, 8, {0x22, 0x6E, 0x65, 0x43, 0x41, 0x4E, 0x00, 0x00}, false},
    {0x151, 8, {0x23, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, false},
    {0x151, 8, {0x24, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, false},
    {0x151, 8, {0x25, 0x00, 0x00, 0x7E, 0x57, 0x61, 0x69, 0x74}, false},
    {0x151, 8, {0x26, 0x69, 0x6E, 0x67, 0x20, 0x66, 0x6F, 0x72}, false},
    {0x151, 8, {0x27, 0x20, 0x70, 0x68, 0x6F, 0x6E, 0x65, 0x00}, false},
    {0x151, 8, {0x28, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, false},
    {0x151, 8, {0x29, 0x01, 0x7F, 0x66, 0x6F, 0x72, 0x20, 0x41}, false},
    {0x151, 8, {0x2A, 0x4D, 0x53, 0x20, 0x64, 0x65, 0x76, 0x69}, false},
    {0x151, 8, {0x2B, 0x63, 0x65, 0x00, 0x00, 0x00, 0x00, 0x00}, false},
    {0x151, 8, {0x2C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, false},
};

const Frame kFullscreenNavCd[] = {
    {0x151, 8, {0x10, 0x60, 0x21, 0x05, 0xFF, 0x00, 0x00, 0x40}, false},  // [0..7]
    {0x151, 8, {0x21, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, false},  // [8..14]
    {0x151, 8, {0x22, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, false},  // [15..21]
    {0x151, 8, {0x23, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, false},  // [22..28]
    {0x151, 8, {0x24, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20, 0x20}, false},  // [29..35]
    {0x151, 8, {0x25, 0x50, 0x4C, 0x45, 0x41, 0x53, 0x45, 0x20}, false},  // [36..42]
    {0x151, 8, {0x26, 0x49, 0x4E, 0x53, 0x45, 0x52, 0x54, 0x0D}, false},  // [43..49]
    {0x151, 8, {0x27, 0x4E, 0x41, 0x56, 0x49, 0x47, 0x41, 0x54}, false},  // [50..56]
    {0x151, 8, {0x28, 0x49, 0x4F, 0x4E, 0x20, 0x43, 0x44, 0x0D}, false},  // [57..63]
    {0x151, 8, {0x29, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20}, false},  // [64..70]
    {0x151, 8, {0x2A, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20}, false},  // [71..77]
    {0x151, 8, {0x2B, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20}, false},  // [78..84]
    {0x151, 8, {0x2C, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20}, false},  // [85..91]
    {0x151, 8, {0x2D, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x00}, false},  // [92..97]+filler
};

// [CAP-VERBATIM] logs/device-monitor-260617-004150.log
const Frame kPopupVol28[] = {
    {0x151, 8, {0x10, 0x0E, 0x74, 0x09, 0x55, 0xFF, 0x60, 0x01}, false},
    {0x151, 8, {0x21, 0x56, 0x4F, 0x4C, 0x20, 0x32, 0x38, 0x20}, false},
    {0x151, 8, {0x22, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, false},
};

// [CAP-VERBATIM] @TX 151 02 54 03 00 00 00 00 00, and independently seen FROM the OEM head
// unit. hidePopup() and hideFullscreenText() are the same three bytes.
const Frame kCloseWindow[] = {
    {0x151, 8, {0x02, 0x54, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00}, false},
};

// showConfirmBox("OK", "Line one", "Line two") — 113 bytes, 16 frames, last PCI 0x2F, the
// continuation counter's absolute ceiling. [DERIVED] end to end.
const Frame kConfirmBoxOk[] = {
    {0x151, 8, {0x10, 0x6F, 0x21, 0x05, 0x00, 0x00, 0x01, 0x49}, false},
    {0x151, 8, {0x21, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, false},
    {0x151, 8, {0x22, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, false},
    {0x151, 8, {0x23, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, false},
    {0x151, 8, {0x24, 0x00, 0x00, 0x00, 0x00, 0x00, 0x4F, 0x4B}, false},
    {0x151, 8, {0x25, 0x00, 0x00, 0x00, 0x00, 0x4C, 0x69, 0x6E}, false},
    {0x151, 8, {0x26, 0x65, 0x20, 0x6F, 0x6E, 0x65, 0x0D, 0x4C}, false},
    {0x151, 8, {0x27, 0x69, 0x6E, 0x65, 0x20, 0x74, 0x77, 0x6F}, false},
    {0x151, 8, {0x28, 0x0D, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, false},
    {0x151, 8, {0x29, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, false},
    {0x151, 8, {0x2A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, false},
    {0x151, 8, {0x2B, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, false},
    {0x151, 8, {0x2C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, false},
    {0x151, 8, {0x2D, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, false},
    {0x151, 8, {0x2E, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, false},
    {0x151, 8, {0x2F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, false},
};

// showInfoPopup("AUX ON", "AF ON", "SPEED 0") — THREE messages, two frames each.
//
// SPACE-PADDED, which is the OEM form and a deliberate divergence from the extracted
// builder's `char padded[8] = {' '}` (that initialises element 0 only and NUL-pads 1..7).
// docs/WIRE-SPEC.md §8.10 and its kCarminatInfoPopupOem vector have been corrected to
// match, and record that no capture in the corpus witnesses a continuation frame of an
// info row at all — the [CAP-VERBATIM] evidence covers first frames only, which carry
// t0..t2 and therefore never a pad byte. Doc and suite now agree.
const Frame kInfoPopupOem[] = {
    {0x151, 8, {0x10, 0x0B, 0x76, 0x60, 0x41, 0x41, 0x55, 0x58}, false},  // A U X
    {0x151, 8, {0x21, 0x20, 0x4F, 0x4E, 0x20, 0x20, 0x00, 0x00}, false},  // ' ' O N + pad
    {0x151, 8, {0x10, 0x0B, 0x76, 0x60, 0x44, 0x41, 0x46, 0x20}, false},  // A F ' '
    {0x151, 8, {0x21, 0x4F, 0x4E, 0x20, 0x20, 0x20, 0x00, 0x00}, false},  // O N + pad
    {0x151, 8, {0x10, 0x0B, 0x76, 0x60, 0x48, 0x53, 0x50, 0x45}, false},  // S P E
    {0x151, 8, {0x21, 0x45, 0x44, 0x20, 0x30, 0x20, 0x00, 0x00}, false},  // E D ' ' 0 pad
};

// ---------------------------------------------------------------------------
// Rig
// ---------------------------------------------------------------------------

struct Rig {
  LoopbackLink<256> link;
  affatest::FakeClock clk;
  CarminatDisplay d;

  Rig() : d(link, clk) {}

  // Handshake + FUNCSREG latched + the wire drained, so every test below starts from a
  // state where the next call's frames are the only frames.
  //
  // The clock is left FROZEN. Nothing in these tests wants a heartbeat in the middle of a
  // golden vector, and freezing is how a wire test stays a wire test.
  void up() {
    d.begin();
    link.inject(affatest::panelSyncRequest());
    d.poll();
    TEST_ASSERT_TRUE(d.synced());

    d.setSelfAck(true);          // PARTIAL while bytes remain, DONE on the last
    ASSERT_RESULT(Ok, d.setPower(true));
    pumpUntilIdle(d);
    TEST_ASSERT_TRUE_MESSAGE(d.registered(), "FUNCSREG must latch before the wire tests");
    drain(link);
  }

  // Switch from the self-ACK emulator to a HARDWARE-style panel: `partials` PARTIALs, then
  // DONE. Setting partials to (declaredFrames - 1) makes the link behave exactly like a
  // panel that terminates at its declared FF_DL.
  void hardwareAck(uint16_t partials) {
    d.setSelfAck(false);
    link.setAutoAck(true);
    link.setAutoAckPartials(partials);
  }
};

}  // namespace

// ---------------------------------------------------------------------------
// setText / setTime / setPower
// ---------------------------------------------------------------------------

void test_setText_is_capture_verbatim(void) {
  Rig r;
  r.up();
  ASSERT_RESULT(Ok, r.d.setText("RENAULT", 0));
  pumpUntilIdle(r.d);
  expectFrames(r.link, kSetTextRenault, 3, "setText(\"RENAULT\") [CAP-VERBATIM]");

  ASSERT_RESULT(Ok, r.d.setText("HELLO"));
  pumpUntilIdle(r.d);
  expectFrames(r.link, kSetTextHello, 3, "setText(\"HELLO\")");
}

void test_setText_declares_0x0E_for_20_transmitted_bytes(void) {
  // DO NOT "FIX" THIS. 0x0E says 14 content bytes; the builder transmits 20 (a 6-byte
  // header plus 14 text cells) and the panel consumes only the declared 14. It has been
  // rendering correctly for months and a length byte is glass, not style.
  Rig r;
  r.up();
  r.d.setText("HELLO");
  pumpUntilIdle(r.d);

  Frame f;
  TEST_ASSERT_TRUE(r.link.takeSent(f));
  TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x0E, f.data[1], "setText declares 0x0E");
  // 3 frames = 8 + 7 + 7 = 22 payload bytes offered, of which 20 are ours.
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(2, r.link.sentCount(), "setText is three frames");
  drain(r.link);
}

void test_setText_transliterates_before_the_wire(void) {
  // The one deliberate byte-affecting change against the extracted builder, sanctioned by
  // WIRE-SPEC §8.1: UTF-8 that reaches the panel is garbage on the glass, so every string
  // goes through affa::toAscii at the builder.
  Rig r;
  r.up();
  r.d.setText("\xC5\x81\xC3\xB3" "d" "\xC5\xBA");   // "Lodz" in Polish
  pumpUntilIdle(r.d);

  Frame f0, f1, f2;
  TEST_ASSERT_TRUE(r.link.takeSent(f0));
  TEST_ASSERT_TRUE(r.link.takeSent(f1));
  TEST_ASSERT_TRUE(r.link.takeSent(f2));
  static const uint8_t kWant[8] = {0x21, 'L', 'o', 'd', 'z', 0x00, 0x00, 0x00};
  TEST_ASSERT_EQUAL_HEX8_ARRAY_MESSAGE(kWant, f1.data, 8, "transliterated to ASCII");
  drain(r.link);
}

void test_setTime_is_one_frame_and_rejects_a_short_string(void) {
  Rig r;
  r.up();
  ASSERT_RESULT(Ok, r.d.setTime("1234"));
  pumpUntilIdle(r.d);
  expectFrames(r.link, kSetTime1234, 1, "setTime(\"1234\")");

  // Legacy indexed clock[0..3] unconditionally and could read past a short string.
  ASSERT_RESULT(BadArgument, r.d.setTime("12"));
  ASSERT_RESULT(BadArgument, r.d.setTime(nullptr));
  pumpUntilIdle(r.d);
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, r.link.sentCount(), "a rejected setTime sends nothing");
}

void test_setPower_declares_0x03_for_four_bytes(void) {
  // NOT unified with UpdateList's `04 52 …`. Only UpdateList's length byte is
  // self-consistent; both have been accepted by their panels for months.
  Rig r;
  r.up();
  ASSERT_RESULT(Ok, r.d.setPower(true));
  pumpUntilIdle(r.d);
  expectFrames(r.link, kSetStateEnable, 1, "setPower(true) [CAP-VERBATIM]");

  ASSERT_RESULT(Ok, r.d.setPower(false));
  pumpUntilIdle(r.d);
  expectFrames(r.link, kSetStateDisable, 1, "setPower(false) [CAP-VERBATIM]");
}

// ---------------------------------------------------------------------------
// highlightItem
// ---------------------------------------------------------------------------

void test_highlightItem_is_one_frame_per_row(void) {
  Rig r;
  r.up();
  ASSERT_RESULT(Ok, r.d.highlightItem(0));
  pumpUntilIdle(r.d);
  expectFrames(r.link, kHighlightTop, 1, "highlightItem(0) [CAP-VERBATIM]");

  ASSERT_RESULT(Ok, r.d.highlightItem(1));
  pumpUntilIdle(r.d);
  expectFrames(r.link, kHighlightBottom, 1, "highlightItem(1) [CAP-VERBATIM]");

  // Deliberate tightening: legacy treated any non-zero row as row 1. The panel renders
  // exactly two rows, so anything else is a caller error rather than a silent row 1.
  ASSERT_RESULT(BadArgument, r.d.highlightItem(2));
  pumpUntilIdle(r.d);
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, r.link.sentCount(), "row 2 puts nothing on the wire");
}

// ---------------------------------------------------------------------------
// showMenu — parameterised by ACK model
// ---------------------------------------------------------------------------

void test_showMenu_is_13_frames_under_a_hardware_ack(void) {
  // A panel answers DONE once it holds the declared FF_DL 0x5A = 90 content bytes, which
  // 6 + 12*7 satisfies exactly. Twelve PARTIALs then DONE: 13 frames, last PCI 0x2C.
  Rig r;
  r.up();
  r.hardwareAck(12);

  ASSERT_RESULT(Ok, r.d.showMenu("Main Menu", "Voltage:0V", "Boost:0mbar", 0x0B));
  pumpUntilIdle(r.d);

  expectFrames(r.link, kShowMenuMain, kShowMenuMain_HW, "showMenu [ACK model: HW]");
  // DONE WHILE BYTES REMAIN IS SUCCESS. Reporting SendFailed here would make the menu look
  // permanently broken, on every single render.
  ASSERT_RESULT(Ok, r.d.lastResult());
}

void test_showMenu_is_14_frames_through_the_self_ack_emulator(void) {
  Rig r;
  r.up();                       // up() leaves setSelfAck(true)
  ASSERT_RESULT(Ok, r.d.showMenu("Main Menu", "Voltage:0V", "Boost:0mbar", 0x0B));
  pumpUntilIdle(r.d);
  expectFrames(r.link, kShowMenuMain, kShowMenuMain_EMU, "showMenu [ACK model: EMU]");
  ASSERT_RESULT(Ok, r.d.lastResult());
}

void test_showMenu_matches_the_capture_verbatim_vector(void) {
  // logs/device-monitor-260616-230730.log, against the real panel: 13 frames, ends at 0x2C.
  Rig r;
  r.up();
  r.hardwareAck(12);
  ASSERT_RESULT(Ok, r.d.showMenu("MeganeCAN", "Waiting for phone", "for AMS device", 0x00));
  pumpUntilIdle(r.d);
  expectFrames(r.link, kShowMenuCaptured, 13, "showMenu [CAP-VERBATIM]");
}

void test_showMenu_declares_0x5A_while_building_96_bytes(void) {
  Rig r;
  r.up();
  r.d.showMenu("Main Menu", "Voltage:0V", "Boost:0mbar", 0x0B);
  pumpUntilIdle(r.d);
  Frame f;
  TEST_ASSERT_TRUE(r.link.takeSent(f));
  TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x10, f.data[0], "payload byte 0 is the caller's 0x10");
  TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x5A, f.data[1], "showMenu declares 0x5A for 94 built");
  drain(r.link);
}

// ---------------------------------------------------------------------------
// Popup / fullscreen / confirm box
// ---------------------------------------------------------------------------

void test_showPopupText_is_capture_verbatim(void) {
  Rig r;
  r.up();
  ASSERT_RESULT(Ok, r.d.showPopupText("VOL 28", 0x09, 0xFF, 0x60));
  pumpUntilIdle(r.d);
  expectFrames(r.link, kPopupVol28, 3, "showPopupText(\"VOL 28\") [CAP-VERBATIM]");
}

void test_hidePopup_and_hideFullscreenText_are_the_same_three_bytes(void) {
  Rig r;
  r.up();
  ASSERT_RESULT(Ok, r.d.hidePopup());
  pumpUntilIdle(r.d);
  expectFrames(r.link, kCloseWindow, 1, "hidePopup() [CAP-VERBATIM]");

  ASSERT_RESULT(Ok, r.d.hideFullscreenText());
  pumpUntilIdle(r.d);
  expectFrames(r.link, kCloseWindow, 1, "hideFullscreenText() — identical bytes");
}

void test_showFullscreenText_is_14_frames(void) {
  Rig r;
  r.up();
  ASSERT_RESULT(Ok, r.d.showFullscreenText("PLEASE INSERT", "NAVIGATION CD", ""));
  pumpUntilIdle(r.d);
  expectFrames(r.link, kFullscreenNavCd, 14, "showFullscreenText");
}

void test_showConfirmBox_sits_at_the_113_byte_ceiling(void) {
  // 2 + 6 + 105 = 113 = 8 + 15*7. Sixteen frames, last PCI 0x2F, zero headroom.
  Rig r;
  r.up();
  ASSERT_RESULT(Ok, r.d.showConfirmBox("OK", "Line one", "Line two"));
  pumpUntilIdle(r.d);
  expectFrames(r.link, kConfirmBoxOk, 16, "showConfirmBox at the transport ceiling");
}

void test_showConfirmBox_caption_abuts_the_row_region(void) {
  // The caption region (content 0x1A..0x20) ABUTS the row region at 0x20: a 7-character
  // caption writes content[32], which row0 then overwrites. Pinned so that anyone who
  // "fixes" the overlap sees a failing test rather than a silently different screen.
  Rig r;
  r.up();
  ASSERT_RESULT(Ok, r.d.showConfirmBox("ABCDEFG", "Zebra", ""));
  pumpUntilIdle(r.d);

  Frame f;
  for (int i = 0; i < 5; ++i) TEST_ASSERT_TRUE(r.link.takeSent(f));   // frames 0..4
  // frame 4 (PCI 0x24) carries content[21..27] = the first six caption cells at 26,27.
  TEST_ASSERT_EQUAL_HEX8_MESSAGE('A', f.data[6], "caption starts at content 0x1A");
  TEST_ASSERT_EQUAL_HEX8_MESSAGE('B', f.data[7], "caption cell 1");
  TEST_ASSERT_TRUE(r.link.takeSent(f));                                // frame 5, PCI 0x25
  // content[28..34]: caption cells 2..6 land at 28..32, then row0 OVERWRITES 32 onward.
  static const uint8_t kWant[8] = {0x25, 'C', 'D', 'E', 'F', 'Z', 'e', 'b'};
  TEST_ASSERT_EQUAL_HEX8_ARRAY_MESSAGE(kWant, f.data, 8,
                                       "the 7th caption byte is overwritten by row0");
  drain(r.link);
}

// ---------------------------------------------------------------------------
// showInfoPopup
// ---------------------------------------------------------------------------

void test_showInfoPopup_is_three_messages_space_padded(void) {
  Rig r;
  r.up();
  ASSERT_RESULT(Ok, r.d.showInfoPopup("AUX ON", "AF ON", "SPEED 0"));
  // Three separate messages: each occupies its own queue slot and they must not coalesce
  // against each other, because all three share funcId 0x151 and RenderSlot::InfoPopup.
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(2, r.d.queued(), "three rows = three queued jobs");
  pumpUntilIdle(r.d);
  expectFrames(r.link, kInfoPopupOem, 6, "showInfoPopup — OEM SPACE padding");
}

void test_hideInfoPopup_falls_back_to_the_source_banner(void) {
  // The real popup-close command has never been observed; inventing one would be a guess
  // presented as a fact, so this is documented as best-effort and pinned as such.
  Rig r;
  r.up();
  ASSERT_RESULT(Ok, r.d.hideInfoPopup());
  pumpUntilIdle(r.d);
  expectFrames(r.link, kSetTextRenault, 3, "hideInfoPopup -> setText(\"RENAULT\")");
}

// ---------------------------------------------------------------------------
// Registration order, which is on the wire
// ---------------------------------------------------------------------------

void test_first_send_after_a_resync_registers_both_functions_in_order(void) {
  Rig r;
  r.d.begin();
  r.link.inject(affatest::panelSyncRequest());
  r.d.poll();
  drain(r.link);
  r.d.setSelfAck(true);

  ASSERT_RESULT(Ok, r.d.setPower(true));
  TEST_ASSERT_FALSE_MESSAGE(r.d.registered(), "FUNCSREG must not latch before the probes");
  pumpUntilIdle(r.d);

  static const Frame kWant[] = {
      {0x151, 8, {0x70, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, false},
      {0x1F1, 8, {0x70, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, false},
      {0x151, 8, {0x03, 0x52, 0x09, 0xFF, 0xFF, 0x00, 0x00, 0x00}, false},
  };
  expectFrames(r.link, kWant, 3, "lazy registration walks {0x151, 0x1F1} in table order");
  TEST_ASSERT_TRUE(r.d.registered());
}

// ---------------------------------------------------------------------------

void setUp(void) {}
void tearDown(void) {}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_setText_is_capture_verbatim);
  RUN_TEST(test_setText_declares_0x0E_for_20_transmitted_bytes);
  RUN_TEST(test_setText_transliterates_before_the_wire);
  RUN_TEST(test_setTime_is_one_frame_and_rejects_a_short_string);
  RUN_TEST(test_setPower_declares_0x03_for_four_bytes);
  RUN_TEST(test_highlightItem_is_one_frame_per_row);
  RUN_TEST(test_showMenu_is_13_frames_under_a_hardware_ack);
  RUN_TEST(test_showMenu_is_14_frames_through_the_self_ack_emulator);
  RUN_TEST(test_showMenu_matches_the_capture_verbatim_vector);
  RUN_TEST(test_showMenu_declares_0x5A_while_building_96_bytes);
  RUN_TEST(test_showPopupText_is_capture_verbatim);
  RUN_TEST(test_hidePopup_and_hideFullscreenText_are_the_same_three_bytes);
  RUN_TEST(test_showFullscreenText_is_14_frames);
  RUN_TEST(test_showConfirmBox_sits_at_the_113_byte_ceiling);
  RUN_TEST(test_showConfirmBox_caption_abuts_the_row_region);
  RUN_TEST(test_showInfoPopup_is_three_messages_space_padded);
  RUN_TEST(test_hideInfoPopup_falls_back_to_the_source_banner);
  RUN_TEST(test_first_send_after_a_resync_registers_both_functions_in_order);
  return UNITY_END();
}
