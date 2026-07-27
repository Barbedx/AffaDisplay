// Everything the AFFA2 family shares below the text encoding: the SyncProfile and function
// table (0x121, 0x1B1), setPower's 0x1B1 payload, the 0x0A9 key channel, the 0x121 inbound
// radio-text sniff, and the AMS key-forwarding gesture with its non-blocking feedback.
//
// ABSTRACT ON PURPOSE — no setText and no supports(). The two concrete panels differ in
// exactly the text encoding, and a base answering supports(Feature::Text) == true while
// setText returned NotSupported would be a capability lie.
#pragma once
#include "../AffaConfig.h"
#if AFFA_PANEL_UPDATELIST

#include "UpdateListConstants.h"
#include "../core/AffaDisplayBase.h"

namespace affa {

class UpdateListBase : public AffaDisplayBase {
 public:
  UpdateListBase(ICanLink& link, IClock& clock);

  // NAME HIDING, not decoration. The protected `bool onFrame(const Frame&)` below hides
  // EVERY base member called onFrame, including the public Layer-0 tap
  // `void onFrame(FrameTap, void*)`; without this line `display.onFrame(&tap, ctx)` does
  // not compile through an UpdateListDisplay&.
  using AffaDisplayBase::onFrame;

  // 0x1B1: `04 52 <state> FF FF` padded with 0x81. Enqueued on RenderSlot::Control, so it
  // never coalesces against a text render. Asynchronous, like every render call.
  [[nodiscard]] Result setPower(bool on) override;

  // ---- AMS key forwarding -------------------------------------------------
  // "Hold Load toggles whether wheel keys reach the application, and the panel says so" —
  // UI policy, so it ships on as a replaceable default.
  //
  // While forwarding is DISABLED, decoded keys other than the toggle gesture are dropped
  // before KeyCb and before EventKind::Key. Layer 0 (onFrame tap) and Layer 1 (subscribe)
  // still see the raw 0x0A9 frames, so nothing becomes unobservable.
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

  // Applies the AMS policy, then chains to AffaDisplayBase::routeKey. It suppresses the
  // fall-through only for the toggle gesture and for keys arriving while forwarding is
  // disabled; anything else reaches the base untouched.
  void routeKey(Key k, KeyEdge e) override;

  // Advances the AMS banner repeat schedule. A subclass that overrides onPoll() MUST
  // call this first — UpdateListDisplay does.
  void onPoll() override;

#if AFFA_ENABLE_ISOTP_RX
  // 0x121 is the id WE render on, so inbound text there is another head unit's. Feeds
  // onText(), which delivers the whole reassembled string — onRadioText(bool) below stays
  // as it is: a single-frame AUX heuristic for the panel's own re-assert reaction, and
  // deliberately not the same thing.
  uint16_t textRxId() const override { return updatelist::kIdSetText; }
  bool decodeText(const uint8_t* payload, uint8_t len, char* out,
                  uint8_t outSize) const override;
#endif

  // Called when another node (the radio) transmits the segment text encoding on 0x121.
  // `isAux` is a heuristic and nothing more: the first three cells of the sender's "old
  // text" field spell AUX. Exists for the one library-side reaction that is a panel
  // concern — re-asserting our own content after someone else overwrote it.
  virtual void onRadioText(bool isAux) { (void)isAux; }

  // True while the banner owns the screen: from the gesture until kAmsRepeats *
  // kAmsRepeatMs has elapsed. A renderer that would overwrite it must hold off for exactly
  // that window (the legacy delay(100) loop did this by blocking).
  bool amsFeedbackPending() const { return !expired(_clock.millis(), _amsHoldUntilMs); }

  // Shared capability table; the two concrete panels differ in no Feature, only in bytes.
  static bool familySupports(Feature f);

  // enqueue() + the Result mapping every render call in this family repeats.
  [[nodiscard]] Result enqueueRender(uint16_t funcId, const uint8_t* data, uint8_t len,
                                     RenderSlot slot);

 private:
  void scheduleAmsBanner();

  bool     _amsEnabled     = true;
  bool     _amsHotkeyOn    = true;
  Key      _amsHotkey      = Key::Load;
  KeyEdge  _amsHotkeyEdge  = KeyEdge::Hold;

  // Per-instance, never file-static: in a library that is shared state between two
  // displays on two buses.
  const char* _amsBanner      = nullptr;  // string literal; no ownership, no copy
  uint8_t     _amsRepeatsLeft = 0;
  uint32_t    _amsNextMs      = 0;
  uint32_t    _amsHoldUntilMs = 0;        // banner owns the screen until this deadline
};

}  // namespace affa

#endif  // AFFA_PANEL_UPDATELIST
