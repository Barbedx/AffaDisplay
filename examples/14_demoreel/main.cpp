// 14_demoreel — six effects on a 20x3 dashboard panel, eight seconds each, forever.
//
// 13_starfield was correct and boring: sparse dots at six frames a second are a subtle
// effect on a display whose whole character is BRIGHT. This is the opposite bet — dense,
// full-width, high-contrast motion, and a new effect before you can get tired of the last
// one. The reel is the answer to "boring", not any single effect in it.
//
//   1. SCANNER   a Knight Rider sweep with a decaying trail. On a Renault dashboard this is
//                the only correct joke, and it is also the highest-contrast thing the panel
//                can do: a full-height bar against blank.
//   2. FIRE      seeded on the bottom row, averaged and decayed upward. The panel is orange;
//                the effect is orange. It is the one that looks like it belongs there.
//   3. SCROLLER  a message scrolling right-to-left with each column bobbed by a sine — the
//                demoscene standard, and it uses all three rows as one surface.
//   4. RINGS     a shockwave expanding from the centre on a rhythm, over and over.
//   5. BOUNCE    a ball with a decaying trail, reflecting off all four walls.
//   6. RAIN      columns of glyphs falling at independent speeds.
//
//   pio run -e ex14_demoreel -t upload --upload-port COM5
//
// EVERY FRAME IS A COMPLETE FULLSCREEN — fourteen acknowledged ISO-TP frames — and unlike a
// marquee the picture changes every single frame, so nothing is skipped by repaint-on-change.
// At 120 ms that is a sustained ~8 screens/second, which is the rate the 1 h 36 m soak ran
// at. This is a demo and a load test at the same time.

#include <Arduino.h>
#include <AffaDisplay.h>
#include <Preferences.h>
#include <PsychicHttp.h>
#include <WiFi.h>

#if !AFFA_PANEL_CARMINAT
#  error "14_demoreel is a Carminat example: build with -D AFFA_PANEL_CARMINAT=1"
#endif
#if !AFFA_ENABLE_FULLSCREEN
#  error "14_demoreel draws fullscreens: build with -D AFFA_ENABLE_FULLSCREEN=1"
#endif

