// The extracted menu model, driven at THREE GEOMETRIES over the same three-item content.
//
// The whole point of src/widget/ is that the algorithm no longer knows what it is drawn on,
// so every behavioural test below runs three times — rows = 2 (the Carminat menu window),
// rows = 3 (the info screen, and the case where a three-item list FITS and the arrows must
// disappear) and rows = 6 (a 4" OLED, where the window is bigger than the list and most of
// it is blank). A test that passes at one geometry and not another is exactly the class of
// defect the extraction was meant to make impossible.
//
// WHAT THIS SUITE DELIBERATELY DOES NOT INCLUDE: a panel. No CAN link, no ISO-TP, no frame
// builder, no CarminatDisplay — the include list below is the claim that the model is
// display-agnostic, and it would stop compiling if that ever stopped being true. The
// renderer here is a Recorder that keeps the last frame; what a frame COSTS is the adapter's
// business and is asserted in test_nav against the wire instead. WHICH of the two calls the
// model makes for a given key — a whole frame, or highlightOnly() — is the MODEL's, and
// section 10 pins it.
//
// The expected values are HAND-WRITTEN per geometry rather than derived from the model's own
// rules. A scroll-mask table computed with the same expression MenuModel uses would agree
// with any bug that lived in that expression.

#include <unity.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "widget/MenuModel.h"

using affa::widget::Field;
using affa::widget::IMenuRenderer;
using affa::widget::integerField;
using affa::widget::listField;
using affa::widget::MenuGeometry;
using affa::widget::MenuItem;
using affa::widget::MenuModel;
using affa::widget::readOnlyField;

namespace {

// ---------------------------------------------------------------------------
// Message formatting: every assertion carries the geometry it failed at, because the same
// line runs three times.
// ---------------------------------------------------------------------------

const char* M(uint8_t rows, const char* what) {
  static char buf[200];
  std::snprintf(buf, sizeof(buf), "rows=%u: %s", static_cast<unsigned>(rows), what);
  return buf;
}

// ---------------------------------------------------------------------------
// The recording renderer
// ---------------------------------------------------------------------------

constexpr uint8_t kMaxRecRows = 8;   // enough for the widest geometry the suite uses

// Keeps the LAST frame and counts frames. It also polices the seam's own contract on every
// redraw, so the protocol assertions do not have to be repeated in each test:
//   * begin / rows 0..n-1 in order / end, never nested,
//   * a non-null header and non-null row text,
//   * exactly `rows` rows per frame, no more and no fewer,
//   * no row longer than geometry.rowChars,
//   * exactly one row carrying the selection.
struct Recorder final : IMenuRenderer {
  explicit Recorder(uint8_t rowsPerFrame, uint8_t maxChars)
      : expectRows(rowsPerFrame), widthLimit(maxChars) {}

  uint8_t expectRows;
  uint8_t widthLimit;

  char    header[64] = {0};
  uint8_t mask       = 0xEE;                    // a value the model can never emit
  char    rows[kMaxRecRows][AFFA_MENU_ROW_MAX]  = {{0}};
  uint8_t rowCount   = 0;
  int     selectedRow = -1;
  int     selectedCount = 0;
  int     frames     = 0;
  bool    inFrame    = false;

  void beginFrame(const char* h, uint8_t m) override {
    TEST_ASSERT_FALSE_MESSAGE(inFrame, "beginFrame arrived inside an unfinished frame");
    TEST_ASSERT_NOT_NULL_MESSAGE(h, "the model substitutes \"\" for a null header");
    inFrame = true;
    std::snprintf(header, sizeof(header), "%s", h);
    mask          = m;
    rowCount      = 0;
    selectedRow   = -1;
    selectedCount = 0;
    ++frames;
  }

  void row(uint8_t index, const char* text, bool selected) override {
    TEST_ASSERT_TRUE_MESSAGE(inFrame, "row() outside a frame");
    TEST_ASSERT_LESS_THAN_MESSAGE(kMaxRecRows, rowCount, "more rows than the recorder holds");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(rowCount, index, "rows must arrive in order 0..rows-1");
    TEST_ASSERT_NOT_NULL_MESSAGE(text, "row text is never null");
    TEST_ASSERT_TRUE_MESSAGE(std::strlen(text) <= widthLimit,
                             "row text is longer than geometry.rowChars");
    std::snprintf(rows[rowCount], sizeof(rows[0]), "%s", text);
    if (selected) {
      selectedRow = index;
      ++selectedCount;
    }
    ++rowCount;
  }

  void endFrame() override {
    TEST_ASSERT_TRUE_MESSAGE(inFrame, "endFrame without a beginFrame");
    inFrame = false;
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(expectRows, rowCount,
                                    "a frame is always the whole window, blanks included");
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, selectedCount,
                                  "exactly one row carries the selection while open");
  }

  const char* text(uint8_t i) const { return (i < rowCount) ? rows[i] : "<row not emitted>"; }
};

// ---------------------------------------------------------------------------
// Content: ONE three-item model, shared by every geometry
// ---------------------------------------------------------------------------

const char* const kModes[] = {"Off", "Low", "High"};

// Item 0 unedited and with each field under edit. Written out rather than composed, so a
// change to the `*Label: <value>` convention has to be admitted here.
const char* const kItem0        = "Bright: 50% Off 12V";
const char* const kItem0EditF0  = "*Bright: <50%> Off 12V";
const char* const kItem0EditF1  = "*Bright: 50% <Off> 12V";
const char* const kItem0EditF2  = "*Bright: 50% Off <12V>";
const char* const kItem1        = "Second";
const char* const kItem2        = "Third";

// What onChange saw, from inside onChange.
struct Watch {
  const Recorder* rec       = nullptr;
  int             calls     = 0;
  uint8_t         lastField = 0xFF;
  int32_t         valueAtCall = -1;
  int             framesAtCall = -1;
};

void onChangeCb(const MenuItem& it, uint8_t fieldIndex, void* ctx) {
  Watch* w = static_cast<Watch*>(ctx);
  ++w->calls;
  w->lastField    = fieldIndex;
  w->valueAtCall  = it.fields[fieldIndex].value;
  w->framesAtCall = w->rec ? w->rec->frames : -1;
}

MenuGeometry makeGeometry(uint8_t rows, uint8_t rowChars, bool wrap) {
  MenuGeometry g;
  g.rows     = rows;
  g.rowChars = rowChars;
  g.wrap     = wrap;
  return g;
}

struct Rig {
  Recorder  rec;
  MenuModel m;
  Watch     watch;

  explicit Rig(uint8_t rows, uint8_t rowChars = 26, bool wrap = false)
      : rec(rows, rowChars), m(rec, makeGeometry(rows, rowChars, wrap), "MENU") {
    watch.rec = &rec;
  }

  // The three-item model. Item 0 carries all three field kinds: an integer with a coarse
  // step, a list, and a read-only live reading. Items 1 and 2 carry none, so "select on an
  // item with nothing to edit" is reachable at every geometry.
  void addThreeItems() {
    MenuItem it;
    it.label       = "Bright";
    it.fields[0]   = integerField(50, 0, 100, 5, 10, "%");
    it.fields[1]   = listField(kModes, 3, 0);
    it.fields[2]   = readOnlyField(12, "V");
    it.fieldCount  = 3;
    it.onChange    = onChangeCb;
    it.ctx         = &watch;
    TEST_ASSERT_EQUAL_INT(0, m.addItem(it));

    MenuItem b;
    b.label = "Second";
    TEST_ASSERT_EQUAL_INT(1, m.addItem(b));

    MenuItem c;
    c.label = "Third";
    TEST_ASSERT_EQUAL_INT(2, m.addItem(c));
  }

