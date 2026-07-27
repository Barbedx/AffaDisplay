// The ENTIRE body is inside the gate, so this translation unit is empty when the panel
// is not selected. That, plus -ffunction-sections + --gc-sections, is what makes the
// flash cost of an unselected panel actually zero. See AffaConfig.h.
#include "../AffaConfig.h"
#if AFFA_PANEL_UPDATELIST

#include "UpdateListBase.h"
#if AFFA_ENABLE_ISOTP_RX
#  include "../proto/ScreenDecode.h"
#endif

namespace affa {
namespace {
constexpr const char* kTag = "UL";
}

using namespace updatelist;

UpdateListBase::UpdateListBase(ICanLink& link, IClock& clock)
    : AffaDisplayBase(link, clock, kSync, kFuncIds, kFuncCount) {
  // Arm the banner deadlines on the CLOCK'S epoch rather than on zero. Every comparison
  // in this library is wrap-safe against a 32-bit difference, which means a literal zero
  // is only a safe initial value while millis() is below 2^31 — true on a fresh boot and
  // not true for an object constructed after 24.8 days of uptime, where an unarmed
  // deadline would read as "still pending" and hold the marquee off.
  _amsNextMs      = clock.millis();
  _amsHoldUntilMs = _amsNextMs;
}

// ---------------------------------------------------------------------------
// Capability
// ---------------------------------------------------------------------------

bool UpdateListBase::familySupports(Feature f) {
  switch (f) {
    case Feature::Text:  return true;
    case Feature::Power: return true;
    case Feature::KeyTx: return true;   // 0x0A9; pressKey(..., Wire) can transmit

    // setTime is not a wire operation in this family. The extracted setTime returned
    // NoError and put nothing on the bus, which is exactly the silent no-op this library
    // exists to stop returning.
    case Feature::Time:       return false;

    // Neither variant has a two-row window, a popup overlay, a fullscreen mode, a confirm
    // box or an info menu. showMenu() genuinely cannot be rendered here: the segment
    // display has eight characters and the LCD variant has no window geometry. Menu
    // rendering for this family is whatever the application draws through setText().
    case Feature::Menu:       return false;
    case Feature::Popup:      return false;
    case Feature::Fullscreen: return false;
    case Feature::ConfirmBox: return false;
    case Feature::InfoPopup:  return false;

    // Per docs/API.md §6, and identical to CarminatDisplay's answer. It reports the
    // COMPILE GATE only: reassembly lives in proto/, and API.md §2's dependency table
    // says this folder depends on core/ and util/ ONLY, so the panel deliberately does
    // not reach into the reassembler. What it does with inbound 0x121 is the single-frame
    // AUX sniff in onFrame(), reported through the protected onRadioText() hook — a
    // subclass seam, not this capability and not a published Event.
    case Feature::RadioText:  return (AFFA_ENABLE_ISOTP_RX != 0);
  }
  return false;
}

// ---------------------------------------------------------------------------
// Transmit helpers
// ---------------------------------------------------------------------------

Result UpdateListBase::enqueueRender(uint16_t funcId, const uint8_t* data, uint8_t len,
                                     RenderSlot slot) {
  TxOptions opt;
  opt.slot = slot;
  const TxTicket t = enqueue(funcId, data, len, opt);
  // The Result is an ACCEPTANCE verdict. The delivery verdict arrives later through
  // onComplete(), keyed by lastEnqueued().
  return (t == kNoTicket) ? lastResult() : Result::Ok;
}

Result UpdateListBase::setPower(bool on) {
  const uint8_t data[kPowerLen] = {
      kPowerSfDl,
      kCmdSetState,
      on ? kPowerOn : kPowerOff,
      kPowerTail,
      kPowerTail,
  };
  return enqueueRender(kIdDisplayCtrl, data, kPowerLen, RenderSlot::Control);
}

// ---------------------------------------------------------------------------
// Receive
// ---------------------------------------------------------------------------

bool UpdateListBase::shouldAutoAck(const Frame& f) const {
  // `03 <not 89>` on the key channel is a malformed key frame and the extracted driver
  // answered it with silence. Everything else on 0x0A9 — the panel's `70` registration
  // probe, `02 64 0F ..`, `05 63 "0037"` — is acknowledged, exactly as before.
  if (f.id == kIdKeyPressed && f.len >= 2 && f.data[0] == kKeyFrameByte0 &&
      f.data[1] != kKeyFrameByte1) {
    return false;
  }
  return true;
}

bool UpdateListBase::onFrame(const Frame& f) {
  if (f.id == kIdKeyPressed) {
    Key     k = Key::Load;
    KeyEdge e = KeyEdge::Click;
    // The `03 89` guard is inside decodeKeyFrame() and it is load-bearing: this id also
    // carries `70 A3..`, `02 64 0F A3..` and `05 63 "0037"`, and a decoder without it
    // invents keys 0x640F and 0x3030 out of that traffic.
    if (!decodeKeyFrame(f, k, e)) return false;
    AFFA_LOGD(kTag, "key 0x%04X %s", static_cast<unsigned>(k),
              e == KeyEdge::Hold ? "hold" : "click");
    routeKey(k, e);
    return true;
  }

  // Inbound text on our own text function id. The base never auto-ACKs an id in the
  // function table, so this frame is correctly left unacknowledged — it was addressed to
  // the display, not to us.
  if (f.id == kIdSetText) {
    bool isAux = false;
    if (f.len >= kAuxProbeOffset + 3 && f.data[0] == kCmdSetText && f.data[1] == kSegFfDl) {
      isAux = (f.data[kAuxProbeOffset + 0] == 'A' &&
               f.data[kAuxProbeOffset + 1] == 'U' &&
               f.data[kAuxProbeOffset + 2] == 'X');
    }
    AFFA_LOGD(kTag, "radio text on 0x%03X, isAux=%d", static_cast<unsigned>(kIdSetText),
              static_cast<int>(isAux));
    onRadioText(isAux);
    return true;
  }

  return false;
}

#if AFFA_ENABLE_ISOTP_RX
// Both AFFA2 text encodings carry the SAME two fields — the text the panel will show
// ("new") and the text it is replacing ("old") — at different offsets, because 0x7F adds a
// three-byte icon header in front. We report `new`: it is what the sender is putting on the
// glass. The icon latch that distinguishes the two is not modelled here; a caller that
// needs it subscribes to the raw frames.
bool UpdateListBase::decodeText(const uint8_t* payload, uint8_t len, char* out,
                                uint8_t outSize) const {
  uint8_t from = 0, to = 0;
  if (payload[2] == screen::kSegCmd && len >= screen::kSegMinLen) {
    from = screen::kSegNew; to = 25;
  } else if (payload[2] == screen::kSegIconCmd && len >= screen::kSegIconMinLen) {
    from = screen::kSegIconNew; to = 28;
  } else {
    return false;
  }
  if (to > len - 1) to = static_cast<uint8_t>(len - 1);
  screen::asciiz(payload, len, from, to, out, outSize);
  return true;
}
#endif

// ---------------------------------------------------------------------------
// Keys
// ---------------------------------------------------------------------------

void UpdateListBase::setAmsHotkey(Key k, KeyEdge e) {
  _amsHotkey = k; _amsHotkeyEdge = e; _amsHotkeyOn = true;
}
void UpdateListBase::clearAmsHotkey() { _amsHotkeyOn = false; }
bool UpdateListBase::amsHotkey(Key& k, KeyEdge& e) const {
  if (!_amsHotkeyOn) return false;
  k = _amsHotkey; e = _amsHotkeyEdge;
  return true;
}

void UpdateListBase::routeKey(Key k, KeyEdge e) {
  if (_amsHotkeyOn && k == _amsHotkey && e == _amsHotkeyEdge) {
    _amsEnabled = !_amsEnabled;
    AFFA_LOGI(kTag, "AMS key forwarding %s", _amsEnabled ? "enabled" : "disabled");
    scheduleAmsBanner();
    return;   // the gesture belongs to the panel, not to the application
  }

  if (!_amsEnabled) return;   // forwarding suppressed — the whole point of the switch

  AffaDisplayBase::routeKey(k, e);
}

// ---------------------------------------------------------------------------
// AMS banner — the delay(100) loop, unwound
// ---------------------------------------------------------------------------
// Legacy: `for (i = 0; i < 3; i++) { setText(msg); delay(100); }`. Three renders 100 ms
// apart so the banner outlives the next scroll step. Same three renders, same spacing,
// no delay: a repeat count plus a deadline, advanced from onPoll(). The first one goes
// out on the very next poll rather than immediately, which keeps every transmit in this
// library on one path.

void UpdateListBase::scheduleAmsBanner() {
  const uint32_t now = _clock.millis();
  _amsBanner      = _amsEnabled ? kAmsOnText : kAmsOffText;
  _amsRepeatsLeft = kAmsRepeats;
  _amsNextMs      = now;   // due now: emitted by this poll's onPoll()
  // The banner owns the screen for the whole of what the blocking loop occupied, and NOT
  // merely until the last send is queued: without the trailing interval the scroll would
  // redraw in the same poll() pass that emitted repeat 3 and the banner would never be
  // seen at all.
  _amsHoldUntilMs = now + static_cast<uint32_t>(kAmsRepeats) * kAmsRepeatMs;
}

void UpdateListBase::onPoll() {
  if (_amsRepeatsLeft == 0) return;

  const uint32_t now = _clock.millis();
  if (!expired(now, _amsNextMs)) return;

  // `= now + interval`, never `+= interval`: a caller that stalled must not produce a
  // catch-up burst of banners.
  _amsNextMs = now + kAmsRepeatMs;
  --_amsRepeatsLeft;

  // Virtual: the concrete panel owns the encoding. Its Result is deliberately dropped —
  // a banner that could not be queued (no sync yet, queue full) is cosmetic, and the
  // remaining repeats will try again.
  (void)setText(_amsBanner, 255);
}

}  // namespace affa

#endif  // AFFA_PANEL_UPDATELIST