namespace {

constexpr gpio_num_t kRxPin   = GPIO_NUM_5;
constexpr gpio_num_t kTxPin   = GPIO_NUM_4;
constexpr uint32_t   kBitrate = 500000;

constexpr int kCols = 20;
constexpr int kRows = 3;

// THE INTENSITY RAMP, and everything below is written in terms of it. Five levels is what a
// single-colour dot-matrix can actually express through glyph density; more steps just look
// like different characters rather than different brightnesses.
const char kRamp[] = {' ', '.', ':', '*', '#'};
constexpr uint8_t kLevels = sizeof(kRamp);

uint8_t g_buf[kRows][kCols];        // intensity 0..4, rendered through kRamp at the end
char    g_row[kRows][kCols + 2];

struct ArduinoClock final : affa::IClock {
  uint32_t millis() const override { return ::millis(); }
};

affa::CanCommonLink   g_link;
ArduinoClock          g_clock;
affa::CarminatDisplay g_display(g_link, g_clock);

bool     g_busy = false;
uint32_t g_nextFrameAt = 0, g_frames = 0, g_ok = 0, g_fail = 0;

enum class Fx : uint8_t { Scanner, Fire, Scroller, Rings, Bounce, Rain, kCount };
Fx       g_fx      = Fx::Scanner;
uint32_t g_fxUntil = 0;
uint32_t g_tick    = 0;             // frames since this effect started

uint32_t g_frameMs = 120;
uint32_t g_fxMs    = 8000;
constexpr uint32_t kFrameMin = 80, kFrameMax = 1000;
constexpr uint32_t kFxMin = 2000,  kFxMax = 60000;
char g_msg[64] = "AFFADISPLAY 0.5.0   BOTH FAMILIES ON GLASS   ";

const char* fxName(Fx f) {
  switch (f) {
    case Fx::Scanner:  return "SCANNER";
    case Fx::Fire:     return "FIRE";
    case Fx::Scroller: return "SCROLLER";
    case Fx::Rings:    return "RINGS";
    case Fx::Bounce:   return "BOUNCE";
    case Fx::Rain:     return "RAIN";
    default:           return "?";
  }
}

void clearBuf() { memset(g_buf, 0, sizeof(g_buf)); }

// A cheap integer sine, 0..255 in, -100..100 out. No <math.h>, no floats: this runs inside
// the render of every frame and the panel cannot tell the difference between this and a
// real sine at three rows of resolution.
int isin(uint8_t a) {
  const bool neg = a >= 128;
  uint8_t x = neg ? a - 128 : a;          // 0..127 over half a period
  if (x > 64) x = 128 - x;                // fold to a quarter
  const int v = (x * 100) / 64;           // triangle, close enough at this scale
  const int s = (v * (200 - v)) / 100;    // round the corners
  return neg ? -s : s;
}

// ---------------------------------------------------------------------------
// 1. SCANNER — the Knight Rider sweep
// ---------------------------------------------------------------------------
// THE TRAIL IS THE EFFECT. A bar that simply moves reads as a glitch; a bar with three cells
// of decay behind it reads as motion with direction. The head is full height, which is the
// strongest mark a 3-row panel can make.
void fxScanner() {
  clearBuf();
  const int span = (kCols - 1) * 2;                  // there and back
  const int p    = g_tick % span;
  const int head = p < kCols ? p : span - p;
  const int dir  = p < kCols ? -1 : 1;               // trail lags BEHIND the direction
  for (int t = 0; t < 4; ++t) {
    const int c = head + dir * t;
    if (c < 0 || c >= kCols) continue;
    const uint8_t level = static_cast<uint8_t>(kLevels - 1 - t);
    for (int r = 0; r < kRows; ++r)
      if (level > g_buf[r][c]) g_buf[r][c] = level;
  }
}

// ---------------------------------------------------------------------------
// 2. FIRE — seeded low, decayed upward
// ---------------------------------------------------------------------------
// The classic cellular fire, with the one change three rows forces: the seed row is
// RE-RANDOMISED every frame rather than held, because with only two rows above it there is
// no room for a slow-moving front and a static seed reads as a bar.
void fxFire() {
  static uint8_t heat[kRows][kCols];
  for (int c = 0; c < kCols; ++c)
    heat[kRows - 1][c] = static_cast<uint8_t>(esp_random() % 2 ? kLevels - 1 : kLevels - 2);
  for (int r = kRows - 2; r >= 0; --r) {
    for (int c = 0; c < kCols; ++c) {
      const int l = heat[r + 1][c > 0 ? c - 1 : 0];
      const int m = heat[r + 1][c];
      const int rr = heat[r + 1][c < kCols - 1 ? c + 1 : kCols - 1];
      int v = (l + m + rr) / 3;
      // Decay is randomised so the flame edge is ragged. A fixed decay produces three
      // perfectly flat bands, which reads as a gradient rather than as fire.
      if (esp_random() % 3 == 0 && v > 0) --v;
      if (v > 0) --v;
      heat[r][c] = static_cast<uint8_t>(v < 0 ? 0 : v);
    }
  }
  for (int r = 0; r < kRows; ++r)
    for (int c = 0; c < kCols; ++c) g_buf[r][c] = heat[r][c];
}

// ---------------------------------------------------------------------------
// 3. SCROLLER — sine-bobbed message
// ---------------------------------------------------------------------------
// Text is written straight into the character grid rather than the intensity buffer, so this
// one bypasses the ramp. Its vertical offset comes from the column's phase, which makes the
// message ripple across the panel instead of sliding flat.
void fxScroller(char out[kRows][kCols + 2]) {
  for (int r = 0; r < kRows; ++r) {
    memset(out[r], ' ', kCols);
    out[r][kCols] = 0;
  }
  const int len = static_cast<int>(strlen(g_msg));
  if (!len) return;
  const int off = static_cast<int>(g_tick / 2) % len;      // one cell per two frames
  for (int c = 0; c < kCols; ++c) {
    const char ch = g_msg[(off + c) % len];
    if (ch == ' ') continue;
    // THE WAVE MUST BE SLOWER THAN ONE CYCLE PER SCREEN, and the first version was not: at
    // 20 units of phase per column the sine completed a full period every ~13 cells, so
    // neighbouring letters landed on different rows and the message shredded into confetti.
    // Six units per column puts a little under half a period across the whole panel, which
    // is one gentle arc — the letters stay adjacent and the word survives the ride.
    //
    // Three rows means the bob quantises to -1/0/+1 whatever the maths says, so the
    // thresholds are wide: a narrow band would put almost every letter on the middle row and
    // the wave would vanish.
    const int bob = isin(static_cast<uint8_t>((g_tick * 3 + c * 6) & 0xFF));
    const int r   = 1 + (bob > 55 ? 1 : (bob < -55 ? -1 : 0));
    out[r < 0 ? 0 : (r >= kRows ? kRows - 1 : r)][c] = ch;
  }
}

// ---------------------------------------------------------------------------
// 4. RINGS — a shockwave on a rhythm
// ---------------------------------------------------------------------------
void fxRings() {
  clearBuf();
  const int period = 14;
  for (int wave = 0; wave < 2; ++wave) {              // two in flight, half a period apart
    const int age = (g_tick + wave * period / 2) % period;
    const int rad = age * 2;                          // columns are half the height, so the
                                                      // ring must travel twice as fast in x
    for (int r = 0; r < kRows; ++r) {
      const int dy = (r - 1) * 2;
      for (int c = 0; c < kCols; ++c) {
        const int dx = c - kCols / 2;
        const int d  = dx * dx + dy * dy;
        const int lo = (rad - 1) * (rad - 1), hi = (rad + 1) * (rad + 1);
        if (d >= lo && d <= hi) {
          const uint8_t level =
              static_cast<uint8_t>(age > 5 ? 1 : (kLevels - 1 - age / 2));
          if (level > g_buf[r][c]) g_buf[r][c] = level;
        }
      }
    }
  }
}

// ---------------------------------------------------------------------------
// 5. BOUNCE — a ball with a decaying trail
// ---------------------------------------------------------------------------
void fxBounce() {
  static int x = 3, y = 0, vx = 1, vy = 1;
  // The trail lives in the buffer between frames: everything fades by one level, then the
  // ball is stamped at full. That is why this effect does NOT clear.
  for (int r = 0; r < kRows; ++r)
    for (int c = 0; c < kCols; ++c) if (g_buf[r][c]) --g_buf[r][c];

  x += vx; y += vy;
  if (x <= 0)         { x = 0;         vx = 1; }
  if (x >= kCols - 1) { x = kCols - 1; vx = -1; }
  if (y <= 0)         { y = 0;         vy = 1; }
  if (y >= kRows - 1) { y = kRows - 1; vy = -1; }
  g_buf[y][x] = kLevels - 1;
}

// ---------------------------------------------------------------------------
// 6. RAIN — columns falling at their own speeds
// ---------------------------------------------------------------------------
void fxRain() {
  static uint8_t pos[kCols], speed[kCols];
  static bool init = false;
  if (!init) {
    for (int c = 0; c < kCols; ++c) {
      pos[c]   = static_cast<uint8_t>(esp_random() % 12);
      speed[c] = static_cast<uint8_t>(1 + esp_random() % 3);
    }
    init = true;
  }
  clearBuf();
  for (int c = 0; c < kCols; ++c) {
    if ((g_tick % speed[c]) == 0) pos[c] = static_cast<uint8_t>((pos[c] + 1) % 12);
    const int head = static_cast<int>(pos[c]) - 4;    // most of the cycle is off-screen, so
                                                      // each column has a gap before it falls
    for (int t = 0; t < 3; ++t) {
      const int r = head - t;
      if (r < 0 || r >= kRows) continue;
      g_buf[r][c] = static_cast<uint8_t>(kLevels - 1 - t);
    }
  }
}

void renderBuf() {
  for (int r = 0; r < kRows; ++r) {
    for (int c = 0; c < kCols; ++c) {
      const uint8_t v = g_buf[r][c] >= kLevels ? kLevels - 1 : g_buf[r][c];
      g_row[r][c] = kRamp[v];
    }
    g_row[r][kCols] = 0;
  }
}

void step() {
  switch (g_fx) {
    case Fx::Scanner:  fxScanner();  renderBuf(); break;
    case Fx::Fire:     fxFire();     renderBuf(); break;
    case Fx::Rings:    fxRings();    renderBuf(); break;
    case Fx::Bounce:   fxBounce();   renderBuf(); break;
    case Fx::Rain:     fxRain();     renderBuf(); break;
    case Fx::Scroller: fxScroller(g_row); break;    // writes characters, not intensities
    default: break;
  }
  ++g_tick;
}

void onDone(affa::TxTicket, affa::Result r, void*) {
  g_busy = false;
  if (r == affa::Result::Ok) ++g_ok; else ++g_fail;
}

// ---------------------------------------------------------------------------
// Web config
// ---------------------------------------------------------------------------
PsychicHttpServer g_server;
constexpr const char* kCfgNs = "reel";

void startWifi() {
  String ssid, pass;
  Preferences p;
  if (p.begin("megaopen", true)) {
    ssid = p.getString("ssid", ""); pass = p.getString("pass", ""); p.end();
  }
  WiFi.persistent(false);
  WiFi.setSleep(true);
  bool sta = false;
  if (ssid.length()) {
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid.c_str(), pass.c_str());
    const uint32_t until = millis() + 10000;   // bounded: a slow router is not a dead panel
    while (millis() < until && WiFi.status() != WL_CONNECTED) delay(100);
    sta = WiFi.status() == WL_CONNECTED;
  }
  if (!sta) { WiFi.mode(WIFI_AP); WiFi.softAP("AffaReel", "affareel"); }
  Serial.printf("console: http://%s/\n",
                (sta ? WiFi.localIP() : WiFi.softAPIP()).toString().c_str());
}