  // n plain items called I0..I7, for the long-list scroll cases.
  void addFillerItems(uint8_t n) {
    static const char* const kNames[] = {"I0", "I1", "I2", "I3", "I4", "I5", "I6", "I7", "I8"};
    for (uint8_t i = 0; i < n; ++i) {
      MenuItem it;
      it.label = kNames[i];
      TEST_ASSERT_EQUAL_INT(static_cast<int>(i), m.addItem(it));
    }
  }
};

// The text the three-item model puts on window row `r` when the window starts at `top`.
const char* expectedRow(unsigned top, uint8_t r) {
  const unsigned idx = top + r;
  switch (idx) {
    case 0:  return kItem0;
    case 1:  return kItem1;
    case 2:  return kItem2;
    default: return "";     // past the end of a three-item list: emitted, empty
  }
}

// Asserts the whole visible window against an INDEPENDENTLY tracked (index, row) pair:
// the item on row 0 is index - row, every row holds the item that follows it, rows past the
// end are blank, and the lit row is the selection's.
void expectWindow(Rig& rig, uint8_t rows, uint8_t index, uint8_t row, const char* what) {
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(index, rig.m.selectedIndex(), M(rows, what));
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(row, rig.m.selectedRow(), M(rows, what));
  TEST_ASSERT_TRUE_MESSAGE(rig.m.selectedRow() < rows,
                           M(rows, "the selected row escaped 0..rows-1"));
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(index - row, rig.m.topIndex(), M(rows, what));
  TEST_ASSERT_EQUAL_INT_MESSAGE(row, rig.rec.selectedRow, M(rows, what));
  for (uint8_t r = 0; r < rows; ++r) {
    TEST_ASSERT_EQUAL_STRING_MESSAGE(expectedRow(index - row, r), rig.rec.text(r), M(rows, what));
  }
}

// ---------------------------------------------------------------------------
// 1. Window arithmetic
// ---------------------------------------------------------------------------

// Forward the selection fills the window before it scrolls (row = min(index, rows-1));
// backward the window scrolls only once the selection has reached row 0. Both walks are
// tracked here by hand and compared against the model at every stop.
void windowArithmetic(uint8_t rows) {
  Rig rig(rows);
  rig.addThreeItems();
  rig.m.open();

  expectWindow(rig, rows, 0, 0, "open() puts the selection on the first item, top row");
  TEST_ASSERT_EQUAL_STRING_MESSAGE("MENU", rig.rec.header, M(rows, "header"));

  uint8_t index = 0;
  uint8_t row   = 0;
  for (uint8_t step = 0; step < 2; ++step) {
    TEST_ASSERT_TRUE_MESSAGE(rig.m.next(), M(rows, "an open model consumes next()"));
    ++index;
    if (row + 1 < rows) ++row;                       // else the window scrolls under it
    expectWindow(rig, rows, index, row, "walking down");
  }
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(2, index, M(rows, "three items, two steps"));

  for (uint8_t step = 0; step < 2; ++step) {
    TEST_ASSERT_TRUE_MESSAGE(rig.m.prev(), M(rows, "an open model consumes prev()"));
    --index;
    if (row > 0) --row;
    expectWindow(rig, rows, index, row, "walking back up");
  }
  expectWindow(rig, rows, 0, 0, "a full walk down and back returns to the top");
}

// ---------------------------------------------------------------------------
// 2. The scroll mask at the first, middle and last item
// ---------------------------------------------------------------------------

// Hand-written per geometry. At rows = 2 a three-item list does not fit, so the window is
// either at the top (0x0B) or at the bottom (0x07); at rows = 3 and rows = 6 the SAME list
// fits entirely and the arrows must vanish — that 0x00 is the deliberate one, and the reason
// it exists is that an arrow pointing at nothing sent someone hunting for items that were
// not there (docs/API.md §8.6).
void scrollMaskAtEachPosition(uint8_t rows) {
  static const uint8_t kWant2[3] = {0x0B, 0x0B, 0x07};
  static const uint8_t kWant3[3] = {0x00, 0x00, 0x00};
  static const uint8_t kWant6[3] = {0x00, 0x00, 0x00};
  const uint8_t* want = (rows == 2) ? kWant2 : (rows == 3) ? kWant3 : kWant6;

  // Walking back up, the SAME items give a different answer at rows = 2, and that is the
  // whole point of deriving the mask from the window instead of from the selection: item 1
  // is reached on the way down with the window still at the top (0x0B) and on the way up
  // with the window at the bottom (0x07). Where the list fits the window there is nothing to
  // scroll and the answer is 0x00 in both directions.
  static const uint8_t kBack2[2] = {0x07, 0x0B};
  static const uint8_t kBack3[2] = {0x00, 0x00};
  static const uint8_t kBack6[2] = {0x00, 0x00};
  const uint8_t* back = (rows == 2) ? kBack2 : (rows == 3) ? kBack3 : kBack6;

  Rig rig(rows);
  rig.addThreeItems();
  rig.m.open();

  for (uint8_t i = 0; i < 3; ++i) {
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(want[i], rig.m.scrollMask(), M(rows, "scroll mask"));
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(want[i], rig.rec.mask,
                                   M(rows, "the renderer got the mask the model reports"));
    if (i + 1 < 3) rig.m.next();
  }

  for (uint8_t i = 0; i < 2; ++i) {
    rig.m.prev();
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(back[i], rig.m.scrollMask(), M(rows, "mask, walking up"));
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(back[i], rig.rec.mask, M(rows, "mask on the wire, walking up"));
  }
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, rig.m.selectedIndex(), M(rows, "back at the first item"));
}

// A list that FITS the window shows no arrows at any selection — one item, two items, and
// exactly `rows` items.
void noArrowsWhenTheListFits(uint8_t rows) {
  for (uint8_t n = 1; n <= rows; ++n) {
    Rig rig(rows);
    rig.addFillerItems(n);
    rig.m.open();
    for (uint8_t i = 0; i < n; ++i) {
      TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00, rig.m.scrollMask(),
                                     M(rows, "a list that fits the window has no arrows"));
      TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00, rig.rec.mask, M(rows, "no arrows on the wire"));
      rig.m.next();
    }
    // The blank rows below a short list are still emitted (the recorder asserts the count).
    if (n < rows) {
      TEST_ASSERT_EQUAL_STRING_MESSAGE("", rig.rec.text(static_cast<uint8_t>(rows - 1)),
                                       M(rows, "the row past the end of the list is blank"));
    }
  }
}

// 0x0C — both arrows — needs a window with list above AND below it, so each geometry gets a
// list long enough to have a middle. Masks hand-written per position.
void bothArrowsInTheMiddleOfALongList(uint8_t rows) {
  static const uint8_t kWant2[5] = {0x0B, 0x0B, 0x0C, 0x0C, 0x07};
  static const uint8_t kWant3[5] = {0x0B, 0x0B, 0x0B, 0x0C, 0x07};
  static const uint8_t kWant6[8] = {0x0B, 0x0B, 0x0B, 0x0B, 0x0B, 0x0B, 0x0C, 0x07};
  const uint8_t* want = (rows == 2) ? kWant2 : (rows == 3) ? kWant3 : kWant6;
  const uint8_t  n    = (rows == 6) ? 8 : 5;

  Rig rig(rows);
  rig.addFillerItems(n);
  rig.m.open();
  for (uint8_t i = 0; i < n; ++i) {
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(want[i], rig.m.scrollMask(), M(rows, "long-list mask"));
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(want[i], rig.rec.mask, M(rows, "long-list mask on the wire"));
    rig.m.next();
  }
}

