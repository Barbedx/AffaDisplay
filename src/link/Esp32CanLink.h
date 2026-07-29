#pragma once
#include "../AffaConfig.h"
#if AFFA_ENABLE_ESP32CAN_LINK

#include <driver/gpio.h>          // gpio_num_t ONLY. Not <esp32_can.h>.
#include <atomic>
#include "../core/ICanLink.h"
#include "../core/AffaRing.h"

namespace affa {

// Named so the two pins cannot be swapped at the call site. They have been; the symptom is
// a silent bus — no TX error, no RX, nothing.
//   this board (ESP32-C3 SuperMini) : rx = GPIO_NUM_4, tx = GPIO_NUM_3
//   MeganeCAN's board is MIRRORED   : rx = GPIO_NUM_3, tx = GPIO_NUM_4
struct CanPins { gpio_num_t rx; gpio_num_t tx; };

// The only class in this library that knows a driver exists.
//
// PROHIBITION, NOT ADVICE. After begin() returns, this class never touches the driver again
// except send() and twai_get_status_info() — no mode setter, no second watchFor(), no
// state-mutating twai_* call, and no bus-off recovery of its own. Every runtime mode setter
// is disable()+enable() underneath, i.e. a driver reinstall on a live bus that from inside
// the general callback deletes its own caller.
//
// docs/ESP32CAN-CONTRACT.md is the contract: rules 1-21, with a file:line citation behind
// every sentence. Read rule 4 before adding any call here.
class Esp32CanLink final : public ICanLink {
 public:
  Esp32CanLink() = default;

  // Runs one fixed sequence once — setCANPins(rx, tx), begin(), setGeneralCallback(),
  // watchFor() LAST — and each step's ordering has a reason: CONTRACT rule 2, and rule 3
  // for why the pin order is (rx, tx) and not the driver config's (tx, rx, mode).
  //
  // Returns false if the driver did not end up RUNNING. That is the ONLY way to learn it
  // was never installed: CAN0.begin() returns the requested bitrate even for a rate absent
  // from valid_timings[], having installed nothing. Pass a rate the driver knows — 500000.
  // A second call returns false rather than reinstalling on a live bus.
  //
  // forceRecoveryMs: 0 leaves bus-off recovery ending STOPPED and isLive() reporting the
  // link down — bringing a shared bus back is the application's policy. Non-zero arms
  // setForceRecovery() BEFORE begin(), the one path the prohibition permits. CONTRACT §3
  // has why the forced path is the only one that completes on a two-node bus; the bench
  // notes have why 250 and not 0 or 2000.
  //
  // NORMAL IS THE ONLY MODE THAT WORKS, AND ListenOnly IS REFUSED AT RUNTIME.
  //
  // begin(..., LinkMode::ListenOnly) logs an error and returns false — it does NOT bring the
  // link up in some degraded mode. collin80/esp32_can has no safe place to enter listen-only:
  // setListenOnlyMode() is disable()+enable(), and before begin() that enable() starts tasks
  // which block on queues _init() has not created yet (dead board, before WiFi, i.e. before
  // OTA — measured, at the cost of a cable), while after begin() it is the live driver
  // reinstall this class prohibits outright. The .cpp carries the full reasoning.
  //
  // THE SUBSTITUTE, for the question listen-only exists to answer — "can we read this bus at
  // all, and is the noise ours?" — is setTxEnabled(false): the software TX gate. It stops
  // every frame WE would send while the controller keeps ACKing other nodes in hardware,
  // which a two-node bus requires. If busErr keeps climbing and rxFrames stays 0 while the
  // gate is shut, we are silent and cannot be the cause; the fault is on the wire.
  enum class LinkMode : uint8_t { Normal, ListenOnly };

  bool begin(CanPins pins, uint32_t bitrate = 500000, uint32_t forceRecoveryMs = 0,
             LinkMode mode = LinkMode::Normal);

  bool  send(const Frame& f) override;   // never blocks longer than the driver's ~4 ms
                                         // worst case; false if the TX gate is shut OR the
                                         // driver is in listen-only (counted in txDropped)
  bool  recv(Frame& out) override;       // pops the RX ring
  bool  isLive() const override;         // began, TX gate open, NOT listen-only, RUNNING
  bool  healthy() const override;        // began and controller RUNNING — gate AND mode ignored
  Stats stats() const override;

