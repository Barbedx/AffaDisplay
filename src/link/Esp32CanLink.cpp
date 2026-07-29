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

// periph_module_reset() moved headers between IDF 4 and 5 and is a private API in 5.x. Both
// spellings are tried and the knob turns itself off if neither exists, because a core we
// have not seen must not become a build failure in the one file a consumer cannot patch.
#if AFFA_ESP32CAN_PERIPH_RESET
#  if __has_include(<esp_private/periph_ctrl.h>)
#    include <esp_private/periph_ctrl.h>
#  elif __has_include(<driver/periph_ctrl.h>)
#    include <driver/periph_ctrl.h>
#  else
#    undef  AFFA_ESP32CAN_PERIPH_RESET
#    define AFFA_ESP32CAN_PERIPH_RESET 0
#  endif
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
//
// ATOMIC because it is written by the task that calls begin() and read by task_CAN, which
// is already running by the time begin()'s failure path clears it: the callback is
// registered and watchFor() has opened the filter before twai_get_status_info() is
// consulted. Relaxed — publication of *this is ordered by the setGeneralCallback() call
// that follows the store, not by this variable.
std::atomic<Esp32CanLink*> s_self{nullptr};

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
    Esp32CanLink* const self = s_self.load(std::memory_order_relaxed);
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

bool Esp32CanLink::begin(CanPins pins, uint32_t bitrate, uint32_t forceRecoveryMs,
                         LinkMode mode) {
  if (_began) {
    AFFA_LOGE(kTag, "begin() called twice: a second call reinstalls the driver on a live "
                    "bus, leaks both queues and wipes all 32 filter slots. Refused.");
    return false;
  }
  Esp32CanLink* const owner = s_self.load(std::memory_order_relaxed);
  if (owner && owner != this) {
    AFFA_LOGE(kTag, "another Esp32CanLink already owns the general callback. Refused.");
    return false;
  }

  _rx.reset();
  _stats = Stats{};
  _rxFrames.store(0, std::memory_order_relaxed);

  // MAKE A WARM BOOT LOOK LIKE A COLD ONE. ESP.restart() does not reset peripherals, so
  // after an OTA update or a watchdog restart the TWAI block still holds the previous run's
  // configuration and the driver installs on top of it. On the bench rig that degraded
  // measurably with every warm reboot — flaps 7 -> 139, bus errors 0 -> 827 across eight
  // flashes in one evening — and ended in a controller cycling through BUS_OFF for ever
  // beside a healthy panel. Only a power cycle cleared it.
  //
  // This is the one moment it is safe: no driver is installed (the _began and s_self guards
  // above have just proved it), and it is a reset-control write rather than a twai_* call,
  // so the prohibition in the header is intact. AFFA_ESP32CAN_PERIPH_RESET=0 opts out for a
  // firmware where something else owns the peripheral.
#if AFFA_ESP32CAN_PERIPH_RESET
  periph_module_reset(PERIPH_TWAI_MODULE);
  AFFA_LOGI(kTag, "TWAI peripheral reset before install (warm boots do not do this)");
#endif

  // BEFORE begin(), which is the one window in which touching the driver is sanctioned:
  // setForceRecovery only writes two members (esp32_can_builtin.cpp:88-92) and the
  // watchdog task reads them later. Calling it after begin() would be no more dangerous,
  // but keeping every driver call on this side of begin() is what makes the prohibition
  // in the header a line you can grep for rather than a judgement call.
  if (forceRecoveryMs) {
    CAN0.setForceRecovery(true, forceRecoveryMs);
    AFFA_LOGI(kTag, "bus-off auto-recovery armed: full driver restart after %lums",
              static_cast<unsigned long>(forceRecoveryMs));
  }

  // LISTEN-ONLY IS NOT AVAILABLE, AND THE REASON IS WORTH THE PARAGRAPH.
  //
  // The obvious implementation is CAN0.setListenOnlyMode(true) before begin(), reasoning
  // that the prohibition only covers calls AFTER begin() and that its internal disable()
  // is a no-op with no driver installed. Both halves of that are true and the conclusion
  // is still wrong: setListenOnlyMode() is disable() THEN enable(), and enable() creates
  // task_CAN and task_LowLevelRX, which block on callbackQueue and rx_queue — queues that
  // only _init() creates, and _init() has not run yet. The driver comes up with two tasks
  // waiting on null handles and the board is dead before WiFi, i.e. before OTA. Measured,
  // on the bench, at the cost of a cable.
  //
  // Calling it AFTER begin() is worse: that is the live-reinstall the header forbids.
  // So there is no safe place for it in this driver, and the parameter is honoured only
  // by refusing to pretend.
  if (mode == LinkMode::ListenOnly) {
    AFFA_LOGE(kTag, "LinkMode::ListenOnly is not implementable on collin80/esp32_can — "
                    "setListenOnlyMode() installs the driver before its queues exist. "
                    "Refused; bring the link up Normal and shut the software TX gate "
                    "instead (that stops OUR frames, though the controller still ACKs).");
    return false;
  }

  // Order is load-bearing; see the header. watchFor() is LAST.
  CAN0.setCANPins(pins.rx, pins.tx);
  CAN0.begin(bitrate);                      // return value is the requested rate, not a
                                            // health check — verify below instead
  s_self.store(this, std::memory_order_relaxed);
  CAN0.setGeneralCallback(&Esp32CanTrampoline::onFrame);
  CAN0.watchFor();

  twai_status_info_t st;
  if (twai_get_status_info(&st) != ESP_OK || st.state != TWAI_STATE_RUNNING) {
    AFFA_LOGE(kTag, "driver not RUNNING after begin(%lu) — is that bitrate in "
                    "valid_timings[]?", static_cast<unsigned long>(bitrate));
    s_self.store(nullptr, std::memory_order_relaxed);
    return false;
  }

  _began = true;
  AFFA_LOGI(kTag, "up: rx=%d tx=%d %lu bit/s",
            static_cast<int>(pins.rx), static_cast<int>(pins.tx),
            static_cast<unsigned long>(bitrate));
  return true;
}

