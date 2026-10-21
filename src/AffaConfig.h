// SPDX-License-Identifier: MIT
// AffaDisplay — the single knob header.
//
// Every gate is #define'd to 0 rather than left undefined, so code uses `#if AFFA_X` and
// -Wundef catches a misspelling inside the library. It cannot catch a misspelling in a
// CONSUMER's build_flags (-D AFFA_ENALBE_MENU=0 defines a macro nothing reads), so the
// panel flags are covered by a second mechanism: silence is an #error, below.
//
// WHY EACH OPTIONAL .cpp GATES ITS WHOLE BODY: the Library Dependency Finder compiles
// every .cpp under a lib_deps library; the consumer's build_src_filter cannot reach into
// it and library.json's srcFilter cannot see the consumer's build_flags. The preprocessor
// is the only mechanism that can remove a translation unit, so each optional .cpp wraps
// its body in its gate and compiles to an empty object when off. That plus
// -ffunction-sections -fdata-sections -Wl,--gc-sections is what makes an unused panel
// cost zero flash. See docs/API.md §5.

#pragma once

// ---------------------------------------------------------------------------
// Panel selection
// ---------------------------------------------------------------------------
// SILENCE IS AN ERROR, NOT A DEFAULT — for the display. A build that includes AffaDisplay.h
// must name at least one panel: a misspelled flag leaves every real macro undefined and
// -Wundef cannot see it, so AffaDisplay.h #errors instead. A build that only wants the CAN
// link (AffaTwai.h — a sniffer, a listen-only tap) names no panel and is not refused.
// AFFA_PANEL_DEFAULT_ALL=1 asks for both families — for first builds, not for shipping.
#ifndef AFFA_PANEL_DEFAULT_ALL
#  define AFFA_PANEL_DEFAULT_ALL 0
#endif

#if AFFA_PANEL_DEFAULT_ALL
#  ifndef AFFA_PANEL_CARMINAT
#    define AFFA_PANEL_CARMINAT 1
#  endif
#  ifndef AFFA_PANEL_UPDATELIST
#    define AFFA_PANEL_UPDATELIST 1
#  endif
#endif

#ifndef AFFA_PANEL_CARMINAT
#  define AFFA_PANEL_CARMINAT 0        // Carminat/AFFA3: 0x3AF sync, 0x151 + 0x1F1 data
#endif
#ifndef AFFA_PANEL_UPDATELIST
#  define AFFA_PANEL_UPDATELIST 0      // UpdateList/AFFA2 8-segment: 0x3DF, 0x121 + 0x1B1
#endif

// The dashboard cluster: a THIRD sync profile, transcribed from ONE capture and NEVER RUN
// against hardware. It is NOT in AFFA_PANEL_DEFAULT_ALL and it is not on by any other
// route — you have to ask for it, because everything it claims is inference from a single
// sample (docs/NOTES.md §1.1). It renders no text at all: the capture contains no
// text frame, so the encoding is unknown and supports(Feature::Text) is false.
#ifndef AFFA_PANEL_CLUSTER
#  define AFFA_PANEL_CLUSTER 0
#endif

// ---------------------------------------------------------------------------
// Feature gates
// ---------------------------------------------------------------------------

// THE LIBRARY IS A TRANSPORT, NOT UI (owner's rule, 2026-08-08). Which item is selected, what
// a key means and how fast a title scrolls belong to the application; the gates below only
// remove render calls a build does not want.

// showPopupText / hidePopup (the mode 0x74 overlay). 0: both return NotSupported.
#ifndef AFFA_ENABLE_POPUP
#  define AFFA_ENABLE_POPUP 1
#endif

// showFullscreenText (0x21 mode 0x05). 0: both return NotSupported.
#ifndef AFFA_ENABLE_FULLSCREEN
#  define AFFA_ENABLE_FULLSCREEN 1
#endif

// showConfirmBox and its offset builder. 0: returns NotSupported.
#ifndef AFFA_ENABLE_CONFIRMBOX
#  define AFFA_ENABLE_CONFIRMBOX 1
#endif

// showInfoPopup (the 3-row info menu). 0: returns NotSupported.
#ifndef AFFA_ENABLE_INFOPOPUP
#  define AFFA_ENABLE_INFOPOPUP 1
#endif

