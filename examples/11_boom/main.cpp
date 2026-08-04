// 11_boom — a countdown and a three-row explosion, on a Carminat panel.
//
// The point of the Carminat family here is the CANVAS. UpdateList gives you one line of
// twelve cells; Carminat's fullscreen gives you THREE ROWS OF TWENTY, which is enough for
// actual ASCII art rather than punctuation standing in for it.
//
//   pio run -e ex11_boom -t upload --upload-port COM5
//   pio device monitor -e ex11_boom
//
// IT IS ALSO A SOAK, and a harsher one than a marquee. Every frame below is a complete
// fullscreen — fourteen acknowledged ISO-TP frames — and the explosion runs at 180 ms per
// frame, which is only about three times the duration of one transfer. A panel or a bus
// that starts falling behind shows it here as a stutter, where a two-second phrase cycle
// would have hidden it entirely.
//
// THE ONE-IN-FLIGHT RULE IS WHY IT LOOKS SMOOTH. Two renders in a row COALESCE — they share
// RenderSlot::Fullscreen, so the second silently replaces the first — which on an animation
// means dropped frames rather than an error. Nothing here is queued until the panel has
// acknowledged the last thing, and the frame clock is a wall-clock deadline rather than a
// count of poll() calls.

#include <Arduino.h>
#include <AffaDisplay.h>
#include <Preferences.h>
#include <PsychicHttp.h>
#include <WiFi.h>

#if !AFFA_PANEL_CARMINAT
#  error "11_boom is a Carminat example: build with -D AFFA_PANEL_CARMINAT=1"
#endif
#if !AFFA_ENABLE_FULLSCREEN
#  error "11_boom draws fullscreens: build with -D AFFA_ENABLE_FULLSCREEN=1"
#endif