// ---------------------------------------------------------------------------
// 3. The ends, with and without wrap
// ---------------------------------------------------------------------------

// No wrap is the Carminat behaviour and the default: the selection stops dead, and — the
// part worth pinning — it does NOT redraw. A frame emitted for a key that changed nothing is
// a 96-byte screen on that panel.
void theEndsStopWithoutWrap(uint8_t rows) {
  Rig rig(rows);
  rig.addThreeItems();
  rig.m.open();

  int frames = rig.rec.frames;
  TEST_ASSERT_TRUE_MESSAGE(rig.m.prev(), M(rows, "prev() at the top is still consumed"));
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, rig.m.selectedIndex(), M(rows, "no wrap past the top"));
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, rig.m.selectedRow(), M(rows, "row unchanged at the top"));
  TEST_ASSERT_EQUAL_INT_MESSAGE(frames, rig.rec.frames,
                                M(rows, "nothing moved at the top, so nothing was drawn"));

  // The hold edge moves the selection exactly like the click edge while not editing.
  TEST_ASSERT_TRUE_MESSAGE(rig.m.decrease(), M(rows, "decrease() at the top is consumed"));
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, rig.m.selectedIndex(), M(rows, "held wheel does not wrap"));
  TEST_ASSERT_EQUAL_INT_MESSAGE(frames, rig.rec.frames, M(rows, "and still draws nothing"));

  rig.m.next();
  rig.m.next();
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(2, rig.m.selectedIndex(), M(rows, "at the last item"));

  frames = rig.rec.frames;
  TEST_ASSERT_TRUE_MESSAGE(rig.m.next(), M(rows, "next() at the bottom is still consumed"));
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(2, rig.m.selectedIndex(), M(rows, "no wrap past the bottom"));
  TEST_ASSERT_EQUAL_INT_MESSAGE(frames, rig.rec.frames,
                                M(rows, "nothing moved at the bottom, so nothing was drawn"));
  TEST_ASSERT_TRUE_MESSAGE(rig.m.increase(), M(rows, "increase() at the bottom is consumed"));
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(2, rig.m.selectedIndex(), M(rows, "held wheel does not wrap"));
  TEST_ASSERT_EQUAL_INT_MESSAGE(frames, rig.rec.frames, M(rows, "and still draws nothing"));
}

// wrap = true. Wrapping UP lands on the last row the window has to offer, so the window
// shows the tail of the list rather than the last item over a blank; wrapping DOWN lands on
// row 0. With three items that is row 1 at rows = 2 and row 2 at rows = 3 and rows = 6.
void theEndsWrapWhenTheGeometrySaysSo(uint8_t rows) {
  const uint8_t wrappedRow = (3 < rows) ? 2 : static_cast<uint8_t>(rows - 1);

  Rig rig(rows, 26, true);
  rig.addThreeItems();
  rig.m.open();

  int frames = rig.rec.frames;
  TEST_ASSERT_TRUE_MESSAGE(rig.m.prev(), M(rows, "prev() wraps"));
  TEST_ASSERT_EQUAL_INT_MESSAGE(frames + 1, rig.rec.frames, M(rows, "a wrap redraws"));
  expectWindow(rig, rows, 2, wrappedRow, "wrapping up lands on the tail of the list");

  frames = rig.rec.frames;
  TEST_ASSERT_TRUE_MESSAGE(rig.m.next(), M(rows, "next() wraps"));
  TEST_ASSERT_EQUAL_INT_MESSAGE(frames + 1, rig.rec.frames, M(rows, "a wrap redraws"));
  expectWindow(rig, rows, 0, 0, "wrapping down lands on the head of the list");
}

// ---------------------------------------------------------------------------
// 4. Select: enter edit, walk the fields, leave
// ---------------------------------------------------------------------------

void selectWalksTheFieldsThenExits(uint8_t rows) {
  Rig rig(rows);
  rig.addThreeItems();
  rig.m.open();
  TEST_ASSERT_FALSE_MESSAGE(rig.m.isEditing(), M(rows, "open() does not edit"));
  TEST_ASSERT_EQUAL_STRING_MESSAGE(kItem0, rig.rec.text(0), M(rows, "unedited item text"));

  TEST_ASSERT_TRUE_MESSAGE(rig.m.select(), M(rows, "select() is consumed"));
  TEST_ASSERT_TRUE_MESSAGE(rig.m.isEditing(), M(rows, "select() enters edit"));
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, rig.m.editingField(), M(rows, "edit starts at field 0"));
  TEST_ASSERT_EQUAL_STRING_MESSAGE(kItem0EditF0, rig.rec.text(0), M(rows, "field 0 marked"));

  rig.m.select();
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(1, rig.m.editingField(), M(rows, "field 0 -> 1"));
  TEST_ASSERT_EQUAL_STRING_MESSAGE(kItem0EditF1, rig.rec.text(0), M(rows, "field 1 marked"));

  rig.m.select();
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(2, rig.m.editingField(), M(rows, "field 1 -> 2"));
  TEST_ASSERT_EQUAL_STRING_MESSAGE(kItem0EditF2, rig.rec.text(0), M(rows, "field 2 marked"));

  // The last field exits; it does not wrap back to field 0.
  rig.m.select();
  TEST_ASSERT_FALSE_MESSAGE(rig.m.isEditing(), M(rows, "the last field exits edit mode"));
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, rig.m.editingField(), M(rows, "the edit cursor is reset"));
  TEST_ASSERT_EQUAL_STRING_MESSAGE(kItem0, rig.rec.text(0), M(rows, "markers gone"));
  expectWindow(rig, rows, 0, 0, "leaving edit does not move the selection");

  // An item with nothing to edit swallows select() and draws nothing.
  rig.m.next();
  const int frames = rig.rec.frames;
  TEST_ASSERT_TRUE_MESSAGE(rig.m.select(), M(rows, "select() is consumed on any item"));
  TEST_ASSERT_FALSE_MESSAGE(rig.m.isEditing(), M(rows, "an item with no fields never edits"));
  TEST_ASSERT_EQUAL_INT_MESSAGE(frames, rig.rec.frames, M(rows, "and draws nothing"));
}

// ---------------------------------------------------------------------------
// 5. The coarse step and the clamps
// ---------------------------------------------------------------------------

