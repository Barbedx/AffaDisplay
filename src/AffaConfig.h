// AffaDisplay — the single knob header.
//
// Every optional part of the library is gated from here, and every gate is #define'd to
// 0 rather than left undefined so that code can use `#if AFFA_X` and never `#ifdef`.
// A -Wundef build (which this library's own platformio.ini turns on) then catches a
// misspelling INSIDE THE LIBRARY — `#if AFFA_ENALBE_MENU` is a diagnostic, not a silently
// false branch.
//
// WHAT -Wundef CANNOT CATCH, and what does catch it. A misspelling in a CONSUMER's
// build_flags (-D AFFA_ENALBE_MENU=0) defines a macro that nothing ever reads: there is no
// undefined macro anywhere, so there is nothing to diagnose, and the intended gate quietly
// keeps its default. For the PANEL flags that is fatal rather than cosmetic, so those are
// covered by a second mechanism — silence is an #error, see below. For the feature gates it
// is not solvable in the preprocessor at all; the failure is "the feature you tried to turn
// off is still on", which is visible in the map file and in the flash number.
//
// WHY EACH OPTIONAL .cpp GATES ITS WHOLE BODY
// PlatformIO's build_src_filter applies to the *project's* src/. A library pulled in
// through lib_deps is compiled by the Library Dependency Finder, which builds EVERY .cpp
// under the library's src/; the consumer's build_src_filter cannot reach into it and
// library.json's srcFilter cannot see the consumer's build_flags. The preprocessor is
// therefore the only mechanism that can remove a translation unit, so each optional .cpp
// wraps its entire body in its gate and compiles to an empty object file when off. That,
// plus -ffunction-sections -fdata-sections -Wl,--gc-sections, is what makes the flash
// cost of an unused panel actually zero. See docs/API.md §5.1.

#pragma once

// ---------------------------------------------------------------------------
// Panel selection
// ---------------------------------------------------------------------------
// SILENCE IS AN ERROR, NOT A DEFAULT. You must name at least one panel in build_flags:
//
//     -D AFFA_PANEL_CARMINAT=1
//
// This is the shape that catches the failure mode nothing else can. A misspelled flag
// (-D AFFA_PANEL_CARMINET=1) leaves every REAL macro undefined, and -Wundef cannot help:
// the misspelled macro IS defined, merely never used, so there is no undefined macro to
// diagnose. The only observable consequence of the typo is "no panel was selected", and
// that is exactly what the #error below reports.
//
// Earlier revisions made "define nothing" mean "compile all three". That silently turned
// the typo above into a bigger image with all three panels linked in and no diagnostic
// whatsoever — the opposite of what the guard's own comment claimed. If you genuinely
// want every panel, say so:
//
//     -D AFFA_PANEL_DEFAULT_ALL=1
//
// which is a convenience for first-time users and for the footprint reference builds, and
// still not a shipping selection.
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

// UpdateListMenuDisplay derives from UpdateListDisplay: selecting the LCD variant
// necessarily compiles the 8-segment base too.
#if AFFA_PANEL_UPDATELIST_MENU && !AFFA_PANEL_UPDATELIST
#  undef  AFFA_PANEL_UPDATELIST
#  define AFFA_PANEL_UPDATELIST 1
#endif

// The guard this file exists to make meaningful. It fires when every panel flag is 0 —
// which is the state a misspelled -D AFFA_PANEL_* leaves behind, and also the state of a
// build that simply forgot to select one.
#if !AFFA_PANEL_CARMINAT && !AFFA_PANEL_UPDATELIST && !AFFA_PANEL_UPDATELIST_MENU
#  error "AffaDisplay: no panel selected. Add -D AFFA_PANEL_CARMINAT=1 (and/or _UPDATELIST / _UPDATELIST_MENU), or -D AFFA_PANEL_DEFAULT_ALL=1 for all three. Check your spelling: a typo'd AFFA_PANEL_* flag lands here."
#endif