void loadCfg() {
  Preferences p;
  if (!p.begin(kCfgNs, true)) return;
  g_frameMs = p.getUInt("frame", g_frameMs);
  g_fxMs    = p.getUInt("hold", g_fxMs);
  const String m = p.getString("msg", "");
  if (m.length()) snprintf(g_msg, sizeof(g_msg), "%s", m.c_str());
  p.end();
}

void saveCfg() {
  Preferences p;
  if (!p.begin(kCfgNs, false)) return;
  p.putUInt("frame", g_frameMs);
  p.putUInt("hold", g_fxMs);
  p.putString("msg", g_msg);
  p.end();
}

void routes() {
  g_server.on("/", HTTP_GET, [](PsychicRequest* r) {
    if (r->hasParam("frame") || r->hasParam("hold") || r->hasParam("msg") ||
        r->hasParam("fx")) {
      if (r->hasParam("frame")) {
        const long v = r->getParam("frame")->value().toInt();
        g_frameMs = v < static_cast<long>(kFrameMin) ? kFrameMin
                  : (v > static_cast<long>(kFrameMax) ? kFrameMax
                                                      : static_cast<uint32_t>(v));
      }
      if (r->hasParam("hold")) {
        const long v = r->getParam("hold")->value().toInt();
        g_fxMs = v < static_cast<long>(kFxMin) ? kFxMin
               : (v > static_cast<long>(kFxMax) ? kFxMax : static_cast<uint32_t>(v));
      }
      if (r->hasParam("msg") && r->getParam("msg")->value().length())
        snprintf(g_msg, sizeof(g_msg), "%s", r->getParam("msg")->value().c_str());
      // Jump straight to an effect, so you do not have to wait out the reel to see one.
      if (r->hasParam("fx")) {
        const long v = r->getParam("fx")->value().toInt();
        if (v >= 0 && v < static_cast<long>(Fx::kCount)) {
          g_fx = static_cast<Fx>(v);
          g_tick = 0;
          g_fxUntil = millis() + g_fxMs;
        }
      }
      saveCfg();
    }
    String out;
    out.reserve(2200);
    char b[900];
    snprintf(b, sizeof(b),
             "<!doctype html><meta charset=utf-8><meta http-equiv=refresh content=3>"
             "<title>AffaDisplay demo reel</title>"
             "<body style='background:#120800;color:#fb0;font:14px ui-monospace,monospace'>"
             "<pre>up %lus   phase %s   now playing: <b>%s</b>\n"
             "frames %lu   screens ok %lu failed %lu   txErr %lu rxErr %lu busErr %lu\n\n"
             "+--------------------+\n|%s|\n|%s|\n|%s|\n+--------------------+</pre>",
             static_cast<unsigned long>(millis() / 1000),
             affa::phaseName(g_display.phase()), fxName(g_fx),
             static_cast<unsigned long>(g_frames), static_cast<unsigned long>(g_ok),
             static_cast<unsigned long>(g_fail),
             static_cast<unsigned long>(g_link.driver().txErr),
             static_cast<unsigned long>(g_link.driver().rxErr),
             static_cast<unsigned long>(g_link.driver().busErr),
             g_row[0], g_row[1], g_row[2]);
    out += b;
    for (uint8_t i = 0; i < static_cast<uint8_t>(Fx::kCount); ++i) {
      snprintf(b, sizeof(b), "<a href='/?fx=%u' style='color:#fb0'>[ %s ]</a> ", i,
               fxName(static_cast<Fx>(i)));
      out += b;
    }
    snprintf(b, sizeof(b),
             "<form style='margin-top:1em'><table>"
             "<tr><td>frame (ms)</td><td><input name=frame value=%lu size=6></td></tr>"
             "<tr><td>seconds per effect (ms)</td><td><input name=hold value=%lu size=7></td></tr>"
             "<tr><td>scroller text</td><td><input name=msg value=\"%s\" size=40></td></tr>"
             "</table><button>apply</button></form>",
             static_cast<unsigned long>(g_frameMs), static_cast<unsigned long>(g_fxMs),
             g_msg);
    out += b;
    return r->reply(200, "text/html", out.c_str());
  });
}

}  // namespace