namespace {

constexpr gpio_num_t kRxPin   = GPIO_NUM_5;
constexpr gpio_num_t kTxPin   = GPIO_NUM_4;
constexpr uint32_t   kBitrate = 500000;

// TWENTY CELLS PER ROW, AND IT IS A WIRE FACT. Over-running it does not truncate politely:
// the strings go into an ISO-TP payload whose declared length is computed from them, so a
// twenty-first character changes the frame count. Every literal below is padded to exactly
// twenty and checked at boot.
constexpr size_t kCols = 20;

struct Screen { const char* l1; const char* l2; const char* l3; };

// ---------------------------------------------------------------------------
// The countdown
// ---------------------------------------------------------------------------
// TUNABLE FROM THE WEB PAGE, so a different countdown does not cost a cable and a reflash.
// Defaults, and the bounds, are here rather than in the form handler: a value that arrives
// over HTTP is untrusted input, and a zero tick or a 4000-frame countdown is a wedged
// animation with no error anywhere.
uint8_t  g_countFrom   = 10;
uint32_t g_tickMs      = 1000;
uint32_t g_boomFrameMs = 180;
constexpr uint32_t kSmoulderMs  = 2500;
constexpr uint8_t  kCountMin = 1,   kCountMax = 60;
constexpr uint32_t kTickMin  = 100, kTickMax  = 5000;
constexpr uint32_t kFrameMin = 60,  kFrameMax = 2000;

// THE DECORATION ESCALATES WITH THE COUNT. The panel has one type size and no colour, so
// urgency has to come from the punctuation and from the fuse burning down.
const char* urgency(uint8_t n) {
  if (n > 10) return "    STAND BACK      ";
  if (n > 3)  return "    GET CLEAR!      ";
  return "     !!! RUN !!!    ";
}

// ---------------------------------------------------------------------------
// The explosion
// ---------------------------------------------------------------------------
// Nine frames: a spark, the flash, the bang held across three, then debris and smoke.
const Screen kBoom[] = {
    {"         .          ", "        ...         ", "         .          "},
    {"       \\  |  /      ", "      -- *** --     ", "       /  |  \\      "},
    {"     \\\\  \\|/  //    ", "   == * BOOM * ==   ", "     //  /|\\  \\\\    "},
    {"   \\\\\\  \\\\|//  ///  ", "  ==== *BOOM!* ==== ", "   ///  //|\\\\  \\\\\\  "},
    {"  *    \\  |  /    * ", "     -- BOOM --     ", "  *    /  |  \\    * "},
    {"  .    *     *    . ", "        BOOM        ", "  .    *     *    . "},
    {" .        *       . ", "      .  .  .       ", "   *           *    "},
    {"        ~~~~        ", "       ~~~~~~       ", "        ~~~~        "},
    {"         ~~         ", "        ~  ~        ", "                    "},
};
constexpr size_t kBoomFrames = sizeof(kBoom) / sizeof(kBoom[0]);

enum class Act : uint8_t { Counting, Exploding, Smouldering };

struct ArduinoClock final : affa::IClock {
  uint32_t millis() const override { return ::millis(); }
};

affa::CanCommonLink   g_link;
ArduinoClock          g_clock;
affa::CarminatDisplay g_display(g_link, g_clock);

Act      g_act         = Act::Counting;
uint8_t  g_count       = 10;
size_t   g_boomIx      = 0;
uint32_t g_nextFrameAt = 0;
uint32_t g_cycles      = 0;
bool     g_busy        = false;   // one render in flight, always
uint32_t g_ok = 0, g_fail = 0;

char g_l1[kCols + 2], g_l2[kCols + 2], g_l3[kCols + 2];

void onSync(affa::SyncState s, void*) {
  Serial.printf("[%8lu] ** sync 0x%02X %s\n", static_cast<unsigned long>(millis()),
                static_cast<unsigned>(s),
                affa::hasFlag(s, affa::SyncState::FuncsReg) ? "REGISTERED" : "");
}

void onDone(affa::TxTicket, affa::Result r, void*) {
  // DELIVERY ARRIVES HERE. The render call returned an ACCEPTANCE verdict; this is the
  // panel's. Releasing the latch anywhere else drops animation frames silently.
  g_busy = false;
  if (r == affa::Result::Ok) ++g_ok; else { ++g_fail;
    Serial.printf("[%8lu] !! render failed (%u)\n", static_cast<unsigned long>(millis()),
                  static_cast<unsigned>(r)); }
}

// Draws the three rows and takes the in-flight latch. Returns false if the queue refused,
// which is reported rather than swallowed: a silent refusal is a frozen animation with
// every counter looking healthy, and that is exactly how it presented on this bench once.
bool draw(const char* l1, const char* l2, const char* l3) {
  const affa::Result r = g_display.showFullscreenText(l1, l2, l3);
  if (r != affa::Result::Ok) {
    Serial.printf("[%8lu] !! showFullscreenText refused (%u)\n",
                  static_cast<unsigned long>(millis()), static_cast<unsigned>(r));
    return false;
  }
  g_busy = true;
  return true;
}

// ---------------------------------------------------------------------------
// Web config — one page, one form, no JavaScript
// ---------------------------------------------------------------------------
PsychicHttpServer g_server;
constexpr const char* kCfgNs = "boom";

// THE WIFI CREDENTIALS ARE SHARED WITH 09_golden, on purpose. They live in the "megaopen"
// namespace and the board already has them, so this example joins the same network with no
// setup screen of its own. It deliberately does NOT offer a way to change them: one example
// owning that is enough, and two forms writing the same keys is a way to lose a network.
void startWifi() {
  String ssid, pass;
  Preferences p;
  if (p.begin("megaopen", true)) {
    ssid = p.getString("ssid", "");
    pass = p.getString("pass", "");
    p.end();
  }
  WiFi.persistent(false);
  WiFi.setSleep(true);
  bool sta = false;
  if (ssid.length()) {
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid.c_str(), pass.c_str());
    // BOUNDED, AND THE BOUND IS LOAD-BEARING. A blocking WiFi.begin() once sat for 258 s on
    // this bench and banked 372k bogus bus errors while it did — the panel looked dead and
    // the fault was a router. Ten seconds, then carry on without it: the animation is the
    // point and the console is a convenience.
    const uint32_t until = millis() + 10000;
    while (millis() < until && WiFi.status() != WL_CONNECTED) delay(100);
    sta = WiFi.status() == WL_CONNECTED;
  }
  if (!sta) { WiFi.mode(WIFI_AP); WiFi.softAP("AffaBoom", "affaboom"); }
  Serial.printf("console: http://%s/\n",
                (sta ? WiFi.localIP() : WiFi.softAPIP()).toString().c_str());
}

void loadCfg() {
  Preferences p;
  if (!p.begin(kCfgNs, true)) return;
  g_countFrom   = p.getUChar("from", g_countFrom);
  g_tickMs      = p.getUInt("tick", g_tickMs);
  g_boomFrameMs = p.getUInt("frame", g_boomFrameMs);
  p.end();
}

