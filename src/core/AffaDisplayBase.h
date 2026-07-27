#pragma once
#include "../AffaConfig.h"
#include "AffaTypes.h"
#include "AffaConstants.h"
#include "AffaSyncProfile.h"
#include "ICanLink.h"
#include "IClock.h"
#include "IDisplay.h"
#include "IPanel.h"
#include "../util/AffaLog.h"
#if AFFA_ENABLE_ISOTP_RX
#  include "../proto/IsoTp.h"
#endif

namespace affa {

// Implements both interfaces. The four IPanel primitives (showMenu, setText,
// highlightItem, showPopupText) are declared with SIGNATURES IDENTICAL to IDisplay's, so
// a single override in a panel satisfies both bases with no ambiguity. Interfaces carry
// no data, so there is no diamond. Do not "tidy" either signature.
class AffaDisplayBase : public IDisplay, public IPanel {
 public:
  // C function pointers, not std::function: no allocation, no hidden vtable, and a plain
  // pointer can be compared and reset.
  using KeyCb      = void (*)(Key k, KeyEdge e, void* ctx);
  using CompleteCb = void (*)(TxTicket t, Result r, void* ctx);
  using SyncCb     = void (*)(SyncState s, void* ctx);

  // The link, the clock and the profile are constructor arguments because none of them is
  // optional and none of them may change after begin(). `funcIds` must outlive this
  // object and its ORDER IS ON THE WIRE — it is the order the lazy 0x70 registration
  // probes go out in.
  AffaDisplayBase(ICanLink& link, IClock& clock, const SyncProfile& profile,
                  const uint16_t* funcIds, uint8_t funcCount);
  ~AffaDisplayBase() override = default;

  AffaDisplayBase(const AffaDisplayBase&)            = delete;
  AffaDisplayBase& operator=(const AffaDisplayBase&) = delete;

  // ---- lifecycle ----------------------------------------------------------
  // Resets the FSMs, clears the queue (queued tickets complete Cancelled), arms the peer
  // deadline and the heartbeat. Transmits NOTHING by itself — the first frame leaves on
  // the first poll(). Safe to call again; idempotent apart from the reset.
  bool begin() override;

  // The single pump. This order, every call, with no early-out that can skip a step:
  //   1. drain the RX ring; per frame: tap -> subscriptions -> library consumption
  //      (sync frames, ACKs, auto-ACK, key frames -> KeyCb)
  //   2. sync FSM   3. TX FSM   4. onPoll() panel hook
  //
  // Step 1 strictly precedes step 3 so key latency is bounded by the poll period ALONE —
  // not by queue depth, not by a message in flight, not by a WaitAck with 1900 ms left on
  // its deadline. Moving step 3 above step 1 breaks the headline guarantee.
  //
  // FREQUENCY-INDEPENDENT: every periodic behaviour compares against IClock::millis() and
  // nothing counts calls, so 1 Hz and 1 MHz produce the same frames in the same order.
  void poll() override;

  // ---- ports and options --------------------------------------------------
  void setLogSink(ILogSink* s);
  void onKey(KeyCb cb, void* ctx);
  void onComplete(CompleteCb cb, void* ctx);
  void onSync(SyncCb cb, void* ctx);        // fires only on an actual state change

  // ---- observation seam ----------------------------------------------------
  // Layer 0: every frame in and out, unfiltered, in wire order, for sniffers and
  // consoles. One tap; a second call replaces the first; nullptr removes it. It is on the
  // path of EVERY frame on the bus — keep it to a ring push, and never render from it.
  void onFrame(FrameTap cb, void* ctx);