void setup() {
  pinMode(kTxPin, OUTPUT);
  digitalWrite(kTxPin, HIGH);
  delay(2000);
  Serial.begin(115200);
  delay(300);
  Serial.println("\n=== 14_demoreel — six effects on 20x3 ===");

  if (!g_link.begin(kRxPin, kTxPin, kBitrate))
    Serial.println("!! the CAN link did not come up");

  g_display.onComplete(&onDone, nullptr);
  g_display.begin();

  loadCfg();
  for (int r = 0; r < kRows; ++r) { memset(g_row[r], ' ', kCols); g_row[r][kCols] = 0; }
  startWifi();
  g_server.config.max_uri_handlers = 8;
  g_server.config.stack_size       = 8192;
  g_server.listen(80);
  routes();
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

  if (g_display.phase() != affa::Phase::Ready || g_busy) return;
  if (static_cast<int32_t>(now - g_nextFrameAt) < 0) return;

  if (static_cast<int32_t>(now - g_fxUntil) >= 0) {
    g_fx = static_cast<Fx>((static_cast<uint8_t>(g_fx) + 1) %
                           static_cast<uint8_t>(Fx::kCount));
    g_tick = 0;
    g_fxUntil = now + g_fxMs;
    // The buffer carries state between effects — BOUNCE deliberately never clears it — so it
    // is wiped at the boundary rather than inside the effects that do not want it.
    clearBuf();
    Serial.printf("[%8lu] >> %s\n", static_cast<unsigned long>(now), fxName(g_fx));
  }

  step();
  if (g_display.showFullscreenText(g_row[0], g_row[1], g_row[2]) == affa::Result::Ok) {
    g_busy = true;
    ++g_frames;
    g_nextFrameAt = now + g_frameMs;
  } else {
    g_nextFrameAt = now + 100;
  }
}
