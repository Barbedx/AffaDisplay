// 13_starfield — perspective 3D on a 20x3 dot-matrix panel, at ~6 frames a second.
//
// WHY A STARFIELD AND NOT A CUBE. Twenty columns by three rows is not enough to project a
// wireframe solid — a rotating cube resolves to a blob and reads as noise. A starfield does
// not have that problem, because the depth cue is not the SHAPE, it is the RATE: points near
// the camera sweep outward fast and points far away crawl. Three rows is plenty for that,
// and the effect is genuinely three-dimensional rather than a picture of something 3D.
//
// It is procedural, not a canned frame table like 11_boom's explosion. Each star carries an
// (dx, dy) direction and a depth z; every frame z decreases and the star is projected as
//
//     column = 10 + dx * kFocal / z        row = 1 + dy * kFocal / z
//
// which is ordinary perspective division, in integers. As z falls the divisor shrinks, so
// the same fixed direction sweeps outward and accelerates — exactly what flying through a
// star field looks like. When a star leaves the viewport it is respawned deep and far.
//
// FOUR GLYPHS OF DEPTH: `.` far, `+` middle distance, `*` near, `#` about to pass you. That
// ramp is doing as much work as the motion; with a single glyph the same maths reads as
// scattered dots rather than as depth.
//
//   pio run -e ex13_starfield -t upload --upload-port COM5
//
// IT IS ALSO A SOAK. Every frame is a complete fullscreen — fourteen acknowledged ISO-TP
// frames — and unlike a marquee the picture changes EVERY frame, so nothing is ever skipped
// by the repaint-on-change rule. That makes it the steadiest continuous load in the tree.

#include <Arduino.h>
#include <AffaDisplay.h>
#include <Preferences.h>
#include <PsychicHttp.h>
#include <WiFi.h>

#if !AFFA_PANEL_CARMINAT
#  error "13_starfield is a Carminat example: build with -D AFFA_PANEL_CARMINAT=1"
#endif
#if !AFFA_ENABLE_FULLSCREEN
#  error "13_starfield draws fullscreens: build with -D AFFA_ENABLE_FULLSCREEN=1"
#endif

