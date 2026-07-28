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
  // ListenOnly emits no dominant bit at all, so it cannot be driven bus-off or disturb
  // anything. It answers the first question worth asking on a bench that will not talk:
  // can we read this bus AT ALL? Clean here and broken in Normal isolates the fault to our
  // transmit side. Normal is the only mode the library can work in — an unacknowledged
  // frame is retransmitted by the panel forever.
  enum class LinkMode : uint8_t { Normal, ListenOnly };

  bool begin(CanPins pins, uint32_t bitrate = 500000, uint32_t forceRecoveryMs = 0,
             LinkMode mode = LinkMode::Normal);

  bool  send(const Frame& f) override;   // never blocks longer than the driver's ~4 ms
                                         // worst case; false if the TX gate is shut
  bool  recv(Frame& out) override;       // pops the RX ring
  bool  isLive() const override;         // began, TX gate open, controller RUNNING
  Stats stats() const override;

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
  bool  _txEnabled = true;
};

} // namespace affa
#endif // AFFA_ENABLE_ESP32CAN_LINK
