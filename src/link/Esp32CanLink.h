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
// PROHIBITION, not advice. After begin() returns, this class never touches the driver
// again except send() and twai_get_status_info(). It must NEVER call setListenOnlyMode(),
// setNoACKMode(), enable(), disable(), set_baudrate(), beginAutoSpeed(),
// forceDriverRestart(), setDebuggingMode(true), a second watchFor(), a per-mailbox
// callback on mailbox 0 or 1, or any state-mutating twai_* function (twai_start,
// twai_stop, twai_driver_install/uninstall, twai_initiate_recovery,
// twai_clear_transmit_queue, twai_reconfigure_alerts).
//
// Reason (esp32_can_builtin.cpp:450-462): every runtime mode setter is disable() +
// assignment + enable() — twai_stop mid-frame, vTaskDelete of both RX tasks, uninstall,
// install, two new tasks, twai_start. A driver reinstall on a live bus; from inside the
// general callback it deletes its own caller.
//
// It also implements NO bus-off recovery of its own: the driver's watchdog owns bus-off,
// and two initiators racing twai_initiate_recovery() is how a half-recovered peripheral
// happens. Note the driver's DEFAULT recovery does not restore service — it leaves the
// controller STOPPED with nothing calling twai_start() — so isLive() reports it, and
// automatic restoration is the application passing forceRecoveryMs below.
//
// docs/ESP32CAN-CONTRACT.md has the file:line citation behind every sentence.
class Esp32CanLink final : public ICanLink {
 public:
  Esp32CanLink() = default;

  // Exactly this sequence, exactly once:
  //   CAN0.setCANPins(pins.rx, pins.tx);  // signature IS (rx, tx) —
  //                                       // esp32_can_builtin.h:93. Never confuse it with
  //                                       // TWAI_GENERAL_CONFIG_DEFAULT, which is (tx, rx, mode).
  //   CAN0.begin(bitrate);
  //   CAN0.setGeneralCallback(&trampoline);
  //   CAN0.watchFor();                    // LAST: it opens the software filter, and with a
  //                                       // filter set but no callback registered
  //                                       // processFrame() fills a 64-deep rx_queue that
  //                                       // nothing will ever drain.
  //
  // Returns false if the driver did not end up RUNNING — the only way an application can
  // learn it was never installed, since CAN0.begin() returns the requested bitrate even
  // for a rate absent from valid_timings[], having installed nothing. Pass a rate the
  // driver knows; 500000 for this bus.
  //
  // A second call on an already-begun instance returns false and logs (it would be a live
  // reinstall plus a queue leak plus a wipe of all 32 filter slots), and a second instance
  // cannot steal the callback.
  //
  // forceRecoveryMs: 0 leaves the driver's bus-off policy as it ships — recovery ends in
  // TWAI_STATE_STOPPED and isLive() reports the link down. Whether to bring a shared bus
  // back up by itself is the application's call. Non-zero arms CAN0.setForceRecovery(true,
  // ms) BEFORE CAN0.begin() (the one path the prohibition permits), whose watchdog does a
  // full uninstall/delay/reinstall ending RUNNING. Unlike twai_initiate_recovery() it
  // needs no bus traffic, which matters on a two-node bus: once we stop ACKing, the panel
  // goes quiet too and nothing generates the recessive bits standard recovery waits for.
  // Bench value 2000; the bus is dead for that long.
  //
  // ListenOnly never emits a dominant bit — no ACKs, no error frames, no transmissions —
  // so it cannot be driven bus-off or disturb anything. It answers the first question
  // worth asking on a bench that will not talk: can we read this bus at all? Clean here
  // and broken in Normal means our transmit side; broken here too means bitrate or wiring.
  // Normal is the only mode in which the library can do its job: unacknowledged frames are
  // retransmitted by the panel forever.
  enum class LinkMode : uint8_t { Normal, ListenOnly };

  bool begin(CanPins pins, uint32_t bitrate = 500000, uint32_t forceRecoveryMs = 0,
             LinkMode mode = LinkMode::Normal);

  bool  send(const Frame& f) override;   // never blocks longer than the driver's ~4 ms
                                         // worst case; false if the TX gate is shut
  bool  recv(Frame& out) override;       // pops the RX ring
  bool  isLive() const override;         // began, TX gate open, controller RUNNING
  Stats stats() const override;

  // "Silent mode" is a SOFTWARE TX GATE: send() returns false. It is not a driver mode
  // change — see the prohibition. With the gate shut the controller still ACKs other nodes,
  // which on a two-node bus is required or the panel retransmits every frame forever.
  void setTxEnabled(bool on);
  bool txEnabled() const;

  // Raw controller state, for bring-up. Stats::rxFrames counts what reached OUR callback;
  // this counts what reached the DRIVER. msgsToRx climbing with rxFrames flat means the
  // controller receives and esp32_can does not deliver; both at zero means nothing arrives
  // at the peripheral. Nothing in the library reacts to it.
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

  // Called from task_CAN (prio 15) via the driver's general callback. Pushes into the ring
  // and returns: no logging, allocation, blocking, clock read or user code — the reason
  // ICanLink is a pull port. The driver's CAN_FRAME* points at task_CAN's stack and dies on
  // return, so the copy is mandatory.
  void ingest(const Frame& f);

  AffaRing<Frame, AFFA_RX_RING_DEPTH> _rx;

  // The only counter task_CAN touches, hence the only one that may not live in the plain
  // Stats struct: stats() copies that struct field by field from the poll() task, which
  // would race a `++` on another core. Relaxed suffices — it is a diagnostic and orders
  // nothing. Folded into Stats::rxFrames by stats(), so the public shape is unchanged.
  std::atomic<uint32_t> _rxFrames{0};

  Stats _stats{};                 // poll()-task only
  bool  _began = false;
  bool  _txEnabled = true;
};

} // namespace affa
#endif // AFFA_ENABLE_ESP32CAN_LINK
