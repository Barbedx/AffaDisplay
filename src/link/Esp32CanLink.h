#pragma once
#include "../AffaConfig.h"
#if AFFA_ENABLE_ESP32CAN_LINK

#include <driver/gpio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <atomic>

#include "../core/ICanLink.h"
#include "../core/AffaRing.h"

namespace affa {

// Keep the call-site names in signal direction order. The ESP-IDF TWAI macro itself takes
// TX then RX; Esp32CanLink performs that inversion in exactly one place.
struct CanPins { gpio_num_t rx; gpio_num_t tx; };

// Direct ESP-IDF TWAI transport. It owns the controller, one bounded RX task, and every
// driver lifecycle transition. The protocol only sees the nonblocking ICanLink seam.
class Esp32CanLink final : public ICanLink {
 public:
  enum class LinkMode : uint8_t { Normal, ListenOnly };

  Esp32CanLink() = default;

  // Starts direct TWAI at the requested supported bitrate. GPIO intent remains `rx, tx` at
  // the public boundary; the driver configuration is deliberately supplied `tx, rx`.
  // `forceRecoveryMs` is retained for source compatibility; recovery is now owned by
  // recover() and the Affa poll backoff, not a hidden driver task.
  bool begin(CanPins pins, uint32_t bitrate = 500000, uint32_t forceRecoveryMs = 0,
             LinkMode mode = LinkMode::Normal);

  // A zero-timeout direct submit: never waits for the controller queue. true means TWAI
  // accepted the frame, not that the panel processed it; panel 0x74 remains the protocol
  // completion proof.
  bool send(const Frame& f) override;
  TxDisposition trySend(const Frame& f) override;
  bool recv(Frame& out) override;
  bool isLive() const override;
  bool healthy() const override;
  bool recover(bool force = false) override;
  Stats stats() const override;

  // Direct TWAI supports real listen-only from startup and after a controlled restart.
  // In that mode healthy() remains true while isLive() is false, so protocol TX is held.
  bool setListenOnly(bool on);
  bool listenOnly() const { return _listenOnly.load(std::memory_order_relaxed); }

  // Software gate for OTA / application silence. It never reconfigures TWAI.
  void setTxEnabled(bool on);
  bool txEnabled() const;

  uint32_t restarts() const { return _restarts.load(std::memory_order_relaxed); }

  struct DriverState {
    bool     valid = false;
    uint8_t  state = 0;  // TWAI state: stopped, running, bus-off, recovering
    uint32_t msgsToRx = 0, msgsToTx = 0;
    uint32_t txErr = 0, rxErr = 0, busErr = 0, arbLost = 0, rxMissed = 0;
  };
  DriverState driverState() const;

 private:
  static void rxTask(void* arg);

  bool installAndStart(LinkMode mode);
  // `reconfiguringAlreadyHeld` keeps the software TX gate closed across recover()'s
  // status probe and a forced full restart; accepted frames must never be cleared by the
  // immediately following twai_stop().
  bool restartDriver(LinkMode mode, bool reconfiguringAlreadyHeld = false);
  bool startRxTask();
  bool stopRxTask();
  bool controllerRunning() const;
  void ingest(const Frame& f);

  AffaRing<Frame, AFFA_RX_RING_DEPTH> _rx;

  CanPins  _pins{GPIO_NUM_0, GPIO_NUM_0};
  uint32_t _bitrate = 0;
  uint32_t _forceRecoveryMs = 0;
  LinkMode _mode = LinkMode::Normal;

  std::atomic<uint32_t> _rxFrames{0};
  std::atomic<uint32_t> _txFrames{0};
  std::atomic<uint32_t> _txDropped{0};
  std::atomic<uint32_t> _restarts{0};

  std::atomic<bool> _began{false};
  std::atomic<bool> _txEnabled{true};
  std::atomic<bool> _listenOnly{false};
  std::atomic<bool> _reconfiguring{false};
  std::atomic<bool> _rxStop{false};
  std::atomic<bool> _rxExited{true};
  SemaphoreHandle_t _driverMutex = nullptr;
  TaskHandle_t _rxTask = nullptr;
};

}  // namespace affa
#endif  // AFFA_ENABLE_ESP32CAN_LINK