// showMenuN() — the N-item list screen. showMenu() draws two rows because the library
// hard-codes 0x82 and 90 bytes; the OEM corpus has 2, 4 and 6 items with [6] = 0x80|count
// and length 36 + 27*count, so the panel takes at least six. The screen is built in the
// caller's scratch buffer and copied into the long-payload pool (AFFA_BULK_SLOTS).
#ifndef AFFA_ENABLE_BIGMENU
#  define AFFA_ENABLE_BIGMENU 1
#endif

// The Carminat navigation pane: showNavBitmap() and navTick(). 48x48 monochrome on 0x1F1,
// decoded from the OEM's own transfer and confirmed rendering on the bench 2026-08-05.
//
// It is an INDEPENDENT LAYER, not a screen mode — the info menu draws with it and it can be
// replaced under an open popup — which is what makes it worth an API rather than a one-off.
// The image is copied into the long-payload pool (AFFA_BULK_SLOTS), so the caller may
// reuse or free it as soon as the call returns.
#ifndef AFFA_ENABLE_NAV
#  define AFFA_ENABLE_NAV 1
#endif

// AffaText.cpp and its mapping table (~1.2 kB). 0 IS DANGEROUS: toAscii becomes a bounded
// copy that passes bytes through unchanged, and any UTF-8 reaching the wire renders as
// garbage on the panel — a visual failure, not a compile error.
#ifndef AFFA_ENABLE_TRANSLITERATION
#  define AFFA_ENABLE_TRANSLITERATION 1
#endif

// The AFFA_LOG* macros and AffaLog.cpp. 0: every macro expands to `do {} while (0)` and no
// format strings enter flash — so never put a side effect inside a log argument.
#ifndef AFFA_ENABLE_LOG
#  define AFFA_ENABLE_LOG 1
#endif

// 0 off, 1 error, 2 warn, 3 info, 4 debug, 5 trace. Compile-time: levels above this emit
// nothing at all.
#ifndef AFFA_LOG_LEVEL
#  define AFFA_LOG_LEVEL 3
#endif

// src/rtos/ — the library creates and owns a FreeRTOS task that calls poll(), and the
// application never calls poll() at all.
//
// ON BY DEFAULT FOR ESP32. It changes WHICH TASK a consumer's callbacks run on (the
// library's, not the caller's), and in exchange every render is safe from any task: an
// application that pumps poll() from loop() and renders from an HTTP handler races the
// transmit queue, and this mode removes that whole class. The display itself is the
// thread-safe surface, so the task costs a consumer nothing but the two lines that start it.
//
// A build that genuinely wants to own poll() itself still says so: -D AFFA_ENABLE_TASK=0,
// and the contract reverts to the original one — poll() from exactly one task, render from
// that task too.
//
// The #error below still fires for every non-FreeRTOS target; src/rtos/ is the one
// directory a port omits.
#ifndef AFFA_ENABLE_TASK
#  if defined(ESP_PLATFORM) || defined(ARDUINO_ARCH_ESP32)
#    define AFFA_ENABLE_TASK 1
#  else
#    define AFFA_ENABLE_TASK 0
#  endif
#endif
#if AFFA_ENABLE_TASK && !defined(ESP_PLATFORM) && !defined(ARDUINO_ARCH_ESP32)
#  error "AffaDisplay: AFFA_ENABLE_TASK=1 needs a FreeRTOS target (ESP_PLATFORM / ARDUINO_ARCH_ESP32). src/rtos/ is the only part of this library that is not portable; on any other target call poll() from exactly one task of your own (docs/API.md §4)."
#endif

// KEY LATENCY IS BOUNDED BY THIS AND NOTHING ELSE. KeyCb fires synchronously inside poll()
// on the owned task, before any TX pumping, so 2 ms is the whole added delay between a
// key frame landing in the RX ring and the application seeing it. Do not "save power" by
// raising it: 10 ms is a fivefold regression in the one number this mode exists to protect.
#ifndef AFFA_TASK_PERIOD_MS
#  define AFFA_TASK_PERIOD_MS 2
#endif

