// UpdateListBase — everything the AFFA2 family shares below the text encoding.
//
// ABSTRACT ON PURPOSE. It does not implement setText and does not implement supports():
// the two concrete panels differ in exactly the text encoding, and a base that answered
// supports(Feature::Text) == true while returning NotSupported from setText would be a
// capability lie in the one place applications are told to trust.
//
// What lives here:
//   * the family's SyncProfile and function table (0x121, 0x1B1);
//   * setPower  — the 0x1B1 display-control payload;
//   * the 0x0A9 key channel: decode, the malformed-frame auto-ACK veto, routing;
//   * the 0x121 inbound radio-text sniff;
//   * the AMS key-forwarding gesture and its non-blocking on-screen feedback.
#pragma once
#include "../AffaConfig.h"
#if AFFA_PANEL_UPDATELIST

#include "UpdateListConstants.h"
#include "../core/AffaDisplayBase.h"

namespace affa {

class UpdateListBase : public AffaDisplayBase {
 public:
  UpdateListBase(ICanLink& link, IClock& clock);

  // NAME HIDING, not decoration. The protected `bool onFrame(const Frame&)` hook below
  // hides EVERY base member called onFrame, including the public Layer-0 tap
  // `void onFrame(FrameTap, void*)` — so without this line `display.onFrame(&tap, ctx)`
  // fails to compile through an UpdateListDisplay& and only works through an
  // AffaDisplayBase&. The derived override still hides the base's same-signature member,
  // so this changes nothing else. Found by examples/06_counter_preempt.
  using AffaDisplayBase::onFrame;

  // 0x1B1: `04 52 <state> FF FF` padded with 0x81. Enqueued on RenderSlot::Control, so it
  // never coalesces against a text render. Asynchronous, like every render call.
  Result setPower(bool on) override;

  // ---- AMS key forwarding -------------------------------------------------
  // "Hold Load toggles whether wheel keys reach the application, and the panel says so"
  // is the extracted behaviour. It is UI policy that happens to be this installation's
  // convention, so — boundary principle — it ships ON as a default that can be turned
  // off, never as something an application cannot reach.
  //
  // While forwarding is DISABLED, decoded keys other than the toggle gesture are dropped
  // before KeyCb and before EventKind::Key. That is the legacy semantic and it is the
  // point of the switch; Layer 0 (onFrame tap) and Layer 1 (subscribe) still see the raw
  // 0x0A9 frames, so nothing goes unobservable.
  void setAmsHotkey(Key k, KeyEdge e);         // default: Key::Load, KeyEdge::Hold
  void clearAmsHotkey();                       // no gesture toggles; forwarding is fixed
  bool amsHotkey(Key& k, KeyEdge& e) const;    // false when cleared

  bool amsKeysEnabled() const { return _amsEnabled; }
  // Silent: sets the state without drawing the banner. The GESTURE draws the banner;
  // a programmatic change is the application's to announce (or not).
  void setAmsKeysEnabled(bool on) { _amsEnabled = on; }

 protected:
  uint8_t  packetFiller() const override { return updatelist::kFiller; }
  uint16_t keyTxId()      const override { return updatelist::kIdKeyPressed; }

  // 0x0A9 key frames and 0x121 radio text. Everything else falls through unconsumed.
  bool onFrame(const Frame& f) override;

  // The one family quirk on top of the base's own suppression list: a MALFORMED key frame
  // (`03` with something other than `89` behind it) is not acknowledged. Every other
  // frame on 0x0A9 — including the panel's own `70` registration probe — is.
  bool shouldAutoAck(const Frame& f) const override;

  // Applies the AMS policy, then chains to AffaDisplayBase::routeKey for the normal
  // fall-through. It replaces the fall-through in exactly two cases, both of them the
  // policy itself: the toggle gesture (which belongs to the panel) and every key while
  // forwarding is disabled (which is what "disabled" means). Any other key reaches the
  // base untouched.
  void routeKey(Key k, KeyEdge e) override;

  // Advances the AMS banner repeat schedule. A subclass that overrides onPoll() MUST
  // call this first — UpdateListDisplay does.
  void onPoll() override;

  // Called when another node (the radio) transmits the segment text encoding on 0x121.
  // `isAux` is the extracted heuristic and nothing more: the first three cells of the
  // sender's "old text" field spell AUX. Radio behaviour is application territory, so
  // this hook exists for the one library-side reaction that is a panel concern —
  // re-asserting our own content after someone else overwrote it.
  virtual void onRadioText(bool isAux) { (void)isAux; }

  // True while the banner owns the screen — from the gesture until the last repeat's
  // interval has elapsed, i.e. kAmsRepeats * kAmsRepeatMs. A renderer that would
  // otherwise overwrite it must hold off for exactly that window, which is what the
  // legacy delay(100) loop achieved by blocking the entire loop.
  bool amsFeedbackPending() const { return !expired(_clock.millis(), _amsHoldUntilMs); }

  // Shared capability table, so the two concrete panels do not answer it twice. They
  // differ in no Feature — only in the bytes setText builds.
  static bool familySupports(Feature f);

  // enqueue() + the Result mapping every render call in this family repeats.
  Result enqueueRender(uint16_t funcId, const uint8_t* data, uint8_t len, RenderSlot slot);

 private:
  void scheduleAmsBanner();

  bool     _amsEnabled     = true;
  bool     _amsHotkeyOn    = true;
  Key      _amsHotkey      = Key::Load;
  KeyEdge  _amsHotkeyEdge  = KeyEdge::Hold;

  // Per-instance, never file-static and never a function-local static: the extracted code
  // kept its queue, its window index and its timeout at file scope, which in a library is
  // shared state between two displays on two buses.
  const char* _amsBanner      = nullptr;  // string literal; no ownership, no copy
  uint8_t     _amsRepeatsLeft = 0;
  uint32_t    _amsNextMs      = 0;
  uint32_t    _amsHoldUntilMs = 0;        // banner owns the screen until this deadline
};

}  // namespace affa

#endif  // AFFA_PANEL_UPDATELIST
