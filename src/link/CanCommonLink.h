// CanCommonLink — AffaDisplay's CAN seam on collin80's esp32_can / can_common.
//
// WHY THIS EXISTS ALONGSIDE Esp32CanLink. Esp32CanLink drives ESP-IDF's TWAI driver
// directly. This one sits on the esp32_can library instead, which is what a large amount of
// existing Renault/ESP32 code already uses — including the reference implementation this
// protocol was reverse-engineered from. Having both means an application can adopt the
// library without abandoning the CAN stack it already has.
//
// It is also the stack that was proven end to end on real glass on 2026-08-04: handshake,
// registration, clock and a three-row animated screen, sustained with every error counter at
// zero. See examples/07_cantime and examples/08_rows3.
//
// PULL, NOT PUSH. esp32_can hands frames over on its own task through a callback; ICanLink
// is deliberately a pull port (see ICanLink's comment on the ACK deadlock). So the callback
// does nothing but push into a lock-free ring, and recv() drains it from poll(). Nothing
// protocol-shaped ever runs on the driver's task.
//
// ONE INSTANCE. esp32_can exposes a single global CAN0, so a second CanCommonLink would
// fight the first for the callback. begin() refuses politely rather than corrupting both.
#pragma once
#include "../AffaConfig.h"

#if AFFA_ENABLE_CANCOMMON_LINK

#include <esp32_can.h>
#include <driver/twai.h>

#include "../core/AffaTypes.h"
#include "../core/ICanLink.h"

namespace affa {

class CanCommonLink final : public ICanLink {
 public:
  CanCommonLink() = default;
  CanCommonLink(const CanCommonLink&) = delete;
  CanCommonLink& operator=(const CanCommonLink&) = delete;

  // rx/tx are the MCU pins wired to the transceiver's R and D pins respectively. The names
  // are the transceiver's: CRX must reach the MCU's CAN *RX*. Swapping them yields a link
  // that never errors and never receives — which reads as a dead peer, not as miswiring.
  bool begin(gpio_num_t rx, gpio_num_t tx, uint32_t bitrate = 500000) {
    if (s_self) return false;                  // CAN0 is global; one owner only
    s_self = this;
    _rx.head = _rx.tail = 0;

    // ORDER IS LOAD-BEARING: pins, begin(), callback, watchFor() LAST. watchFor() with no
    // argument accepts every id; setting it before the callback loses early frames.
    CAN0.setCANPins(rx, tx);
    CAN0.begin(bitrate);
    CAN0.setGeneralCallback(&CanCommonLink::onFrame);
    CAN0.watchFor();
    _begun = true;
    return true;
  }

  // ---- ICanLink -----------------------------------------------------------
  bool send(const Frame& f) override {
    return trySend(f) == TxDisposition::Accepted;
  }

  TxDisposition trySend(const Frame& f) override {
    if (!_begun || !_txGate) { ++_txDropped; return TxDisposition::Rejected; }
    CAN_FRAME o;
    o.id       = f.id;
    o.extended = f.ext ? 1 : 0;
    o.rtr      = 0;
    o.length   = f.len;
    for (uint8_t i = 0; i < 8; ++i) o.data.uint8[i] = f.data[i];
    // esp32_can queues in software and reports only accepted/refused, so a locally full
    // controller is indistinguishable from a hard refusal here. Rejected is the safe
    // reading: AffaDisplayBase then leaves the payload's bytes uncommitted either way.
    if (CAN0.sendFrame(o)) { ++_txFrames; return TxDisposition::Accepted; }
    ++_txDropped;
    return TxDisposition::Rejected;
  }

  bool recv(Frame& out) override {
    if (_rx.tail == _rx.head) return false;
    const Rec& r = _rx.slot[_rx.tail];
    out          = Frame{};
    out.id       = r.id;
    out.len      = r.len;
    out.ext      = r.ext;
    out.fromSelf = false;                      // a real controller never hears itself
    for (uint8_t i = 0; i < 8; ++i) out.data[i] = r.d[i];
    _rx.tail = static_cast<uint8_t>((_rx.tail + 1) % kRing);
    ++_rxFrames;
    return true;
  }

  bool isLive() const override { return _begun && _txGate && controllerRunning(); }
  bool healthy() const override { return _begun && controllerRunning(); }