  // Layer 1: filtered raw subscription. Fixed table of AFFA_MAX_SUBSCRIPTIONS entries, no
  // allocation. Returns kNoSub when the table is full or the match is unsatisfiable (dir
  // with no bits set, or len > 8) — CHECK valid(), an ignored return is a subscription
  // that silently never fires.
  //
  // Observational, never consuming: subscriptions fire BEFORE the library's own handling
  // and nothing a callback does prevents it. Slots are scanned in index order and
  // registration order survives only until a slot is freed and reused, so do not depend on
  // the relative order of two subscriptions — if two callbacks must be ordered, that is
  // one callback.
  [[nodiscard]] SubHandle subscribe(const FrameMatch& m, FrameCb cb, void* ctx);
  bool      unsubscribe(SubHandle h);       // false if the handle is stale
  uint8_t   subscriptions() const;          // slots in use, for diagnostics

  // Layer 2: decoded protocol events. One sink; a second call replaces the first. Fires
  // IN ADDITION TO KeyCb/CompleteCb/SyncCb, never instead of them.
  void onEvent(EventCb cb, void* ctx);

#if AFFA_ENABLE_ISOTP_RX
  // Text ANOTHER node drew on the panel's text channel: reassembled from its ISO-TP frames
  // by the base, decoded by the panel, delivered once per complete message. In the radio
  // role nothing else produces it — we are the node that normally writes that channel — so
  // this is the sniff/MITM seam, which is why it costs AFFA_ENABLE_ISOTP_RX.
  //
  // `text` POINTS INTO LIBRARY STORAGE and is valid only for the duration of the callback.
  // Copy it if you need it afterwards. Fired from inside poll(); rendering from it is safe,
  // blocking in it is not.
  using TextCb = void (*)(const char* text, void* ctx);
  void onText(TextCb cb, void* ctx);
#endif

  // Passive mode: a real radio is on the bus and owns the handshake. We then send no sync
  // frames, no hello, no generic 0x74 ACK, and never latch FUNCSREG — we only inject
  // data. This was setSkipFuncReg(). On a vehicle bus, set it.
  void setPassive(bool on);
  bool passive() const;

  // Bench self-ACK. With no panel on the bus the per-frame ACK never arrives and only the
  // first frame of a multi-frame message would go out; with this on the TX FSM
  // acknowledges its own frames (Partial while bytes remain, Done on the last) so the full
  // sequence is emitted for a PC-side decoder. Wire bytes are identical to a real send.
  //
  // Pinning a golden vector against this: a real panel terminates at the declared FF_DL,
  // so showMenu is 13 frames on hardware and 14 here. Easy to misattribute to the driver.
  void setSelfAck(bool on);

  // ---- observation --------------------------------------------------------
  SyncState syncState() const override;
  bool      synced()     const;   // !hasFlag(state, Failed)
  bool      registered() const;   //  hasFlag(state, FuncsReg)
  bool      busy()       const;   // a job is in flight or queued
  Result    lastResult() const;   // Result of the most recently COMPLETED ticket, except
                                  // immediately after a rejected enqueue, where it holds
                                  // the rejection reason
  TxTicket  lastTicket() const;   // that ticket
  // The ticket from the most recent successful enqueue, INCLUDING one made inside a render
  // call — how an application that used setText() rather than enqueue() learns which
  // ticket to match in onComplete. kNoTicket if that enqueue was rejected. Read it
  // immediately: the next enqueue overwrites it, including a render the menu makes for you.
  TxTicket  lastEnqueued() const;
  uint8_t   queued()     const;   // jobs waiting behind the active one
  Stats     stats()      const;   // forwarded from the link

  // ---- capability ---------------------------------------------------------
  bool supports(Feature f) const override = 0;   // each panel answers for itself

  // ---- transmit -----------------------------------------------------------
  // Copy `len` bytes into a queue slot and return immediately; the bytes need not outlive
  // the call. `opt` carries the coalescing slot, the priority and the per-message
  // coalescing opt-out; the default is a plain FIFO append (slot None never coalesces).
  //
  // [[nodiscard]] because kNoTicket is the ONLY signal that the message was rejected, with
  // the reason in lastResult(). Dropping it turns a QueueFull into a screen that never
  // appears. `(void)enqueue(...)` if you genuinely do not care.
  [[nodiscard]] TxTicket enqueue(uint16_t funcId, const uint8_t* data, uint8_t len,
                                 TxOptions opt = TxOptions{});