// ABOVE the Arduino loop task, which runs at 1. A key must not wait behind an application
// that is busy. Below the WiFi/TCP stack tasks (18..23), which must not wait behind us.
#ifndef AFFA_TASK_PRIO
#  define AFFA_TASK_PRIO 2
#endif

// poll() plus the deepest documented callback nesting (docs/API.md §4.2). MEASURED on the
// bench rig (C3, marquee widget, rendering KeyCb, examples/17_mediascreen): 2036 bytes
// still free, i.e. about half of this unused. Status::stackFreeBytes reports it live, so
// shrinking this is checkable against your own callbacks rather than a guess.
#ifndef AFFA_TASK_STACK
#  define AFFA_TASK_STACK 4096
#endif

// -1 is tskNO_AFFINITY. The C3 is single-core and ignores it; on a dual-core part, pinning
// the owned task to the core that is NOT running WiFi is worth measuring.
#ifndef AFFA_TASK_CORE
#  define AFFA_TASK_CORE -1
#endif

// An iteration slower than this many periods logs once (rate-limited) and is recorded in
// Status::pollLateMaxUs. It is how a blocking user callback becomes visible instead of
// mysterious — the three incidents in docs/NOTES.md §3 all presented as "the
// panel is frozen" with every error counter at zero.
#ifndef AFFA_TASK_LATE_FACTOR
#  define AFFA_TASK_LATE_FACTOR 8
#endif


// ---------------------------------------------------------------------------
// Sizing knobs
// ---------------------------------------------------------------------------

// Latest-value-wins replacement of a queued, not-yet-started render of the same RenderSlot.
// 0: a repeated render stacks, which is the "panel keeps counting after Pause" symptom.
// Prefer TxOptions::coalesce = false on the specific messages that must all be seen.
#ifndef AFFA_TX_COALESCE
#  define AFFA_TX_COALESCE 1
#endif

// Queue slots; sizeof(TxJob) ~= AFFA_MAX_PAYLOAD + 12 B each. Below 3 the lazy
// registration burst (2 probes + 1 payload) cannot fit and every send after a resync
// returns QueueFull.
//
// 6, not 4: the worst case is the first call after a resync — 2 probes + showInfoPopup's 3
// non-coalescing rows = 5 outstanding — and at depth 4 the popup renders with a blank line.
// The sixth slot is headroom for an Urgent arriving mid-burst.
#ifndef AFFA_TX_QUEUE_DEPTH
#  define AFFA_TX_QUEUE_DEPTH 6
#endif
#if AFFA_TX_QUEUE_DEPTH < 5 && AFFA_ENABLE_INFOPOPUP
#  warning "AffaDisplay: AFFA_TX_QUEUE_DEPTH < 5 with the info popup enabled — the first showInfoPopup() after a resync will drop its third row (2 registration probes + 3 rows)."
#endif

// ---------------------------------------------------------------------------
// Delivery: what the library does about a transfer that did not land
// ---------------------------------------------------------------------------
// THE APPLICATION IS NOT THE RECOVERY LAYER. Every consumer that has integrated this
// library has written the same three things — a retry with a backoff, a quiet period after
// an interrupted transfer, and a "hold this until the panel comes back" — and got at least
// one of them wrong, at a cost measured in thousands of failed renders (docs/API.md §3.1,
// docs/NOTES.md §3). It is protocol behaviour, it belongs here, and it is a
// state machine over IClock like everything else in this file.

// How many times a job is re-attempted after a TRANSIENT failure — Timeout or LinkDown.
// SendFailed (the panel answered with something else) is not retried, nor is a permanent
// one (NotSupported, BadArgument, TooLong, UnknownFunc) or a deliberate one (Aborted,
// Cancelled): the application asked.
//
// 0 means one attempt, then the failure is reported.
#ifndef AFFA_TX_MAX_RETRIES
#  define AFFA_TX_MAX_RETRIES 3
#endif

// First backoff, doubling per attempt (250, 500, 1000 …) and capped at
// AFFA_TX_RETRY_MAX_MS. NOT zero and not "immediately": an immediate retry into a panel
// that has just failed to answer is the storm this exists to prevent, and on a two-node bus
// it is also what drives our own controller toward BUS_OFF — there is no third node to
// acknowledge us, so an unanswered transmitter punishes itself.
#ifndef AFFA_TX_RETRY_MS
#  define AFFA_TX_RETRY_MS 250
#endif
#ifndef AFFA_TX_RETRY_MAX_MS
#  define AFFA_TX_RETRY_MAX_MS 2000
#endif