// step 5, stepMultiplier 10: a click moves 5 and a HOLD moves 50 — the multiplier applies to
// the delta before the step (legacy order). Both ends clamp, and a step that changes nothing
// draws nothing.
void coarseStepAndClamping(uint8_t rows) {
  Rig rig(rows);
  rig.addThreeItems();
  rig.m.open();
  rig.m.select();                                   // edit field 0, value 50

  const Field& f = rig.m.item(0)->fields[0];

  TEST_ASSERT_TRUE_MESSAGE(rig.m.next(), M(rows, "next() while editing is consumed"));
  TEST_ASSERT_EQUAL_INT32_MESSAGE(55, f.value, M(rows, "a click takes one step"));

  TEST_ASSERT_TRUE_MESSAGE(rig.m.increase(), M(rows, "increase() is consumed"));
  TEST_ASSERT_EQUAL_INT32_MESSAGE(100, f.value,
                                  M(rows, "a hold takes step x multiplier, clamped at max"));

  int frames = rig.rec.frames;
  rig.m.increase();
  TEST_ASSERT_EQUAL_INT32_MESSAGE(100, f.value, M(rows, "stays clamped at max"));
  TEST_ASSERT_EQUAL_INT_MESSAGE(frames, rig.rec.frames, M(rows, "no change, no frames"));
  rig.m.next();
  TEST_ASSERT_EQUAL_INT32_MESSAGE(100, f.value, M(rows, "a click at max changes nothing"));
  TEST_ASSERT_EQUAL_INT_MESSAGE(frames, rig.rec.frames, M(rows, "no change, no frames"));

  TEST_ASSERT_TRUE_MESSAGE(rig.m.decrease(), M(rows, "decrease() is consumed"));
  TEST_ASSERT_EQUAL_INT32_MESSAGE(50, f.value, M(rows, "the coarse step comes back down"));
  rig.m.prev();
  TEST_ASSERT_EQUAL_INT32_MESSAGE(45, f.value, M(rows, "a click steps down by one step"));

  rig.m.decrease();
  TEST_ASSERT_EQUAL_INT32_MESSAGE(0, f.value, M(rows, "a coarse step past min clamps to min"));
  frames = rig.rec.frames;
  rig.m.decrease();
  TEST_ASSERT_EQUAL_INT32_MESSAGE(0, f.value, M(rows, "stays clamped at min"));
  TEST_ASSERT_EQUAL_INT_MESSAGE(frames, rig.rec.frames, M(rows, "no change, no frames"));

  TEST_ASSERT_EQUAL_STRING_MESSAGE("*Bright: <0%> Off 12V", rig.rec.text(0),
                                   M(rows, "the clamped value is what the row shows"));
}

// ---------------------------------------------------------------------------
// 6. A list field must reach BOTH ends
// ---------------------------------------------------------------------------

// The extracted clamp wrote `if (newIndex >= size-1) newIndex = size-1`, which cannot tell
// "one past the end" from "the end". Every index is asserted individually in both
// directions, so a clamp that skips or sticks fails here rather than looking plausible.
void aListFieldReachesBothEnds(uint8_t rows) {
  Rig rig(rows);
  rig.addThreeItems();
  rig.m.open();
  rig.m.select();                                   // field 0
  rig.m.select();                                   // field 1, the list
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(1, rig.m.editingField(), M(rows, "editing the list field"));

  const Field& f = rig.m.item(0)->fields[1];
  TEST_ASSERT_EQUAL_INT32_MESSAGE(0, f.value, M(rows, "starts at the FIRST entry"));
  TEST_ASSERT_EQUAL_STRING_MESSAGE(kItem0EditF1, rig.rec.text(0), M(rows, "'Off'"));

  rig.m.next();
  TEST_ASSERT_EQUAL_INT32_MESSAGE(1, f.value, M(rows, "list 0 -> 1"));
  TEST_ASSERT_EQUAL_STRING_MESSAGE("*Bright: 50% <Low> 12V", rig.rec.text(0), M(rows, "'Low'"));

  rig.m.next();
  TEST_ASSERT_EQUAL_INT32_MESSAGE(2, f.value, M(rows, "list 1 -> 2: THE LAST ENTRY IS REACHED"));
  TEST_ASSERT_EQUAL_STRING_MESSAGE("*Bright: 50% <High> 12V", rig.rec.text(0), M(rows, "'High'"));

  int frames = rig.rec.frames;
  rig.m.next();
  TEST_ASSERT_EQUAL_INT32_MESSAGE(2, f.value, M(rows, "clamped at the last entry"));
  TEST_ASSERT_EQUAL_INT_MESSAGE(frames, rig.rec.frames, M(rows, "no change, no frames"));

  // A list ignores the multiplier: its stepMultiplier is 1, so a hold moves one entry.
  rig.m.decrease();
  TEST_ASSERT_EQUAL_INT32_MESSAGE(1, f.value, M(rows, "a held wheel walks a list one at a time"));

  rig.m.prev();
  TEST_ASSERT_EQUAL_INT32_MESSAGE(0, f.value, M(rows, "list 1 -> 0: THE FIRST ENTRY IS REACHED"));
  TEST_ASSERT_EQUAL_STRING_MESSAGE(kItem0EditF1, rig.rec.text(0), M(rows, "'Off' again"));

  frames = rig.rec.frames;
  rig.m.prev();
  TEST_ASSERT_EQUAL_INT32_MESSAGE(0, f.value, M(rows, "clamped at the first entry"));
  TEST_ASSERT_EQUAL_INT_MESSAGE(frames, rig.rec.frames, M(rows, "no change, no frames"));
}

// ---------------------------------------------------------------------------
// 7. Read-only fields
// ---------------------------------------------------------------------------

// Select walks PAST a read-only field rather than around it: field 2 is landed on, rendered
// with the edit markers, and then left. What it does not do is move. (The shipped Carminat
// behaviour, kept: Menu::nextFieldOrExit has no read-only case either. Skipping the field
// entirely would be a change of behaviour, not a port — see the note in the port's
// openIssues.)
void aReadOnlyFieldIsWalkedPastButNeverEdited(uint8_t rows) {
  Rig rig(rows);
  rig.addThreeItems();
  rig.m.open();
  rig.m.select();
  rig.m.select();
  rig.m.select();
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(2, rig.m.editingField(), M(rows, "select() reaches field 2"));
  TEST_ASSERT_EQUAL_STRING_MESSAGE(kItem0EditF2, rig.rec.text(0),
                                   M(rows, "a read-only field is rendered like any other"));

  const Field& f      = rig.m.item(0)->fields[2];
  const int    frames = rig.rec.frames;
  const int    calls  = rig.watch.calls;

  TEST_ASSERT_TRUE_MESSAGE(rig.m.next(), M(rows, "the key is still consumed"));
  TEST_ASSERT_TRUE_MESSAGE(rig.m.prev(), M(rows, "the key is still consumed"));
  TEST_ASSERT_TRUE_MESSAGE(rig.m.increase(), M(rows, "the key is still consumed"));
  TEST_ASSERT_TRUE_MESSAGE(rig.m.decrease(), M(rows, "the key is still consumed"));

  TEST_ASSERT_EQUAL_INT32_MESSAGE(12, f.value, M(rows, "a read-only field ignores the wheel"));
  TEST_ASSERT_EQUAL_INT_MESSAGE(frames, rig.rec.frames, M(rows, "and draws nothing"));
  TEST_ASSERT_EQUAL_INT_MESSAGE(calls, rig.watch.calls, M(rows, "and fires no onChange"));

  rig.m.select();
  TEST_ASSERT_FALSE_MESSAGE(rig.m.isEditing(), M(rows, "select() leaves from a read-only field"));
}

// ---------------------------------------------------------------------------
// 8. Truncation at rowChars — the info-screen case
// ---------------------------------------------------------------------------

