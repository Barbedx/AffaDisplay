// 15_iphone — the panel's clock, set from an iPhone over BLE, with WiFi up at the same time.
//
// ############################################################################
// # STATUS 2026-08-05: UNPROVEN. IT HAS NEVER PAIRED WITH A PHONE.           #
// #                                                                          #
// # What IS proven, on hardware: BLE, WiFi, an HTTP server and a live 500     #
// # kbit/s CAN session run together on one ESP32 with the panel at            #
// # Phase::Ready. That was the question this example was written to answer    #
// # and the answer is yes.                                                    #
// #                                                                          #
// # What is NOT proven: everything past "the phone connects". Three real bugs #
// # were found and fixed by the board (see the comments at each site), but    #
// # after the third fix the session ended before a pairing attempt was made.  #
// # Do not trust the CTS read path below until a phone has been through it.   #
// #                                                                          #
// # NEXT STEP, and do this before changing any more code: scan with nRF       #
// # Connect (or LightBlue) and look at the raw advertising packet. That tells #
// # you whether iOS is FILTERING us or whether the packet is WRONG, and those #
// # have completely different fixes. Guessing between them at 2 a.m. is what  #
// # produced three flashes and no pairing.                                    #
// ############################################################################
//
// THE QUESTION THIS ANSWERS IS COEXISTENCE. An ESP32 running the BLE stack, a WiFi station,
// a web server and a 500 kbit/s CAN session at once is the shape every real integration has,
// and it is the shape most likely to fall over: BLE and WiFi share one radio, and the CAN
// session has a five-second peer watchdog that a stalled loop will trip. If this holds, the
// integration shape holds.
//
// AND IT CLOSES THE CLOCK. `docs/BENCH-VERIFIED.md` records 162 probes that failed to set an
// UpdateList panel's clock, and the working conclusion that the panel owns it on that family.
// Carminat is the family where the radio DOES set it — `151 05 56 "HHMM"`, proven on this
// exact panel — and the thing a car radio has never had is a trustworthy source for the
// value. An iPhone in your pocket is one, and it is already advertising it: the Current Time
// Service, 0x1805, characteristic 0x2A2B, ten bytes.
//
// THE ROLES ARE THE OTHER WAY ROUND FROM THE OBVIOUS. We are the GAP *peripheral* — we
// advertise, the phone connects to us — but for CTS we are the GATT *client* and the phone is
// the server. So the sequence is: advertise, get connected, bond, and only then reach back
// down the same connection to read the phone's clock. Apple will not expose CTS to an
// unbonded peer, which is why bonding is not optional here.
//
//   pio run -e ex15_iphone -t upload --upload-port COM5
//   pio device monitor -e ex15_iphone
//
// Then on the phone: Settings > Bluetooth > "AffaClock" > Pair.
//
// WHAT IS DELIBERATELY NOT HERE: the Apple Media Service, which is what puts a track title on
// the glass. AMS is entity-update subscriptions, an attribute list written to the phone, and
// truncated-value handling — several hundred lines, and it needs a phone in hand to iterate
// against. The seam for it is `pushToPanel()` below: give it a title instead of a clock and
// the display half needs no changes at all. MegaOpen's `src/apple_media_service.cpp` is the
// worked implementation to port from.

#include <Arduino.h>
#include <AffaDisplay.h>
#include <NimBLEDevice.h>
#include <NimBLEHIDDevice.h>
#include <Preferences.h>
#include <PsychicHttp.h>
#include <WiFi.h>

#if !AFFA_PANEL_CARMINAT
#  error "15_iphone is a Carminat example: build with -D AFFA_PANEL_CARMINAT=1"
#endif