// Extra quiet time before re-attempting a job whose bytes had ALREADY STARTED going out.
//
// The panel is then holding a partial transfer and is waiting for continuation frames that
// will never come. Bench-measured 2026-07-28: it does recover on its own — an interrupted
// multi-frame message costs a message or two, not a wedge — but it recovers FASTER and more
// predictably if the next thing it hears is silence rather than a fresh first frame landing
// in the middle of its reassembly buffer.
#ifndef AFFA_TX_DIRTY_QUIET_MS
#  define AFFA_TX_DIRTY_QUIET_MS 400
#endif

// How long a queued render waits for a link that is not ready, before it is given up as
// NoSync (no session) or LinkDown (no link). This is what makes "the app never thinks about recovery" true: a render issued
// while the panel is asleep, mid-resync or bus-off is HELD and sent when the link returns,
// rather than rejected at the call site with NoSync for the application to re-issue.
//
// It is bounded on purpose. A screen that appears ninety seconds late is worse than one
// that never appeared, and latest-value-wins coalescing means the held job is always the
// newest value for its slot anyway. 0 disables holding: enqueue() then rejects with NoSync
// or LinkDown immediately and the application owns the recovery.
#ifndef AFFA_TX_HOLD_MS
#  define AFFA_TX_HOLD_MS 8000
#endif

// ---------------------------------------------------------------------------
// Link recovery: what the library does about a CONTROLLER that has stopped
// ---------------------------------------------------------------------------
// The link owns the TWAI driver lifecycle (TwaiLink, or an application's own ICanLink).
// ICanLink::recover() is called only from the library poll path, after the deadline below;
// a controlled driver restart may briefly block there. Applications keep rendering state only —
// they neither restart TWAI nor race the library's recovery.
//
// How long the controller must be CONTINUOUSLY down before the first attempt:
#ifndef AFFA_LINK_RECOVER_MS
#  define AFFA_LINK_RECOVER_MS 3000
#endif

// The backoff DOUBLES per attempt and stops here. It never gives up and it never escalates
// to anything louder, and both halves of that are deliberate:
//
// A cap rather than a give-up, because "give up" is a decision that needs information this
// layer does not have. Thirty seconds is cheap enough to run for ever against a genuinely
// broken wire — a board that sits there retrying and saying so is worth more than one that
// reboots itself into a flash-wear loop and loses its own log ring — and fast enough that a
// transient fault costs seconds.
//
// A doubling backoff rather than a fixed interval, because a direct TWAI restart is not
// free: it stops the RX task, reinstalls the controller, and then starts the task again.
// Retrying that at 3 s for ever against a dead bus is its own kind of storm.
//
// 0 disables link recovery entirely: isLive() goes false and stays false.
#ifndef AFFA_LINK_RECOVER_MAX_MS
#  define AFFA_LINK_RECOVER_MAX_MS 30000
#endif

#if AFFA_LINK_RECOVER_MS && AFFA_LINK_RECOVER_MAX_MS < AFFA_LINK_RECOVER_MS
#  error "AFFA_LINK_RECOVER_MAX_MS is the backoff CEILING; it cannot be below the first delay."
#endif