  // ---- preemption ----------------------------------------------------------
  // Drop every job QUEUED AND NOT YET STARTED — not one byte handed to ICanLink::send().
  // The job on the wire is untouched, and so are pending Registration jobs: a payload
  // reaching the panel before its function is registered is rejected, and the resulting
  // SendFailed looks exactly like a wire-format bug. Dropped tickets are reported through
  // onComplete with Result::Aborted; returns how many.
  //
  // The queue is mutated BEFORE any callback fires, so a nested abortPending() from inside
  // one of those callbacks finds nothing and returns 0.
  uint8_t abortPending();

  // abortPending() plus abandoning the message on the wire, at the next FRAME BOUNDARY —
  // after the in-flight frame's ACK or deadline, never mid-frame — with the ISO-TP
  // continuation counter reset so the next message starts clean at frame 0. True if a job
  // was actually abandoned.
  //
  // The panel is left holding a partial transfer, and WHETHER IT RECOVERS CLEANLY IS NOT
  // VERIFIED ON HARDWARE. Bench and shutdown tool; routine preemption is coalescing +
  // abortPending() + Priority::Urgent.
  bool abortAll();

  // Is a not-yet-started job for this slot sitting in the queue? Cheap, exact, and the
  // thing to assert in a test rather than counting frames.
  bool pending(RenderSlot s) const;

  // ---- input seam ----------------------------------------------------------
  // Emulate a key press. The Local half takes the IDENTICAL path to a key decoded off the
  // wire. Safe from an application task; NOT safe from an ISR (it can render, which
  // enqueues).
  //
  // BOTH DEFAULT TO Local: in the radio role we RECEIVE key frames, so transmitting one
  // adds nothing but a frame nothing listens for. KeySource::Wire is for impersonating the
  // panel at a REAL radio and puts phantom button presses on the bus — harmless on a
  // bench, input other modules may act on in a vehicle.
  [[nodiscard]] Result pressKey(Key k, KeyEdge e, KeySource src = KeySource::Local);
  [[nodiscard]] Result nav(NavCommand c,          KeySource src = KeySource::Local);

#if AFFA_ENABLE_MENU
  // The gesture that OPENS the menu — UI policy, not wire format. Affects OPENING ONLY;
  // once open, key routing into the menu is not configurable. nav(NavCommand::Open) works
  // regardless, so clearing the hotkey makes it the only way in.
  void setMenuHotkey(Key k, KeyEdge e);   // default: Key::Load, KeyEdge::Hold
  void clearMenuHotkey();                 // no gesture opens the menu; only nav(Open)
  bool menuHotkey(Key& k, KeyEdge& e) const;   // false when cleared
#endif

  // ---- rendering: default bodies return NotSupported ------------------------
  [[nodiscard]] Result setText(const char*, uint8_t digit = 255) override;
  [[nodiscard]] Result setTime(const char*) override;
  [[nodiscard]] Result setPower(bool) override;
  [[nodiscard]] Result showMenu(const char*, const char*, const char*,
                                uint8_t = 0x0B) override;
  [[nodiscard]] Result highlightItem(uint8_t) override;
  [[nodiscard]] Result showPopupText(const char*, uint8_t = 0x09, uint8_t = 0xFF,
                                     uint8_t = 0x60) override;
  [[nodiscard]] Result hidePopup() override;
  [[nodiscard]] Result showFullscreenText(const char*, const char*, const char*) override;
  [[nodiscard]] Result hideFullscreenText() override;
  [[nodiscard]] Result showConfirmBox(const char*, const char*, const char*) override;
  [[nodiscard]] Result showInfoPopup(const char*, const char*, const char*) override;
  [[nodiscard]] Result hideInfoPopup() override;