  // THE PROHIBITION'S ONE EXCEPTION, and the only driver call this class makes after
  // begin() besides send() and twai_get_status_info(). CONTRACT rule 4a.
  //
  // Restarts the driver in place — ESP32CAN::forceDriverRestart(), which is disable() +
  // settle delay + enable(). That is exactly what the driver's OWN watchdog task calls for a
  // bus-off controller (esp32_can_builtin.cpp:140), so calling it from the poll task is the
  // same safety class as the path already shipping, PROVIDED it is not called from task_CAN.
  // AffaDisplayBase::pumpLink() is its only caller and runs on the poll owner, never in a
  // driver callback.
  //
  // Why it needs no re-configuration afterwards: filters[], the general callback and the pin
  // configuration are members of ESP32CAN and survive the cycle. Re-issuing watchFor() would
  // consume a SECOND of the 32 filter slots for the same id.
  //
  // BLOCKS for the restart's settle delay — hundreds of milliseconds. Refuses while the
  // controller is RECOVERING, because twai_driver_uninstall() rejects that state and a
  // half-torn-down driver is worse than a bus-off one; the caller's backoff will come back.
  //
  // `force` restarts even a controller that reports RUNNING. Without it, the RUNNING-but-
  // deaf state — state 1, rxErr pinned, reception frozen; measured 2026-07-29 — is
  // unreachable by recovery, because the early-out for "already running" (correct for the
  // bus-off race) answers success without touching anything.
  //
  // Returns whether the controller is RUNNING AFTERWARDS, never merely that a call was made.
  bool recover(bool force = false) override;

  // How many times recover() has actually restarted the driver. Survives nothing; it is here
  // to be read off a status endpoint before deciding the fault is in the wire.
  uint32_t restarts() const { return _restarts; }

  // ---- listen-only, AFTER begin() and only after ----------------------------------------
  //
  // begin(..., ListenOnly) is still refused, and that refusal is still right: before the
  // driver exists, setListenOnlyMode() is disable()+enable() on queues _init() has not
  // created, and the board dies before WiFi comes up. THIS call is a different thing. It
  // runs on a driver that is up, so its disable()+enable() is EXACTLY the operation
  // recover() already performs and the CONTRACT already sanctions (rule 4a) — same tasks
  // deleted and recreated, same settle, same requirement that it not be called from
  // task_CAN. Filters, pins and the general callback are ESP32CAN members and survive.
  //
  // WHY IT IS WORTH HAVING AT ALL, given the library can never render in this mode: it is
  // the only way to observe the bus WITHOUT ACKNOWLEDGING IT, and the difference between
  // those two is not a detail — on this bench it is the difference between decoding 157 000
  // frames cleanly and decoding none at 1472 bus errors a second. An application that cannot
  // toggle that one bit cannot tell "the panel is silent" from "our acknowledgement is what
  // destroys every frame", and those two demand opposite fixes.
  //
  // THE GATE AND THE MODE ARE INDEPENDENT. setTxEnabled() is the application's wish;
  // listen-only is the driver's state; send() refuses (and counts txDropped) when EITHER
  // forbids, and isLive() reports not-live in listen-only so the protocol layer HOLDS
  // renders instead of feeding a driver that silently drops them. The old design saved the
  // gate on entry and restored it on exit, which returned the ENTRY-time gate over anything
  // set while listening — measured on the bench as a gate that reopened itself.
  //
  // BLOCKS for the restart. Returns whether the controller is RUNNING afterwards; the mode
  // flag follows the DRIVER'S configuration even on a failed restart, so listenOnly() never
  // reports NORMAL over a listen-only driver.
  bool setListenOnly(bool on);
  bool listenOnly() const { return _listenOnly; }

  // "Silent mode" is a SOFTWARE TX gate — send() returns false. NOT a driver mode change
  // (the prohibition). The controller still ACKs other nodes, which a two-node bus requires.
  void setTxEnabled(bool on);
  bool txEnabled() const;

  // Raw controller state, for bring-up; nothing in the library reacts to it. Field meanings,
  // the recovery-cycle signature that makes a single sample misleading, and the TX-gate test
  // that proves whether a fault is ours or external: CONTRACT §3.1.
  struct DriverState {
    bool     valid    = false;   // false: twai_get_status_info() itself failed
    uint8_t  state    = 0;       // twai_state_t: 0 stopped, 1 running, 2 bus-off, 3 recovering
    uint32_t msgsToRx = 0;       // frames queued in the DRIVER, not yet taken by esp32_can
    uint32_t msgsToTx = 0;
    uint32_t txErr = 0, rxErr = 0, busErr = 0, arbLost = 0, rxMissed = 0;
  };
  DriverState driverState() const;

 private:
  friend struct Esp32CanTrampoline;   // defined in the .cpp, where CAN_FRAME exists

  // task_CAN (prio 15), via the general callback. Pushes into the ring and returns: no
  // logging, allocation, blocking, clock read or user code — the reason ICanLink is a pull
  // port. The driver's CAN_FRAME* dies on return, so the copy is mandatory. CONTRACT rule 6.
  void ingest(const Frame& f);

  AffaRing<Frame, AFFA_RX_RING_DEPTH> _rx;

  // The only counter task_CAN touches, so the only one that cannot live in plain Stats:
  // stats() copies that struct from the poll() task and would race a `++`. Relaxed suffices.
  std::atomic<uint32_t> _rxFrames{0};

  Stats _stats{};                 // poll()-task only
  bool  _began = false;
  bool  _txEnabled = true;        // the application's wish; independent of the mode
  bool  _listenOnly = false;      // the driver's mode; follows its config even on failure
  uint32_t _restarts = 0;         // completed forceDriverRestart() calls
};

} // namespace affa
#endif // AFFA_ENABLE_ESP32CAN_LINK
