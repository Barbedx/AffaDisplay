// 01_link_check — bring the CAN link up, NAME what goes past, and count what we accepted.
//
// No display driver: this drives the shared core only, so it builds against any panel
// selection and is the right first thing to flash on a new board.
//
// A CLEAN BUS TRACE DOES NOT PROVE OUR CODE ACCEPTED ANYTHING. A logic analyser, or a
// serial dump of every frame, proves the transceiver, the pins and the bit timing — and
// nothing above them. So every counter below is incremented by THIS APPLICATION from the
// library's Layer-0 tap, at the point where the frame was recognised: the counters live in
// the consumer on purpose. If the panel is pinging and `peer-ping` is not moving, the
// fault is above the wire.
//
// BEGIN() TRANSMITS. The first heartbeat leaves on the first poll(). Do not run this on a
// vehicle bus until you know what else is listening on 0x3AF.

#include <Arduino.h>
#include <AffaDisplay.h>

namespace {

struct ArduinoClock final : affa::IClock {          // the whole IClock implementation
  uint32_t millis() const override { return ::millis(); }
};

// rx = GPIO4, tx = GPIO3 on this ESP32-C3 SuperMini. MeganeCAN's board is MIRRORED (same
// module, opposite soldering); the named struct is what stops that becoming a silent bus.
constexpr affa::CanPins kPins{ .rx = GPIO_NUM_4, .tx = GPIO_NUM_3 };

// Spelled out here because the panel folder is not part of this build. A real application
// uses affa::CarminatDisplay, which owns these.
constexpr uint8_t kHello[3][8] = {
  {0x70, 0x1A, 0x11, 0x00, 0x00, 0x00, 0x00, 0x01},
  {0xB0, 0x14, 0x11, 0x00, 0x1F, 0x00, 0x00, 0x00},
  {0xB0, 0x14, 0x11, 0x00, 0x1F, 0x00, 0x00, 0x00},   // sent TWICE — observed, not a typo
};
constexpr affa::SyncProfile kProfile{0x3AF, 0x3CF, 0x0400, 0xB9, 0xBA, 0x00, 0x00,
                                     kHello, 3};
constexpr uint16_t kFuncIds[] = { 0x151, 0x1F1 };   // ORDER IS ON THE WIRE

class LinkCheck final : public affa::AffaDisplayBase {
 public:
  LinkCheck(affa::ICanLink& l, affa::IClock& c)
      : AffaDisplayBase(l, c, kProfile, kFuncIds, 2) {}
  bool supports(affa::Feature f) const override { return f == affa::Feature::KeyTx; }
 protected:
  uint8_t  packetFiller() const override { return 0x00; }
  uint16_t keyTxId()      const override { return 0x1C1; }
};

affa::Esp32CanLink g_link;      // `link` alone is ambiguous with ::link() from <unistd.h>
ArduinoClock       g_clock;
LinkCheck          g_display(g_link, g_clock);

// The six frames worth naming. Nothing here matches a FILLER byte: it is per-node (our
// bench panel pads 0xA3, an OEM cluster 0x84, the OEM radio 0xFF) and means nothing.
enum Known : uint8_t { kSyncReq, kPing, kKey, kReg, kAckDone, kAckPart, kKindCount };
const char* const kName[kKindCount] = {
  "3CF 61 11  sync request", "3CF 69     peer ping", "1C1 03 89  key",
  "1C1 70     registration", "|400 74    ACK DONE",  "|400 30 01 00 ACK PARTIAL",
};
uint32_t g_count[kKindCount] = {0};
uint32_t g_lastMs[kKindCount] = {0};
uint32_t g_txAttempts = 0;
uint32_t g_nextReport = 0;

int classify(const affa::Frame& f) {
  const uint8_t* d = f.data;
  if (f.id == 0x3CF && f.len >= 2 && d[0] == 0x61 && d[1] == 0x11) return kSyncReq;
  if (f.id == 0x3CF && f.len >= 1 && d[0] == 0x69)                 return kPing;
  if (f.id == 0x1C1 && f.len >= 2 && d[0] == 0x03 && d[1] == 0x89) return kKey;
  if (f.id == 0x1C1 && f.len >= 1 && d[0] == 0x70)                 return kReg;
  if ((f.id & 0x400) && f.len >= 1 && d[0] == 0x74)                return kAckDone;
  if ((f.id & 0x400) && f.len >= 3 && d[0] == 0x30 && d[1] == 0x01 && d[2] == 0x00)
    return kAckPart;
  return -1;
}

void onFrameTap(const affa::Frame& f, affa::Direction dir, void*) {
  const bool rx = (dir == affa::Direction::Rx);
  if (!rx) ++g_txAttempts;
  Serial.printf("%s %03X ", rx ? "RX" : "TX", static_cast<unsigned>(f.id));
  for (uint8_t i = 0; i < f.len; ++i) Serial.printf("%02X ", f.data[i]);
  const int k = classify(f);
  if (k >= 0) {
    Serial.printf(" <%s>", kName[k]);
    if (rx) { ++g_count[k]; g_lastMs[k] = ::millis(); }   // only INBOUND is evidence
  }
  Serial.println();
}

void report() {
  const affa::Stats s  = g_display.stats();
  const uint32_t   now = ::millis();
  // peerAcked is the whole point of the summary: the panel answered something WE sent.
  const bool acked = (g_count[kAckDone] + g_count[kAckPart]) != 0;
  Serial.printf("---- sync=0x%02X synced=%d registered=%d peerAcked=%d | ourTx=%lu "
                "txFrames=%lu txDropped=%lu txErr=%lu txFailed=%lu rxErr=%lu ovf=%lu\n",
                static_cast<unsigned>(g_display.syncState()), g_display.synced(),
                g_display.registered(), acked, g_txAttempts, s.txFrames, s.txDropped,
                s.txErr, s.txFailed, s.rxErr, s.ringOverflow);
  for (uint8_t i = 0; i < kKindCount; ++i) {
    if (!g_count[i]) { Serial.printf("     %-26s     -\n", kName[i]); continue; }
    Serial.printf("     %-26s x%-6lu age %lu ms\n", kName[i], g_count[i],
                  now - g_lastMs[i]);
  }
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(200);                        // the application may sleep; the library may not
  if (!g_link.begin(kPins, 500000)) {
    Serial.println("CAN did not come up — check pins, transceiver and termination");
    return;
  }
  g_display.onFrame(&onFrameTap, nullptr);
  g_display.begin();
}

void loop() {
  g_display.poll();                  // that is the whole integration
  const uint32_t now = ::millis();   // a deadline, never a counter and never a delay()
  if (affa::expired(now, g_nextReport)) { g_nextReport = now + 2000; report(); }
}