namespace {

constexpr gpio_num_t kRxPin   = GPIO_NUM_5;
constexpr gpio_num_t kTxPin   = GPIO_NUM_4;
constexpr uint32_t   kBitrate = 500000;

constexpr int  kCols = 20;
constexpr int  kRows = 3;
constexpr int  kCx   = kCols / 2;      // the vanishing point
constexpr int  kCy   = kRows / 2;

// THE FOCAL LENGTH, and it is the one number that decides whether this reads as depth or as
// static. Too small and every star sits at the centre until it vanishes; too large and they
// all start off-screen. 64 puts a mid-depth star about a third of the way out, which is
// where the eye reads perspective.
constexpr int32_t kFocal = 64;

constexpr uint16_t kZFar  = 950;
constexpr uint16_t kZNear = 60;

// Tunable from the web form. Bounds live here, not in the handler: a zero step is a frozen
// field and a step past kZFar makes every star teleport, and neither raises an error.
constexpr size_t kMaxStars = 24;
uint8_t  g_stars   = 14;
uint16_t g_zStep   = 55;
uint32_t g_frameMs = 160;
constexpr uint8_t  kStarsMin = 1,  kStarsMax = kMaxStars;
constexpr uint16_t kStepMin  = 5,  kStepMax  = 300;
constexpr uint32_t kFrameMin = 80, kFrameMax = 2000;

struct Star {
  int16_t  dx, dy;      // fixed direction from the vanishing point
  uint16_t z;           // depth; smaller is nearer
};
Star g_field[kMaxStars];

struct ArduinoClock final : affa::IClock {
  uint32_t millis() const override { return ::millis(); }
};

affa::CanCommonLink   g_link;
ArduinoClock          g_clock;
affa::CarminatDisplay g_display(g_link, g_clock);

bool     g_busy = false;
uint32_t g_nextFrameAt = 0, g_frames = 0, g_ok = 0, g_fail = 0;
char     g_row[kRows][kCols + 2];

// SPAWNED DEEP, AND NEVER EXACTLY ON THE AXIS. A star with dx == 0 and dy == 0 sits on the
// vanishing point for its whole life and never moves — one motionless dot in the middle of
// the screen, which looks like a stuck pixel rather than a star.
void spawn(Star& s, bool anywhere) {
  do {
    s.dx = static_cast<int16_t>(esp_random() % 281) - 140;
    s.dy = static_cast<int16_t>(esp_random() % 37) - 18;
  } while (s.dx == 0 && s.dy == 0);
  // On the first fill, scatter the depths — otherwise the whole field arrives in one wave
  // and the animation pulses instead of flowing.
  s.z = anywhere ? static_cast<uint16_t>(kZNear + esp_random() % (kZFar - kZNear))
                 : kZFar;
}

char glyphFor(uint16_t z) {
  if (z > 620) return '.';
  if (z > 360) return '+';
  if (z > 160) return '*';
  return '#';
}

void stepField() {
  for (int r = 0; r < kRows; ++r) {
    memset(g_row[r], ' ', kCols);
    g_row[r][kCols] = 0;
  }
  for (uint8_t i = 0; i < g_stars; ++i) {
    Star& s = g_field[i];
    // Perspective division. Integer, and the divisor is guaranteed non-zero by kZNear.
    const int col = kCx + static_cast<int>((static_cast<int32_t>(s.dx) * kFocal) / s.z);
    const int row = kCy + static_cast<int>((static_cast<int32_t>(s.dy) * kFocal) / s.z);

    if (col < 0 || col >= kCols || row < 0 || row >= kRows || s.z <= kZNear) {
      spawn(s, /*anywhere=*/false);      // it swept past the camera; send it back out
      continue;
    }
    // NEARER WINS. Two stars can project onto the same cell, and drawing them in array order
    // would let a distant `.` overwrite a near `#` — depth would flicker at random.
    const char g = glyphFor(s.z);
    const char cur = g_row[row][col];
    if (cur == ' ' || glyphFor(s.z) == '#' ||
        (cur == '.' && g != '.') || (cur == '+' && (g == '*' || g == '#')))
      g_row[row][col] = g;

    s.z = (s.z > g_zStep + kZNear) ? static_cast<uint16_t>(s.z - g_zStep) : kZNear;
  }
}

void onDone(affa::TxTicket, affa::Result r, void*) {
  g_busy = false;
  if (r == affa::Result::Ok) ++g_ok; else ++g_fail;
}

// ---------------------------------------------------------------------------
// Web config — same shape as 11_boom, same shared credentials
// ---------------------------------------------------------------------------
PsychicHttpServer g_server;
constexpr const char* kCfgNs = "stars";

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
    const uint32_t until = millis() + 10000;   // bounded; a slow router is not a dead panel
    while (millis() < until && WiFi.status() != WL_CONNECTED) delay(100);
    sta = WiFi.status() == WL_CONNECTED;
  }
  if (!sta) { WiFi.mode(WIFI_AP); WiFi.softAP("AffaStars", "affastars"); }
  Serial.printf("console: http://%s/\n",
                (sta ? WiFi.localIP() : WiFi.softAPIP()).toString().c_str());
}

void loadCfg() {
  Preferences p;
  if (!p.begin(kCfgNs, true)) return;
  g_stars   = p.getUChar("n", g_stars);
  g_zStep   = p.getUShort("step", g_zStep);
  g_frameMs = p.getUInt("frame", g_frameMs);
  p.end();
  if (g_stars > kStarsMax) g_stars = kStarsMax;
  if (g_stars < kStarsMin) g_stars = kStarsMin;
}

