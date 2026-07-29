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

// The four IPanel primitives are declared with SIGNATURES IDENTICAL to IDisplay's, so one
// override in a panel satisfies both bases with no ambiguity. Do not "tidy" either.
class AffaDisplayBase : public IDisplay, public IPanel {
 public:
  // C function pointers, not std::function: no allocation, and a plain pointer compares.
  using KeyCb      = void (*)(Key k, KeyEdge e, void* ctx);
  using CompleteCb = void (*)(TxTicket t, Result r, void* ctx);
  using SyncCb     = void (*)(SyncState s, void* ctx);

  // None of these may change after begin(). `funcIds` must outlive this object and its
  // ORDER IS ON THE WIRE — the order the lazy 0x70 registration probes go out in.
  AffaDisplayBase(ICanLink& link, IClock& clock, const SyncProfile& profile,
                  const uint16_t* funcIds, uint8_t funcCount);
  ~AffaDisplayBase() override = default;

  AffaDisplayBase(const AffaDisplayBase&)            = delete;
  AffaDisplayBase& operator=(const AffaDisplayBase&) = delete;

  // ---- lifecycle ----------------------------------------------------------
  // Resets the FSMs, clears the queue (queued tickets complete Cancelled), arms the peer
  // deadline and the heartbeat. Transmits NOTHING by itself — the first frame leaves on the
  // first poll().
  bool begin() override;

  // The single pump: RX ring -> sync FSM -> TX FSM -> onPoll(). THAT ORDER, EVERY CALL.
  // RX strictly precedes TX so key latency is bounded by the poll period ALONE, not by
  // queue depth or a message in flight; reordering breaks the headline guarantee.
  // FREQUENCY-INDEPENDENT: nothing counts calls. docs/API.md §4.
  //
  // A call from a task that is not the registered poll owner (see setPollOwner) does
  // NOTHING and is counted in foreignPolls(). Two tasks pumping the same instance corrupts
  // the transmit FSM, and it does it silently.
  void poll() override;

  // ---- poll ownership (used by src/rtos/, optional everywhere else) -------
  // Identity of the calling task, as an opaque token. A FUNCTION POINTER, not a FreeRTOS
  // call: core/ compiles on the host against nothing but C++17, and it stays that way.
  using TaskIdFn = void* (*)();

  // Declare which task owns poll(). `fn` returns the current task's token; poll() is a
  // no-op unless fn() == owner. Both null (the default) disables the check entirely, which
  // is the caller-owned mode: one task, by contract, unchecked.
  void setPollOwner(void* owner, TaskIdFn fn);
  uint32_t foreignPolls() const;   // poll() calls refused because they were off-task

  // Has begin() run? AffaTask::start() refuses on false — a task that starts polling a
  // display that was never begun transmits nothing and reports no reason.
  bool begun() const;

  // ---- ports and options --------------------------------------------------
  void setLogSink(ILogSink* s);
  void onKey(KeyCb cb, void* ctx);
  void onComplete(CompleteCb cb, void* ctx);
  void onSync(SyncCb cb, void* ctx);        // fires only on an actual state change

  // ---- observation seam ----------------------------------------------------
  // Layer 0: every frame in and out, unfiltered, in wire order. One tap; a second call
  // replaces it. On the path of EVERY frame — keep it to a ring push, never render from it.
  void onFrame(FrameTap cb, void* ctx);

  // Layer 1: filtered raw subscription, fixed table, no allocation. Returns kNoSub when the
  // table is full or the match is unsatisfiable — CHECK valid(), an ignored return is a
  // subscription that silently never fires. Observational, never consuming. Do NOT depend
  // on the relative order of two subscriptions. docs/API.md §7b.
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

  // Passive mode: a real radio owns the handshake, so we send no sync frames, no hello and
  // no 0x74 ACK, and never latch FUNCSREG — we only inject data. On a vehicle bus, set it.
  void setPassive(bool on);
  bool passive() const;