void saveCfg() {
  Preferences p;
  if (!p.begin(kCfgNs, false)) return;
  p.putUChar("from", g_countFrom);
  p.putUInt("tick", g_tickMs);
  p.putUInt("frame", g_boomFrameMs);
  p.end();
}

// CLAMPED AT THE DOOR. A value off a form is untrusted input, and the failure modes here are
// silent rather than loud: a zero tick is a countdown that never advances, and a 4000-second
// one is an animation nobody will ever see finish. Out-of-range is corrected, not rejected,
// because a form that argues with you is worse than one that rounds.
template <class T>
T clampArg(PsychicRequest* r, const char* name, T cur, T lo, T hi) {
  if (!r->hasParam(name)) return cur;
  const long v = r->getParam(name)->value().toInt();
  if (v < static_cast<long>(lo)) return lo;
  if (v > static_cast<long>(hi)) return hi;
  return static_cast<T>(v);
}

void routes() {
  g_server.on("/", HTTP_GET, [](PsychicRequest* r) {
    if (r->hasParam("from") || r->hasParam("tick") || r->hasParam("frame")) {
      g_countFrom   = clampArg<uint8_t>(r, "from", g_countFrom, kCountMin, kCountMax);
      g_tickMs      = clampArg<uint32_t>(r, "tick", g_tickMs, kTickMin, kTickMax);
      g_boomFrameMs = clampArg<uint32_t>(r, "frame", g_boomFrameMs, kFrameMin, kFrameMax);
      saveCfg();
      Serial.printf("[%8lu] ** config: from %u, tick %lu ms, frame %lu ms\n",
                    static_cast<unsigned long>(millis()),
                    static_cast<unsigned>(g_countFrom),
                    static_cast<unsigned long>(g_tickMs),
                    static_cast<unsigned long>(g_boomFrameMs));
      // Restart the countdown so the change is visible immediately rather than at the end
      // of whatever cycle happened to be running.
      g_act = Act::Counting;
      g_count = g_countFrom;
      g_nextFrameAt = millis();
    }
    char b[1400];
    snprintf(b, sizeof(b),
             "<!doctype html><meta charset=utf-8><meta http-equiv=refresh content=5>"
             "<title>AffaDisplay boom</title>"
             "<body style='background:#111;color:#ddd;font:14px ui-monospace,monospace'>"
             "<pre>up %lus   phase %s   cycles %lu\n"
             "screens ok %lu  failed %lu\n"
             "driver txErr %lu rxErr %lu busErr %lu</pre>"
             "<form><table>"
             "<tr><td>countdown from</td><td><input name=from value=%u size=4></td>"
             "<td>%u..%u</td></tr>"
             "<tr><td>seconds per tick (ms)</td><td><input name=tick value=%lu size=6></td>"
             "<td>%lu..%lu</td></tr>"
             "<tr><td>explosion frame (ms)</td><td><input name=frame value=%lu size=6></td>"
             "<td>%lu..%lu</td></tr>"
             "</table><button>apply</button></form>",
             static_cast<unsigned long>(millis() / 1000),
             affa::phaseName(g_display.phase()),
             static_cast<unsigned long>(g_cycles),
             static_cast<unsigned long>(g_ok), static_cast<unsigned long>(g_fail),
             static_cast<unsigned long>(g_link.driver().txErr),
             static_cast<unsigned long>(g_link.driver().rxErr),
             static_cast<unsigned long>(g_link.driver().busErr),
             static_cast<unsigned>(g_countFrom),
             static_cast<unsigned>(kCountMin), static_cast<unsigned>(kCountMax),
             static_cast<unsigned long>(g_tickMs),
             static_cast<unsigned long>(kTickMin), static_cast<unsigned long>(kTickMax),
             static_cast<unsigned long>(g_boomFrameMs),
             static_cast<unsigned long>(kFrameMin), static_cast<unsigned long>(kFrameMax));
    return r->reply(200, "text/html", b);
  });
}

}  // namespace