// rowChars = 8 is where truncation actually bites: the same content that fits twice over on
// a 26-character Carminat row loses most of itself. The window arithmetic above it is
// unchanged, which is the point.
void rowTextTruncatesAtRowChars(uint8_t rows) {
  Rig rig(rows, 8);
  rig.addThreeItems();
  rig.m.open();

  // "Bright: 50% Off 12V" cut at eight characters. The recorder has already asserted that
  // NO row exceeded the width; this pins where the cut lands.
  TEST_ASSERT_EQUAL_STRING_MESSAGE("Bright: ", rig.rec.text(0), M(rows, "truncated item text"));
  TEST_ASSERT_EQUAL_STRING_MESSAGE(kItem1, rig.rec.text(1), M(rows, "a short label is untouched"));

  // rowText() is public because an adapter that re-lays-out text needs it. It truncates to
  // outSize - 1 whatever the geometry says, and an out-of-range item is an EMPTY string
  // rather than a read past the end of the array.
  char buf[4];
  rig.m.rowText(0, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING_MESSAGE("Bri", buf, M(rows, "rowText honours the caller's buffer"));
  rig.m.rowText(9, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING_MESSAGE("", buf, M(rows, "an out-of-range item is empty, not a crash"));

  // Under edit the leading '*' survives the cut, so "this item is being edited" is still
  // legible on a row too narrow to show the value.
  rig.m.select();
  TEST_ASSERT_EQUAL_STRING_MESSAGE("*Bright:", rig.rec.text(0),
                                   M(rows, "the edit marker is not what gets dropped"));

  // A label short enough to leave room shows BOTH markers at eight characters.
  Rig narrow(rows, 8);
  MenuItem it;
  it.label      = "Lv";
  it.fields[0]  = integerField(7, 0, 9);
  it.fieldCount = 1;
  narrow.m.addItem(it);
  narrow.m.open();
  TEST_ASSERT_EQUAL_STRING_MESSAGE("Lv: 7", narrow.rec.text(0), M(rows, "narrow row, no edit"));
  narrow.m.select();
  TEST_ASSERT_EQUAL_STRING_MESSAGE("*Lv: <7>", narrow.rec.text(0),
                                   M(rows, "both edit markers fit in eight characters"));

  // rowText() reflects the edit state, so the marker is what a narrow buffer keeps.
  rig.m.rowText(0, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING_MESSAGE("*Br", buf, M(rows, "the edited item keeps its marker"));

  // A multi-byte label cut at eight characters must never leave half a UTF-8 sequence on the
  // row: the model transliterates each caller-owned string BEFORE it assembles the row, so
  // the bytes truncation can reach are always 7-bit. (True while AFFA_ENABLE_TRANSLITERATION
  // is on, which is the default and what this environment builds.)
  Rig utf8(rows, 8);
  MenuItem cyr;
  cyr.label = "\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD1\x96\xD1\x82";   // "Привіт"
  utf8.m.addItem(cyr);
  utf8.m.open();
  const char* line = utf8.rec.text(0);
  TEST_ASSERT_TRUE_MESSAGE(std::strlen(line) <= 8, M(rows, "the row still respects rowChars"));
  for (const char* p = line; *p; ++p) {
    TEST_ASSERT_TRUE_MESSAGE(static_cast<unsigned char>(*p) < 0x80,
                             M(rows, "a UTF-8 sequence was cut in half by the truncation"));
  }
}

// ---------------------------------------------------------------------------
// 9. Callbacks
// ---------------------------------------------------------------------------

// onChange fires AFTER the value moved (the callback reads the new value through the item it
// is handed), BEFORE the redraw, exactly once per change, and not at all for a step that
// changed nothing.
void onChangeFiresAfterTheChangeExactlyOnce(uint8_t rows) {
  Rig rig(rows);
  rig.addThreeItems();
  rig.m.open();
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, rig.watch.calls, M(rows, "open() changes no values"));

  rig.m.select();
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, rig.watch.calls, M(rows, "entering edit changes no values"));

  int frames = rig.rec.frames;
  rig.m.next();
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, rig.watch.calls, M(rows, "one change, one callback"));
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, rig.watch.lastField, M(rows, "the field that moved"));
  TEST_ASSERT_EQUAL_INT32_MESSAGE(55, rig.watch.valueAtCall,
                                  M(rows, "the callback saw the NEW value, not the old one"));
  TEST_ASSERT_EQUAL_INT_MESSAGE(frames, rig.watch.framesAtCall,
                                M(rows, "the callback ran before the redraw"));
  TEST_ASSERT_EQUAL_INT_MESSAGE(frames + 1, rig.rec.frames, M(rows, "and the redraw followed"));

  rig.m.next();
  TEST_ASSERT_EQUAL_INT_MESSAGE(2, rig.watch.calls, M(rows, "a second change, a second call"));
  TEST_ASSERT_EQUAL_INT32_MESSAGE(60, rig.watch.valueAtCall, M(rows, "value 60"));

  // Clamped: no change, so no callback.
  rig.m.increase();                                 // 60 -> 100 (max)
  TEST_ASSERT_EQUAL_INT_MESSAGE(3, rig.watch.calls, M(rows, "the clamping step still changed it"));
  rig.m.increase();                                 // already at max
  TEST_ASSERT_EQUAL_INT_MESSAGE(3, rig.watch.calls, M(rows, "a step that changes nothing is silent"));

  // The same hook reports the field index, which is what makes it both per-item and
  // per-field.
  rig.m.select();
  rig.m.next();
  TEST_ASSERT_EQUAL_INT_MESSAGE(4, rig.watch.calls, M(rows, "the list field changed"));
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(1, rig.watch.lastField, M(rows, "reported as field 1"));
  TEST_ASSERT_EQUAL_INT32_MESSAGE(1, rig.watch.valueAtCall, M(rows, "the new list index"));

  // A value pushed in from outside fires the same hook.
  TEST_ASSERT_TRUE_MESSAGE(rig.m.setFieldValue(0, 0, 33), M(rows, "setFieldValue accepted"));
  TEST_ASSERT_EQUAL_INT_MESSAGE(5, rig.watch.calls, M(rows, "setFieldValue fires onChange too"));
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, rig.watch.lastField, M(rows, "for the field it wrote"));
  TEST_ASSERT_EQUAL_INT32_MESSAGE(33, rig.watch.valueAtCall, M(rows, "with the new value"));

  TEST_ASSERT_FALSE_MESSAGE(rig.m.setFieldValue(9, 0, 1), M(rows, "no such item"));
  TEST_ASSERT_FALSE_MESSAGE(rig.m.setFieldValue(1, 0, 1), M(rows, "no such field"));
  TEST_ASSERT_EQUAL_INT_MESSAGE(5, rig.watch.calls, M(rows, "a rejected write fires nothing"));
}

// ---------------------------------------------------------------------------
// 10. The highlight-only path
// ---------------------------------------------------------------------------
//
// IMenuRenderer::highlightOnly() is the seam's fourth call: "only the lit row changed, the
// text and the arrows are as the last frame left them". On the Carminat that is one 07 29 01
// frame against a 96-byte ISO-TP screen, ~14x less bus time, and it is the reason Menu and
// Highlight are different RenderSlots — so the model must raise it in EXACTLY the states the
// old Menu::selectNext called highlightItem() alone, and in no others.
//
// This is a SECOND recorder rather than a flag on the one above, deliberately: Recorder does
// not override highlightOnly() at all, so every one of the thirty-six cases above is also the
// assertion that the DEFAULT still emits every frame it emitted before this call existed.