void saveCfg() {
  Preferences p;
  if (!p.begin(kCfgNs, false)) return;
  p.putUChar("n", g_stars);
  p.putUShort("step", g_zStep);
  p.putUInt("frame", g_frameMs);
  p.end();
}

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
    if (r->hasParam("n") || r->hasParam("step") || r->hasParam("frame")) {
      const uint8_t was = g_stars;
      g_stars   = clampArg<uint8_t>(r, "n", g_stars, kStarsMin, kStarsMax);
      g_zStep   = clampArg<uint16_t>(r, "step", g_zStep, kStepMin, kStepMax);
      g_frameMs = clampArg<uint32_t>(r, "frame", g_frameMs, kFrameMin, kFrameMax);
      // Stars that were never in the field carry uninitialised depth. Spawn only the NEW
      // ones, scattered, so raising the count does not restart the whole field.
      for (uint8_t i = was; i < g_stars; ++i) spawn(g_field[i], /*anywhere=*/true);
      saveCfg();
      Serial.printf("[%8lu] ** config: %u stars, step %u, frame %lu ms\n",
                    static_cast<unsigned long>(millis()), static_cast<unsigned>(g_stars),
                    static_cast<unsigned>(g_zStep), static_cast<unsigned long>(g_frameMs));
    }
    char b[1500];
    snprintf(b, sizeof(b),
             "<!doctype html><meta charset=utf-8><meta http-equiv=refresh content=5>"
             "<title>AffaDisplay starfield</title>"
             "<body style='background:#000;color:#9f9;font:14px ui-monospace,monospace'>"
             "<pre>up %lus   phase %s   frames %lu\n"
             "screens ok %lu  failed %lu\n"
             "driver txErr %lu rxErr %lu busErr %lu\n\n"
             "+--------------------+\n|%s|\n|%s|\n|%s|\n+--------------------+</pre>"
             "<form><table>"
             "<tr><td>stars</td><td><input name=n value=%u size=4></td><td>%u..%u</td></tr>"
             "<tr><td>speed (z per frame)</td><td><input name=step value=%u size=5></td>"
             "<td>%u..%u</td></tr>"
             "<tr><td>frame (ms)</td><td><input name=frame value=%lu size=6></td>"
             "<td>%lu..%lu</td></tr></table><button>apply</button></form>",
             static_cast<unsigned long>(millis() / 1000),
             affa::phaseName(g_display.phase()),
             static_cast<unsigned long>(g_frames),
             static_cast<unsigned long>(g_ok), static_cast<unsigned long>(g_fail),
             static_cast<unsigned long>(g_link.driver().txErr),
             static_cast<unsigned long>(g_link.driver().rxErr),
             static_cast<unsigned long>(g_link.driver().busErr),
             g_row[0], g_row[1], g_row[2],
             static_cast<unsigned>(g_stars),
             static_cast<unsigned>(kStarsMin), static_cast<unsigned>(kStarsMax),
             static_cast<unsigned>(g_zStep),
             static_cast<unsigned>(kStepMin), static_cast<unsigned>(kStepMax),
             static_cast<unsigned long>(g_frameMs),
             static_cast<unsigned long>(kFrameMin), static_cast<unsigned long>(kFrameMax));
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
  Serial.println();
  Serial.println("=== 13_starfield — perspective 3D on 20x3 ===");

  if (!g_link.begin(kRxPin, kTxPin, kBitrate))
    Serial.println("!! the CAN link did not come up");

  g_display.onComplete(&onDone, nullptr);
  g_display.begin();

  loadCfg();
  for (uint8_t i = 0; i < kMaxStars; ++i) spawn(g_field[i], /*anywhere=*/true);
  Serial.printf("%u stars, z step %u, frame %lu ms\n", static_cast<unsigned>(g_stars),
                static_cast<unsigned>(g_zStep), static_cast<unsigned long>(g_frameMs));

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

  // Ready means registered AND lit — the library powers the panel itself. Nothing is drawn
  // before that, and one fullscreen is in flight at a time: two in a row would COALESCE and
  // the dropped one is a skipped animation frame rather than an error.
  if (g_display.phase() != affa::Phase::Ready || g_busy) return;
  if (static_cast<int32_t>(now - g_nextFrameAt) < 0) return;

  stepField();
  if (g_display.showFullscreenText(g_row[0], g_row[1], g_row[2]) == affa::Result::Ok) {
    g_busy = true;
    ++g_frames;
    g_nextFrameAt = now + g_frameMs;
  } else {
    g_nextFrameAt = now + 100;
  }

  static uint32_t s_nextLog = 0;
  if (static_cast<int32_t>(now - s_nextLog) >= 0) {
    s_nextLog = now + 10000;
    Serial.printf("[%8lu] |%s|%s|%s|  frames %lu ok %lu fail %lu\n",
                  static_cast<unsigned long>(now), g_row[0], g_row[1], g_row[2],
                  static_cast<unsigned long>(g_frames), static_cast<unsigned long>(g_ok),
                  static_cast<unsigned long>(g_fail));
  }
}