namespace {

constexpr gpio_num_t kRxPin   = GPIO_NUM_5;
constexpr gpio_num_t kTxPin   = GPIO_NUM_4;
constexpr uint32_t   kBitrate = 500000;

constexpr const char* kBleName = "AffaClock";

// Current Time Service. Both 16-bit SIG UUIDs; the phone is the server.
const NimBLEUUID kCtsService((uint16_t)0x1805);
const NimBLEUUID kCtsCurrent((uint16_t)0x2A2B);

struct ArduinoClock final : affa::IClock {
  uint32_t millis() const override { return ::millis(); }
};

affa::CanCommonLink   g_link;
ArduinoClock          g_clock;
affa::CarminatDisplay g_display(g_link, g_clock);

// ---------------------------------------------------------------------------
// State shared between the BLE callbacks and the loop
// ---------------------------------------------------------------------------
// THE BLE STACK CALLS BACK ON ITS OWN TASK. Nothing here touches the display from a callback
// — the library is per-instance and unlocked by design, and a render from the NimBLE task
// racing the loop task's poll() corrupts the transmit FSM silently. The callbacks set flags;
// the loop acts on them. That is the same rule MegaOpen's AffaMailbox exists to enforce, in
// the smallest form that works for one producer.
volatile bool     g_connected   = false;
volatile bool     g_bonded      = false;
volatile bool     g_wantCtsRead = false;   // set by the auth callback, consumed by loop()
volatile uint32_t g_peerHandle  = 0;
NimBLEAddress     g_peerAddr;

char     g_hhmm[8]   = {0};      // last time read from the phone, "HHMM"
uint32_t g_timeAtMs  = 0;        // millis() when we read it — the clock is free-running
                                 // on the panel afterwards, so this is only for the console
uint32_t g_syncs     = 0, g_syncFails = 0;
char     g_lastErr[64] = "none";

bool     g_clockEnabled = true;  // web toggle
bool     g_bleEnabled   = true;
uint32_t g_resyncMs     = 3600000;   // re-read the phone's clock once an hour
uint32_t g_nextResyncAt = 0;

bool     g_busy = false;
char     g_l1[24] = {0}, g_l2[24] = {0}, g_l3[24] = {0};

void onDone(affa::TxTicket, affa::Result r, void*) {
  g_busy = false;
  if (r != affa::Result::Ok)
    Serial.printf("[%8lu] !! render failed (%u)\n", static_cast<unsigned long>(millis()),
                  static_cast<unsigned>(r));
}

// ---------------------------------------------------------------------------
// BLE — we advertise, the phone connects, then we read ITS clock
// ---------------------------------------------------------------------------
NimBLEServer* g_bleServer = nullptr;

class ServerCb final : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* s, NimBLEConnInfo& info) override {
    g_connected  = true;
    g_peerHandle = info.getConnHandle();
    g_peerAddr   = info.getAddress();
    Serial.printf("[%8lu] BLE connected: %s\n", static_cast<unsigned long>(millis()),
                  g_peerAddr.toString().c_str());

    // WE HAVE TO ASK FOR SECURITY. iOS attaches happily and then does nothing: it will not
    // bond on its own, and without a bond it does not expose the Current Time Service. The
    // first version of this file waited for `onAuthenticationComplete` that was never going
    // to fire — the log showed "BLE connected" and then silence for ever.
    //
    // ASYNC IS NOT OPTIONAL. `secureConnection(false)` waits on the host task with
    // BLE_NPL_TIME_FOREVER, from inside a host callback — that is a deadlock, not a delay.
    NimBLEClient* c = s->getClient(info.getConnHandle());
    if (c) {
      c->secureConnection(/*async=*/true);
      Serial.printf("[%8lu] BLE pairing requested\n", static_cast<unsigned long>(millis()));
    } else {
      Serial.printf("[%8lu] !! no client for handle %u\n",
                    static_cast<unsigned long>(millis()),
                    static_cast<unsigned>(info.getConnHandle()));
    }
    // Advertising deliberately stays stopped while connected: a second peer would want a
    // second client, and one client is what keeps this example readable.
  }

  void onDisconnect(NimBLEServer* s, NimBLEConnInfo& info, int reason) override {
    (void)info;
    g_connected = false;
    g_bonded    = false;
    Serial.printf("[%8lu] BLE disconnected (reason %d) — advertising again\n",
                  static_cast<unsigned long>(millis()), reason);
    if (g_bleEnabled) s->startAdvertising();
  }

  void onAuthenticationComplete(NimBLEConnInfo& info) override {
    g_bonded = info.isEncrypted();
    Serial.printf("[%8lu] BLE auth complete: encrypted=%d bonded=%d\n",
                  static_cast<unsigned long>(millis()), info.isEncrypted(), info.isBonded());
    // THE READ WAITS FOR ENCRYPTION. Apple does not expose CTS to an unbonded peer, so a
    // read fired on connect returns an insufficient-authentication error and looks like a
    // missing service. This is the correct moment.
    if (g_bonded) g_wantCtsRead = true;
  }
};
ServerCb g_serverCb;