// Counts both paths. `reply` is what highlightOnly() answers — a display that can move its
// highlight (true) and one that cannot but is still offered the chance (false).
struct HighlightRecorder final : IMenuRenderer {
  HighlightRecorder(bool answer, uint8_t rowsPerFrame)
      : reply(answer), expectRows(rowsPerFrame) {}

  bool    reply;
  uint8_t expectRows;
  int     frames             = 0;
  int     highlightCalls     = 0;
  int     lastHighlightIndex = -1;
  int     selectedRow        = -1;    // the lit row as of the last thing that went out
  uint8_t rowCount           = 0;
  uint8_t mask               = 0xEE;
  char    rows[kMaxRecRows][AFFA_MENU_ROW_MAX] = {{0}};

  void beginFrame(const char* h, uint8_t m) override {
    TEST_ASSERT_NOT_NULL(h);
    mask     = m;
    rowCount = 0;
    ++frames;
  }

  void row(uint8_t index, const char* text, bool selected) override {
    TEST_ASSERT_LESS_THAN_MESSAGE(kMaxRecRows, rowCount, "more rows than the recorder holds");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(rowCount, index, "rows must arrive in order 0..rows-1");
    std::snprintf(rows[rowCount], sizeof(rows[0]), "%s", text);
    ++rowCount;
    if (selected) selectedRow = static_cast<int>(index);
  }

  void endFrame() override {
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(expectRows, rowCount, "a frame is still the whole window");
  }

  bool highlightOnly(uint8_t index) override {
    // The contract the model owes a renderer that takes this path: the previous frame is on
    // the glass, so there is always one.
    TEST_ASSERT_TRUE_MESSAGE(frames > 0, "highlightOnly arrived before any frame");
    ++highlightCalls;
    lastHighlightIndex = static_cast<int>(index);
    if (reply) selectedRow = static_cast<int>(index);   // the renderer drew it itself
    return reply;
  }

  const char* text(uint8_t i) const { return (i < rowCount) ? rows[i] : "<row not emitted>"; }
};

void fillItems(MenuModel& m, uint8_t n) {
  static const char* const kNames[] = {"I0", "I1", "I2", "I3", "I4", "I5", "I6", "I7", "I8"};
  for (uint8_t i = 0; i < n; ++i) {
    MenuItem it;
    it.label = kNames[i];
    TEST_ASSERT_EQUAL_INT(static_cast<int>(i), m.addItem(it));
  }
}

struct HiRig {
  HighlightRecorder rec;
  MenuModel         m;

  HiRig(uint8_t rows, bool reply)
      : rec(reply, rows), m(rec, makeGeometry(rows, 26, false), "MENU") {}
};

// A list LONGER than the window, so both cases are reachable at every geometry: rows-1 moves
// that stay inside the window, then one that makes it scroll.
uint8_t longerThanTheWindow(uint8_t rows) { return static_cast<uint8_t>(rows + 3); }

// Moving the selection inside the window calls highlightOnly() and emits NO full frame.
void aMoveInsideTheWindowIsAHighlightNotAFrame(uint8_t rows) {
  HiRig rig(rows, /*reply=*/true);
  fillItems(rig.m, longerThanTheWindow(rows));
  rig.m.open();

  TEST_ASSERT_EQUAL_INT_MESSAGE(1, rig.rec.frames, M(rows, "open() draws one whole frame"));
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, rig.rec.highlightCalls,
                                M(rows, "open() is a redraw, never a highlight"));

  const int framesAtOpen = rig.rec.frames;
  for (uint8_t r = 1; r < rows; ++r) {
    TEST_ASSERT_TRUE_MESSAGE(rig.m.next(), M(rows, "an open model consumes next()"));
    TEST_ASSERT_EQUAL_INT_MESSAGE(framesAtOpen, rig.rec.frames,
                                  M(rows, "a move inside the window drew NO full frame"));
    TEST_ASSERT_EQUAL_INT_MESSAGE(r, rig.rec.highlightCalls,
                                  M(rows, "exactly one highlight per in-window move"));
    TEST_ASSERT_EQUAL_INT_MESSAGE(r, rig.rec.lastHighlightIndex,
                                  M(rows, "the ROW INDEX that is now lit"));
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(r, rig.m.selectedRow(), M(rows, "and it is the model's row"));
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, rig.m.topIndex(), M(rows, "the window has not moved"));
  }

  // Back up the same way. The window still has not moved, so every step is a highlight.
  for (uint8_t r = static_cast<uint8_t>(rows - 1); r > 0; --r) {
    const int hi = rig.rec.highlightCalls;
    TEST_ASSERT_TRUE_MESSAGE(rig.m.prev(), M(rows, "an open model consumes prev()"));
    TEST_ASSERT_EQUAL_INT_MESSAGE(framesAtOpen, rig.rec.frames,
                                  M(rows, "walking back inside the window drew no frame either"));
    TEST_ASSERT_EQUAL_INT_MESSAGE(hi + 1, rig.rec.highlightCalls, M(rows, "one highlight"));
    TEST_ASSERT_EQUAL_INT_MESSAGE(r - 1, rig.rec.lastHighlightIndex, M(rows, "moving up a row"));
  }
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, rig.m.selectedIndex(), M(rows, "back at the first item"));

  // A key that moves nothing is neither: no wrap, no frame, and no highlight for a highlight
  // that would say what the panel already shows.
  const int frames = rig.rec.frames;
  const int hi     = rig.rec.highlightCalls;
  TEST_ASSERT_TRUE_MESSAGE(rig.m.prev(), M(rows, "prev() at the top is still consumed"));
  TEST_ASSERT_EQUAL_INT_MESSAGE(frames, rig.rec.frames, M(rows, "and draws no frame"));
  TEST_ASSERT_EQUAL_INT_MESSAGE(hi, rig.rec.highlightCalls, M(rows, "and no highlight either"));
}

// Crossing the window boundary emits a full frame and does NOT call highlightOnly(): every
// row's text changes when the window scrolls, which is precisely what the cheap path may not
// assume.
void aMoveAcrossTheBoundaryIsAFrameNotAHighlight(uint8_t rows) {
  HiRig rig(rows, /*reply=*/true);
  fillItems(rig.m, longerThanTheWindow(rows));
  rig.m.open();

  for (uint8_t r = 1; r < rows; ++r) rig.m.next();     // fill the window, highlights only
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(static_cast<uint8_t>(rows - 1), rig.m.selectedRow(),
                                  M(rows, "the selection is on the last row of the window"));

  int frames = rig.rec.frames;
  int hi     = rig.rec.highlightCalls;

  TEST_ASSERT_TRUE_MESSAGE(rig.m.next(), M(rows, "next() at the bottom of the window"));
  TEST_ASSERT_EQUAL_INT_MESSAGE(frames + 1, rig.rec.frames,
                                M(rows, "the window scrolled, so a WHOLE frame went out"));
  TEST_ASSERT_EQUAL_INT_MESSAGE(hi, rig.rec.highlightCalls,
                                M(rows, "and highlightOnly was NOT called"));
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(1, rig.m.topIndex(), M(rows, "the window moved by one item"));
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(static_cast<uint8_t>(rows - 1), rig.m.selectedRow(),
                                  M(rows, "and the selection stayed on the last row"));
  TEST_ASSERT_EQUAL_STRING_MESSAGE("I1", rig.rec.text(0),
                                   M(rows, "the frame carries the NEW window's text"));
  TEST_ASSERT_EQUAL_INT_MESSAGE(rows - 1, rig.rec.selectedRow,
                                M(rows, "with the right row lit"));

  // Walk back up to row 0 — highlights — and then one more, which scrolls the window the
  // other way and is a frame again.
  for (uint8_t r = static_cast<uint8_t>(rows - 1); r > 0; --r) rig.m.prev();
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, rig.m.selectedRow(), M(rows, "at the top row of the window"));
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(1, rig.m.topIndex(), M(rows, "the window is still where it was"));

  frames = rig.rec.frames;
  hi     = rig.rec.highlightCalls;
  TEST_ASSERT_TRUE_MESSAGE(rig.m.prev(), M(rows, "prev() at the top of the window"));
  TEST_ASSERT_EQUAL_INT_MESSAGE(frames + 1, rig.rec.frames,
                                M(rows, "scrolling up is a whole frame too"));
  TEST_ASSERT_EQUAL_INT_MESSAGE(hi, rig.rec.highlightCalls, M(rows, "and not a highlight"));
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, rig.m.topIndex(), M(rows, "the window is back at the top"));
  TEST_ASSERT_EQUAL_STRING_MESSAGE("I0", rig.rec.text(0), M(rows, "showing item 0 again"));
}

