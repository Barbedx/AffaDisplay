#pragma once
#include "../AffaConfig.h"

// THE ESP32 LINK: the library's CAN seam straight on the ESP-IDF TWAI driver, which ships in
// the Arduino core, so there is no third-party CAN library underneath it.
//
// It replaced CanCommonLink (collin80 esp32_can), and the bench is why. esp32_can installs
// the driver with rx_queue_len = 6; an unanswered panel retransmits one id at 4 000-10 700
// frames/s, and on MegaOpen's bench that was 0 frames received / 43 806 bus errors in 30 s.
// The same panel on a raw TWAI driver with a 64-deep queue: 115 623 frames, 0 bus errors.
//
// What it does besides moving frames, each one learned on a bench:
//   * a deep RX queue (TwaiConfig::rxQueue, 64 by default) — poll() takes at most
//     AFFA_RX_BUDGET of it per call, so a storm costs latency, never the sync pump;
//   * truthful transmit dispositions: a full TX queue is Busy (retried for free), a
//     controller that cannot send — bus-off, stopped, listen-only, gate shut — is Rejected;
//   * TxGate: with nobody ACKing, the controller retransmits one frame for ever (~1400 bus
//     errors a second). service() shuts transmit within 2 s of that and re-probes with a
//     backoff, so an unplugged panel is quiet and a re-plugged one is heard at once;
//   * a raw tap, so an application can capture or log the panel bus without a second
//     reader on the driver queue.
//
// Compiled only where <driver/twai.h> exists; on the host this header is empty and
// LoopbackLink is the link.

#if __has_include(<driver/twai.h>)

#include <atomic>
#include <driver/gpio.h>
#include <driver/twai.h>

#include "../core/AffaTypes.h"
#include "../core/ICanLink.h"
#include "TxGate.h"

namespace affa {

struct TwaiConfig {
  gpio_num_t tx       = GPIO_NUM_NC;
  gpio_num_t rx       = GPIO_NUM_NC;
  uint32_t   bitrate  = 500000;
  // Never transmits and never ACKs, so it cannot disturb the bus or be driven bus-off.
  bool       listenOnly = false;
  // The driver's own queues. 64 RX: the IDF default of 5 overruns in microseconds at
  // ~1000 frames/s, silently. 8 TX: a 96-byte screen is 13 frames, and Busy covers the rest.
  uint16_t   rxQueue  = 64;
  uint8_t    txQueue  = 8;
  // A floating CRX reads dominant and the controller then sees a bus that never idles. With
  // the pull-up an open joint reads recessive: the board is deaf, but the bus is not clamped.
  bool       rxPullup = true;
  // TxGate on or off. Off only for a bench that wants to watch the retransmit storm.
  bool       txGate   = true;
};

class TwaiLink final : public ICanLink {
 public:
  // Every frame received (Direction::Rx) and every frame the driver accepted
  // (Direction::Tx). Runs on the task that calls recv()/trySend(), so it must be quick.
  using RawTap = void (*)(const Frame& f, Direction d, void* ctx);

  TwaiLink() = default;
  TwaiLink(const TwaiLink&)            = delete;
  TwaiLink& operator=(const TwaiLink&) = delete;

  // Installs and starts the driver. False if either step failed; recover() keeps trying
  // the same configuration afterwards.
  bool begin(const TwaiConfig& cfg);
  void end();
  bool restart();                       // uninstall + install, same configuration

  // Listen-only and normal are driver modes, so switching is a reinstall.
  bool setListenOnly(bool on);
  bool listenOnly() const { return _cfg.listenOnly; }

  // The application's own gate: shut it for an OTA write, or to ask "is it us or the bus".
  // healthy() stays true, so the library holds renders instead of tearing the driver down.
  void setTxEnabled(bool on) { _txEnabled = on; }
  bool txEnabled() const     { return _txEnabled; }

  void onRaw(RawTap tap, void* ctx) { _tapCtx = ctx; _tap = tap; }

  // The owner task's sleep: block in twai_receive() for up to `ms` and keep the frame that
  // ends it for the next recv(). A task that sleeps here wakes on a panel key the moment it
  // arrives instead of a fixed period later. Without a driver it is a plain delay (Timeout).
  RxWait waitRx(uint32_t ms) override;

  // Drops the queued frames AND the one the controller is retransmitting (twai_stop() puts
  // it in reset; clearing the queue alone leaves that one on the wire for ever). Owner task.
  bool abortTx();

  // ---- ICanLink ----
  TxDisposition trySend(const Frame& f) override;
  bool          recv(Frame& out) override;
  bool          isLive() const override;
  bool          healthy() const override;
  bool          recover(bool force = false) override;
  void          service(uint32_t nowMs) override;
  Stats         stats() const override;

  // The controller's own view, for a status page.
  struct Driver {
    bool     installed = false;
    uint8_t  state     = 0;   // twai_state_t: 0 stopped, 1 running, 2 bus-off, 3 recovering
    uint32_t txErr = 0, rxErr = 0, busErr = 0, arbLost = 0, txFailed = 0;
    uint32_t rxMissed = 0, rxOverrun = 0, queuedRx = 0, queuedTx = 0;
    uint32_t rxFrames = 0, txFrames = 0, txBusy = 0, txRejected = 0, installs = 0;
    bool     gateShut = false, gateProbing = false;
    uint32_t gateShuts = 0, gateBackoffMs = 0;
  };
  Driver driver() const;
  const TxGate& gate() const { return _gate; }

 private:
  bool install();
  bool running() const;
  void deliver(const twai_message_t& m, Frame& out);

  TwaiConfig        _cfg{};
  bool              _configured = false;
  // Atomic: one task installs and removes the driver, but a status page reads it from
  // another.
  std::atomic<bool> _installed{false};
  volatile bool     _txEnabled = true;
  volatile bool     _gateShut  = false;

  Frame    _stash{};
  bool     _stashed = false;

  RawTap   _tap    = nullptr;
  void*    _tapCtx = nullptr;

  TxGate   _gate{};
  uint32_t _nextGateMs = 0;

  uint32_t _rxFrames = 0, _txFrames = 0, _txBusy = 0, _txRejected = 0, _installs = 0;
};

}  // namespace affa

#endif  // __has_include(<driver/twai.h>)
