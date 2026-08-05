#include "CarminatDisplay.h"

// The ENTIRE body is inside the panel gate, so a build that did not select Carminat
// compiles this translation unit to an empty object file — that, plus -ffunction-sections
// and --gc-sections, is what makes the flash cost of an unselected panel actually zero.
#if AFFA_PANEL_CARMINAT

#include "../util/AffaText.h"
#include "../util/AffaLog.h"
#if AFFA_ENABLE_ISOTP_RX
#  include "../proto/ScreenDecode.h"
#endif
#include <cstring>

namespace affa {
namespace {

constexpr const char* kTag = "CARM";

// Transliterate into a stack buffer. NO Arduino String anywhere on the send path: the
// panel charset cannot render UTF-8, losing this looks like garbage on the glass rather
// than a compile error, and it is applied at the single choke point of every builder.
// `in == nullptr` yields an empty string.
struct Ascii {
  char buf[AFFA_TEXT_MAX];
  size_t len;
  explicit Ascii(const char* in) { len = toAscii(in, buf, sizeof(buf)); }
  const char* c() const { return buf; }
};

}  // namespace

// File-local, and only in this .cpp: the builders below are dense with wire constants and
// spelling `carminat::` on every one of them buries the bytes that matter.
using namespace carminat;

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

CarminatDisplay::CarminatDisplay(ICanLink& link, IClock& clock,
                                 carminat::CarminatHelloProfile hello)
    : AffaDisplayBase(link, clock, carminat::syncProfile(hello), carminat::kFuncIds,
                       carminat::kFuncCount)
#if AFFA_ENABLE_MENU
      ,
      // The AffaDisplayBase subobject is fully constructed before any member is, so binding
      // the renderer's IPanel& to *this here is well defined. The model then binds to the
      // renderer and the controller to the model, which is why they are declared in that
      // order.
      _menuRenderer(*this),
      _menu(_menuRenderer, CarminatMenuRenderer::geometry(), "Main Menu"),
      _menuCtrl(_menu)
#endif
{
#if AFFA_ENABLE_MENU
  initializeMenu();
#endif
}

bool CarminatDisplay::supports(Feature f) const {
  switch (f) {
    case Feature::Text:        return true;
    case Feature::Time:        return true;
    case Feature::Power:       return true;
    case Feature::Menu:        return AFFA_ENABLE_MENU != 0;
    case Feature::Popup:       return AFFA_ENABLE_POPUP != 0;
    case Feature::Fullscreen:  return AFFA_ENABLE_FULLSCREEN != 0;
    case Feature::ConfirmBox:  return AFFA_ENABLE_CONFIRMBOX != 0;
    case Feature::InfoPopup:   return AFFA_ENABLE_INFOPOPUP != 0;
    case Feature::NavBitmap:   return AFFA_ENABLE_NAV != 0;
    case Feature::KeyTx:       return true;                        // 0x1C1
    case Feature::RadioText:   return AFFA_ENABLE_ISOTP_RX != 0;
  }
  return false;
}

Result CarminatDisplay::submit(uint16_t funcId, const uint8_t* data, uint8_t len,
                               RenderSlot slot, bool coalesce, Priority priority,
                               bool reassertAfterSession) {
  TxOptions opt;
  opt.slot     = slot;
  opt.coalesce = coalesce;
  opt.priority = priority;
  opt.reassertAfterSession = reassertAfterSession;
  // kNoTicket carries its reason in lastResult(); a render call returns that reason rather
  // than a bare boolean, because "queued" and "rejected because the panel is not synced"
  // are different answers.
  return (enqueue(funcId, data, len, opt) == kNoTicket) ? lastResult() : Result::Ok;
}

// ---------------------------------------------------------------------------
// Receive
// ---------------------------------------------------------------------------

// Called for frames the base did not consume — never for a sync frame, never for an ACK,
// and never for our own echo (fromSelf is dropped before this, before the ACK matcher and
// before the auto-ACK). The generic 0x74 auto-ACK for this frame has ALREADY gone out;
// that is the legacy order, and the panel expects `RX 1C1 .. -> TX 5C1 74 ..`.
bool CarminatDisplay::onFrame(const Frame& f) {
  if (f.id != carminat::kIdKeyPressed) return false;

  Key     k;
  KeyEdge e;
  // decodeKeyFrame carries the `03 89` guard, and it is load-bearing: 0x1C1 also carries
  // `70 A3..`, `02 64 0F A3..` and `05 63 "0037"` from the panel, out of which a decoder
  // without the guard invents keys 0x640F and 0x3030.
  if (!decodeKeyFrame(f, k, e)) return false;

  AFFA_LOGD(kTag, "key 0x%04X %s", static_cast<unsigned>(k),
            e == KeyEdge::Hold ? "hold" : "click");
  routeKey(k, e);
  return true;
}

#if AFFA_ENABLE_ISOTP_RX
// The 0x74 (full window) and 0x77 (windowed radio text) screens are the two that carry a
// text line; everything else on 0x151 — the 0x21 menu, the 0x76 info row — is a screen, and
// returning false leaves it to a subscribe() that wants the raw bytes.
//
// Text runs from kOffWinText to the last byte RECEIVED, not to the declared length: setText
// declares 0x0E and transmits 20 (WIRE-SPEC §8.1), so trusting the declaration here would
// cut the string short. asciiz() stops at the first NUL and drops the panel filler.
bool CarminatDisplay::decodeText(const uint8_t* payload, uint8_t len, char* out,
                                 uint8_t outSize) const {
  if (len < screen::kWinTextMinLen) return false;
  if (payload[2] != screen::kWinTextCmdFull && payload[2] != screen::kWinTextCmdWindow)
    return false;
  screen::asciiz(payload, len, screen::kOffWinText, static_cast<uint8_t>(len - 1), out,
                 outSize);
  return true;
}
#endif

void CarminatDisplay::onPoll() {
#if AFFA_ENABLE_MENU
  // The active page's own tick. It is called once per poll() and is NOT a period: a page
  // that needs one compares against its own clock. Nothing here counts calls.
  _menuCtrl.tickCurrentPage();
#endif
}

// ---------------------------------------------------------------------------
// Menu seams
// ---------------------------------------------------------------------------

#if AFFA_ENABLE_MENU

void CarminatDisplay::initializeMenu() {
  // EMPTY, on purpose. What the items are called, what a field means, what happens when
  // it changes and whether that change is written to NVS is the application's business —
  // it fills the menu through getMenu(). The extracted version read Preferences here and
  // hard-wired a car's diagnostics list.
  _menu.onClose(&CarminatDisplay::onMenuClosed, this);
}

// The OEM convention when the menu closes: the panel goes back to the source banner. It
// ships as a default because it is the convention, and it is replaceable because it is
// policy — MenuModel::onClose() overwrites it.
void CarminatDisplay::onMenuClosed(void* ctx) {
  // Deliberately dropped: this is a CloseCb with nowhere to report to, and the banner is
  // cosmetic. An application that needs the verdict installs its own onClose().
  (void)static_cast<CarminatDisplay*>(ctx)->setText("RENAULT", 0);
}

bool CarminatDisplay::menuOpen() const { return _menu.isOpen(); }

bool CarminatDisplay::openMenu() {
  // An active page owns every key, including the hotkey. Refusing here is what makes the
  // base fall through to routeKeyToMenu(), which hands the key to the page.
  if (_menuCtrl.currentPage() != nullptr) return false;
  _menu.open();
  return _menu.isOpen();          // false on an empty menu, which never opens
}

bool CarminatDisplay::routeKeyToMenu(Key k, KeyEdge e) { return _menuCtrl.routeKey(k, e); }

#endif  // AFFA_ENABLE_MENU

// setText — 0x151, docs/WIRE-SPEC.md §8.1
//
// DO NOT "FIX" THE DECLARED LENGTH. 0x0E says 14 content bytes; we transmit 20. The panel
// consumes only the declared 14 — header plus the first EIGHT text bytes, hence "max 7
// characters shown". Observed working behaviour; shortening it is an untested change.
Result CarminatDisplay::setText(const char* text, uint8_t digit) {
  (void)digit;                      // no digit addressing here; it is UpdateList's signature
  const Ascii t(text);

  uint8_t d[8 + kTextCells];
  uint8_t n  = 0;
  d[n++] = kPciFirstFrame;
  d[n++] = 0x0E;                    // declared content length = 14 (see above)
  d[n++] = kCmdText;                // 0x77 windowed. 0x74 is the full-window overlay, and
                                    // sending 0x77 while no window is applied has been
                                    // observed to leave the main screen frozen.
  d[n++] = kIconsNone;              // 0x55 RDS icon bank: none
  d[n++] = kIconBank2;              // 0x55 fixed, meaning unknown
  d[n++] = kSrcIconNone;            // 0xFF: no source label
  d[n++] = kFormatPlain;            // 0x60: plain ASCII
  d[n++] = kControlByte;            // always 0x01

  for (uint8_t i = 0; i < kTextCells; ++i)   // NUL-padded, truncated at 14
    d[n++] = (i < t.len) ? static_cast<uint8_t>(t.buf[i]) : 0x00;

  return submit(kIdSetText, d, n, RenderSlot::Text);
}

// ---------------------------------------------------------------------------
// setTime — 0x151, §8.2.  05 56 <h> <h> <m> <m> 00 00
// ---------------------------------------------------------------------------
Result CarminatDisplay::setTime(const char* hhmm) {
  char t[16];
  const size_t n = toAscii(hhmm, t, sizeof(t));
  // The legacy builder indexed clock[0..3] unconditionally and would read past a short
  // string. Four ASCII digits, "HHMM", or nothing.
  if (n < kClockDigits) return Result::BadArgument;

  const uint8_t d[8] = {0x05,                             // single frame, SF_DL = 5
                        kCmdClock,                        // 'V'
                        static_cast<uint8_t>(t[0]), static_cast<uint8_t>(t[1]),
                        static_cast<uint8_t>(t[2]), static_cast<uint8_t>(t[3]),
                        0x00, 0x00};
  return submit(kIdSetText, d, sizeof(d), RenderSlot::Clock);
}

// setPower — 0x151, WIRE-SPEC §8.3.  03 52 <09|00> 00 00 00 00 00
//
// DO NOT UNIFY THIS WITH UpdateList: Carminat's captured control byte is 0x03, whereas
// UpdateList uses 0x04 for its distinct control payload.
Result CarminatDisplay::setPower(bool on) {
  // The monitor-proven AFFA3 form is a complete zero-padded 8-byte frame. The historical
  // source used FF FF in bytes 3/4; keep that variant documented, but do not inject it
  // into this captured profile.
  const uint8_t d[8] = {0x03, kCmdCtrl, on ? kDisplayCtrlOn : kDisplayCtrlOff,
                        0x00, 0x00, 0x00, 0x00, 0x00};
  return submit(kIdDisplayCtrl, d, sizeof(d), RenderSlot::Control,
                /*coalesce=*/true, Priority::Urgent, /*reassertAfterSession=*/true);
}

// highlightItem — 0x151, WIRE-SPEC §8.4.  07 29 01 <7E|7F> 80 00 00 00
//
// Its OWN RenderSlot, not Menu's: a highlight must not replace a pending full redraw, and
// a full redraw must not replace a pending highlight.
Result CarminatDisplay::highlightItem(uint8_t row) {
  if (row > 1) return Result::BadArgument;   // the panel renders exactly two rows
  const uint8_t d[8] = {0x07, kCmdHilite, 0x01,
                        row == 0 ? kRowTagTop : kRowTagBottom,
                        0x80, 0x00, 0x00, 0x00};
  return submit(kIdSetText, d, sizeof(d), RenderSlot::Highlight);
}

// showMenu — 0x151, WIRE-SPEC §8.5. The 96-byte two-row window, always exactly 96.
//
// DECLARED LENGTH 0x5A = 90, BUILT 94 — AND THAT IS CORRECT. The panel stops as soon as it
// holds the declared count (6 + 12*7 = 90), so hardware DONEs after PCI 0x2C at 13 frames
// while the self-ACK emulator runs to 14. row1 therefore has 26 usable characters even
// though the builder accepts 30. Change neither number without panel testing.
Result CarminatDisplay::showMenu(const char* header, const char* row0, const char* row1,
                                 uint8_t scrollIndicator) {
  const Ascii h(header);
  const Ascii r0(row0);
  const Ascii r1(row1);

  uint8_t p[96];
  std::memset(p, 0x00, sizeof(p));
  uint8_t idx = 0;

  p[idx++] = kPciFirstFrame;    // 0x10
  p[idx++] = 0x5A;              // declared content length = 90 (see above)
  p[idx++] = kCmdScreen;        // 0x21
  p[idx++] = kScreenWindowed;   // 0x01 windowed (0x05 = fullscreen)
  p[idx++] = kRowTagTop;        // 0x7E
  p[idx++] = 0x80;
  p[idx++] = 0x00;
  p[idx++] = 0x00;

  p[idx++] = 0x82;
  p[idx++] = 0xFF;
  p[idx++] = scrollIndicator;   // 0x00 none, 0x07 up, 0x0B down, 0x0C both

  for (uint8_t i = 0; i < kMenuHeaderMax && h.buf[i]; ++i)
    p[idx++] = static_cast<uint8_t>(h.buf[i]);
  while (idx < 37) p[idx++] = 0x00;

  p[idx++] = 0x00;              // row-0 index
  p[idx++] = kRowTagTop;        // row-0 tag — the same byte highlightItem(0) sends
  for (const char* s = r0.c(); *s && idx < 64; ++s) p[idx++] = static_cast<uint8_t>(*s);
  while (idx < 64) p[idx++] = 0x00;

  p[idx++] = 0x01;              // row-1 index
  p[idx++] = kRowTagBottom;     // row-1 tag
  for (const char* s = r1.c(); *s && idx < 96; ++s) p[idx++] = static_cast<uint8_t>(*s);
  while (idx < 96) p[idx++] = 0x00;

  AFFA_LOGT(kTag, "showMenu %u bytes, scroll 0x%02X", static_cast<unsigned>(idx),
            static_cast<unsigned>(scrollIndicator));
  return submit(kIdSetText, p, idx, RenderSlot::Menu);
}

// ---------------------------------------------------------------------------
// showPopupText / hidePopup — 0x151, §8.7 / §8.8
// ---------------------------------------------------------------------------
#if AFFA_ENABLE_POPUP

// The transient overlay ("VOL 28"): setText's wire family with mode 0x74, and here the
// declared length IS correct.
//
// THE POPUP IS THE ONE TRUE OVERLAY ON THIS PANEL, and its lifetime belongs to the
// APPLICATION. Measured 2026-07-28 on a real Carminat: with a popup up, the screen
// underneath was redrawn (BASE-A -> BASE-B, visible at the line ends the overlay does not
// cover) and the popup stayed on top. No auto-revert was observed — hidePopup() is what
// clears it, so an application that shows one owns a deadline to close it.
//
// Hence RenderSlot::Popup never coalescing against RenderSlot::Text: they are two
// independent layers, not two versions of one screen.
Result CarminatDisplay::showPopupText(const char* text, uint8_t icon, uint8_t srcIcon,
                                      uint8_t fmt) {
  const Ascii t(text);

  // 8 cells minimum keeps the captured "VOL 28" byte-identical (declared 0x0E).
  uint8_t cells = static_cast<uint8_t>(t.len);
  if (cells < kPopupCellsMin) cells = kPopupCellsMin;
  if (cells > kPopupCellsMax) cells = kPopupCellsMax;

  uint8_t d[8 + kPopupCellsMax];
  uint8_t n = 0;
  d[n++] = kPciFirstFrame;
  d[n++] = static_cast<uint8_t>(6 + cells);   // declared content length: header + cells
  d[n++] = kCmdPopup;                         // 0x74 full-window overlay
  d[n++] = icon;                              // left icon set (capture: 0x09)
  d[n++] = kIconBank2;                        // 0x55 fixed
  d[n++] = srcIcon;                           // source/right icon (capture: 0xFF none)
  d[n++] = fmt;                               // text format (capture: 0x60 plain)
  d[n++] = kControlByte;
  // SPACE-padded here, unlike setText, which is NUL-padded. Both are as captured.
  for (uint8_t i = 0; i < cells; ++i)
    d[n++] = (i < t.len) ? static_cast<uint8_t>(t.buf[i]) : ' ';

  return submit(kIdSetText, d, n, RenderSlot::Popup);
}

// 02 54 03 — the close-window command, independently observed FROM the OEM head unit.
Result CarminatDisplay::hidePopup() {
  const uint8_t d[3] = {0x02, kCmdClose, 0x03};
  return submit(kIdSetText, d, sizeof(d), RenderSlot::Popup);
}

#else

Result CarminatDisplay::showPopupText(const char* text, uint8_t icon, uint8_t srcIcon,
                                      uint8_t fmt) {
  (void)text; (void)icon; (void)srcIcon; (void)fmt;
  return Result::NotSupported;
}
Result CarminatDisplay::hidePopup() { return Result::NotSupported; }

#endif  // AFFA_ENABLE_POPUP

// ---------------------------------------------------------------------------
// showFullscreenText / hideFullscreenText — 0x151, §8.6 / §8.8
// ---------------------------------------------------------------------------
#if AFFA_ENABLE_FULLSCREEN

// The same 0x21 screen command as showMenu with mode 0x05: three equal lines on the whole
// glass, no row tags.
//
// IT IS NOT AN OVERLAY AND IT NEEDS NO TEARDOWN. Measured on a real Carminat, 2026-07-28:
// a plain setText() over a live fullscreen REPLACED it, with no hideFullscreenText() sent.
// Every full-screen-class render — showMenu, showFullscreenText, setText — simply replaces
// the last one, so a fullscreen can be animated at the rate the wire sustains (~190 ms per
// screen, 14 frames, every one acknowledged) with no close in between.
//
// The POPUP is the true overlay: it survives a redraw of the screen underneath, the redraw
// still lands, and only hidePopup() clears it. WIRE-SPEC.md §8.6 had these two the wrong
// way round until that bench session; if you are holding a comment that says a fullscreen
// owns the glass until closed, it is the old one.
//
#if AFFA_ENABLE_NAV
// The navigation pane. `21 0B` is kCmdScreen with its own screen type, on kIdNav instead of
// kIdSetText — the same command family as the menu and the fullscreen screen, which is why
// the header opens the way it does.
//
// THE HEADER IS COPIED, THE IMAGE IS BORROWED. That split is the whole reason this costs no
// RAM at rest: 16 bytes of stack per call, and 288 bytes that stay in the caller's flash.
// See AffaDisplayBase::enqueueSplit.
Result CarminatDisplay::showNavBitmap(const uint8_t* bitmap) {
  if (!bitmap) return Result::BadArgument;

  uint8_t prefix[2 + sizeof(kNavHeader)];
  constexpr uint16_t kDeclared = sizeof(kNavHeader) + kNavBitmapBytes;   // 302
  static_assert(kDeclared == 302, "the OEM declares 302 bytes on 0x1F1");
  prefix[0] = static_cast<uint8_t>(kPciFirstFrame | (kDeclared >> 8));   // 0x11
  prefix[1] = static_cast<uint8_t>(kDeclared & 0xFF);                    // 0x2E
  std::memcpy(prefix + 2, kNavHeader, sizeof(kNavHeader));

  TxOptions opt;                    // RenderSlot::None: enqueueSplit refuses anything else,
  opt.coalesce = false;             // and a borrowed image must never be replaced in place
  return (enqueueSplit(kIdNav, prefix, static_cast<uint8_t>(sizeof(prefix)),
                       bitmap, kNavBitmapBytes, opt) == kNoTicket)
             ? lastResult()
             : Result::Ok;
}

// `25 00 00 00` / `25 00 03 00`. The OEM alternates these at 820 ms while the nav screen is
// up — `nav still.csv` is twenty of them and nothing else on the bus — which reads as the
// blink of a flashing element. Four bytes, verbatim. NOT CONFIRMED ON GLASS: what it
// actually does is still a guess, and on this panel an ACK proves nothing.
Result CarminatDisplay::navTick(bool phase) {
  const uint8_t p[5] = { 0x04, 0x25, 0x00, static_cast<uint8_t>(phase ? 0x03 : 0x00), 0x00 };
  return submit(kIdSetText, p, sizeof(p), RenderSlot::None, /*coalesce=*/false);
}
#endif  // AFFA_ENABLE_NAV

// The most strongly corroborated builder in the library: the OEM head unit's own "Please
// insert navigation CD" screen is in the capture with matching header, frame count and
// 0x0D separators. Declared 0x60 = 96 is correct here and all 14 frames go out.
Result CarminatDisplay::showFullscreenText(const char* l1, const char* l2, const char* l3) {
  const Ascii a(l1);
  const Ascii b(l2);
  const Ascii c(l3);

  uint8_t p[2 + 96];
  std::memset(p, 0x00, sizeof(p));
  p[0] = kPciFirstFrame;
  p[1] = 0x60;                 // 96 content bytes follow — correct
  p[2] = kCmdScreen;           // 0x21
  p[3] = kScreenFullscreen;    // 0x05
  p[4] = 0xFF;
  p[5] = 0x00;
  p[6] = 0x00;
  p[7] = 0x40;
  // SPACE-filled so unused cells render blank rather than as garbage.
  for (size_t k = 2 + 32; k < sizeof(p); ++k) p[k] = 0x20;

  size_t q = 2 + 34;             // two leading spaces at content 32..33 — as captured

  const auto putLine = [&](const Ascii& s) {
    if (s.len == 0) return;    // an empty line is skipped ENTIRELY: no separator either
    for (size_t i = 0; i < s.len && q < 2 + 95; ++i) p[q++] = static_cast<uint8_t>(s.buf[i]);
    if (q < 2 + 96) p[q++] = kLineSeparator;
  };
  putLine(a);
  putLine(b);
  putLine(c);

  return submit(kIdSetText, p, static_cast<uint8_t>(sizeof(p)), RenderSlot::Fullscreen);
}

// OPTIONAL, unlike hidePopup(). Drawing any other screen leaves a fullscreen, so this is
// the explicit "close the window" command for the case where there is nothing to draw
// instead — not a teardown the caller owes the panel after every fullscreen.
//
// Identical bytes to hidePopup(); a different RenderSlot, because a pending "close the
// fullscreen" must not be superseded by a pending "close the popup".
Result CarminatDisplay::hideFullscreenText() {
  const uint8_t d[3] = {0x02, kCmdClose, 0x03};
  return submit(kIdSetText, d, sizeof(d), RenderSlot::Fullscreen);
}

#else

Result CarminatDisplay::showFullscreenText(const char* l1, const char* l2, const char* l3) {
  (void)l1; (void)l2; (void)l3;
  return Result::NotSupported;
}
Result CarminatDisplay::hideFullscreenText() { return Result::NotSupported; }

#endif  // AFFA_ENABLE_FULLSCREEN

// ---------------------------------------------------------------------------
// showConfirmBox — 0x151, §8.9
// ---------------------------------------------------------------------------
#if AFFA_ENABLE_CONFIRMBOX

// THE THINNEST ICE IN THE REPOSITORY. It is DERIVED-ONLY end to end — no capture
// corroborates a single byte of it — and it sits at exactly the 113-byte transport
// ceiling with zero headroom: 2 + 6 + 105 = 113 = 8 + 15*7, sixteen frames, last PCI 0x2F,
// which is the continuation counter's absolute maximum before it would wrap.
//
// The caption region (content 0x1A..0x20) ABUTS the row region at 0x20: a 7-character
// caption writes content[32], which row0 then overwrites. Keep captions to 6 characters.
Result CarminatDisplay::showConfirmBox(const char* caption, const char* row0,
                                       const char* row1) {
  const Ascii cap(caption);
  const Ascii r0(row0);
  const Ascii r1(row1);

  uint8_t content[105];
  std::memset(content, 0x00, sizeof(content));

  for (uint8_t i = 0; i < kConfirmCapMax && cap.buf[i]; ++i)
    content[0x1A + i] = static_cast<uint8_t>(cap.buf[i]);

  uint8_t off = 0x20;
  const auto insertRow = [&](const Ascii& s) {
    const char* t = s.c();
    while (*t && off < 0x36) content[off++] = static_cast<uint8_t>(*t++);
    if (off < 0x36) content[off++] = kLineSeparator;
  };
  insertRow(r0);
  insertRow(r1);

  uint8_t buf[2 + 6 + sizeof(content)];
  uint8_t n = 0;
  buf[n++] = kPciFirstFrame;
  buf[n++] = 0x6F;            // declared content length = 111 = 6 + 105 — correct
  buf[n++] = kCmdScreen;      // ---- fixed 6-byte header ----
  buf[n++] = kScreenFullscreen;
  buf[n++] = 0x00;
  buf[n++] = 0x00;
  buf[n++] = 0x01;
  buf[n++] = 0x49;
  std::memcpy(buf + n, content, sizeof(content));
  n = static_cast<uint8_t>(n + sizeof(content));

  return submit(kIdSetText, buf, n, RenderSlot::ConfirmBox);
}

#else

Result CarminatDisplay::showConfirmBox(const char* caption, const char* row0,
                                       const char* row1) {
  (void)caption; (void)row0; (void)row1;
  return Result::NotSupported;
}

#endif  // AFFA_ENABLE_CONFIRMBOX

// ---------------------------------------------------------------------------
// showInfoPopup / showInfoMenu / hideInfoPopup — 0x151, §8.10
// ---------------------------------------------------------------------------
#if AFFA_ENABLE_INFOPOPUP

// The 3-row settings-list popup. ONE MESSAGE PER ROW, 13 payload bytes each:
//
//   10 0B 76 <prefix> <offset> t0 t1 t2      (frame 0, eight raw bytes)
//   21 t3 t4 t5 t6 t7 00 00                  (continuation, 5 bytes + 2 filler)
//
// One message PER ROW, and they must NOT coalesce against each other: all three share
// funcId 0x151 and RenderSlot::InfoPopup, so with coalescing on, rows 1 and 2 would each
// replace row 0 and only the last would ever be drawn. The cost is three queue slots.
Result CarminatDisplay::showInfoMenu(const char* row0, const char* row1, const char* row2,
                                     uint8_t offset0, uint8_t offset1, uint8_t offset2,
                                     uint8_t infoPrefix) {
  const auto sendRow = [&](uint8_t offset, const char* text) {
    const Ascii t(text);

    // SPACE-padded to 8, which is the OEM capture's form. NOT NUL-padded like setText.
    char cell[kInfoCells];
    for (uint8_t i = 0; i < kInfoCells; ++i) cell[i] = (i < t.len) ? t.buf[i] : ' ';

    uint8_t d[5 + kInfoCells];
    uint8_t n = 0;
    d[n++] = kPciFirstFrame;
    d[n++] = 0x0B;              // declared content length = 11 = 3 header + 8 text
    d[n++] = kCmdInfoRow;       // 0x76
    d[n++] = infoPrefix;        // text format, same semantics as setText byte 6
    d[n++] = offset;            // row slot: the OEM list uses 0x41 / 0x44 / 0x48
    for (uint8_t i = 0; i < kInfoCells; ++i) d[n++] = static_cast<uint8_t>(cell[i]);

    return submit(kIdSetText, d, n, RenderSlot::InfoPopup, /*coalesce=*/false);
  };

  // Every row is attempted; the FIRST failure is returned — a partially drawn list is a
  // failure the caller has to know about.
  const Result a = sendRow(offset0, row0);
  const Result b = sendRow(offset1, row1);
  const Result c = sendRow(offset2, row2);
  if (a != Result::Ok) return a;
  if (b != Result::Ok) return b;
  return c;
}

// The offsets and the prefix reproduce the OEM settings list byte for byte:
// `76 60 41 .. AUX`, `76 60 44 .. AUTO`, `76 60 48 .. SPEED`.
Result CarminatDisplay::showInfoPopup(const char* l1, const char* l2, const char* l3) {
  return showInfoMenu(l1, l2, l3, kInfoOffset0, kInfoOffset1, kInfoOffset2, kInfoPrefix);
}

// Best-effort dismiss: back to the source banner. The exact popup-close command has never
// been observed, and inventing one would be a guess presented as a fact.
Result CarminatDisplay::hideInfoPopup() { return setText("RENAULT", 0); }

#else

Result CarminatDisplay::showInfoMenu(const char* row0, const char* row1, const char* row2,
                                     uint8_t offset0, uint8_t offset1, uint8_t offset2,
                                     uint8_t infoPrefix) {
  (void)row0; (void)row1; (void)row2;
  (void)offset0; (void)offset1; (void)offset2; (void)infoPrefix;
  return Result::NotSupported;
}
Result CarminatDisplay::showInfoPopup(const char* l1, const char* l2, const char* l3) {
  (void)l1; (void)l2; (void)l3;
  return Result::NotSupported;
}
Result CarminatDisplay::hideInfoPopup() { return Result::NotSupported; }

#endif  // AFFA_ENABLE_INFOPOPUP

}  // namespace affa

#endif  // AFFA_PANEL_CARMINAT
