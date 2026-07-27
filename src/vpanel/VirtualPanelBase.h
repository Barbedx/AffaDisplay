// Shared twin mechanics, protocol-parameterised. Everything that differs between the
// three panels is data in VirtualPanelProfile — the same technique as SyncProfile, and
// for the same reason: duplicating a state machine is what let one defect live twice.
#pragma once

#include "../AffaConfig.h"
#if AFFA_ENABLE_VIRTUAL_PANEL

#include "IVirtualPanel.h"
#include "../proto/IsoTp.h"
#include "../proto/ScreenDecode.h"
#include "../core/AffaConstants.h"

namespace affa {

// The panel-side mirror of SyncProfile. FIELD MEANINGS ARE SHARED WITH IT DELIBERATELY —
// `syncId`, `syncReplyId`, `replyFlag`, `filler`, `aliveByte` and `requestByte` mean
// exactly what they mean there, seen from the other end of the wire. Inventing a second
// vocabulary for the same six bytes is how a twin and a driver drift apart.
//
// Directionality, because it is the one thing that is NOT symmetric:
//   syncId       — WE (the radio) transmit on it; the twin LISTENS on it.
//   syncReplyId  — the twin TRANSMITS on it; we listen on it.
//   aliveByte    — the RADIO's heartbeat byte (0xB9 / 0x79) that the twin recognises.
//   requestByte  — the RADIO's sync request  (0xBA / 0x7A) that the twin answers.
struct VirtualPanelProfile {
  uint16_t ctrlId;       // primary text/control id we send on (twin decodes + ACKs)
  uint16_t textId;       // secondary text id (UpdateList 0x121); == ctrlId if unused
  uint16_t keyId;        // key transmit id, panel -> radio (Carminat 0x1C1, UL 0x0A9)
  uint16_t syncId;       // 0x3AF Carminat / 0x3DF UpdateList
  uint16_t syncReplyId;  // 0x3CF, both families
  uint16_t replyFlag;    // 0x400. The ACK id is COMPUTED as funcId | replyFlag and never
                         // tabulated: 0x0A9 | 0x400 is 0x4A9, not 0x5A9, because bit 8 is
                         // already clear in 0x0A9 — uniquely in the table.
  uint8_t  filler;       // 0x00 Carminat / 0x81 UpdateList

  // ---- appended, all defaulted, so the seven-field aggregate init in docs/API.md
  // ---- §2.14 still compiles unchanged ------------------------------------------
  uint8_t  aliveByte   = 0;        // radio heartbeat byte; 0 = do not answer heartbeats
  uint8_t  requestByte = 0;        // radio sync-request byte; 0 = do not answer requests
  // The panel's FUNCTION TABLE: every id we may send a payload on, in the order the
  // driver registers them. The twin must ACK all of them, not just ctrlId/textId — the
  // lazy 0x70 registration burst walks the WHOLE table (Carminat 0x151 AND 0x1F1), and a
  // twin that acknowledges only 0x151 stalls the second probe for AFFA_ACK_TIMEOUT_MS and
  // then fails the payload behind it. Must outlive the twin; nullptr means "ctrlId and
  // textId only".
  const uint16_t* funcIds   = nullptr;
  uint8_t         funcCount = 0;
};

// The name docs/API.md §2.14 uses. Kept as an alias rather than as the primary name
// because VirtualPanelProfile is what pairs with SyncProfile by eye in a file listing.
using VdProtocol = VirtualPanelProfile;

class VirtualPanelBase : public IVirtualPanel {
 public:
  explicit VirtualPanelBase(const VirtualPanelProfile& p) : _p(p) {}

  bool begin(ICanLink& link, IClock& clock) override;
  void onFrame(const Frame& f) override;
  bool transmitKey(uint16_t code, bool hold) override;
  const ScreenModel& screen() const override { return _screen; }

  // EMULATION vs PASSIVE. PASSIVE decodes only — no ACK, no sync reply, not one byte
  // transmitted — which is how you run a twin ALONGSIDE a real panel on a live bus to see
  // what the panel sees. Two ACKers on one bus is the failure this switch prevents.
  // Default: EMULATION.
  void setEmulate(bool on) { _emulate = on; }
  bool emulating() const   { return _emulate; }

  // Per-frame ACK content. THE THREE MODELS ARE NOT INTERCHANGEABLE, AND A GOLDEN VECTOR
  // MUST SAY WHICH ONE IT WAS RECORDED UNDER.
  //
  //  Done     — 0x74 to every frame. The default, and what a single-frame transfer wants.
  //             On a MULTI-frame message this terminates the sender after frame 0: the
  //             transmit FSM treats "DONE while bytes remain" as SUCCESS (it has to —
  //             showMenu depends on it on every render), so the remaining frames are
  //             never built. A twin in this mode sees 8 bytes of a 96-byte menu.
  //
  //  Partial  — 30 01 00 to every frame, never a Done. This is what makes a REAL radio's
  //             transmit FSM emit its whole multi-frame buffer over the bus for capture:
  //             the sender breaks only on Done. The last frame then draws a Partial with
  //             no bytes left, which the sender reports as SendFailed — harmless for a
  //             capture rig, and the reason this is not the default.
  //
  //  Declared — Partial while the DECLARED FF_DL is unsatisfied, Done at it. THIS IS WHAT
  //             THE HARDWARE DOES, and it is the only mode in which the twin reproduces
  //             the frame counts in docs/WIRE-SPEC.md: showMenu 13 frames ending at PCI
  //             0x2C (not 14 at 0x2D), setText 3, UpdateList setText 4, the LCD variant 5,
  //             an info row 2. Use it for anything that claims to model a panel; use Done
  //             or Partial for the emulator behaviours above.
  //
  // "13 on hardware, 14 through the emulator" is exactly this distinction, and it is easy
  // to misattribute to the CAN driver.
  enum class AckMode : uint8_t { Done, Partial, Declared };
  void    setAckMode(AckMode m) { _ackMode = m; }
  AckMode ackMode() const       { return _ackMode; }