// A controller can be RUNNING and stone deaf, and that state needs an owner.
// Measured on the bench, 2026-07-29: state 1 (RUNNING), txErr 0, never bus-off — and rxErr
// pinned at 129, bus errors at exactly the panel's frame rate, rxFrames frozen. healthy()
// is "state == RUNNING", so pumpLink() saw nothing wrong, the driver's watchdog reacts to
// BUS_OFF only, and the link stayed deaf until a human power-cycled the rig. The working
// image that morning heard 498 frames and then went deaf mid-run the same way.
//
// This is the deadline on that gap. If the link is up, transmission is enabled (a
// deliberately gated link is expected to be one-sided), we HAVE heard the bus since the
// last stall, and nothing has been received for this long, pumpLink() treats the link as
// down: same flap accounting, same backoff, same recover(), same handshake teardown as a
// controller that stopped RUNNING.
//
// IT FIRES ONCE PER SILENCE. After a stall-triggered recovery the watchdog re-arms only
// when a frame is actually heard again, so a panel that is genuinely powered off costs one
// controlled restart and then quiet, not a restart per backoff for ever. A teardown against
// a silent bus is deliberately rate-limited by the recovery backoff.
//
// The default clears the bench panel's slowest healthy signal — the ~1 Hz peer-alive — by
// 2.5x, and sits below AFFA_PEER_TIMEOUT_MS deliberately: the driver reinstall this
// watchdog buys is what makes the subsequent re-handshake possible at all. 0 disables it.
#ifndef AFFA_RX_STALL_MS
#  define AFFA_RX_STALL_MS 2500
#endif

// Largest single ISO-TP message, in payload bytes before framing. It is the size of the
// inline copy buffer in a TxJob, so it costs RAM per queue slot — it is A BUDGET, and the
// zero-copy enqueueExternal() path is what carries anything genuinely large.
//
// 113 WAS CALLED A WIRE LIMIT AND IT NEVER WAS ONE. The old note here read "8 + 15*7 = 113;
// anything longer needs a counter wrap never validated against the panel". Both halves are
// wrong:
//
//   * the counter wrap is validated, continuously, by this library. The nav pane is a
//     304-byte transfer whose PCIs run 11 20 21..2F 20 — it wraps every single frame — and
//     a bench soak ran 24 912 of them with `failed 0`. The OEM wraps too, on its own 302-byte
//     0x1F1 message and on its two-button message box.
//   * 8 + 15*7 is just where the counter first repeats, which is a numbering fact, not a
//     ceiling. ISO-TP sequence numbers are meant to wrap.
//
// 119 is the two-button message box: 2 PCI + 6 header + 6+6 labels + 105 body = the OEM's
// own `CONFIRM SCREEN NO.csv`, 117 declared, 17 frames, last CF `0x20`, ACKed by the panel.
// Raising the ceiling to hold it costs 6 bytes per queue slot.
// Below 96 the Carminat menu screen returns TooLong. See docs/WIRE.md
#ifndef AFFA_MAX_PAYLOAD
#  define AFFA_MAX_PAYLOAD 119
#endif
#if AFFA_MAX_PAYLOAD > 119 && !defined(AFFA_UNSAFE_LONG_PAYLOAD)
#  error "AffaDisplay: AFFA_MAX_PAYLOAD > 119 is past the longest message this library builds (the two-button box). Use enqueueExternal() for a large payload, or define AFFA_UNSAFE_LONG_PAYLOAD."
#endif
#if AFFA_MAX_PAYLOAD < 96
#  warning "AffaDisplay: AFFA_MAX_PAYLOAD < 96 — showMenu will return Result::TooLong."
#endif

// THE LONG-PAYLOAD POOL: enqueueExternal() and enqueueSplit() copy into one of
// AFFA_BULK_SLOTS library-owned buffers of AFFA_MAX_EXTERNAL_PAYLOAD bytes, so a caller's
// buffer is free again the moment the call returns — on any task — and nothing borrowed
// crosses the task boundary. A slot is released when its job leaves the queue.
//
// 320 holds every long screen this library builds: the 0x1F1 nav pane (304 on the wire) and
// showMenuN at its 10-item maximum (308). Costs AFFA_BULK_SLOTS x this in static RAM, inside
// the display object, and nothing on the heap.
//
// 2 slots: one on the wire and one queued behind it. A third long render while both are in
// use is refused with QueueFull at the call, before a ticket is minted. 0 removes the pool,
// and both calls then return NotSupported.
//
// AFFA_MAX_PAYLOAD above still governs enqueue() and every short screen.
#ifndef AFFA_MAX_EXTERNAL_PAYLOAD
#  define AFFA_MAX_EXTERNAL_PAYLOAD 320
#endif
#ifndef AFFA_BULK_SLOTS
#  define AFFA_BULK_SLOTS 2
#endif
// 0x10|(len>>8) leaves 12 bits of ISO-TP length; past 4095 the first frame cannot describe
// the message and the panel would be told the wrong size.
#if AFFA_MAX_EXTERNAL_PAYLOAD > 4095
#  error "AffaDisplay: AFFA_MAX_EXTERNAL_PAYLOAD > 4095 exceeds the 12-bit ISO-TP length field."
#endif

