// 04_sniffer — the CAN link with no panel: listen-only, print every frame.
//
// Built with NO AFFA_PANEL_* flag, through <AffaTwai.h>. Listen-only never transmits and never
// ACKs, so it cannot disturb the bus it watches. One line per frame on Serial:
//
//   3CF 8 61 11 01 A3 A3 A3 A3 A3
#include <Arduino.h>
#include <AffaTwai.h>

#ifndef AFFA_CAN_RX
#  define AFFA_CAN_RX 5
#endif
#ifndef AFFA_CAN_TX
#  define AFFA_CAN_TX 4
#endif

namespace {
affa::TwaiLink g_link;

// The link's own lines (driver up, bus-off, recovery) — no display needed for a sink.
struct SerialSink final : affa::ILogSink {
  void write(uint8_t, const char* tag, const char* msg) override {
    Serial.printf("[%s] %s\n", tag, msg);
  }
} g_sink;
}

void setup() {
  Serial.begin(115200);
  delay(300);
  affa::setLogSink(&g_sink);
  affa::TwaiConfig cfg;
  cfg.rx         = static_cast<gpio_num_t>(AFFA_CAN_RX);
  cfg.tx         = static_cast<gpio_num_t>(AFFA_CAN_TX);
  cfg.listenOnly = true;
  if (!g_link.begin(cfg)) Serial.println("[sniff] controller did not come up");
}

void loop() {
  (void)g_link.waitRx(10);                      // sleep until a frame, or 10 ms
  affa::Frame f;
  while (g_link.recv(f)) {
    Serial.printf("%03lX %u", static_cast<unsigned long>(f.id), static_cast<unsigned>(f.len));
    for (uint8_t i = 0; i < f.len; ++i) Serial.printf(" %02X", f.data[i]);
    Serial.println();
  }
  if (!g_link.healthy()) (void)g_link.recover();
}
