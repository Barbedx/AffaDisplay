// The ENTIRE body is inside the gate, so this translation unit is empty when the panel
// is not selected. That, plus -ffunction-sections + --gc-sections, is what makes the
// flash cost of an unselected panel actually zero. See AffaConfig.h.
#include "../AffaConfig.h"
#if AFFA_PANEL_UPDATELIST

#include "UpdateListBase.h"

namespace affa {
namespace {
constexpr const char* kTag = "UL";
}

using namespace updatelist;

UpdateListBase::UpdateListBase(ICanLink& link, IClock& clock)
    : AffaDisplayBase(link, clock, kSync, kFuncIds, kFuncCount) {}

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

  }
  return false;
}

// ---------------------------------------------------------------------------
// Transmit helpers
// ---------------------------------------------------------------------------

Submitted UpdateListBase::enqueueRender(uint16_t funcId, const uint8_t* data, uint8_t len,
                                     RenderSlot slot) {
  TxOptions opt;
  opt.slot = slot;
  // Straight through since 2.0 — see CarminatDisplay::submit() for why this stopped being a
  // three-way translation. Submitted is an ACCEPTANCE verdict; the delivery verdict arrives
  // later through onComplete(), keyed by the ticket this returns.
  return enqueue(funcId, data, len, opt);
}

Submitted UpdateListBase::setPower(bool on) {
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
    return true;
  }

  return false;
}


// ---------------------------------------------------------------------------
// Keys
// ---------------------------------------------------------------------------


}  // namespace affa

#endif  // AFFA_PANEL_UPDATELIST