// Frames one poll() takes off the link, at most. The rest stay in the driver queue for the
// next poll, in order.
//
// A BOUND, NOT A TUNING KNOB. An unanswered panel retransmits one id at 4 000-10 700
// frames/s; a drain-to-empty loop then never empties, pumpSync() after it never runs, the
// heartbeat that would have answered the panel never leaves, and the storm sustains itself.
// 64 matches TwaiLink's default driver queue: one poll takes what was waiting, not what
// arrives during the drain. Must be at least 1.
#ifndef AFFA_RX_BUDGET
#  define AFFA_RX_BUDGET 64
#endif
#if AFFA_RX_BUDGET < 1
#  error "AffaDisplay: AFFA_RX_BUDGET must be at least 1, or poll() never receives."
#endif

// Per-frame ACK deadline. 2000 matches the legacy blocking wait, so panel timing is
// unchanged. Too low: Timeout mid-transfer leaves the screen half-drawn. Too high: a dead
// panel wedges the queue (not the loop) for that long per frame.
#ifndef AFFA_ACK_TIMEOUT_MS
#  define AFFA_ACK_TIMEOUT_MS 2000
#endif

// How long the link may go without a 0x69 ping before sync is torn down. The panel pings at
// ~1 Hz; below ~3000 a single missed ping tears down a working link. NEVER lower it below
// the longest flash write the application performs: the TWAI ISR is not in IRAM, so an OTA
// or NVS write stops reception and looks exactly like a silent panel.
//
// THE EFFECTIVE SILENCE WINDOW IS UP TO AFFA_PEER_TIMEOUT_MS + AFFA_SYNC_INTERVAL_MS (~6 s).
// The watchdog is evaluated only on a heartbeat tick, so a test that jumps the clock by
// AFFA_PEER_TIMEOUT_MS + 1 will NOT see a teardown; starve it by the sum plus one.
#ifndef AFFA_PEER_TIMEOUT_MS
#  define AFFA_PEER_TIMEOUT_MS 5000
#endif

// Heartbeat cadence, enforced inside poll() against IClock::millis(). 1000 is what the
// capture shows; changing it changes what the panel sees. Treat as fixed.
#ifndef AFFA_SYNC_INTERVAL_MS
#  define AFFA_SYNC_INTERVAL_MS 1000
#endif

// HOW LONG TO WAIT FOR THE DISPLAY'S OWN `1C1 70` BEFORE REGISTERING ANYWAY, and it exists
// because waiting for ever is a permanent wedge off a SINGLE lost frame.
//
// The ordering it bounds is real and measured 4/4: the display opens its own channel between
// B0#1 and B0#2, we answer `5C1 74`, and our `151 70` follows ~61 ms later. Nothing about
// that is in doubt. What was wrong was treating it as a precondition instead of a preference.
//
// MEASURED ON THE MegaOpen BENCH, 2026-09-23, and it took a per-id observation table to see.
// That `1C1 70` arrives exactly ONCE, in the middle of the panel's own line-rate `61 11`
// storm — 1570 frames/s, because helloRequiresAnnounce means only the SECOND request draws
// the burst and the panel has long since gone to line rate by then. On a consumer whose RX
// queue overflows in that window the frame is simply gone: two boots of identical firmware,
// one with `rxMissed 0` that registered and lit the glass, one with `rxMissed 19652` where
// 0x1C1 never appeared in the table at all. Same hardware, same panel, same second.
//
// And a wedge it was, not a delay: no registration without the latch, no B9 without FuncsReg,
// and the peer-timeout that would reset the session sits after that `return` — unreachable.
// `phase` stays AwaitPeerChannel and the transmit counters freeze at the boot burst, for ever,
// until somebody power-cycles and wins the same coin toss again.
//
// 400 ms is far outside the ~1.5 ms the captured display needs and still well inside the
// panel's own ~1 s patience, so a display that opens its channel keeps the measured order on
// every boot and only one that never managed to gets registered without it.
#ifndef AFFA_AWAIT_PEER_MS
#  define AFFA_AWAIT_PEER_MS 400
#endif

