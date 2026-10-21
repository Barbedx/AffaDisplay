// SPDX-License-Identifier: MIT
#include "TwaiLink.h"

#if __has_include(<driver/twai.h>)

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "../util/AffaLog.h"

namespace affa {
namespace {

constexpr const char* kTag = "AFFA-TWAI";

// How often service() samples the controller for TxGate. The gate's constants are written
// against this cadence (NO_RX_MS 2000 = 20 samples).
constexpr uint32_t kGateSampleMs = 100;

inline TickType_t ticks(uint32_t ms) {
  const TickType_t t = pdMS_TO_TICKS(ms);
  return t ? t : 1;
}

}  // namespace

bool TwaiLink::install() {
  twai_timing_config_t t;
  switch (_cfg.bitrate) {
    case 1000000: t = TWAI_TIMING_CONFIG_1MBITS();   break;
    case 800000:  t = TWAI_TIMING_CONFIG_800KBITS(); break;
    case 250000:  t = TWAI_TIMING_CONFIG_250KBITS(); break;
    case 125000:  t = TWAI_TIMING_CONFIG_125KBITS(); break;
    case 100000:  t = TWAI_TIMING_CONFIG_100KBITS(); break;
    case 50000:   t = TWAI_TIMING_CONFIG_50KBITS();  break;
    default:      t = TWAI_TIMING_CONFIG_500KBITS(); break;
  }
  // The macro takes (tx, rx, mode) — the opposite order from most pin APIs.
  twai_general_config_t g = TWAI_GENERAL_CONFIG_DEFAULT(
      _cfg.tx, _cfg.rx, _cfg.listenOnly ? TWAI_MODE_LISTEN_ONLY : TWAI_MODE_NORMAL);
  g.rx_queue_len = _cfg.rxQueue;
  g.tx_queue_len = _cfg.txQueue;
  const twai_filter_config_t f = TWAI_FILTER_CONFIG_ACCEPT_ALL();

  const esp_err_t inst = twai_driver_install(&g, &t, &f);
  if (inst != ESP_OK) {
    AFFA_LOGE(kTag, "driver install failed (%d)", static_cast<int>(inst));
    return false;
  }
  // After install: the driver routes the pad through the GPIO matrix with no pull.
  if (_cfg.rxPullup) gpio_set_pull_mode(_cfg.rx, GPIO_PULLUP_ONLY);
  if (twai_start() != ESP_OK) {
    AFFA_LOGE(kTag, "driver start failed");
    twai_driver_uninstall();
    return false;
  }
  _installed = true;
  ++_installs;
  AFFA_LOGI(kTag, "up: %lu bit/s, %s, rxq %u, tx=%d rx=%d",
            static_cast<unsigned long>(_cfg.bitrate),
            _cfg.listenOnly ? "listen-only" : "normal", static_cast<unsigned>(_cfg.rxQueue),
            static_cast<int>(_cfg.tx), static_cast<int>(_cfg.rx));
  return true;
}

bool TwaiLink::begin(const TwaiConfig& cfg) {
  end();
  _cfg        = cfg;
  _configured = true;
  _gate       = TxGate{};
  _gateShut   = false;
  return install();
}

void TwaiLink::end() {
  _stashed = false;
  if (!_installed) return;
  twai_stop();
  twai_driver_uninstall();
  _installed = false;
}

bool TwaiLink::restart() {
  if (!_configured) return false;
  end();
  return install();
}

bool TwaiLink::setListenOnly(bool on) {
  if (_cfg.listenOnly == on && _installed) return true;
  _cfg.listenOnly = on;
  return restart();
}

bool TwaiLink::running() const {
  if (!_installed) return false;
  twai_status_info_t s{};
  return twai_get_status_info(&s) == ESP_OK && s.state == TWAI_STATE_RUNNING;
}

void TwaiLink::deliver(const twai_message_t& m, Frame& out) {
  out     = Frame{};
  out.id  = m.identifier;
  out.len = m.data_length_code > 8 ? 8 : m.data_length_code;
  out.ext = m.extd != 0;
  for (uint8_t i = 0; i < out.len; ++i) out.data[i] = m.data[i];
  ++_rxFrames;
}

bool TwaiLink::recv(Frame& out) {
  if (_stashed) {
    out      = _stash;
    _stashed = false;
  } else {
    if (!_installed) return false;
    twai_message_t m{};
    if (twai_receive(&m, 0) != ESP_OK) return false;
    deliver(m, out);
  }
  if (_tap) _tap(out, Direction::Rx, _tapCtx);
  return true;
}

RxWait TwaiLink::waitRx(uint32_t ms) {
  if (_stashed) return RxWait::Ready;
  if (!_installed) { vTaskDelay(ticks(ms)); return RxWait::Timeout; }
  twai_message_t m{};
  if (twai_receive(&m, ticks(ms)) != ESP_OK) return RxWait::Timeout;
  deliver(m, _stash);
  _stashed = true;
  return RxWait::Ready;
}

TxDisposition TwaiLink::trySend(const Frame& f) {
  if (!isLive()) { ++_txRejected; return TxDisposition::Rejected; }

  twai_message_t m{};
  m.identifier       = f.id;
  m.extd             = f.ext ? 1 : 0;
  m.data_length_code = f.len > 8 ? 8 : f.len;
  for (uint8_t i = 0; i < m.data_length_code; ++i) m.data[i] = f.data[i];

  // Zero timeout: this runs on the task that owns the protocol deadlines.
  const esp_err_t e = twai_transmit(&m, 0);
  if (e == ESP_OK) {
    ++_txFrames;
    if (_tap) _tap(f, Direction::Tx, _tapCtx);
    return TxDisposition::Accepted;
  }
  // ESP_ERR_TIMEOUT is "queue full right now". Anything else from a controller that still
  // reads RUNNING is treated the same: the next offer sees whatever changed. A controller
  // that stopped since isLive() is Rejected, so the held job can expire instead of being
  // re-armed for ever.
  if (e == ESP_ERR_TIMEOUT || running()) { ++_txBusy; return TxDisposition::Busy; }
  ++_txRejected;
  return TxDisposition::Rejected;
}

bool TwaiLink::isLive() const {
  return _txEnabled && !_gateShut && !_cfg.listenOnly && running();
}

bool TwaiLink::healthy() const { return running(); }

bool TwaiLink::recover(bool force) {
  if (!_configured) return false;
  if (!_installed || force) return restart();
  twai_status_info_t s{};
  if (twai_get_status_info(&s) != ESP_OK) return restart();
  switch (s.state) {
    case TWAI_STATE_RUNNING:    return true;
    // Recovery takes 128 x 11 recessive bits and ends in STOPPED; the next call starts it.
    case TWAI_STATE_BUS_OFF:    twai_initiate_recovery(); return false;
    case TWAI_STATE_RECOVERING: return false;
    case TWAI_STATE_STOPPED:    return twai_start() == ESP_OK || restart();
    default:                    return restart();
  }
}

bool TwaiLink::abortTx() {
  if (!_installed) return false;
  if (twai_stop() != ESP_OK) {
    // Not running (bus-off, recovering): nothing is on the wire, and recover() owns the
    // way back.
    twai_clear_transmit_queue();
    return false;
  }
  if (twai_start() == ESP_OK) return true;
  return restart();
}

void TwaiLink::service(uint32_t nowMs) {
  if (!_cfg.txGate || !_installed) return;
  if (static_cast<int32_t>(nowMs - _nextGateMs) < 0) return;
  _nextGateMs = nowMs + kGateSampleMs;

  twai_status_info_t s{};
  if (twai_get_status_info(&s) != ESP_OK) return;
  TxGate::Sample smp;
  smp.nowMs    = nowMs;
  smp.tec      = s.tx_error_counter;
  smp.txFailed = s.tx_failed_count;
  smp.rxFrames = _rxFrames;
  smp.mayTx    = _txEnabled && !_cfg.listenOnly && s.state == TWAI_STATE_RUNNING;

  switch (_gate.tick(smp)) {
    case TxGate::Act::Shut: {
      _gateShut = true;
      const bool aborted = abortTx();
      AFFA_LOGW(kTag, "nobody ACKs: transmit shut (tec %lu, aborted %d, next probe %lu ms)",
                static_cast<unsigned long>(s.tx_error_counter), aborted ? 1 : 0,
                static_cast<unsigned long>(_gate.backoffMs()));
      break;
    }
    case TxGate::Act::Open:
      _gateShut = false;
      AFFA_LOGI(kTag, "transmit open (%s)", _gate.probing() ? "probing" : "the bus is talking");
      break;
    case TxGate::Act::None:
      break;
  }
}

Stats TwaiLink::stats() const {
  Stats st;
  st.rxFrames  = _rxFrames;
  st.txFrames  = _txFrames;
  st.txDropped = _txRejected;
  twai_status_info_t s{};
  if (_installed && twai_get_status_info(&s) == ESP_OK) {
    st.ringOverflow = s.rx_missed_count + s.rx_overrun_count;
    st.txErr        = s.tx_error_counter;
    st.rxErr        = s.rx_error_counter;
    st.txFailed     = s.tx_failed_count;
  }
  return st;
}

TwaiLink::Driver TwaiLink::driver() const {
  Driver d;
  d.installed     = _installed;
  d.rxFrames      = _rxFrames;
  d.txFrames      = _txFrames;
  d.txBusy        = _txBusy;
  d.txRejected    = _txRejected;
  d.installs      = _installs;
  d.gateShut      = _gateShut;
  d.gateProbing   = _gate.probing();
  d.gateShuts     = _gate.shuts();
  d.gateBackoffMs = _gate.backoffMs();
  twai_status_info_t s{};
  if (!_installed || twai_get_status_info(&s) != ESP_OK) return d;
  d.state     = static_cast<uint8_t>(s.state);
  d.txErr     = s.tx_error_counter;
  d.rxErr     = s.rx_error_counter;
  d.busErr    = s.bus_error_count;
  d.arbLost   = s.arb_lost_count;
  d.txFailed  = s.tx_failed_count;
  d.rxMissed  = s.rx_missed_count;
  d.rxOverrun = s.rx_overrun_count;
  d.queuedRx  = s.msgs_to_rx;
  d.queuedTx  = s.msgs_to_tx;
  return d;
}

}  // namespace affa

#endif  // __has_include(<driver/twai.h>)
