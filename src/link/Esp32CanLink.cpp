#include "../AffaConfig.h"
#if AFFA_ENABLE_ESP32CAN_LINK

#include "Esp32CanLink.h"

#include <driver/twai.h>
#include <esp_err.h>

#include "../util/AffaLog.h"

namespace affa {
namespace {

constexpr const char* kTag = "LINK";
constexpr uint32_t kRxQueueLength = 128;
constexpr uint32_t kTxQueueLength = 16;
constexpr uint32_t kRxTaskStackBytes = 4096;
constexpr UBaseType_t kRxTaskPriority = 12;

// ESP-IDF's TWAI driver is process-global.  Keeping a single explicit owner avoids a
// second instance tearing down the first one's controller during begin()/recovery.
std::atomic<Esp32CanLink*> s_owner{nullptr};

bool timingFor(uint32_t bitrate, twai_timing_config_t& timing) {
  switch (bitrate) {
    case 1000000: timing = TWAI_TIMING_CONFIG_1MBITS(); return true;
    case 800000:  timing = TWAI_TIMING_CONFIG_800KBITS(); return true;
    case 500000:  timing = TWAI_TIMING_CONFIG_500KBITS(); return true;
    case 250000:  timing = TWAI_TIMING_CONFIG_250KBITS(); return true;
    case 125000:  timing = TWAI_TIMING_CONFIG_125KBITS(); return true;
    case 100000:  timing = TWAI_TIMING_CONFIG_100KBITS(); return true;
    case 50000:   timing = TWAI_TIMING_CONFIG_50KBITS(); return true;
    case 25000:   timing = TWAI_TIMING_CONFIG_25KBITS(); return true;
    default: return false;
  }
}

}  // namespace

bool Esp32CanLink::begin(CanPins pins, uint32_t bitrate, uint32_t forceRecoveryMs,
                         LinkMode mode) {
  if (_began.load(std::memory_order_acquire)) {
    AFFA_LOGE(kTag, "begin() called twice; direct TWAI ownership is unchanged");
    return false;
  }

  Esp32CanLink* expected = nullptr;
  if (!s_owner.compare_exchange_strong(expected, this, std::memory_order_acq_rel)) {
    AFFA_LOGE(kTag, "another Esp32CanLink already owns the TWAI controller");
    return false;
  }

  if (!_driverMutex) _driverMutex = xSemaphoreCreateMutex();
  if (!_driverMutex) {
    AFFA_LOGE(kTag, "could not allocate the TWAI lifecycle mutex");
    s_owner.store(nullptr, std::memory_order_release);
    return false;
  }

  _rx.reset();
  _pins = pins;
  _bitrate = bitrate;
  _forceRecoveryMs = forceRecoveryMs;
  _mode = mode;
  _rxFrames.store(0, std::memory_order_relaxed);
  _txFrames.store(0, std::memory_order_relaxed);
  _txDropped.store(0, std::memory_order_relaxed);
  _restarts.store(0, std::memory_order_relaxed);
  _listenOnly.store(mode == LinkMode::ListenOnly, std::memory_order_relaxed);
  _reconfiguring.store(false, std::memory_order_relaxed);

  if (!installAndStart(mode)) {
    _listenOnly.store(false, std::memory_order_relaxed);
    s_owner.store(nullptr, std::memory_order_release);
    return false;
  }

  _began.store(true, std::memory_order_release);
  AFFA_LOGI(kTag, "TWAI up: tx=%d rx=%d %lu bit/s%s",
            static_cast<int>(_pins.tx), static_cast<int>(_pins.rx),
            static_cast<unsigned long>(_bitrate),
            mode == LinkMode::ListenOnly ? " listen-only" : "");
  if (_forceRecoveryMs) {
    AFFA_LOGI(kTag, "forceRecoveryMs=%lu retained; poll() owns direct-TWAI recovery",
              static_cast<unsigned long>(_forceRecoveryMs));
  }
  return true;
}

bool Esp32CanLink::installAndStart(LinkMode mode) {
  twai_timing_config_t timing{};
  if (!timingFor(_bitrate, timing)) {
    AFFA_LOGE(kTag, "unsupported TWAI bitrate: %lu", static_cast<unsigned long>(_bitrate));
    return false;
  }

  // TWAI_GENERAL_CONFIG_DEFAULT takes TX first, RX second. CanPins deliberately exposes
  // signal direction as {rx, tx}, so this explicit inversion preserves its public contract.
  twai_general_config_t general = TWAI_GENERAL_CONFIG_DEFAULT(
      _pins.tx, _pins.rx,
      mode == LinkMode::ListenOnly ? TWAI_MODE_LISTEN_ONLY : TWAI_MODE_NORMAL);
  general.tx_queue_len = kTxQueueLength;
  general.rx_queue_len = kRxQueueLength;
  general.alerts_enabled = TWAI_ALERT_NONE;
  twai_filter_config_t filter = TWAI_FILTER_CONFIG_ACCEPT_ALL();

  esp_err_t err = twai_driver_install(&general, &timing, &filter);
  if (err != ESP_OK) {
    AFFA_LOGE(kTag, "twai_driver_install failed: %s", esp_err_to_name(err));
    return false;
  }
  err = twai_start();
  if (err != ESP_OK) {
    AFFA_LOGE(kTag, "twai_start failed: %s", esp_err_to_name(err));
    (void)twai_driver_uninstall();
    return false;
  }
  if (!startRxTask()) {
    AFFA_LOGE(kTag, "could not start TWAI RX task");
    (void)twai_stop();
    (void)twai_driver_uninstall();
    return false;
  }
  return true;
}

bool Esp32CanLink::startRxTask() {
  _rxStop.store(false, std::memory_order_release);
  _rxExited.store(false, std::memory_order_release);
  _rxTask = nullptr;
  const BaseType_t made = xTaskCreate(&Esp32CanLink::rxTask, "affa_twai_rx",
                                      kRxTaskStackBytes, this, kRxTaskPriority, &_rxTask);
  if (made == pdPASS) return true;
  _rxExited.store(true, std::memory_order_release);
  _rxTask = nullptr;
  return false;
}

void Esp32CanLink::rxTask(void* arg) {
  auto* const self = static_cast<Esp32CanLink*>(arg);
  while (!self->_rxStop.load(std::memory_order_acquire)) {
    twai_message_t message{};
    const esp_err_t err = twai_receive(&message, pdMS_TO_TICKS(20));
    if (err != ESP_OK) continue;
    if (message.extd || message.rtr) continue;  // AFFA uses 11-bit data frames only.

    Frame frame;
    frame.id = message.identifier;
    frame.ext = false;
    frame.len = message.data_length_code > 8 ? 8 : message.data_length_code;
    for (uint8_t i = 0; i < frame.len; ++i) frame.data[i] = message.data[i];
    self->ingest(frame);
  }
  self->_rxExited.store(true, std::memory_order_release);
  vTaskDelete(nullptr);
}

void Esp32CanLink::ingest(const Frame& f) {
  if (_rx.push(f)) _rxFrames.fetch_add(1, std::memory_order_relaxed);
}

bool Esp32CanLink::stopRxTask() {
  if (!_rxTask) {
    _rxExited.store(true, std::memory_order_release);
    return true;
  }

  _rxStop.store(true, std::memory_order_release);
  const TickType_t slice = pdMS_TO_TICKS(5) ? pdMS_TO_TICKS(5) : 1;
  for (uint8_t n = 0; n < 60; ++n) {  // RX wait is 20 ms; 300 ms is a bounded recovery pause.
    if (_rxExited.load(std::memory_order_acquire)) {
      _rxTask = nullptr;
      return true;
    }
    vTaskDelay(slice);
  }
  AFFA_LOGE(kTag, "RX task did not leave within 300 ms; driver left untouched");
  return false;
}

TxDisposition Esp32CanLink::trySend(const Frame& f) {
  if (!_began.load(std::memory_order_acquire) ||
      !_txEnabled.load(std::memory_order_relaxed) ||
      _listenOnly.load(std::memory_order_relaxed) ||
      _reconfiguring.load(std::memory_order_acquire) ||
      f.ext || f.id > 0x7FF || f.len > 8) {
    _txDropped.fetch_add(1, std::memory_order_relaxed);
    return TxDisposition::Rejected;
  }

  // Recovery takes this mutex before it can stop/uninstall the controller.  A normal send
  // never waits for it: busy here is the same non-blocking retry condition as a full TX FIFO.
  if (!_driverMutex || xSemaphoreTake(_driverMutex, 0) != pdTRUE) {
    if (_reconfiguring.load(std::memory_order_acquire)) {
      _txDropped.fetch_add(1, std::memory_order_relaxed);
      return TxDisposition::Rejected;
    }
    return TxDisposition::Busy;
  }

  // A recovery can raise the gate after the optimistic check above while this caller waits
  // for the mutex. Check again under the same exclusion that protects twai_stop/uninstall;
  // no frame that began after recovery announced itself may be accepted then cleared.
  if (_reconfiguring.load(std::memory_order_acquire)) {
    xSemaphoreGive(_driverMutex);
    _txDropped.fetch_add(1, std::memory_order_relaxed);
    return TxDisposition::Rejected;
  }

  twai_message_t message{};  // flags must stay zero: standard data, not self, not single-shot.
  message.identifier = f.id;
  message.data_length_code = f.len;
  for (uint8_t i = 0; i < f.len; ++i) message.data[i] = f.data[i];

  const esp_err_t err = twai_transmit(&message, 0);
  xSemaphoreGive(_driverMutex);
  if (err == ESP_OK) {
    _txFrames.fetch_add(1, std::memory_order_relaxed);
    return TxDisposition::Accepted;
  }

  if (err == ESP_ERR_TIMEOUT || err == ESP_FAIL) return TxDisposition::Busy;
  _txDropped.fetch_add(1, std::memory_order_relaxed);
  return TxDisposition::Rejected;
}

bool Esp32CanLink::send(const Frame& f) {
  return trySend(f) == TxDisposition::Accepted;
}

bool Esp32CanLink::recv(Frame& out) { return _rx.pop(out); }

bool Esp32CanLink::controllerRunning() const {
  // This takes the same zero-timeout lifecycle mutex as trySend().  It makes a concurrent
  // status query harmless while recovery is about to uninstall the driver: return "not
  // live right now" rather than dereferencing driver state during teardown. The poll owner
  // sees the mutex free immediately after its own send, so this never adds a wait to the
  // application path.
  if (!_driverMutex || xSemaphoreTake(_driverMutex, 0) != pdTRUE) return false;
  twai_status_info_t status{};
  const bool running = twai_get_status_info(&status) == ESP_OK &&
                       status.state == TWAI_STATE_RUNNING;
  xSemaphoreGive(_driverMutex);
  return running;
}

bool Esp32CanLink::isLive() const {
  return _began.load(std::memory_order_acquire) &&
         _txEnabled.load(std::memory_order_relaxed) &&
         !_listenOnly.load(std::memory_order_relaxed) &&
         !_reconfiguring.load(std::memory_order_acquire) && controllerRunning();
}

bool Esp32CanLink::healthy() const {
  return _began.load(std::memory_order_acquire) &&
         !_reconfiguring.load(std::memory_order_acquire) && controllerRunning();
}

bool Esp32CanLink::recover(bool force) {
  if (!_began.load(std::memory_order_acquire)) return false;
  if (_reconfiguring.exchange(true, std::memory_order_acq_rel)) return false;

  bool restarted = false;
  bool useFullRestart = false;
  if (!_driverMutex || xSemaphoreTake(_driverMutex, pdMS_TO_TICKS(50)) != pdTRUE) {
    _reconfiguring.store(false, std::memory_order_release);
    return false;
  }

  twai_status_info_t status{};
  const esp_err_t stateErr = twai_get_status_info(&status);
  if (stateErr == ESP_ERR_INVALID_STATE) {
    useFullRestart = true;  // Driver is gone: install a fresh known configuration below.
  } else if (stateErr != ESP_OK) {
    AFFA_LOGE(kTag, "recover(): status failed: %s", esp_err_to_name(stateErr));
  } else if (status.state == TWAI_STATE_RUNNING) {
    if (force) {
      useFullRestart = true;
    } else if (_rxExited.load(std::memory_order_acquire)) {
      // A running controller with no consumer task is not healthy. In particular, after a
      // failed task creation AffaDisplayBase cannot arm its RX-stall watchdog because it
      // deliberately waits for one real received frame first. Recreate the task now; if
      // that fails, retain the lifecycle gate and do a full bounded restart below.
      if (startRxTask()) restarted = true;
      else {
        AFFA_LOGE(kTag, "recover(): controller running but RX task could not restart");
        useFullRestart = true;
      }
    } else {
      restarted = true;
    }
  } else if (status.state == TWAI_STATE_BUS_OFF) {
    const esp_err_t err = twai_initiate_recovery();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
      AFFA_LOGE(kTag, "twai_initiate_recovery failed: %s", esp_err_to_name(err));
    }
    // The controller will become STOPPED after the mandated bus-free period.  A later poll
    // starts it; do not uninstall while it is RECOVERING.
  } else if (status.state == TWAI_STATE_STOPPED) {
    const esp_err_t err = twai_start();
    if (err == ESP_OK) {
      if (_rxExited.load(std::memory_order_acquire) && !startRxTask()) {
        AFFA_LOGE(kTag, "recover(): restarted controller but could not start RX task");
        // Do not report a RUNNING-but-deaf driver as recovered. restartDriver() will stop
        // and reinstall it while the reconfiguration gate remains closed.
        useFullRestart = true;
      } else {
        restarted = true;
      }
    } else {
      AFFA_LOGE(kTag, "recover(): twai_start failed: %s", esp_err_to_name(err));
      useFullRestart = true;
    }
  }
  // TWAI_STATE_RECOVERING deliberately remains down until a later poll observes STOPPED.