// ---------------------------------------------------------------------------
// Feature gates
// ---------------------------------------------------------------------------

// Menu, MenuController, IPage routing, nav(), getMenu(). The single largest optional
// block. 0: nav() returns NotSupported and supports(Feature::Menu) is false.
#ifndef AFFA_ENABLE_MENU
#  define AFFA_ENABLE_MENU 1
#endif

// Heuristic inference of a RADIO's audio source by pattern-matching decoded text.
// Every other AFFA_ENABLE_* gate describes something the PANEL defines and is on by
// default. This one describes someone else's product, so it is OFF by default and the
// application is expected to write its own policy against EventKind::RadioText. The
// class is kept because the patterns cost real bench time to find, not because they are
// universal. Cost when on: ~0.4 kB of flash for strings that may be wrong about your
// radio. See docs/API.md §7b.7b.
#ifndef AFFA_ENABLE_AUX_TRACKER
#  define AFFA_ENABLE_AUX_TRACKER 0
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

// AffaText.cpp and its mapping table (~1.2 kB). 0 IS DANGEROUS: toAscii becomes a
// bounded copy that passes bytes through unchanged, and any UTF-8 reaching the wire
// renders as garbage on the panel. Not a compile error — a visual one. Set to 0 only if
// you have proved every string you pass is already 7-bit ASCII.
#ifndef AFFA_ENABLE_TRANSLITERATION
#  define AFFA_ENABLE_TRANSLITERATION 1
#endif

// The AFFA_LOG* macros and AffaLog.cpp. 0: every macro expands to `do {} while (0)` and
// NO FORMAT STRINGS ENTER FLASH. Consequence: never put a side effect inside a log
// argument — it disappears with the string.
#ifndef AFFA_ENABLE_LOG
#  define AFFA_ENABLE_LOG 1
#endif

// 0 off, 1 error, 2 warn, 3 info, 4 debug, 5 trace. Compile-time: levels above this
// emit nothing at all. Trace on a live bus prints the ~2 frames/s of sync chatter.
#ifndef AFFA_LOG_LEVEL
#  define AFFA_LOG_LEVEL 3
#endif

// Esp32CanLink.{h,cpp} and the <esp32_can.h> dependency. 0 on a project that uses a
// different CAN driver: nothing pulls esp32_can in and you supply your own ICanLink.
// 1 without the dependency in lib_deps: link error.
//
// The default follows ARDUINO because the header itself includes <driver/gpio.h> for
// gpio_num_t, which does not exist on the host. Leaving it unconditionally 1 would make
// `pio test -e native` fail on an include rather than on anything the test is about.
#ifndef AFFA_ENABLE_ESP32CAN_LINK
#  if defined(ARDUINO)
#    define AFFA_ENABLE_ESP32CAN_LINK 1
#  else
#    define AFFA_ENABLE_ESP32CAN_LINK 0
#  endif
#endif

// NOT IMPLEMENTED, AND LOUD ABOUT IT.
//
// An owned FreeRTOS task that calls poll() on a period was designed and documented, and
// no code was ever written for it: there is no vTaskCreate anywhere under src/, and there
// is not going to be one while "no vTaskDelay anywhere in src/" is the headline contract.
// Until this revision the knob existed, defaulted to 0, and was referenced by NOTHING —
// so a consumer who set it to 1 got a library that never polled, with no diagnostic. That
// is the exact failure mode this project refuses to ship, so setting it is now an error
// rather than a silent no-op. Own the loop: call poll() from one task. See docs/API.md §4.
#ifdef AFFA_ENABLE_TASK
#  if AFFA_ENABLE_TASK
#    error "AffaDisplay: AFFA_ENABLE_TASK is not implemented. The library owns no task; call poll() from exactly one task of your own (docs/API.md §4)."
#  endif
#endif