void Esp32CanLink::ingest(const Frame& f) {
  // Relaxed atomic, not `++_stats.rxFrames`: this runs in task_CAN while stats() copies
  // the Stats struct on the poll() task.
  if (_rx.push(f)) _rxFrames.fetch_add(1, std::memory_order_relaxed);
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
  // The gate is the ONLY thing this adds over healthy(), and it is what makes isLive() mean
  // "may I transmit right now" while healthy() means "is there a controller at all".
  if (!_txEnabled) return false;
  return healthy();
}

bool Esp32CanLink::healthy() const {
  if (!_began) return false;
  twai_status_info_t st;
  // Side-effect-free snapshot. The driver's own watchdog reacts to BUS_OFF only, so a
  // controller left in TWAI_STATE_STOPPED — which is where forceRecoveryMs=0 leaves it, and
  // where a failed recovery leaves it — is never restarted by anybody. That is precisely
  // what this returns false for, and what AffaDisplayBase::pumpLink() now acts on by calling
  // recover() rather than leaving it to the application to reboot the board.
  if (twai_get_status_info(&st) != ESP_OK) return false;
  return st.state == TWAI_STATE_RUNNING;
}

bool Esp32CanLink::recover() {
  // Never before begin(). There is no driver to restart, and forceDriverRestart() would
  // call enable() on a configuration that setCANPins() has never been given.
  if (!_began) return false;

  twai_status_info_t st;
  if (twai_get_status_info(&st) != ESP_OK) {
    // The status read itself failed, which means the driver is not installed at all. That is
    // not a state a restart is defined over — enable() would install on top of nothing known
    // — so report the link down and let the backoff try again.
    AFFA_LOGE(kTag, "recover(): twai_get_status_info() failed; driver not installed?");
    return false;
  }

  if (st.state == TWAI_STATE_RUNNING) return true;   // nothing to do; somebody beat us to it

  // RECOVERING is the driver's own bus-off path in progress. twai_driver_uninstall() rejects
  // it (IDF requires STOPPED or BUS_OFF), so disable() would leave both tasks deleted and the
  // driver still installed — and enable()'s install would then fail with INVALID_STATE and
  // return early, killing the link permanently. Refusing here is what keeps a transient
  // bus-off from becoming that.
  if (st.state == TWAI_STATE_RECOVERING) {
    AFFA_LOGI(kTag, "recover(): controller is RECOVERING — leaving it to finish");
    return false;
  }

  AFFA_LOGW(kTag, "restarting the TWAI driver (state %u, txErr %lu, rxErr %lu, busErr %lu)",
            static_cast<unsigned>(st.state),
            static_cast<unsigned long>(st.tx_error_counter),
            static_cast<unsigned long>(st.rx_error_counter),
            static_cast<unsigned long>(st.bus_error_count));

  // Shut the software gate across the restart and restore the CALLER'S setting afterwards,
  // not an unconditional true: an application that gated TX off on purpose — an OTA write is
  // in progress, the bench is running the is-it-us test — must not have that silently undone
  // by a recovery it did not ask for.
  const bool wasEnabled = _txEnabled;
  _txEnabled = false;

  // disable() + settle + enable(). Pins, filters and the general callback are ESP32CAN
  // members and survive; re-issuing watchFor() here would burn a second filter slot on the
  // same id. See the header.
  CAN0.forceDriverRestart();
  ++_restarts;

  _txEnabled = wasEnabled;

  // A frame that task_CAN was pushing when it was deleted is lost, not corrupting: AffaRing
  // publishes its head last (core/AffaRing.h). So the ring is NOT reset here — doing so
  // would throw away frames the panel sent that we have not decoded yet, and the overflow
  // counter would lose the history that says whether we are keeping up.
  if (twai_get_status_info(&st) != ESP_OK || st.state != TWAI_STATE_RUNNING) {
    AFFA_LOGE(kTag, "restart did not leave the controller RUNNING (state %u)",
              static_cast<unsigned>(st.state));
    return false;
  }

  AFFA_LOGI(kTag, "TWAI driver restarted; controller RUNNING (restart %lu)",
            static_cast<unsigned long>(_restarts));
  return true;
}

