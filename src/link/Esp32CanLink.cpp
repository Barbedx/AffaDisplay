#include "../AffaConfig.h"          // the ONLY thing outside the gate
#if AFFA_ENABLE_ESP32CAN_LINK

// THE ONLY TRANSLATION UNIT IN THIS LIBRARY PERMITTED TO INCLUDE THIS HEADER.
// A CI grep asserts it. CAN_FRAME stays inside this file; affa::Frame is a separate type
// and the conversion in both directions is this file's job, which is what keeps the rule
// enforceable rather than aspirational.
//
// KEEPING THE DEPENDENCY'S WARNINGS OUT OF OURS.
//
// We build with -Wall -Wextra -Wundef so that a misspelled AFFA_* gate is a diagnostic.
// collin80's headers are not clean under those flags, and four warnings per target
// environment is exactly the noise in which a real one gets missed. Both suppressions are
// AT THE INCLUDE, so nothing our own code does is exempted — everything after this block
// is still fully checked.
//
// 1. can_common.h returns const-qualified scalars (-Wignored-qualifiers). A compiler
//    diagnostic, so the pragma below handles it.
//
// 2. esp32_can.h:6 and :15 test SOC_TWAI_CONTROLLER_NUM, which the IDF only defines from
//    5.x; on the 4.4 core under Arduino 2.0.x it is undefined and -Wundef fires. A
//    `#pragma GCC diagnostic ignored "-Wundef"` CANNOT suppress this — GCC issues -Wundef
//    from the preprocessor, which the diagnostic pragmas do not reach (verified on
//    riscv32-esp-elf 8.4.0: the pragma silences item 1 and not this). The fix is to give
//    the macro the value it already evaluates to, guarded so a future core that really
//    defines it wins, and undefined again immediately so the shim cannot leak into
//    anything else in this translation unit. Behaviour-identical by construction:
//    `#if 0 == 2 && ...` and `#if <undefined> == 2 && ...` take the same branch.
#ifndef SOC_TWAI_CONTROLLER_NUM
#  define SOC_TWAI_CONTROLLER_NUM 0
#  define AFFA_UNDEF_SOC_TWAI_CONTROLLER_NUM
#endif

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wignored-qualifiers"
#include <esp32_can.h>
#include <driver/twai.h>
#pragma GCC diagnostic pop

#ifdef AFFA_UNDEF_SOC_TWAI_CONTROLLER_NUM
#  undef SOC_TWAI_CONTROLLER_NUM
#  undef AFFA_UNDEF_SOC_TWAI_CONTROLLER_NUM
#endif

#include "Esp32CanLink.h"
#include "../util/AffaLog.h"