 protected:
  // ---- panel hooks ---------------------------------------------------------
  // Every frame we build pads with this. Carminat 0x00, UpdateList 0x81.
  virtual uint8_t packetFiller() const = 0;

  // The id this panel transmits key frames on: Carminat 0x1C1, UpdateList 0x0A9.
  // 0 means "this family has no key transmit id", and pressKey(..., Wire) then returns
  // NotSupported. Note this id is deliberately NOT excluded from the auto-ACK: the
  // capture shows RX 1C1 70 .. answered with TX 5C1 74 ..., and the panel expects it.
  virtual uint16_t keyTxId() const { return 0; }

  // Called for each received frame the base did not consume itself — i.e. not a sync
  // frame on syncReplyId and not an ACK on funcId|replyFlag. Panels decode their key
  // frame and their radio-text frame here. Return true if consumed.
  //
  // Runs AFTER the auto-ACK for that frame has been transmitted, which is the legacy
  // order (Carminat acknowledged 0x1C1 before it validated the key bytes).
  virtual bool onFrame(const Frame& f) { (void)f; return false; }

#if AFFA_ENABLE_ISOTP_RX
  // The id inbound text arrives on — Carminat 0x151, UpdateList 0x121. 0 (the default)
  // means this panel decodes no inbound text, and the reassembler is never fed.
  virtual uint16_t textRxId() const { return 0; }

  // Decode one reassembled payload into a NUL-terminated string. Panel-specific because
  // the command byte is: Carminat text is 0x74/0x77, UpdateList 0x76/0x7F. Return false
  // for a payload that is not text — a screen, an info row — and nothing is delivered.
  virtual bool decodeText(const uint8_t* payload, uint8_t len, char* out,
                          uint8_t outSize) const {
    (void)payload; (void)len; (void)out; (void)outSize; return false;
  }
#endif

  // Veto the generic 0x74 auto-ACK for one frame. The base already suppresses it in
  // passive mode, for the sync ids, for reply-flagged ids and for every id in the function
  // table (an inbound frame there is our own echo or another node's traffic; we owe
  // neither a 0x74). Panels override only for a family quirk — UpdateList does not
  // acknowledge a malformed key frame.
  virtual bool shouldAutoAck(const Frame& f) const { (void)f; return true; }

  // Called after a key is decoded (from the wire, or from pressKey/nav with a Local
  // source). The base applies the menu hotkey, routes to the panel's menu, then falls
  // through to KeyCb and EventKind::Key. Panels override to ADD routing, never to replace
  // the fall-through.
  virtual void routeKey(Key k, KeyEdge e);

#if AFFA_ENABLE_MENU
  // The three seams the menu hangs on. Here rather than in the panel because the hotkey
  // POLICY is the base's, while the Menu type is panel-specific.
  virtual bool menuOpen() const { return false; }
  virtual bool openMenu()       { return false; }   // true if a menu exists and opened
  virtual bool routeKeyToMenu(Key k, KeyEdge e) { (void)k; (void)e; return false; }
#endif

  // The Wire half of pressKey(). Builds and transmits:
  //     keyId : 03 89 <hi> <lo | (hold ? 0xC0 : 0)> 00 00 00 00
  // Bytes 4..7 are literal zero and NOT packetFiller() — that is what the capture of our
  // own emulated key shows.
  //
  // NotSupported when the panel has no key transmit id, or for a Hold edge on a wheel
  // code: 0x0101|0xC0 and 0x0141|0xC0 are both 0x01C1, so a held detent has no
  // distinguishable wire form and sending the click form would produce a fine step where
  // the caller asked for a coarse one.
  //
  // NOT QUEUED: a single frame on its own id with no ACK and no function registration;
  // behind the ISO-TP queue it would have exactly the latency preemption exists to remove.
  // Tagged fromSelf, reported to the tap as Direction::Tx.
  [[nodiscard]] Result transmitKey(Key k, KeyEdge e);