  // Bench self-ACK: the TX FSM acknowledges its own frames, so a multi-frame message goes
  // out in full with no panel attached. Wire bytes are identical to a real send.
  //
  // PINNING A GOLDEN VECTOR AGAINST THIS: a real panel terminates at the declared FF_DL, so
  // showMenu is 13 frames on hardware and 14 here. Easy to misattribute to the driver.
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
  // The most recent successful enqueue, INCLUDING one made inside a render call — how an
  // application that used setText() learns which ticket to match in onComplete. READ IT
  // IMMEDIATELY: the next enqueue overwrites it, including a render the menu makes for you.
  TxTicket  lastEnqueued() const;
  uint8_t   queued()     const;   // jobs waiting behind the active one
  Stats     stats()      const;   // forwarded from the link

  // ---- link recovery ------------------------------------------------------
  // COUNTED, NEVER SAMPLED. A controller that goes bus-off and comes back looks, in any
  // single sample, exactly like one that is dead — and exactly like one that is healthy,
  // depending on when you looked. Neither snapshot is the truth. What matters is how often
  // it leaves RUNNING and how long it stays out, so those are the numbers kept.
  struct LinkHealth {
    uint32_t recoveries = 0;   // ICanLink::recover() calls that ended with the link up
    uint32_t failures   = 0;   // ...and that did not
    uint32_t flaps      = 0;   // times the link has left the live state since begin()
    uint32_t downMs     = 0;   // total time not live, summed over completed outages
    uint32_t downSince  = 0;   // start of the outage in progress; 0 when live
    uint32_t nextTryMs  = 0;   // when the next recover() is due; 0 when live
  };
  LinkHealth linkHealth() const;

  // ---- capability ---------------------------------------------------------
  bool supports(Feature f) const override = 0;   // each panel answers for itself

  // ---- transmit -----------------------------------------------------------
  // Copies the bytes and returns immediately; they need not outlive the call. `opt` carries
  // the coalescing slot, priority and per-message opt-out — docs/API.md §3b.4.
  //
  // [[nodiscard]] because kNoTicket is the ONLY signal of rejection, with the reason in
  // lastResult(). Dropping it turns a QueueFull into a screen that never appears.
  [[nodiscard]] TxTicket enqueue(uint16_t funcId, const uint8_t* data, uint8_t len,
                                 TxOptions opt = TxOptions{});

  // ---- preemption ----------------------------------------------------------
  // Drop every job QUEUED AND NOT YET STARTED. The job on the wire is untouched, and so are
  // pending Registration jobs — a payload arriving before its function is registered is
  // rejected, and the SendFailed looks exactly like a wire-format bug. Dropped tickets
  // report Aborted; returns how many. docs/API.md §3b.5.
  uint8_t abortPending();

  // abortPending() plus abandoning the message on the wire at the next FRAME BOUNDARY, never
  // mid-frame, with the continuation counter reset.
  //
  // The panel is left holding a partial transfer and WHETHER IT RECOVERS CLEANLY IS NOT
  // VERIFIED ON HARDWARE. Routine preemption is coalescing + abortPending() + Urgent.
  bool abortAll();

  // Is a not-yet-started job for this slot sitting in the queue? Cheap, exact, and the
  // thing to assert in a test rather than counting frames.
  bool pending(RenderSlot s) const;

  // ---- input seam ----------------------------------------------------------
  // Emulate a key press; the Local half takes the IDENTICAL path to a key off the wire.
  //
  // BOTH DEFAULT TO Local: in the radio role we RECEIVE key frames. KeySource::Wire is for
  // impersonating the panel at a REAL radio and PUTS PHANTOM PRESSES ON THE BUS — harmless
  // on a bench, input other modules may act on in a vehicle. docs/API.md §7b.6.
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

  // Carminat 0x1C1, UpdateList 0x0A9; 0 means the family has none and pressKey(..., Wire)
  // returns NotSupported. Deliberately NOT excluded from the auto-ACK — the capture shows
  // RX 1C1 70 .. answered with TX 5C1 74 .., and the panel expects it.
  virtual uint16_t keyTxId() const { return 0; }