// Reads the phone's Current Time characteristic. RUNS ON THE LOOP TASK, never a callback:
// it blocks on GATT round-trips, and blocking the NimBLE host task is how a stack deadlocks.
bool readPhoneClock(char out[8]) {
  if (!g_connected) { snprintf(g_lastErr, sizeof(g_lastErr), "not connected"); return false; }

  // THE CLIENT COMES FROM THE SERVER, not from NimBLEDevice::createClient(). We are riding
  // the connection the PHONE made to US, and `NimBLEServer::getClient(handle)` is the call
  // that hands you a GATT client over an existing inbound link. createClient() would try to
  // open a second, outbound connection to a device that is already connected — which fails
  // in a way that reads like the phone refusing us.
  if (!g_bleServer) { snprintf(g_lastErr, sizeof(g_lastErr), "no server"); return false; }
  NimBLEClient* c = g_bleServer->getClient(static_cast<uint16_t>(g_peerHandle));
  if (!c) { snprintf(g_lastErr, sizeof(g_lastErr), "no client for handle"); return false; }

  NimBLERemoteService* svc = c->getService(kCtsService);
  if (!svc) { snprintf(g_lastErr, sizeof(g_lastErr), "no CTS service (bonded?)"); return false; }
  NimBLERemoteCharacteristic* ch = svc->getCharacteristic(kCtsCurrent);
  if (!ch) { snprintf(g_lastErr, sizeof(g_lastErr), "no 0x2A2B characteristic"); return false; }

  const std::string v = ch->readValue();
  // TEN BYTES, and the layout is worth spelling out because getting hours from the wrong
  // offset produces a plausible-looking wrong time:
  //   [0..1] year, little endian   [2] month 1-12   [3] day
  //   [4] hours   [5] minutes      [6] seconds      [7] day of week
  //   [8] fractions of a second /256              [9] adjust reason
  if (v.size() < 7) {
    snprintf(g_lastErr, sizeof(g_lastErr), "short CTS read (%u bytes)",
             static_cast<unsigned>(v.size()));
    return false;
  }
  const uint8_t hh = static_cast<uint8_t>(v[4]);
  const uint8_t mm = static_cast<uint8_t>(v[5]);
  if (hh > 23 || mm > 59) {
    snprintf(g_lastErr, sizeof(g_lastErr), "implausible %02u:%02u", hh, mm);
    return false;
  }
  snprintf(out, 8, "%02u%02u", static_cast<unsigned>(hh), static_cast<unsigned>(mm));
  Serial.printf("[%8lu] phone clock: %04u-%02u-%02u %02u:%02u:%02u\n",
                static_cast<unsigned long>(millis()),
                static_cast<unsigned>(static_cast<uint8_t>(v[0]) |
                                      (static_cast<uint8_t>(v[1]) << 8)),
                static_cast<unsigned>(static_cast<uint8_t>(v[2])),
                static_cast<unsigned>(static_cast<uint8_t>(v[3])), hh, mm,
                static_cast<unsigned>(static_cast<uint8_t>(v[6])));
  return true;
}

