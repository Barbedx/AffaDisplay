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
// cost zero flash. See docs/API.md §5.1.

#pragma once

// ---------------------------------------------------------------------------
// Panel selection
// ---------------------------------------------------------------------------
// SILENCE IS AN ERROR, NOT A DEFAULT: name at least one panel in build_flags. A misspelled
// flag leaves every real macro undefined and -Wundef cannot see it; the #error below is what
// catches it. AFFA_PANEL_DEFAULT_ALL=1 asks for all three — for first builds and the
// footprint references, not for shipping.
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
#  ifndef AFFA_PANEL_UPDATELIST_MENU
#    define AFFA_PANEL_UPDATELIST_MENU 1
#  endif
#endif

#ifndef AFFA_PANEL_CARMINAT
#  define AFFA_PANEL_CARMINAT 0        // Carminat/AFFA3: 0x3AF sync, 0x151 + 0x1F1 data
#endif
#ifndef AFFA_PANEL_UPDATELIST
#  define AFFA_PANEL_UPDATELIST 0      // UpdateList/AFFA2 8-segment: 0x3DF, 0x121 + 0x1B1
#endif
#ifndef AFFA_PANEL_UPDATELIST_MENU
#  define AFFA_PANEL_UPDATELIST_MENU 0 // UpdateList LCD variant: different setText encoding
#endif

// UpdateListMenuDisplay derives from UpdateListDisplay.
#if AFFA_PANEL_UPDATELIST_MENU && !AFFA_PANEL_UPDATELIST
#  undef  AFFA_PANEL_UPDATELIST
#  define AFFA_PANEL_UPDATELIST 1
#endif

// The dashboard cluster: a THIRD sync profile, transcribed from ONE capture and NEVER RUN
// against hardware. It is NOT in AFFA_PANEL_DEFAULT_ALL and it is not on by any other
// route — you have to ask for it, because everything it claims is inference from a single
// sample (docs/PROTOCOL-NOTES.md §9). It renders no text at all: the capture contains no
// text frame, so the encoding is unknown and supports(Feature::Text) is false.
#ifndef AFFA_PANEL_CLUSTER
#  define AFFA_PANEL_CLUSTER 0
#endif

#if !AFFA_PANEL_CARMINAT && !AFFA_PANEL_UPDATELIST && !AFFA_PANEL_UPDATELIST_MENU && \
    !AFFA_PANEL_CLUSTER
#  error "AffaDisplay: no panel selected. Add -D AFFA_PANEL_CARMINAT=1 (and/or _UPDATELIST / _UPDATELIST_MENU), or -D AFFA_PANEL_DEFAULT_ALL=1 for all three. Check your spelling: a typo'd AFFA_PANEL_* flag lands here."
#endif

// ---------------------------------------------------------------------------
// Feature gates
// ---------------------------------------------------------------------------

// src/widget/ + CarminatMenuRenderer + MenuController + IPage + nav() + getMenu(). The
// largest optional block, and OFF by default: the panel's whole menu contract is
// showMenu(header,row0,row1,scroll) + highlightItem(rowTag), both always available
// regardless of this flag, and everything above them is one opinion about UI state.
// Gated on this flag ALONE, so src/widget/ compiles on the host with no panel header.
// docs/MENU-WIDGET.md.
#ifndef AFFA_ENABLE_MENU
#  define AFFA_ENABLE_MENU 0
#endif

// src/widget/Marquee and UpdateListDisplay's setScrollText / setScrollActive / reassert.
// A widget like the menu, gated on this flag alone — but ON by default: it is small, and
// eight segment cells do not hold a track title.
#ifndef AFFA_ENABLE_MARQUEE
#  define AFFA_ENABLE_MARQUEE 1
#endif

// showPopupText / hidePopup (the mode 0x74 overlay). 0: both return NotSupported.
#ifndef AFFA_ENABLE_POPUP
#  define AFFA_ENABLE_POPUP 1
#endif

// showFullscreenText / hideFullscreenText (0x21 mode 0x05). 0: both return NotSupported.
#ifndef AFFA_ENABLE_FULLSCREEN
#  define AFFA_ENABLE_FULLSCREEN 1
#endif

// showConfirmBox and its offset builder. 0: returns NotSupported.
#ifndef AFFA_ENABLE_CONFIRMBOX
#  define AFFA_ENABLE_CONFIRMBOX 1
#endif

// showInfoPopup / hideInfoPopup (the 3-row info menu). 0: returns NotSupported.
#ifndef AFFA_ENABLE_INFOPOPUP
#  define AFFA_ENABLE_INFOPOPUP 1
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

// Esp32CanLink.{h,cpp} and the <esp32_can.h> dependency. 1 without that dependency in
// lib_deps: link error. Defaults to ARDUINO because the header includes <driver/gpio.h>
// for gpio_num_t, which does not exist on the host — unconditional 1 would break
// `pio test -e native` on an include.
#ifndef AFFA_ENABLE_ESP32CAN_LINK
#  if defined(ARDUINO)
#    define AFFA_ENABLE_ESP32CAN_LINK 1
#  else
#    define AFFA_ENABLE_ESP32CAN_LINK 0
#  endif
#endif

// src/rtos/ — the library creates and owns a FreeRTOS task that calls poll(), and the
// application never calls poll() at all. Render calls then become legal FROM ANY TASK,
// because they are copied into a command queue that the owned task drains. docs/API.md §4b.
//
// OFF BY DEFAULT, and deliberately not defaulted on for ARDUINO_ARCH_ESP32: turning it on
// changes WHICH TASK a consumer's callbacks run on, which is observable behaviour, so it
// stays something you ask for.
//
// The #error below is the one that used to be unconditional. It still fires for every
// non-FreeRTOS target — src/rtos/ is the one directory a port omits — it just no longer
// fires for the targets that can actually honour the flag.
#ifndef AFFA_ENABLE_TASK
#  define AFFA_ENABLE_TASK 0
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