  // Each received frame the base did not consume itself. Return true if consumed.
  //
  // Runs AFTER the auto-ACK for that frame, which is the legacy order — Carminat
  // acknowledged 0x1C1 before it validated the key bytes.
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

  // Veto the generic 0x74 auto-ACK for one frame. The base already suppresses it in passive
  // mode, for the sync ids, for reply-flagged ids and for every id in the function table.
  // Panels override only for a family quirk — UpdateList does not acknowledge a malformed
  // key frame.
  virtual bool shouldAutoAck(const Frame& f) const { (void)f; return true; }

  // The base applies the menu hotkey, routes to the panel's menu, then falls through to
  // KeyCb and EventKind::Key. Panels override to ADD routing, NEVER to replace the
  // fall-through.
  virtual void routeKey(Key k, KeyEdge e);

#if AFFA_ENABLE_MENU
  // The three seams the menu hangs on. Here rather than in the panel because the hotkey
  // POLICY is the base's, while the Menu type is panel-specific.
  virtual bool menuOpen() const { return false; }
  virtual bool openMenu()       { return false; }   // true if a menu exists and opened
  virtual bool routeKeyToMenu(Key k, KeyEdge e) { (void)k; (void)e; return false; }
#endif

  // The Wire half of pressKey(): `03 89 <hi> <lo|hold>` then LITERAL ZERO, not
  // packetFiller() — that is what the capture shows. NotSupported for a Hold edge on a
  // wheel code, because 0x0101|0xC0 and 0x0141|0xC0 are both 0x01C1 and the click form
  // would step fine where the caller asked coarse. NOT QUEUED: behind the ISO-TP queue it
  // would have exactly the latency preemption exists to remove. docs/WIRE-SPEC.md §7.
  [[nodiscard]] Result transmitKey(Key k, KeyEdge e);

  // Once per poll(), after the sync and TX FSMs, for a panel's own time-driven work. MUST
  // be deadline-driven against _clock, never a call counter.
  virtual void onPoll() {}

  // The wheel codes 0x0101/0x0141 are EXEMPT from the mask: masking 0x0141 rewrites it to
  // 0x0101, i.e. every wheel-DOWN detent is reported as a wheel-up.
  static void decodeKey(uint16_t raw, Key& out, KeyEdge& edge);

  // The `03 89` guard plus decodeKey(). THE GUARD IS LOAD-BEARING: the key id also carries
  // `70 A3..`, `02 64 0F A3..` and `05 63 "0037"`, out of which a decoder without it
  // invents keys 0x640F and 0x3030.
  static bool decodeKeyFrame(const Frame& f, Key& out, KeyEdge& edge);

  // For a panel publishing a decoded event of its own, through the SAME sink rather than a
  // second callback beside it. No shipped panel uses it.
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
    bool       started    = false;      // one byte has gone to ICanLink::send(). THE SINGLE
                                        // AUTHORITY for "not yet started" — never infer it
                                        // from queue position, TxState or frameIndex.
    bool       abandon    = false;      // abortAll() asked; honoured at a frame boundary
    uint8_t    len        = 0;
    uint8_t    sent       = 0;          // bytes already handed to the link
    uint8_t    frameIndex = 0;          // ISO-TP continuation counter `num`
    uint8_t    tries      = 0;          // transmit attempts already made and failed
    uint32_t   readyAtMs  = 0;          // not startable before this: retry backoff, or the
                                        // quiet period a torn transfer owes the panel
    uint32_t   holdUntilMs = 0;         // give up waiting for a usable link at this point
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

  // ALWAYS first in poll(), ahead even of pumpRx(): a controller that is down delivers no
  // frames, so there is nothing for pumpRx() to lose by being second, and every millisecond
  // spent polling a dead controller before asking it to come back is wasted.
  //
  // NEVER GIVES UP AND NEVER REBOOTS. See AffaConfig.h "Link recovery" for why the backoff
  // caps instead.
  void pumpLink();
  void pumpRx();            // delivers keys; second only to pumpLink()
#if AFFA_ENABLE_ISOTP_RX
  void pumpText(const Frame& f);   // reassemble + decode inbound text, from pumpRx()
#endif
  void pumpSync();
  void pumpTx();            // ALWAYS last; never reached before pumpRx() has returned

