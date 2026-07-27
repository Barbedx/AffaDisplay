#pragma once
#include "../AffaConfig.h"
#if AFFA_ENABLE_ESP32CAN_LINK

#include <driver/gpio.h>          // gpio_num_t ONLY. Not <esp32_can.h>.
#include "../core/ICanLink.h"
#include "../core/AffaRing.h"

namespace affa {

// Named so the two pins cannot be swapped at the call site. They have been, and the
// symptom is a silent bus: no TX error, no RX, nothing.
//   this board (ESP32-C3 SuperMini) : rx = GPIO_NUM_4, tx = GPIO_NUM_3
//   MeganeCAN's board is MIRRORED   : rx = GPIO_NUM_3, tx = GPIO_NUM_4
struct CanPins { gpio_num_t rx; gpio_num_t tx; };

// THE ONLY CLASS IN THIS LIBRARY THAT KNOWS A DRIVER EXISTS.
//
// The list below is a PROHIBITION, not advice.
//
// After begin() returns, this class never touches the driver again except to call
// sendFrame() and to read twai_get_status_info(). Specifically it must NEVER call
// setListenOnlyMode(), setNoACKMode(), enable(), disable(), set_baudrate(),
// beginAutoSpeed(), forceDriverRestart(), setDebuggingMode(true), a second watchFor(), a
// per-mailbox callback on mailbox 0 or 1, or any state-mutating twai_* function
// (twai_start, twai_stop, twai_driver_install, twai_driver_uninstall,
// twai_initiate_recovery, twai_clear_transmit_queue, twai_reconfigure_alerts).
//
// Reason, verified in esp32_can_builtin.cpp:450-462: every runtime mode setter is
// implemented as disable() + assignment + enable(), i.e. twai_stop mid-frame,
// vTaskDelete of both RX tasks, twai_driver_uninstall, twai_driver_install, two new
// tasks, twai_start — a driver reinstall on a live bus. From inside the general callback
// any of them deletes its own caller. That is what repeatedly left the controller stopped
// in the previous project. MeganeCAN worked for months precisely because it never touched
// the driver after begin().
//
// It also implements NO bus-off recovery of its own. The driver's watchdog owns bus-off,
// and two initiators racing twai_initiate_recovery() on one controller is how a
// half-recovered peripheral happens. Be aware that the driver's DEFAULT recovery does not
// restore service — twai_initiate_recovery() leaves the controller STOPPED and nothing
// calls twai_start() again — so isLive() reports it and automatic restoration is the
// application calling CAN0.setForceRecovery(true) BEFORE begin(), with its 2 s outage.
//
// See docs/ESP32CAN-CONTRACT.md for the file:line citations behind every sentence above.
class Esp32CanLink final : public ICanLink {
 public:
  Esp32CanLink() = default;

  // Exactly this sequence, exactly once:
  //   CAN0.setCANPins(pins.rx, pins.tx);   // signature IS (rx, tx) — verified in
  //                                        // esp32_can_builtin.h:93:
  //                                        // setCANPins(gpio_num_t rxPin, gpio_num_t txPin)
  //                                        // TWAI_GENERAL_CONFIG_DEFAULT is (tx, rx, mode)
  //                                        // and must never be confused with it.
  //   CAN0.begin(bitrate);
  //   CAN0.setGeneralCallback(&trampoline);
  //   CAN0.watchFor();                     // LAST: it is the gate that opens the software
  //                                        // filter, and with a filter configured but no
  //                                        // callback registered, processFrame() fills a
  //                                        // 64-deep rx_queue nothing will ever drain.
  //
  // Returns false if the driver did not end up RUNNING. That check is the only way an
  // application can learn the driver was never installed: CAN0.begin() returns the
  // requested bitrate even for a rate absent from valid_timings[], having installed
  // nothing. Pass only a rate the driver knows — 500000 for this bus.
  //
  // Calling this twice is a live driver reinstall plus a queue leak plus a wipe of all
  // 32 filter slots, so a second call on an already-begun instance returns false and
  // logs, and a second instance cannot steal the callback.
  bool begin(CanPins pins, uint32_t bitrate = 500000);

  bool  send(const Frame& f) override;   // never blocks longer than the driver's ~4 ms
                                         // worst case; false if the TX gate is shut
  bool  recv(Frame& out) override;       // pops the RX ring
  bool  isLive() const override;         // began, TX gate open, controller RUNNING
  Stats stats() const override;

  // "Silent mode" is a SOFTWARE TX GATE: send() simply returns false. It is NOT a driver
  // mode change and it never will be — see the prohibition above. Note that with the gate
  // shut the controller still ACKs other nodes at the link layer, which on a two-node bus
  // is required: without our ACK the panel retransmits every frame forever.
  void setTxEnabled(bool on);
  bool txEnabled() const;

 private:
  friend struct Esp32CanTrampoline;   // defined in the .cpp, where CAN_FRAME exists

  // Called from task_CAN (prio 15) via the driver's general callback. Pushes into the
  // ring and returns. It does not log, allocate, block, read a clock, or call user code —
  // the whole reason ICanLink is a pull port. The CAN_FRAME* the driver hands over points
  // at task_CAN's stack and dies on return, so the copy is mandatory.
  void ingest(const Frame& f);

  AffaRing<Frame, AFFA_RX_RING_DEPTH> _rx;
  Stats _stats{};
  bool  _began = false;
  bool  _txEnabled = true;
};

} // namespace affa
#endif // AFFA_ENABLE_ESP32CAN_LINK