// A renderer that returns false from highlightOnly() gets a full frame EVERY time — the
// fallback is not a fast path with a hole in it, it is the behaviour that shipped.
void aRendererThatDeclinesGetsAFullFrameEveryTime(uint8_t rows) {
  HiRig rig(rows, /*reply=*/false);
  fillItems(rig.m, longerThanTheWindow(rows));
  rig.m.open();

  for (uint8_t r = 1; r < rows; ++r) {
    const int frames = rig.rec.frames;
    TEST_ASSERT_TRUE_MESSAGE(rig.m.next(), M(rows, "next() consumed"));
    TEST_ASSERT_EQUAL_INT_MESSAGE(frames + 1, rig.rec.frames,
                                  M(rows, "declining the cheap path means a whole frame"));
    TEST_ASSERT_EQUAL_INT_MESSAGE(r, rig.rec.highlightCalls,
                                  M(rows, "it was offered the cheap path first"));
    TEST_ASSERT_EQUAL_INT_MESSAGE(r, rig.rec.selectedRow,
                                  M(rows, "and the frame lights the new row"));
    TEST_ASSERT_EQUAL_STRING_MESSAGE("I0", rig.rec.text(0),
                                     M(rows, "the window did not move, so row 0 is item 0"));
  }

  // The scroll that follows is a frame as well, with no offer made: identical cost to the
  // in-window move, which is exactly the regression this call exists to prevent.
  const int frames = rig.rec.frames;
  const int hi     = rig.rec.highlightCalls;
  rig.m.next();
  TEST_ASSERT_EQUAL_INT_MESSAGE(frames + 1, rig.rec.frames, M(rows, "the scroll is a frame"));
  TEST_ASSERT_EQUAL_INT_MESSAGE(hi, rig.rec.highlightCalls, M(rows, "with nothing offered"));
}

// Only a SELECTION move takes the cheap path. Entering edit, changing a value and leaving
// edit all change the row's text ('*', '<>', the value itself) and must redraw, even though
// the selection has not moved a row.
void onlyASelectionMoveTakesTheCheapPath(uint8_t rows) {
  HiRig rig(rows, /*reply=*/true);
  MenuItem it;
  it.label      = "Bright";
  it.fields[0]  = integerField(50, 0, 100, 5, 10, "%");
  it.fields[1]  = listField(kModes, 3, 0);
  it.fieldCount = 2;
  TEST_ASSERT_EQUAL_INT(0, rig.m.addItem(it));
  MenuItem second;
  second.label = "Second";
  TEST_ASSERT_EQUAL_INT(1, rig.m.addItem(second));
  rig.m.open();

  const int hi = rig.rec.highlightCalls;
  int frames   = rig.rec.frames;

  rig.m.select();                                    // enter edit: the row grows '*' and '<>'
  TEST_ASSERT_EQUAL_INT_MESSAGE(++frames, rig.rec.frames, M(rows, "entering edit redraws"));
  rig.m.next();                                      // 50 -> 55: the value on the row changed
  TEST_ASSERT_EQUAL_INT_MESSAGE(++frames, rig.rec.frames, M(rows, "a value change redraws"));
  rig.m.select();                                    // field 0 -> 1: the markers moved
  TEST_ASSERT_EQUAL_INT_MESSAGE(++frames, rig.rec.frames, M(rows, "advancing a field redraws"));
  rig.m.select();                                    // leave edit: the markers went away
  TEST_ASSERT_EQUAL_INT_MESSAGE(++frames, rig.rec.frames, M(rows, "leaving edit redraws"));
  TEST_ASSERT_EQUAL_INT_MESSAGE(hi, rig.rec.highlightCalls,
                                M(rows, "not one of those was a highlight-only move"));

  // And the selection move that follows is, which is the difference the whole call is about.
  TEST_ASSERT_TRUE(rig.m.next());
  TEST_ASSERT_EQUAL_INT_MESSAGE(frames, rig.rec.frames, M(rows, "the wheel drew no frame"));
  TEST_ASSERT_EQUAL_INT_MESSAGE(hi + 1, rig.rec.highlightCalls, M(rows, "it moved the highlight"));
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, rig.rec.lastHighlightIndex, M(rows, "onto row 1"));
}

// ---------------------------------------------------------------------------
// Not parameterised: the geometry itself
// ---------------------------------------------------------------------------

void test_a_closed_model_consumes_nothing(void) {
  // The false return is the contract Menu::handleKey had, and it is what lets an application
  // own its own open gesture: everything the model did not consume is still the caller's.
  Rig rig(2);
  rig.addThreeItems();
  TEST_ASSERT_FALSE(rig.m.next());
  TEST_ASSERT_FALSE(rig.m.prev());
  TEST_ASSERT_FALSE(rig.m.increase());
  TEST_ASSERT_FALSE(rig.m.decrease());
  TEST_ASSERT_FALSE(rig.m.select());
  TEST_ASSERT_FALSE(rig.m.back());
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, rig.rec.frames, "a closed model draws nothing");

  rig.m.open();
  TEST_ASSERT_TRUE(rig.m.isOpen());
  TEST_ASSERT_TRUE(rig.m.back());
  TEST_ASSERT_FALSE_MESSAGE(rig.m.isOpen(), "back() closes the menu");
  TEST_ASSERT_FALSE_MESSAGE(rig.m.next(), "and the model is closed again");
}

void test_the_geometry_is_sanitised_once_at_construction(void) {
  Recorder rec(1, static_cast<uint8_t>(AFFA_MENU_ROW_MAX - 1));
  MenuModel m(rec, makeGeometry(0, 200, false), "H");
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(1, m.geometry().rows, "rows = 0 is raised to 1, not divided by");
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(AFFA_MENU_ROW_MAX - 1, m.geometry().rowChars,
                                  "rowChars is clamped to what the fixed row buffer holds");

  MenuItem it;
  it.label = "Bright";
  m.addItem(it);
  m.open();
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(1, rec.rowCount, "a one-row window emits one row");
  TEST_ASSERT_EQUAL_STRING("Bright", rec.text(0));
}