// poll() plus the deepest documented callback nesting (docs/API.md §4.3). MEASURED on the
// bench rig (C3, marquee widget, rendering KeyCb, examples/19_owned_task): 2036 bytes
// still free, i.e. about half of this unused. Status::stackFreeBytes reports it live, so
// shrinking this is checkable against your own callbacks rather than a guess.
#ifndef AFFA_TASK_STACK
#  define AFFA_TASK_STACK 4096
#endif

// Command slots. Matches AFFA_TX_QUEUE_DEPTH + 2 for the same reason the transmit queue is
// 6: a deeper command queue than transmit queue only defers QueueFull to a worse place,
// where the caller has already been told the render was accepted.
#ifndef AFFA_TASK_QUEUE_DEPTH
#  define AFFA_TASK_QUEUE_DEPTH 8
#endif

// Bytes per string argument in a queued command, three per command. A command is copied by
// value into the queue — no pointer into a caller's stack ever crosses a task boundary —
// so this is what a queued render truncates at. 48 covers every string any panel in this
// library can put on glass (the Carminat menu row is 26 usable, setText is 14).
#ifndef AFFA_TASK_ARG_MAX
#  define AFFA_TASK_ARG_MAX 48
#endif

// -1 is tskNO_AFFINITY. The C3 is single-core and ignores it; on a dual-core part, pinning
// the owned task to the core that is NOT running WiFi is worth measuring.
#ifndef AFFA_TASK_CORE
#  define AFFA_TASK_CORE -1
#endif

// An iteration slower than this many periods logs once (rate-limited) and is recorded in
// Status::pollLateMaxUs. It is how a blocking user callback becomes visible instead of
// mysterious — the three incidents in docs/CR-0.3.0-OWNED-TASK.md §2 all presented as "the
// panel is frozen" with every error counter at zero.
#ifndef AFFA_TASK_LATE_FACTOR
#  define AFFA_TASK_LATE_FACTOR 8
#endif

// The ISO-TP reassembler, the screen decoder, and the onText() callback they feed. For
// reading a channel somebody else writes; the radio role never needs it, so it is off on
// target and on for the host. docs/API.md §2.14.
#ifndef AFFA_ENABLE_ISOTP_RX
#  if defined(ARDUINO)
#    define AFFA_ENABLE_ISOTP_RX 0
#  else
#    define AFFA_ENABLE_ISOTP_RX 1
#  endif
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

// Largest single ISO-TP message, in payload bytes before framing.
//
// 113 IS A WIRE LIMIT, NOT A BUDGET: 8 bytes in frame 0, 7 per continuation, and the
// continuation counter is 0x20|(num & 0x0F) — 8 + 15*7 = 113. showConfirmBox sits at
// exactly 113. Anything longer needs a counter wrap never validated against the panel.
// Below 96 the Carminat menu screen returns TooLong. See docs/WIRE-SPEC.md §3.3.
#ifndef AFFA_MAX_PAYLOAD
#  define AFFA_MAX_PAYLOAD 113
#endif
#if AFFA_MAX_PAYLOAD > 113 && !defined(AFFA_UNSAFE_LONG_PAYLOAD)
#  error "AffaDisplay: AFFA_MAX_PAYLOAD > 113 exceeds the validated transport ceiling (WIRE-SPEC.md §3.3). Define AFFA_UNSAFE_LONG_PAYLOAD to override once you have bench-validated a longer transmit."
#endif
#if AFFA_MAX_PAYLOAD < 96
#  warning "AffaDisplay: AFFA_MAX_PAYLOAD < 96 — showMenu will return Result::TooLong."
#endif

// RX ring slots; power of two (static_assert in AffaRing). 32 tolerates a ~7 ms gap between
// poll() calls on a saturated 500 kbit/s bus. Too small: ringOverflow climbs, ACKs are lost,
// sends time out and sync flaps.
#ifndef AFFA_RX_RING_DEPTH
#  define AFFA_RX_RING_DEPTH 32
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

// Layer 1 FrameMatch table. sizeof(Sub) ~= 32 B, so 8 slots ~= 256 B and one linear scan
// per frame per direction. subscribe() returns kNoSub once full — ignoring the return
// value silently loses a subscription. 0 removes the table and the scan; Layers 0 and 2
// are unaffected.
#ifndef AFFA_MAX_SUBSCRIPTIONS
#  define AFFA_MAX_SUBSCRIPTIONS 8
#endif

// Menu capacity. addItem() returns -1 past the limit and the item is silently absent.
#ifndef AFFA_MENU_MAX_ITEMS
#  define AFFA_MENU_MAX_ITEMS 12
#endif
// Fields per item; extras are dropped at addItem().
#ifndef AFFA_MENU_MAX_FIELDS
#  define AFFA_MENU_MAX_FIELDS 3
#endif
// Rendered row buffer. The Carminat window row is 26 usable bytes; below 27 truncates
// rows that would have fitted on screen.
#ifndef AFFA_MENU_ROW_MAX
#  define AFFA_MENU_ROW_MAX 32
#endif

// Transliteration scratch buffer on the stack of each render call. Below the longest
// string you pass: silently truncated (never mid-sequence, always NUL-terminated).
#ifndef AFFA_TEXT_MAX
#  define AFFA_TEXT_MAX 64
#endif