  // Called from poll() once per pass, after the sync and TX FSMs. Panels put their own
  // time-driven work here (the UpdateList title scroll). MUST be deadline-driven against
  // _clock, never a call counter.
  virtual void onPoll() {}

  // Decode a raw wire code into a key and an edge. Shared so both panels use the same
  // mask. The wheel codes 0x0101/0x0141 are EXEMPT: masking 0x0141 would rewrite it to
  // 0x0101, i.e. every wheel-down detent would be reported as a wheel-up.
  static void decodeKey(uint16_t raw, Key& out, KeyEdge& edge);

  // The 03 89 guard plus decodeKey(). Returns false for anything that is not a key frame.
  // The guard is load-bearing: the key id also carries `70 A3..`, `02 64 0F A3..` and
  // `05 63 "0037"` from the panel, and a decoder without it invents keys 0x640F and
  // 0x3030 out of that traffic.
  static bool decodeKeyFrame(const Frame& f, Key& out, KeyEdge& edge);

  // For a panel publishing a decoded event of its own — so it arrives through the SAME
  // sink as the rest rather than growing a second callback beside it. No shipped panel
  // uses it; every Event today is raised by this base.
  void emit(const Event& ev);

  ICanLink&          _link;
  IClock&            _clock;
  const SyncProfile& _profile;

 private:
  enum class TxState  : uint8_t { Idle, SendingFrame, WaitAck };
  enum class JobKind  : uint8_t { Payload, Registration };
  enum class SelfAck  : uint8_t { None, Partial, Done };

  struct TxJob {
    uint16_t   funcId     = 0;
    TxTicket   ticket     = kNoTicket;  // kNoTicket for Registration jobs — they are
                                        // invisible to onComplete
    JobKind    kind       = JobKind::Payload;
    RenderSlot slot       = RenderSlot::None;   // coalescing key, together with funcId
    Priority   prio       = Priority::Normal;
    bool       coalesce   = false;      // false = this message is never replaced
    bool       started    = false;      // true once ONE byte has gone to ICanLink::send().
                                        // THE SINGLE AUTHORITY for "not yet started" —
                                        // never infer it from queue position, from
                                        // TxState, or from frameIndex.
    bool       abandon    = false;      // abortAll() asked; honoured at a frame boundary
    uint8_t    len        = 0;
    uint8_t    sent       = 0;          // bytes already handed to the link
    uint8_t    frameIndex = 0;          // ISO-TP continuation counter `num`
    uint8_t    data[AFFA_MAX_PAYLOAD]   = {0};
  };

#if AFFA_MAX_SUBSCRIPTIONS > 0
  // One subscription slot. `gen` is bumped on every unsubscribe so a stale SubHandle
  // cannot unsubscribe the slot's next owner — the silent failure mode of a bare index.
  struct Sub {
    FrameMatch m;
    FrameCb    cb   = nullptr;
    void*      ctx  = nullptr;
    uint8_t    gen  = 0;
    bool       used = false;
  };
#endif

  void pumpRx();            // ALWAYS first in poll(); delivers keys
#if AFFA_ENABLE_ISOTP_RX
  void pumpText(const Frame& f);   // reassemble + decode inbound text, from pumpRx()
#endif
  void pumpSync();
  void pumpTx();            // ALWAYS last; never reached before pumpRx() has returned

  // The choke point every frame passes through in BOTH directions: tap, then the
  // subscription table. Called from pumpRx() and from txFrame(), so a sniffer sees the
  // whole bus in order.
  void observe(const Frame& f, Direction d);
  bool txFrame(Frame f);            // stamps fromSelf, sends, observes. By value: the
                                    // fromSelf stamp must not leak back to the caller's
                                    // buffer, which panels may reuse.
  void reportLinkError(LinkErrorKind k, uint32_t count);