// The panel twins (vpanel/) plus, through them, AFFA_ENABLE_ISOTP_RX. They buy a
// semantic test oracle and a development loop with no panel attached. This is the most
// expensive optional block in the library, so it is off on target and on for the host,
// where being testable is the point. PlatformIO defines ARDUINO for framework=arduino
// and not for platform=native.
#ifndef AFFA_ENABLE_VIRTUAL_PANEL
#  if defined(ARDUINO)
#    define AFFA_ENABLE_VIRTUAL_PANEL 0
#  else
#    define AFFA_ENABLE_VIRTUAL_PANEL 1
#  endif
#endif

// The reassembler and screen decoder ALONE, without the twins — for a consumer who
// wants to decode inbound multi-frame traffic (sniff another head unit, replay a
// capture) and does not want a panel model. The twins cannot work without it.
#ifndef AFFA_ENABLE_ISOTP_RX
#  define AFFA_ENABLE_ISOTP_RX AFFA_ENABLE_VIRTUAL_PANEL
#endif

// An #error and not a silent promotion, unlike AFFA_PANEL_UPDATELIST_MENU above.
// Selecting the LCD panel without its base is obviously a spelling of "I want the LCD
// panel"; asking for the twins while explicitly switching off the decoder they are
// built on means one of the two flags is a mistake, and guessing which would hide it.
#if AFFA_ENABLE_VIRTUAL_PANEL && !AFFA_ENABLE_ISOTP_RX
#  error "AffaDisplay: AFFA_ENABLE_VIRTUAL_PANEL requires AFFA_ENABLE_ISOTP_RX=1."
#endif

// ---------------------------------------------------------------------------
// Sizing knobs
// ---------------------------------------------------------------------------

// Latest-value-wins replacement of a queued, not-yet-started render of the same
// RenderSlot. Costs one linear scan of at most AFFA_TX_QUEUE_DEPTH entries per enqueue.
// 0: a repeated render stacks — at f Hz in front of a T-second transfer you get
// min(ceil(f*T), depth-1) stale messages on screen after the key, and QueueFull for the
// rest. That is the "panel keeps counting after Pause" symptom. Prefer
// TxOptions::coalesce = false on the specific messages that must all be seen.
#ifndef AFFA_TX_COALESCE
#  define AFFA_TX_COALESCE 1
#endif

// Queue slots. sizeof(TxJob) ~= AFFA_MAX_PAYLOAD + 12 bytes each. Below 3 the lazy
// registration burst (2 probes + 1 payload, either panel) cannot fit and the first send
// after a resync returns QueueFull forever.
//
// 6, not 4, because of showInfoPopup. It is THREE separate messages — one per row, with
// coalesce=false, since all three carry the same funcId and the same RenderSlot and would
// otherwise supersede one another. The worst case is the first call after a resync:
// 2 registration probes + 3 rows = 5 jobs outstanding, so a depth of 4 silently loses the
// third row to QueueFull and the popup renders with a blank line. 6 leaves one slot of
// headroom for an Urgent message arriving in the middle of that burst.
// Cost of the extra two slots at the default AFFA_MAX_PAYLOAD: ~250 B of static RAM.
#ifndef AFFA_TX_QUEUE_DEPTH
#  define AFFA_TX_QUEUE_DEPTH 6
#endif
#if AFFA_TX_QUEUE_DEPTH < 5 && AFFA_ENABLE_INFOPOPUP
#  warning "AffaDisplay: AFFA_TX_QUEUE_DEPTH < 5 with the info popup enabled — the first showInfoPopup() after a resync will drop its third row (2 registration probes + 3 rows)."
#endif