Stats Esp32CanLink::stats() const {
  Stats s = _stats;
  s.rxFrames     = _rxFrames.load(std::memory_order_relaxed);
  s.ringOverflow = _rx.overflow();
  twai_status_info_t st;
  if (twai_get_status_info(&st) == ESP_OK) {
    s.txErr    = st.tx_error_counter;
    s.rxErr    = st.rx_error_counter;
    s.txFailed = st.tx_failed_count;
  }
  return s;
}

Esp32CanLink::DriverState Esp32CanLink::driverState() const {
  DriverState d;
  twai_status_info_t st;
  if (twai_get_status_info(&st) != ESP_OK) return d;   // valid stays false
  d.valid    = true;
  d.state    = static_cast<uint8_t>(st.state);
  d.msgsToRx = st.msgs_to_rx;
  d.msgsToTx = st.msgs_to_tx;
  d.txErr    = st.tx_error_counter;
  d.rxErr    = st.rx_error_counter;
  d.busErr   = st.bus_error_count;
  d.arbLost  = st.arb_lost_count;
  d.rxMissed = st.rx_missed_count;
  return d;
}

void Esp32CanLink::setTxEnabled(bool on) { _txEnabled = on; }
bool Esp32CanLink::txEnabled() const { return _txEnabled; }

} // namespace affa

#endif // AFFA_ENABLE_ESP32CAN_LINK