namespace affa {
namespace {

constexpr const char* kTag = "LINK";

// setGeneralCallback takes a plain function pointer with no context argument, so the
// instance has to be reachable from a file-scope pointer. Esp32CanLink is therefore
// effectively a singleton: a second begin() on a second instance is refused rather than
// silently stealing the callback from the first.
Esp32CanLink* s_self = nullptr;

} // namespace

// Named struct rather than a free function so that it can be a friend of the class and
// reach the private ingest() without widening the public surface.
struct Esp32CanTrampoline {
  // RUNS IN task_CAN (priority 15, 8192-byte stack), not in an ISR — but on the critical
  // path of every frame on the bus, behind a callbackQueue only 16 entries deep, which
  // overruns in 1.5-3.6 ms of back-to-back traffic at 500 kbit/s and drops silently.
  //
  // So: copy and return. No send (twai_transmit can block 4 ms in the one task that must
  // keep draining that queue, and ForceRecoveryTask can only run when task_CAN blocks —
  // making a blocked send the single most likely moment for this task to be deleted
  // underneath us). No log. No allocation. No lock (the task can be deleted while holding
  // it). No clock read. No user callback. Budget: 128 bytes of stack, because the task is
  // not ours and its size is hard-coded by enable().
  static void onFrame(CAN_FRAME* f) {
    Esp32CanLink* const self = s_self;
    if (!self || !f) return;
    if (f->rtr || f->extended) return;          // AFFA is 11-bit data frames only
    Frame out;
    out.id  = f->id;
    out.ext = false;
    uint8_t n = f->length;                       // driver copies data_length_code
    if (n > 8) n = 8;                            // unchecked, so clamp here
    out.len = n;
    for (uint8_t i = 0; i < n; ++i) out.data[i] = f->data.uint8[i];
    self->ingest(out);
  }
};

bool Esp32CanLink::begin(CanPins pins, uint32_t bitrate) {
  if (_began) {
    AFFA_LOGE(kTag, "begin() called twice: a second call reinstalls the driver on a live "
                    "bus, leaks both queues and wipes all 32 filter slots. Refused.");
    return false;
  }
  if (s_self && s_self != this) {
    AFFA_LOGE(kTag, "another Esp32CanLink already owns the general callback. Refused.");
    return false;
  }

  _rx.reset();
  _stats = Stats{};

  // Order is load-bearing; see the header. watchFor() is LAST.
  CAN0.setCANPins(pins.rx, pins.tx);
  CAN0.begin(bitrate);                      // return value is the requested rate, not a
                                            // health check — verify below instead
  s_self = this;
  CAN0.setGeneralCallback(&Esp32CanTrampoline::onFrame);
  CAN0.watchFor();

  twai_status_info_t st;
  if (twai_get_status_info(&st) != ESP_OK || st.state != TWAI_STATE_RUNNING) {
    AFFA_LOGE(kTag, "driver not RUNNING after begin(%lu) — is that bitrate in "
                    "valid_timings[]?", static_cast<unsigned long>(bitrate));
    s_self = nullptr;
    return false;
  }

  _began = true;
  AFFA_LOGI(kTag, "up: rx=%d tx=%d %lu bit/s",
            static_cast<int>(pins.rx), static_cast<int>(pins.tx),
            static_cast<unsigned long>(bitrate));
  return true;
}

void Esp32CanLink::ingest(const Frame& f) {
  if (_rx.push(f)) ++_stats.rxFrames;
  // A failed push already bumped the ring's own overflow counter. Nothing else happens
  // here: reporting it is poll()'s job, on the consumer's task.
}

bool Esp32CanLink::send(const Frame& f) {
  if (!_began || !_txEnabled) { ++_stats.txDropped; return false; }

  CAN_FRAME out;
  out.id       = f.id;
  out.extended = 0;
  out.rtr      = 0;
  out.length   = (f.len > 8) ? 8 : f.len;
  out.fid      = 0;
  out.priority = 0;
  for (uint8_t i = 0; i < 8; ++i) out.data.uint8[i] = f.data[i];

  // NEVER TEST THE RETURN VALUE. sendFrame() is a literal `return true` on every path in
  // esp32_can_builtin.cpp:681 — including timeout-and-drop, driver-not-installed and
  // listen-only-refuses. Deriving txDropped from it produces a link that reports success
  // while transmitting nothing. Delivery evidence comes from the panel's ACK on
  // funcId|0x400 and from the controller counters read in stats().
  CAN0.sendFrame(out);
  ++_stats.txFrames;
  return true;
}

bool Esp32CanLink::recv(Frame& out) { return _rx.pop(out); }

bool Esp32CanLink::isLive() const {
  if (!_began || !_txEnabled) return false;
  twai_status_info_t st;
  // Side-effect-free snapshot. This is the whole of our bus-off handling: report it and
  // let AffaDisplayBase stop enqueueing. The driver's watchdog owns recovery, and its
  // default path ends in TWAI_STATE_STOPPED with no restart — which is precisely what
  // this returns false for.
  if (twai_get_status_info(&st) != ESP_OK) return false;
  return st.state == TWAI_STATE_RUNNING;
}

Stats Esp32CanLink::stats() const {
  Stats s = _stats;
  s.ringOverflow = _rx.overflow();
  twai_status_info_t st;
  if (twai_get_status_info(&st) == ESP_OK) {
    s.txErr    = st.tx_error_counter;
    s.rxErr    = st.rx_error_counter;
    s.txFailed = st.tx_failed_count;
  }
  return s;
}

void Esp32CanLink::setTxEnabled(bool on) { _txEnabled = on; }
bool Esp32CanLink::txEnabled() const { return _txEnabled; }

} // namespace affa

#endif // AFFA_ENABLE_ESP32CAN_LINK