// FLOOR BETWEEN TWO HELLO BURSTS, AND IT IS THE DIFFERENCE BETWEEN A HANDSHAKE AND A DEAD
// BUS. The sync request is answered with SyncProfile::helloCount frames — three for
// Carminat — and the answer used to go out once per REQUEST FRAME. That is correct only
// while the panel asks occasionally. A panel that has not been acknowledged repeats
// `61 11 01` back to back at line rate: measured 1472 frames/s on the bench 2026-07-29,
// which turns three-frames-per-request into ~4400 transmit attempts per second on a bus
// whose entire capacity is ~4200 and which is already 92 % full of the panel's own frames.
//
// The result is not a slow handshake, it is no handshake: the direct TWAI transmit queue
// can be permanently full. trySend() returns Busy rather than blocking poll(), so the
// protocol leaves its bytes pending and retries on a later pass.
//
// The sync STATE is still updated on every request — only the frames are paced. 200 ms
// keeps the answer well inside the panel's ~1 s expectation while costing 15 frames/s.
#ifndef AFFA_HELLO_MIN_MS
#  define AFFA_HELLO_MIN_MS 200
#endif

// FLOOR BETWEEN TWO PING REPLIES, for a profile with SyncProfile::replyToPing set. The
// healthy panel pings `69` at ~1 Hz and each ping is answered with one immediate B9 — two
// heartbeats a second in total, which is exactly what MeganeCAN put on the wire. But an
// UNACKNOWLEDGED panel retransmits that same ping at line rate — 126 copies in 32 ms
// measured on the bench — and a reply per copy is the same storm AFFA_HELLO_MIN_MS exists
// to prevent. The PeerAlive STATE is still credited on every ping; only the reply frame is
// paced. 250 keeps the reply comfortably inside the panel's ~1 s expectation.
#ifndef AFFA_PING_REPLY_MIN_MS
#  define AFFA_PING_REPLY_MIN_MS 250
#endif

// HOW LONG A CALLBACK MAY RUN BEFORE THE LIBRARY NAMES IT, in milliseconds.
//
// Callbacks fire inside poll(), on the poll task, and that is deliberate: it is what bounds
// key latency by the poll period ALONE rather than by a queue. The price is that a callback
// which blocks blocks the protocol, and no amount of design prevents an application from
// writing one. So it is measured, and ATTRIBUTED.
//
// The number matters less than the attribution. 16 ms is AFFA_TASK_LATE_FACTOR periods at
// the default 2 ms, the same threshold the owned task uses to call an iteration late; the
// log line says WHICH callback rather than "a callback".
//
// 0 compiles the timing out entirely — no clock reads on the callback path, no fields.
#ifndef AFFA_CALLBACK_BUDGET_MS
#  define AFFA_CALLBACK_BUDGET_MS 16
#endif

// CROSS-TASK DISPATCH SLOTS. Once a poll owner is registered, a render called from any other
// task copies its finished bytes into this ring and the owner drains it inside poll(). It is
// what makes "callable from any task" true for EVERY render — including ones written after
// this comment — rather than for the handful somebody remembered to mirror.
//
// 8: AFFA_TX_QUEUE_DEPTH (6) plus room for the renders posted while the owner is inside one
// poll. Much deeper only defers QueueFull to the drain, where the caller already holds a
// ticket — it then completes QueueFull through onComplete rather than at the call.
//
// Costs sizeof(DispatchItem) ~= AFFA_MAX_PAYLOAD + 20 per slot, and NOTHING in CPU when
// unused — a call already on the owning task never touches it. Must be a power of two.
//
// 0 removes the ring entirely. Do that only for a build with no owned task at all, where the
// contract is the original one: poll() from exactly one task, and render from that task too.
#ifndef AFFA_DISPATCH_DEPTH
#  define AFFA_DISPATCH_DEPTH 8
#endif

// Transliteration scratch buffer on the stack of each render call. Below the longest
// string you pass: silently truncated (never mid-sequence, always NUL-terminated).
#ifndef AFFA_TEXT_MAX
#  define AFFA_TEXT_MAX 64
#endif