void setup() {
  pinMode(kTxPin, OUTPUT);
  digitalWrite(kTxPin, HIGH);      // hold the bus recessive while the transceiver settles
  delay(2000);

  Serial.begin(115200);
  delay(300);
  Serial.println();
  Serial.println("=== 11_boom — countdown and a three-row explosion ===");

  // EVERY FRAME IS MEASURED AT BOOT, because an over-long row is not a typo you can see —
  // it changes the ISO-TP declared length and the frame count with it, and the symptom is a
  // wire-format bug rather than a wonky picture.
  bool widthsOk = true;
  for (size_t i = 0; i < kBoomFrames; ++i) {
    const char* rows[3] = {kBoom[i].l1, kBoom[i].l2, kBoom[i].l3};
    for (int r = 0; r < 3; ++r) {
      if (strlen(rows[r]) > kCols) {
        Serial.printf("!! boom frame %u row %d is %u cells, budget is %u\n",
                      static_cast<unsigned>(i), r,
                      static_cast<unsigned>(strlen(rows[r])),
                      static_cast<unsigned>(kCols));
        widthsOk = false;
      }
    }
  }
  Serial.printf("%u explosion frames, widths %s\n", static_cast<unsigned>(kBoomFrames),
                widthsOk ? "OK" : "BAD — fix them before trusting anything below");

  if (!g_link.begin(kRxPin, kTxPin, kBitrate))
    Serial.println("!! the CAN link did not come up");

  g_display.onSync(&onSync, nullptr);
  g_display.onComplete(&onDone, nullptr);
  g_display.begin();

  loadCfg();
  g_count = g_countFrom;
  Serial.printf("countdown from %u, tick %lu ms, explosion frame %lu ms\n",
                static_cast<unsigned>(g_countFrom), static_cast<unsigned long>(g_tickMs),
                static_cast<unsigned long>(g_boomFrameMs));
  startWifi();
  g_server.config.max_uri_handlers = 8;
  g_server.config.stack_size       = 8192;
  g_server.listen(80);
  routes();

  Serial.println("waiting for the panel. It leads; we answer.");
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

  // NOTHING IS DRAWN BEFORE Ready, and Ready means registered AND the glass is on — the
  // library sends `03 52 09` itself on the way there. Drawing earlier is the failure with
  // no symptom: the panel acknowledges a screen it never lights.
  if (g_display.phase() != affa::Phase::Ready || g_busy) return;
  if (static_cast<int32_t>(now - g_nextFrameAt) < 0) return;

  switch (g_act) {
    case Act::Counting: {
      // THE FUSE IS THE COUNT, SCALED TO THE ROW. Drawing one cell per second only works
      // for a twenty-second countdown; scaling means the bar always starts full and always
      // ends empty, whatever the web page is set to, so the bar and the number can never
      // disagree with each other.
      memset(g_l2, ' ', kCols);
      g_l2[kCols] = 0;
      const size_t lit = (static_cast<size_t>(g_count) * kCols + g_countFrom - 1) /
                         (g_countFrom ? g_countFrom : 1);
      for (size_t i = 0; i < lit && i < kCols; ++i) g_l2[i] = '#';
      snprintf(g_l1, sizeof(g_l1), "     T MINUS %2u     ", static_cast<unsigned>(g_count));
      snprintf(g_l3, sizeof(g_l3), "%s", urgency(g_count));
      if (!draw(g_l1, g_l2, g_l3)) { g_nextFrameAt = now + 250; break; }
      g_nextFrameAt = now + g_tickMs;
      if (--g_count == 0) { g_act = Act::Exploding; g_boomIx = 0; }
      break;
    }
    case Act::Exploding: {
      const Screen& f = kBoom[g_boomIx];
      if (!draw(f.l1, f.l2, f.l3)) { g_nextFrameAt = now + 100; break; }
      Serial.printf("[%8lu] >> boom frame %u/%u\n", static_cast<unsigned long>(now),
                    static_cast<unsigned>(g_boomIx + 1),
                    static_cast<unsigned>(kBoomFrames));
      g_nextFrameAt = now + g_boomFrameMs;
      if (++g_boomIx >= kBoomFrames) {
        g_act = Act::Smouldering;
        g_nextFrameAt = now + kSmoulderMs;
      }
      break;
    }
    case Act::Smouldering: {
      ++g_cycles;
      Serial.printf("[%8lu] == cycle %lu complete | screens %lu ok / %lu failed | "
                    "txErr %lu rxErr %lu busErr %lu\n",
                    static_cast<unsigned long>(now), static_cast<unsigned long>(g_cycles),
                    static_cast<unsigned long>(g_ok), static_cast<unsigned long>(g_fail),
                    static_cast<unsigned long>(g_link.driver().txErr),
                    static_cast<unsigned long>(g_link.driver().rxErr),
                    static_cast<unsigned long>(g_link.driver().busErr));
      g_act   = Act::Counting;
      g_count = g_countFrom;
      break;
    }
  }
}