// Largest single ISO-TP message, in payload bytes before framing.
//
// 113 IS A WIRE LIMIT, NOT A BUDGET. The transport carries 8 bytes in frame 0 and 7 in
// each continuation, and the continuation counter is 0x20|(num & 0x0F): 8 + 15*7 = 113.
// showConfirmBox sits at exactly 113 with no headroom. Anything longer needs a counter
// wrap that has never been validated against the panel. Below 96 the Carminat menu
// screen returns TooLong and the menu never draws. See docs/WIRE-SPEC.md §3.3.
#ifndef AFFA_MAX_PAYLOAD
#  define AFFA_MAX_PAYLOAD 113
#endif
#if AFFA_MAX_PAYLOAD > 113 && !defined(AFFA_UNSAFE_LONG_PAYLOAD)
#  error "AffaDisplay: AFFA_MAX_PAYLOAD > 113 exceeds the validated transport ceiling (WIRE-SPEC.md §3.3). Define AFFA_UNSAFE_LONG_PAYLOAD to override once you have bench-validated a longer transmit."
#endif
#if AFFA_MAX_PAYLOAD < 96
#  warning "AffaDisplay: AFFA_MAX_PAYLOAD < 96 — showMenu will return Result::TooLong."
#endif

// RX ring slots. Must be a power of two (static_assert in AffaRing). 32 * sizeof(Frame)
// = 448 B of static RAM, and at 500 kbit/s it tolerates a ~7 ms gap between poll() calls
// on a fully saturated bus. Too small: Stats::ringOverflow climbs, ACKs are lost, sends
// time out and sync flaps.
#ifndef AFFA_RX_RING_DEPTH
#  define AFFA_RX_RING_DEPTH 32
#endif

// Per-frame ACK deadline. 2000 matches the legacy blocking wait exactly, so
// timing-sensitive panel behaviour is unchanged. Too low: a slow panel yields Timeout
// mid-transfer and the screen is left half-drawn. Too high: a dead panel wedges the
// QUEUE for that long per frame — it no longer wedges the loop, which was defect #2.
#ifndef AFFA_ACK_TIMEOUT_MS
#  define AFFA_ACK_TIMEOUT_MS 2000
#endif

// How long the link may go without a 0x69 ping before sync is torn down. The panel pings
// at ~1 Hz. Below ~3000 you tear down on a single missed ping. This is the value the old
// `SYNC_TIMEOUT = 5` CALL COUNTER was meant to express. Never lower it below the longest
// flash write the application performs: the TWAI ISR is not in IRAM, so an OTA or NVS
// write stops reception outright and looks exactly like a panel that went quiet.
//
// THE EFFECTIVE SILENCE WINDOW IS UP TO AFFA_PEER_TIMEOUT_MS + AFFA_SYNC_INTERVAL_MS
// (~6 s at the defaults), not exactly AFFA_PEER_TIMEOUT_MS. The watchdog is evaluated
// only on a heartbeat tick, and a latched ping is consumed before the expiry branch is
// reached — so at most one whole heartbeat period can pass between the deadline expiring
// and the teardown. That is the FSM being correct rather than slow: a single missed ping
// must not tear a working link down. It is also why a test that jumps the clock by
// AFFA_PEER_TIMEOUT_MS + 1 once will NOT see a teardown, and why test_core and test_twin
// starve the link by `AFFA_PEER_TIMEOUT_MS + AFFA_SYNC_INTERVAL_MS + 1`.
#ifndef AFFA_PEER_TIMEOUT_MS
#  define AFFA_PEER_TIMEOUT_MS 5000
#endif

// Heartbeat cadence, enforced inside poll() against IClock::millis(). Changing it
// changes what the panel sees; 1000 is what the capture shows. Treat as fixed.
#ifndef AFFA_SYNC_INTERVAL_MS
#  define AFFA_SYNC_INTERVAL_MS 1000
#endif

// Layer 1 FrameMatch table. sizeof(Sub) ~= 32 B, so 8 slots ~= 256 B of static RAM and
// one linear scan per frame per direction. subscribe() returns kNoSub once full — an
// application that ignores the return value silently loses a subscription. 0 removes the
// table and the scan entirely; Layers 0 and 2 are unaffected.
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