  // The choke point every frame passes through in BOTH directions, so a sniffer sees the
  // whole bus in order.
  void observe(const Frame& f, Direction d);
  bool txFrame(Frame f);            // BY VALUE: the fromSelf stamp must not leak back into
                                    // the caller's buffer, which panels may reuse.
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
  // Ends the head job: retries it if the failure was transient and it has attempts left,
  // otherwise removes it and reports. THE ONLY PLACE a job leaves the queue on completion.
  //
  // `allowRetry = false` is the give-up path and it is load-bearing: the hold window's own
  // expiry reports LinkDown, and LinkDown is exactly the result the retry branch treats as
  // "not the render's fault, do not spend an attempt". Routing the give-up through the
  // normal path therefore re-armed the job for ever — the hold that could never end.
  void finishJob(Result r, bool allowRetry = true);
  // true if `r` is worth another attempt at all — transient, not permanent, not deliberate.
  static bool retryable(Result r);
  // Re-arm the head job for another attempt. Backoff doubles per try; a job whose bytes had
  // already started going out additionally owes the panel AFFA_TX_DIRTY_QUIET_MS of silence.
  // `extendHold` is false for a link fault, which spends no attempt and must therefore stay
  // bounded by the hold window it started with.
  void armRetry(TxJob& job, uint32_t now, bool torn, bool extendHold = true);
  // Is the link in a state where the head job may be started at all?
  bool linkReady() const;
  // Drops every job in the queue, started or not, reporting `r` for each payload ticket.
  // begin() and a failed registration are its only callers.
  void failAllQueued(Result r);
  // Drops every NOT-YET-STARTED Payload job — the shared body of abortPending() (with
  // Aborted) and of the sync-loss teardown (with Cancelled). Registration jobs survive.
  uint8_t dropUnstarted(Result r);
  // The mirror image: drops queued, not-yet-started REGISTRATION probes and leaves payloads
  // alone. Used when registration failed or the panel forgot us — the probes are void, the
  // renders behind them are still what the application wants on the glass.
  uint8_t dropRegistrations();
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
  // Earliest millisecond at which another hello burst may go out. Paces the ANSWER to the
  // sync request, which the panel can ask for 1500 times a second. See AFFA_HELLO_MIN_MS.
  uint32_t  _nextHelloMs    = 0;
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
  bool      _begun   = false;   // begin() has run at least once
  void*     _pollOwner   = nullptr;   // null = unchecked (caller-owned mode)
  TaskIdFn  _pollOwnerFn = nullptr;
  uint32_t  _foreignPolls = 0;  // poll() calls from a task that is not the owner
  uint32_t  _lastOverflow = 0;  // last ICanLink ringOverflow we reported
  uint32_t  _txDropCount  = 0;  // frames ICanLink::send() refused, counted here so the
                                // LinkError event does not need a driver status read

  // Link recovery. `_linkDownSince` is 0 when the link is live, which is what makes it both
  // the outage stamp and the "were we live last time we looked" flag — one variable, so the
  // two can never disagree.
  LinkHealth _health{};
  uint32_t   _recoverBackoffMs = 0;   // 0 = not yet armed; doubles per attempt

  // The RUNNING-but-deaf watchdog (AFFA_RX_STALL_MS). `_rxHeard` arms it — set by every
  // frame that came off the wire, cleared when the stall latches, so a silence fires it
  // exactly once. `_rxStalled` is the latch pumpLink() treats as "down"; a real frame or a
  // completed recovery attempt clears it.
  uint32_t _lastRxMs = 0;
  bool     _rxHeard  = false;
  bool     _rxStalled = false;
  bool     _wasLive   = false;   // last pass's isLive(): a rising edge re-baselines _lastRxMs

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