void startBle() {
  NimBLEDevice::init(kBleName);
  // Bonding and Secure Connections, no MITM — there is no keyboard or display on this board
  // to type a passkey into, so Just Works is the only pairing that can succeed.
  NimBLEDevice::setSecurityAuth(/*bonding=*/true, /*mitm=*/false, /*sc=*/true);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);

  g_bleServer = NimBLEDevice::createServer();
  g_bleServer->setCallbacks(&g_serverCb, /*deleteCallbacks=*/false);

  // WE HAVE TO BE A HID DEVICE, AND THIS IS THE WHOLE REASON.
  //
  // The first version advertised a bare peripheral with no services and iOS never listed it
  // — Settings > Bluetooth does NOT show generic BLE peripherals. It shows HID, audio and
  // profiles it recognises; everything else is visible only to apps using CoreBluetooth.
  // With nothing to pair with there is no bond, and with no bond the phone will not expose
  // its Current Time Service. So the HID is not a feature here, it is the price of
  // admission: it makes the board appear in Settings, and the pairing it earns is what
  // unlocks the clock.
  //
  // The report map is a consumer-control keyboard — the media keys a car stalk would send —
  // carried over verbatim from MegaOpen's HidRole, which is known good. Nothing in this
  // example presses a key; it exists to be pairable. Wire it to the panel's steering-wheel
  // keys and the same bond gives you both directions.
  static const uint8_t kReportMap[] = {
      0x05, 0x0C,        // Usage Page (Consumer)
      0x09, 0x01,        // Usage (Consumer Control)
      0xA1, 0x01,        // Collection (Application)
      0x85, 0x01,        //   Report ID (1)
      0x15, 0x00,        //   Logical Minimum (0)
      0x26, 0xFF, 0x03,  //   Logical Maximum (1023)
      0x75, 0x10,        //   Report Size (16)
      0x95, 0x01,        //   Report Count (1)
      0x19, 0x00,        //   Usage Minimum (0)
      0x2A, 0xFF, 0x03,  //   Usage Maximum (1023)
      0x81, 0x00,        //   Input (Data, Array, Absolute)
      0xC0               // End Collection
  };
  NimBLEHIDDevice* hid = new NimBLEHIDDevice(g_bleServer);
  hid->setManufacturer("AffaDisplay");
  hid->setPnp(0x02, 0x05AC, 0x0239, 0x0110);
  hid->setHidInfo(0x00, 0x01);
  hid->setReportMap(const_cast<uint8_t*>(kReportMap), sizeof(kReportMap));
  (void)hid->getInputReport(1);
  g_bleServer->start();

  // THE PAYLOAD IS BUILT EXPLICITLY, AND `setFlags(0x06)` IS THE LINE THAT MATTERS.
  //
  // 0x06 is LE General Discoverable + BR/EDR Not Supported. Without the discoverable flag a
  // scanner may see the packet but a PHONE will not offer it: the second version of this
  // file advertised a name, an appearance and the HID UUID with no flags byte at all, and
  // iOS ignored it completely — zero connection attempts in the log across several minutes.
  // Setting the name and UUID on the NimBLEAdvertising object directly does not fill this
  // in for you.
  //
  // Building a NimBLEAdvertisementData and handing it over is also the only way to be sure
  // what is actually in the 31 bytes. This is MegaOpen's HidOnly profile verbatim, which is
  // the payload this phone has already been proven to pair with.
  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  adv->stop();

  NimBLEAdvertisementData advData;
  advData.setFlags(0x06);
  advData.setName(kBleName);
  advData.setAppearance(0x03C1);                    // HID keyboard
  advData.addServiceUUID(NimBLEUUID((uint16_t)0x1812));   // Human Interface Device
  adv->setAdvertisementData(advData);
  adv->enableScanResponse(false);
  adv->start();
  (void)hid;
  Serial.printf("BLE advertising as \"%s\" — HID keyboard, flags 0x06, %d bonds stored\n",
                kBleName, NimBLEDevice::getNumBonds());
}

// ---------------------------------------------------------------------------
// The panel
// ---------------------------------------------------------------------------
// THE SEAM FOR AMS. Everything the display half needs is three lines of text and a clock
// string; swap the clock for a track title and nothing below changes.
void pushToPanel() {
  snprintf(g_l1, sizeof(g_l1), "%s", g_connected ? (g_bonded ? "IPHONE BONDED"
                                                             : "IPHONE LINKED")
                                                 : "WAITING PHONE");
  snprintf(g_l2, sizeof(g_l2), "%s", g_hhmm[0] ? "CLOCK SYNCED" : "NO TIME YET");
  snprintf(g_l3, sizeof(g_l3), "SYNCS %lu  ERR %lu", static_cast<unsigned long>(g_syncs),
           static_cast<unsigned long>(g_syncFails));
}

// ---------------------------------------------------------------------------
// Web UI
// ---------------------------------------------------------------------------
PsychicHttpServer g_server;