  // RECOVERY. After bus-off the peripheral can sit in TWAI_STATE_STOPPED for ever while
  // esp32_can keeps accepting sends into its SOFTWARE queue — so the application's transmit
  // counter climbs and everything looks alive with the controller off the wire. Measured on
  // the bench with the display unplugged: state STOPPED, tx counter past 27, nothing ever
  // recovering. AffaDisplayBase's link-recovery FSM calls this.
  bool recover(bool force = false) override {
    (void)force;
    twai_status_info_t st{};
    if (twai_get_status_info(&st) != ESP_OK) return false;
    if (st.state == TWAI_STATE_BUS_OFF) { twai_initiate_recovery(); return false; }
    if (st.state == TWAI_STATE_STOPPED) return twai_start() == ESP_OK;
    return st.state == TWAI_STATE_RUNNING;
  }

  Stats stats() const override {
    Stats s;
    twai_status_info_t st{};
    if (twai_get_status_info(&st) == ESP_OK) {
      s.rxErr    = st.rx_error_counter;
      s.txErr    = st.tx_error_counter;
      // Stats has no bus-error or arbitration field; those live on driver() below, which is
      // where a console should read them. Arbitration loss is the closest fit for txFailed.
      s.txFailed = st.arb_lost_count;
    }
    s.rxFrames     = _rxFrames;
    s.txFrames     = _txFrames;
    s.txDropped    = _txDropped;
    s.ringOverflow = _overflow;
    return s;
  }

  // ---- extras, for a console ----------------------------------------------
  // The software transmit gate. Shut it for an OTA write, or to run the "is it us or the
  // bus" test: with our transmitter silent, a bus-error counter that keeps climbing cannot
  // be our fault.
  void setTxGate(bool on) { _txGate = on; }
  bool txGate() const { return _txGate; }
  uint32_t overflow() const { return _overflow; }

  // The counters that separate a silent bus from one we cannot decode from a controller that
  // never started. `queuedTx` growing with every error at zero means the controller is
  // waiting for a bus-idle that never comes — CANRX stuck dominant. Without this field those
  // three states are indistinguishable, and that cost a full bench session.
  struct Driver {
    uint8_t  state = 0;                        // 0 stopped, 1 running, 2 bus-off, 3 recovering
    uint32_t txErr = 0, rxErr = 0, busErr = 0, arbLost = 0;
    uint32_t rxMissed = 0, queuedRx = 0, queuedTx = 0;
    bool     valid = false;
  };
  Driver driver() const {
    Driver d;
    twai_status_info_t st{};
    if (twai_get_status_info(&st) != ESP_OK) return d;
    d.valid    = true;
    d.state    = static_cast<uint8_t>(st.state);
    d.txErr    = st.tx_error_counter;
    d.rxErr    = st.rx_error_counter;
    d.busErr   = st.bus_error_count;
    d.arbLost  = st.arb_lost_count;
    d.rxMissed = st.rx_missed_count;
    d.queuedRx = st.msgs_to_rx;
    d.queuedTx = st.msgs_to_tx;
    return d;
  }

 private:
  bool controllerRunning() const {
    twai_status_info_t st{};
    return twai_get_status_info(&st) == ESP_OK && st.state == TWAI_STATE_RUNNING;
  }

  // A single-producer/single-consumer ring: esp32_can's task writes, poll() reads. Index
  // updates are the only shared state and each is written by exactly one side, so this needs
  // no lock — which matters, because taking one on the driver's task is how a receive path
  // starts dropping frames under load.
  static constexpr uint8_t kRing = 64;
  struct Rec { uint32_t id; uint8_t len; bool ext; uint8_t d[8]; };
  struct Ring {
    Rec              slot[kRing];
    volatile uint8_t head = 0, tail = 0;
  };

  static void onFrame(CAN_FRAME* f) {
    if (!f || !s_self) return;
    Ring& r = s_self->_rx;
    const uint8_t next = static_cast<uint8_t>((r.head + 1) % kRing);
    if (next == r.tail) { ++s_self->_overflow; return; }   // full: drop the NEWEST, keep order
    Rec& slot = r.slot[r.head];
    slot.id  = f->id;
    slot.len = f->length;
    slot.ext = f->extended != 0;
    for (uint8_t i = 0; i < 8; ++i) slot.d[i] = f->data.uint8[i];
    r.head = next;
  }

  Ring     _rx;
  bool     _begun    = false;
  bool     _txGate   = true;
  uint32_t _rxFrames = 0;
  uint32_t _txFrames = 0;
  uint32_t _txDropped = 0;
  uint32_t _overflow = 0;

  static CanCommonLink* s_self;
};

// Header-only libraries and a static member: define it inline so no .cpp is required.
inline CanCommonLink* CanCommonLink::s_self = nullptr;

}  // namespace affa

#endif  // AFFA_ENABLE_CANCOMMON_LINK