  xSemaphoreGive(_driverMutex);
  // Preserve the gate across a forced reinstall. Clearing it before restartDriver() lets a
  // concurrent send() queue a frame which twai_stop() immediately discards, turning an
  // Accepted disposition into a lie.
  if (useFullRestart) return restartDriver(_mode, /*reconfiguringAlreadyHeld=*/true);
  _reconfiguring.store(false, std::memory_order_release);
  return restarted;
}

bool Esp32CanLink::restartDriver(LinkMode mode, bool reconfiguringAlreadyHeld) {
  if (!_began.load(std::memory_order_acquire)) {
    if (reconfiguringAlreadyHeld) _reconfiguring.store(false, std::memory_order_release);
    return false;
  }
  if (!reconfiguringAlreadyHeld && _reconfiguring.exchange(true, std::memory_order_acq_rel))
    return false;

  bool ok = false;
  if (!_driverMutex || xSemaphoreTake(_driverMutex, pdMS_TO_TICKS(300)) != pdTRUE) {
    AFFA_LOGE(kTag, "recovery mutex busy; direct-TWAI restart deferred");
    _reconfiguring.store(false, std::memory_order_release);
    return false;
  }

  twai_status_info_t before{};
  const esp_err_t stateErr = twai_get_status_info(&before);
  if (stateErr == ESP_OK && before.state == TWAI_STATE_RECOVERING) {
    // ESP-IDF forbids uninstall in RECOVERING.  Keeping RX alive is safe; another poll will
    // observe STOPPED and start it after bus recovery completes.
    AFFA_LOGI(kTag, "restart deferred: controller is recovering");
  } else if (stateErr != ESP_OK && stateErr != ESP_ERR_INVALID_STATE) {
    AFFA_LOGE(kTag, "restart status failed: %s", esp_err_to_name(stateErr));
  } else if (!stopRxTask()) {
    // Never uninstall while twai_receive() might still be blocked.
  } else {
    bool uninstalled = stateErr == ESP_ERR_INVALID_STATE;
    if (!uninstalled && before.state == TWAI_STATE_RUNNING) {
      const esp_err_t stopErr = twai_stop();
      if (stopErr != ESP_OK) {
        AFFA_LOGE(kTag, "twai_stop failed: %s", esp_err_to_name(stopErr));
      } else {
        uninstalled = twai_driver_uninstall() == ESP_OK;
      }
    } else if (!uninstalled) {
      // STOPPED and BUS_OFF are the two other legal uninstall states.
      uninstalled = twai_driver_uninstall() == ESP_OK;
    }

    if (!uninstalled && stateErr != ESP_ERR_INVALID_STATE) {
      AFFA_LOGE(kTag, "twai_driver_uninstall failed; driver was not replaced");
      // Best effort restore if the old driver is still usable.  If it is not, recovery will
      // retry and eventually install fresh once it reaches a legal state.
      twai_status_info_t after{};
      if (twai_get_status_info(&after) == ESP_OK && after.state == TWAI_STATE_STOPPED) {
        (void)twai_start();
      }
      if (twai_get_status_info(&after) == ESP_OK && after.state == TWAI_STATE_RUNNING) {
        (void)startRxTask();
      }
    } else {
      ok = installAndStart(mode);
      if (ok) {
        _mode = mode;
        _listenOnly.store(mode == LinkMode::ListenOnly, std::memory_order_relaxed);
        _restarts.fetch_add(1, std::memory_order_relaxed);
        AFFA_LOGW(kTag, "TWAI driver restarted (%lu)",
                  static_cast<unsigned long>(_restarts.load(std::memory_order_relaxed)));
      }
    }
  }

  xSemaphoreGive(_driverMutex);
  _reconfiguring.store(false, std::memory_order_release);
  return ok;
}