  // True once the radio has announced itself (the 0x70 hello on syncId).
  bool     synced() const       { return _synced; }
  // Clock timestamp (ms) of the last screen decode, 0 if none. The screen decodes in
  // PASSIVE mode too, so this says how fresh the model is independently of emulation.
  uint32_t lastDecodeMs() const { return _lastDecodeMs; }

  // Counters, for tests that want to assert the twin actually answered.
  uint32_t acksSent()        const { return _acksSent; }
  uint32_t syncRepliesSent() const { return _syncReplies; }

  // ---- the differential icon latch (UpdateList families) -------------------
  // 0x76 versus 0x7F is a DIFFERENTIAL encoding: 0x7F carries the icon header, 0x76 omits
  // it, and the PANEL keeps whatever the last 0x7F set. This is that latch, modelled on
  // the panel side where it actually lives.
  //
  // It is a MEMBER, never a function-local static. The third-party reference kept
  // `static uint8_t old_icons` inside the send function, i.e. process-global and never
  // invalidated: after a resync the panel's real icon state is unknown while the cache
  // still claims to know it, so the first text after a resync goes out as a 0x76 that
  // leaves stale icons on the glass. onResync() clears this — which is the twin-side half
  // of the driver's obligation to force a 0x7F on the same edge.
  bool    iconsKnown() const { return _iconsKnown; }
  uint8_t icons()      const { return _icons; }
  uint8_t iconMode()   const { return _iconMode; }

 protected:
  // Decode one control/text frame into _screen. Called for every frame on ctrlId/textId.
  virtual void decode(const Frame& f) = 0;

  // Called on the not-synced -> synced edge. A resync means the panel was power-cycled or
  // lost the link, so EVERY piece of latched panel state is now unknown: the screen, the
  // half-assembled transfer, and the differential icon latch. Overriders MUST call the
  // base. This is the twin half of the rule that makes the driver force a 0x7F on the
  // same edge (PROTOCOL-NOTES §2.2) — the sender re-states the icons precisely because
  // the panel has forgotten them, and a twin that kept them would hide that bug.
  virtual void onResync();

  // 0x74 (or 30 01 00) + filler on `id | replyFlag`. No-op in PASSIVE mode.
  void autoAck(uint16_t id);

  // Record what a 0x7F "text plus icons" message just set. Called by the UpdateList
  // twins; Carminat never calls it.
  void latchIcons(uint8_t icons, uint8_t mode) {
    _icons = icons; _iconMode = mode; _iconsKnown = true;
  }

  // True when `id` is one this panel acknowledges: ctrlId, textId, or any entry of the
  // profile's function table. Never the sync ids — that channel has no ACK semantics, and
  // ACKing it is precisely the 0x7AF incident (WIRE-SPEC §6.1).
  bool acksId(uint16_t id) const;

  VirtualPanelProfile _p;
  ICanLink*           _link  = nullptr;
  IClock*             _clock = nullptr;
  ScreenModel         _screen;
  isotp::Reassembler  _asm;
  bool                _emulate = true;
  bool                _synced  = false;
  AckMode             _ackMode = AckMode::Done;
  uint32_t            _lastDecodeMs = 0;

 private:
  bool handleSync(const Frame& f);
  void sendSync(uint8_t b0, uint8_t b1, uint8_t b2, bool haveB1B2);

  // Follows the ISO-TP transfer on ONE id at a time so AckMode::Declared can answer the
  // way the panel does. Keyed on the id, because a single frame on 0x1F1 must not be
  // mistaken for a continuation of a 0x151 transfer.
  void trackTransfer(const Frame& f);
  bool declaredDone() const { return !_xferActive || _xferContent >= _ffDl; }

  // PER-INSTANCE, never a file-scope or function-local static. The reference kept its
  // icon cache, its window index, its last-sync-log timestamp and its event queue at file
  // scope; in a library that is shared state between instances, and a second twin on the
  // same host silently corrupts the first.
  uint32_t _acksSent    = 0;
  uint32_t _syncReplies = 0;
  // The 0x69 peer-alive ping is emitted in ANSWER to the radio's ~1 Hz heartbeat rather
  // than on a timer of its own, because IVirtualPanel is fed frames and never polled. The
  // deadline stops an unusually fast heartbeat (a test advancing the clock in 1 ms steps)
  // from turning into a ping storm. Deadline-driven against IClock, never a call counter.
  uint32_t _nextPingMs  = 0;
  bool     _pingArmed   = false;

  // AckMode::Declared bookkeeping. `_ffDl` is the DECLARED content length from payload[1]
  // of the first frame; `_xferContent` counts the content bytes actually received
  // (frame 0 contributes len-2 because payload[0..1] are the PCI and the length byte,
  // each continuation contributes len-1).
  uint32_t _xferId      = 0xFFFFFFFF;
  uint16_t _xferContent = 0;
  uint8_t  _ffDl        = 0;
  bool     _xferActive  = false;

  uint8_t  _icons      = 0;
  uint8_t  _iconMode   = 0;
  bool     _iconsKnown = false;   // false = the panel holds an UNKNOWN icon state, which
                                  // is the honest answer both before the first 0x7F and
                                  // after every resync
};

}  // namespace affa
#endif  // AFFA_ENABLE_VIRTUAL_PANEL