void startWifi() {
  String ssid, pass;
  Preferences p;
  if (p.begin("megaopen", true)) {
    ssid = p.getString("ssid", ""); pass = p.getString("pass", ""); p.end();
  }
  WiFi.persistent(false);
  // WIFI MODEM SLEEP MUST STAY ON, AND THIS IS THE WHOLE COEXISTENCE STORY IN ONE LINE.
  //
  // The obvious thing is to turn it off: power save parks the radio between beacons, so with
  // BLE also wanting the antenna you would expect the parked windows to turn into HTTP
  // stalls. That reasoning is backwards and the board says so immediately —
  // `WiFi.setSleep(false)` makes `NimBLEDevice::init()` ABORT:
  //
  //     abort() at coex_core_enable  <-  coex_enable  <-  esp_bt_controller_enable
  //                                  <-  NimBLEDevice::init
  //
  // ESP32 has ONE radio and BLE and WiFi time-slice it. The coexistence scheduler gets its
  // slices out of WiFi's modem-sleep windows, so disabling modem sleep leaves it with no
  // time to hand BLE and it refuses to start at all. Sleep ON is not a compromise here; it
  // is the precondition. MegaOpen has had `setSleep(true)` all along, which is why its BLE
  // and WiFi have always coexisted.
  WiFi.setSleep(true);
  bool sta = false;
  if (ssid.length()) {
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid.c_str(), pass.c_str());
    const uint32_t until = millis() + 12000;
    while (millis() < until && WiFi.status() != WL_CONNECTED) delay(100);
    sta = WiFi.status() == WL_CONNECTED;
  }
  if (!sta) { WiFi.mode(WIFI_AP); WiFi.softAP("AffaClock", "affaclock"); }
  Serial.printf("console: http://%s/\n",
                (sta ? WiFi.localIP() : WiFi.softAPIP()).toString().c_str());
}

void routes() {
  g_server.on("/", HTTP_GET, [](PsychicRequest* r) {
    if (r->hasParam("clock")) g_clockEnabled = r->getParam("clock")->value() != "0";
    if (r->hasParam("ble")) {
      const bool want = r->getParam("ble")->value() != "0";
      if (want != g_bleEnabled) {
        g_bleEnabled = want;
        // Stopping ADVERTISING rather than deinitialising the stack: a NimBLE deinit with a
        // live connection is a reboot waiting to happen, and the interesting question here
        // is coexistence, which needs the stack up to be worth measuring.
        if (want) NimBLEDevice::getAdvertising()->start();
        else      NimBLEDevice::getAdvertising()->stop();
      }
    }
    if (r->hasParam("sync")) g_wantCtsRead = true;
    if (r->hasParam("unbond")) { NimBLEDevice::deleteAllBonds();
                                 snprintf(g_lastErr, sizeof(g_lastErr), "bonds cleared"); }

    char b[1600];
    snprintf(b, sizeof(b),
             "<!doctype html><meta charset=utf-8><meta http-equiv=refresh content=3>"
             "<title>AffaDisplay + iPhone</title>"
             "<body style='background:#111;color:#ddd;font:14px ui-monospace,monospace'>"
             "<pre>up %lus\n\n"
             "PANEL   phase %-10s  drops %lu\n"
             "        screens ok %lu  txErr %lu rxErr %lu busErr %lu\n\n"
             "BLE     %s   bonded %s   advertising %s\n"
             "        clock from phone: <b>%s</b>   syncs %lu  fails %lu\n"
             "        last error: %s\n\n"
             "WIFI    %s   rssi %d dBm\n"
             "        (BLE and WiFi are both up right now — that is the point)\n</pre>"
             "<a href='/?sync=1'>[ SYNC CLOCK NOW ]</a> "
             "<a href='/?clock=%d'>[ clock push: %s ]</a> "
             "<a href='/?ble=%d'>[ advertising: %s ]</a> "
             "<a href='/?unbond=1'>[ forget bonds ]</a>",
             static_cast<unsigned long>(millis() / 1000),
             affa::phaseName(g_display.phase()),
             static_cast<unsigned long>(g_display.sessionsLost()),
             static_cast<unsigned long>(g_display.stats().txFrames),
             static_cast<unsigned long>(g_link.driver().txErr),
             static_cast<unsigned long>(g_link.driver().rxErr),
             static_cast<unsigned long>(g_link.driver().busErr),
             g_connected ? "CONNECTED" : "waiting",
             g_bonded ? "yes" : "no",
             g_bleEnabled ? "on" : "off",
             g_hhmm[0] ? g_hhmm : "--:--",
             static_cast<unsigned long>(g_syncs), static_cast<unsigned long>(g_syncFails),
             g_lastErr,
             WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString().c_str() : "AP mode",
             static_cast<int>(WiFi.RSSI()),
             g_clockEnabled ? 0 : 1, g_clockEnabled ? "on" : "off",
             g_bleEnabled ? 0 : 1, g_bleEnabled ? "on" : "off");
    return r->reply(200, "text/html", b);
  });
}

}  // namespace