bool Esp32CanLink::setListenOnly(bool on) {
  if (!_began.load(std::memory_order_acquire)) return false;
  const LinkMode wanted = on ? LinkMode::ListenOnly : LinkMode::Normal;
  if (wanted == _mode && !_reconfiguring.load(std::memory_order_acquire)) return true;
  return restartDriver(wanted);
}

Stats Esp32CanLink::stats() const {
  Stats snapshot;
  snapshot.rxFrames = _rxFrames.load(std::memory_order_relaxed);
  snapshot.txFrames = _txFrames.load(std::memory_order_relaxed);
  snapshot.txDropped = _txDropped.load(std::memory_order_relaxed);
  snapshot.ringOverflow = _rx.overflow();
  if (_reconfiguring.load(std::memory_order_acquire)) return snapshot;
  if (!_driverMutex || xSemaphoreTake(_driverMutex, 0) != pdTRUE) return snapshot;

  twai_status_info_t status{};
  if (twai_get_status_info(&status) == ESP_OK) {
    snapshot.txErr = status.tx_error_counter;
    snapshot.rxErr = status.rx_error_counter;
    snapshot.txFailed = status.tx_failed_count;
  }
  xSemaphoreGive(_driverMutex);
  return snapshot;
}

Esp32CanLink::DriverState Esp32CanLink::driverState() const {
  DriverState snapshot;
  if (_reconfiguring.load(std::memory_order_acquire)) return snapshot;
  if (!_driverMutex || xSemaphoreTake(_driverMutex, 0) != pdTRUE) return snapshot;
  twai_status_info_t status{};
  if (twai_get_status_info(&status) == ESP_OK) {
    snapshot.valid = true;
    snapshot.state = static_cast<uint8_t>(status.state);
    snapshot.msgsToRx = status.msgs_to_rx;
    snapshot.msgsToTx = status.msgs_to_tx;
    snapshot.txErr = status.tx_error_counter;
    snapshot.rxErr = status.rx_error_counter;
    snapshot.busErr = status.bus_error_count;
    snapshot.arbLost = status.arb_lost_count;
    snapshot.rxMissed = status.rx_missed_count + status.rx_overrun_count;
  }
  xSemaphoreGive(_driverMutex);
  return snapshot;
}

void Esp32CanLink::setTxEnabled(bool on) {
  _txEnabled.store(on, std::memory_order_release);
}

bool Esp32CanLink::txEnabled() const {
  return _txEnabled.load(std::memory_order_acquire);
}

}  // namespace affa

#endif  // AFFA_ENABLE_ESP32CAN_LINK