  bool handleSyncFrame(const Frame& f);
  bool handleAckFrame(const Frame& f);
  void sendGenericAck(uint16_t id);      // the 0x74 reply on id|replyFlag
  bool isOurTxId(uint16_t id) const;
  bool knownFunc(uint16_t id) const;

  int  findCoalescable(uint16_t funcId, RenderSlot s) const;  // -1 if none
  uint8_t insertIndexFor(Priority p) const;   // after started + Registration jobs
  bool registrationQueued() const;
  void pushJob(uint16_t funcId, const uint8_t* d, uint8_t len, JobKind kind, TxTicket t,
               const TxOptions& opt, uint8_t at);
  void removeJob(uint8_t index);
  void creditAck(bool done);
  void finishJob(Result r);
  // Drops every job in the queue, started or not, reporting `r` for each payload ticket.
  // begin() and a failed registration are its only callers.
  void failAllQueued(Result r);
  // Drops every NOT-YET-STARTED Payload job — the shared body of abortPending() (with
  // Aborted) and of the sync-loss teardown (with Cancelled). Registration jobs survive.
  uint8_t dropUnstarted(Result r);
  void completeTicket(TxTicket t, Result r);
  // Stores the new state and fires SyncCb + EventKind::SyncChanged, but only on an actual
  // change. `extra` fires additionally (Registered / PeerLost); pass SyncChanged for none.
  void setSync(SyncState s, EventKind extra);
  TxTicket nextTicket();

  // Linear, not circular: insertIndexFor() splices into the middle for Priority::Urgent,
  // and at AFFA_TX_QUEUE_DEPTH a memmove inside poll() is cheaper to write correctly than
  // modular arithmetic is to review.
  TxJob     _queue[AFFA_TX_QUEUE_DEPTH];
  uint8_t   _qCount = 0;
  TxState   _tx = TxState::Idle;
  SelfAck   _selfAckPending = SelfAck::None;
  uint32_t  _ackDeadlineMs  = 0;
  uint32_t  _nextSyncMs     = 0;
  uint32_t  _peerDeadlineMs = 0;
  SyncState _sync = SyncState::Failed;
  TxTicket  _nextTicket    = 1;
  TxTicket  _lastCompleted = kNoTicket;
  TxTicket  _lastEnqueued  = kNoTicket;
  Result    _lastResult    = Result::Ok;
  const uint16_t* _funcIds;
  uint8_t   _funcCount;
  bool      _passive = false;
  bool      _selfAck = false;
  bool      _inPoll  = false;   // re-entrancy guard, not a feature
  uint32_t  _lastOverflow = 0;  // last ICanLink ringOverflow we reported
  uint32_t  _txDropCount  = 0;  // frames ICanLink::send() refused, counted here so the
                                // LinkError event does not need a driver status read

  KeyCb      _keyCb  = nullptr;   void* _keyCtx  = nullptr;
  CompleteCb _cplCb  = nullptr;   void* _cplCtx  = nullptr;
  SyncCb     _syncCb = nullptr;   void* _syncCtx = nullptr;
  FrameTap   _tap    = nullptr;   void* _tapCtx  = nullptr;
  EventCb    _evCb   = nullptr;   void* _evCtx   = nullptr;
#if AFFA_ENABLE_ISOTP_RX
  TextCb     _textCb = nullptr;   void* _textCtx = nullptr;
  isotp::Reassembler _textAsm;
#endif
#if AFFA_MAX_SUBSCRIPTIONS > 0
  Sub        _subs[AFFA_MAX_SUBSCRIPTIONS];
#endif
#if AFFA_ENABLE_MENU
  Key        _hotkey     = Key::Load;      // the OEM default, replaceable
  KeyEdge    _hotkeyEdge = KeyEdge::Hold;
  bool       _hotkeyOn   = true;
#endif
};

} // namespace affa