// THE DEFAULT MUST BE CORRECT, NOT FAST. Recorder does not override highlightOnly() at all —
// it is the renderer somebody writes in twenty lines against the three pure virtuals, exactly
// as docs/MENU-WIDGET.md §5 shows — and it must still see every frame it would have seen
// before the fourth call existed. That is why highlightOnly() is not pure virtual, and this
// is the case that would fail if the base ever returned true.
void test_the_default_highlightOnly_falls_back_to_a_full_frame(void) {
  Rig rig(2);
  rig.addFillerItems(4);
  rig.m.open();

  const int frames = rig.rec.frames;
  TEST_ASSERT_TRUE(rig.m.next());
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(1, rig.m.selectedRow(), "the move was inside the window");
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, rig.m.topIndex(), "and the window did not move");
  TEST_ASSERT_EQUAL_INT_MESSAGE(frames + 1, rig.rec.frames,
                                "a renderer that says nothing still gets the whole frame");
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, rig.rec.selectedRow, "with the new row lit");
  TEST_ASSERT_EQUAL_STRING_MESSAGE("I0", rig.rec.text(0), "and the same items on it");
}

// ---------------------------------------------------------------------------
// Every behavioural test above, at every geometry.
// ---------------------------------------------------------------------------

#define AT_EVERY_GEOMETRY(fn)                          \
  void test_##fn##_rows2(void) { fn(2); }              \
  void test_##fn##_rows3(void) { fn(3); }              \
  void test_##fn##_rows6(void) { fn(6); }

AT_EVERY_GEOMETRY(windowArithmetic)
AT_EVERY_GEOMETRY(scrollMaskAtEachPosition)
AT_EVERY_GEOMETRY(noArrowsWhenTheListFits)
AT_EVERY_GEOMETRY(bothArrowsInTheMiddleOfALongList)
AT_EVERY_GEOMETRY(theEndsStopWithoutWrap)
AT_EVERY_GEOMETRY(theEndsWrapWhenTheGeometrySaysSo)
AT_EVERY_GEOMETRY(selectWalksTheFieldsThenExits)
AT_EVERY_GEOMETRY(coarseStepAndClamping)
AT_EVERY_GEOMETRY(aListFieldReachesBothEnds)
AT_EVERY_GEOMETRY(aReadOnlyFieldIsWalkedPastButNeverEdited)
AT_EVERY_GEOMETRY(rowTextTruncatesAtRowChars)
AT_EVERY_GEOMETRY(onChangeFiresAfterTheChangeExactlyOnce)
AT_EVERY_GEOMETRY(aMoveInsideTheWindowIsAHighlightNotAFrame)
AT_EVERY_GEOMETRY(aMoveAcrossTheBoundaryIsAFrameNotAHighlight)
AT_EVERY_GEOMETRY(aRendererThatDeclinesGetsAFullFrameEveryTime)
AT_EVERY_GEOMETRY(onlyASelectionMoveTakesTheCheapPath)

}  // namespace

void setUp(void) {}
void tearDown(void) {}

int main(int, char**) {
  UNITY_BEGIN();

  RUN_TEST(test_windowArithmetic_rows2);
  RUN_TEST(test_windowArithmetic_rows3);
  RUN_TEST(test_windowArithmetic_rows6);

  RUN_TEST(test_scrollMaskAtEachPosition_rows2);
  RUN_TEST(test_scrollMaskAtEachPosition_rows3);
  RUN_TEST(test_scrollMaskAtEachPosition_rows6);

  RUN_TEST(test_noArrowsWhenTheListFits_rows2);
  RUN_TEST(test_noArrowsWhenTheListFits_rows3);
  RUN_TEST(test_noArrowsWhenTheListFits_rows6);

  RUN_TEST(test_bothArrowsInTheMiddleOfALongList_rows2);
  RUN_TEST(test_bothArrowsInTheMiddleOfALongList_rows3);
  RUN_TEST(test_bothArrowsInTheMiddleOfALongList_rows6);

  RUN_TEST(test_theEndsStopWithoutWrap_rows2);
  RUN_TEST(test_theEndsStopWithoutWrap_rows3);
  RUN_TEST(test_theEndsStopWithoutWrap_rows6);

  RUN_TEST(test_theEndsWrapWhenTheGeometrySaysSo_rows2);
  RUN_TEST(test_theEndsWrapWhenTheGeometrySaysSo_rows3);
  RUN_TEST(test_theEndsWrapWhenTheGeometrySaysSo_rows6);

  RUN_TEST(test_selectWalksTheFieldsThenExits_rows2);
  RUN_TEST(test_selectWalksTheFieldsThenExits_rows3);
  RUN_TEST(test_selectWalksTheFieldsThenExits_rows6);

  RUN_TEST(test_coarseStepAndClamping_rows2);
  RUN_TEST(test_coarseStepAndClamping_rows3);
  RUN_TEST(test_coarseStepAndClamping_rows6);

  RUN_TEST(test_aListFieldReachesBothEnds_rows2);
  RUN_TEST(test_aListFieldReachesBothEnds_rows3);
  RUN_TEST(test_aListFieldReachesBothEnds_rows6);

  RUN_TEST(test_aReadOnlyFieldIsWalkedPastButNeverEdited_rows2);
  RUN_TEST(test_aReadOnlyFieldIsWalkedPastButNeverEdited_rows3);
  RUN_TEST(test_aReadOnlyFieldIsWalkedPastButNeverEdited_rows6);

  RUN_TEST(test_rowTextTruncatesAtRowChars_rows2);
  RUN_TEST(test_rowTextTruncatesAtRowChars_rows3);
  RUN_TEST(test_rowTextTruncatesAtRowChars_rows6);

  RUN_TEST(test_onChangeFiresAfterTheChangeExactlyOnce_rows2);
  RUN_TEST(test_onChangeFiresAfterTheChangeExactlyOnce_rows3);
  RUN_TEST(test_onChangeFiresAfterTheChangeExactlyOnce_rows6);

  RUN_TEST(test_aMoveInsideTheWindowIsAHighlightNotAFrame_rows2);
  RUN_TEST(test_aMoveInsideTheWindowIsAHighlightNotAFrame_rows3);
  RUN_TEST(test_aMoveInsideTheWindowIsAHighlightNotAFrame_rows6);

  RUN_TEST(test_aMoveAcrossTheBoundaryIsAFrameNotAHighlight_rows2);
  RUN_TEST(test_aMoveAcrossTheBoundaryIsAFrameNotAHighlight_rows3);
  RUN_TEST(test_aMoveAcrossTheBoundaryIsAFrameNotAHighlight_rows6);

  RUN_TEST(test_aRendererThatDeclinesGetsAFullFrameEveryTime_rows2);
  RUN_TEST(test_aRendererThatDeclinesGetsAFullFrameEveryTime_rows3);
  RUN_TEST(test_aRendererThatDeclinesGetsAFullFrameEveryTime_rows6);

  RUN_TEST(test_onlyASelectionMoveTakesTheCheapPath_rows2);
  RUN_TEST(test_onlyASelectionMoveTakesTheCheapPath_rows3);
  RUN_TEST(test_onlyASelectionMoveTakesTheCheapPath_rows6);

  RUN_TEST(test_a_closed_model_consumes_nothing);
  RUN_TEST(test_the_geometry_is_sanitised_once_at_construction);
  RUN_TEST(test_the_default_highlightOnly_falls_back_to_a_full_frame);

  return UNITY_END();
}