void setup() {
  pinMode(kTxPin, OUTPUT);
  digitalWrite(kTxPin, HIGH);
  delay(2000);
  Serial.begin(115200);
  delay(300);
  Serial.println("\n=== 15_iphone — panel clock from an iPhone, BLE + WiFi together ===");

  if (!g_link.begin(kRxPin, kTxPin, kBitrate))
    Serial.println("!! the CAN link did not come up");
  g_display.onComplete(&onDone, nullptr);
  g_display.begin();

  // CAN FIRST, THEN WIFI, THEN BLE — and the order is not cosmetic. The panel's peer
  // watchdog is five seconds; WiFi association can block for ten if the router is slow, and
  // NimBLE's init is not instant either. Starting the CAN session first means the handshake
  // is already up and the watchdog already being fed before anything else can stall the loop.
  startWifi();
  g_server.config.max_uri_handlers = 8;
  g_server.config.stack_size       = 8192;
  g_server.listen(80);
  routes();

  startBle();
  Serial.println("pair from the phone: Settings > Bluetooth > AffaClock");
}

void loop() {
  g_display.poll();
  const uint32_t now = millis();

  static affa::Phase s_phase = affa::Phase::Silent;
  if (g_display.phase() != s_phase) {
    Serial.printf("[%8lu] ** PHASE %s -> %s\n", static_cast<unsigned long>(now),
                  affa::phaseName(s_phase), affa::phaseName(g_display.phase()));
    s_phase = g_display.phase();
  }

  // The CTS read is a blocking GATT round-trip, so it happens HERE and never in a callback.
  if (g_wantCtsRead) {
    g_wantCtsRead = false;
    char hhmm[8];
    if (readPhoneClock(hhmm)) {
      snprintf(g_hhmm, sizeof(g_hhmm), "%s", hhmm);
      g_timeAtMs = now;
      ++g_syncs;
      snprintf(g_lastErr, sizeof(g_lastErr), "none");
      g_nextResyncAt = now + g_resyncMs;
    } else {
      ++g_syncFails;
      Serial.printf("[%8lu] !! clock read failed: %s\n", static_cast<unsigned long>(now),
                    g_lastErr);
    }
  }
  if (g_connected && g_bonded && g_hhmm[0] &&
      static_cast<int32_t>(now - g_nextResyncAt) >= 0)
    g_wantCtsRead = true;

  if (g_display.phase() != affa::Phase::Ready || g_busy) return;

  // THE CLOCK GOES OUT ONCE PER READ, not on a timer. The panel free-runs it afterwards —
  // measured on this unit: set to 10:00, read back as 10:11 eleven minutes later — so
  // re-sending the same value every second would be pure noise on a bus with a five-second
  // watchdog to feed.
  static char s_pushed[8] = {0};
  if (g_clockEnabled && g_hhmm[0] && strcmp(s_pushed, g_hhmm) != 0) {
    if (g_display.setTime(g_hhmm) == affa::Result::Ok) {
      snprintf(s_pushed, sizeof(s_pushed), "%s", g_hhmm);
      g_busy = true;
      Serial.printf("[%8lu] >> panel clock set to %s\n", static_cast<unsigned long>(now),
                    g_hhmm);
    }
    return;
  }

  static uint32_t s_nextDraw = 0;
  if (static_cast<int32_t>(now - s_nextDraw) < 0) return;
  s_nextDraw = now + 1000;
  pushToPanel();
  if (g_display.showFullscreenText(g_l1, g_l2, g_l3) == affa::Result::Ok) g_busy = true;
}
