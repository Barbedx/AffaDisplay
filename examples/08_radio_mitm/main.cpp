// 08_radio_mitm — the seam test. Three things that used to be hard-wired inside the panel
// class, reimplemented ENTIRELY in application code against the public API:
//
//   a) the radio password sequence — subscribe(FrameMatch) + pressKey(..., KeySource::Wire)
//   b) AUX-source detection       — EventKind::RadioText, a dozen lines
//   c) a custom menu-open gesture — clearMenuHotkey() + nav(NavCommand::Open)
//
// None of this is in the library, none of it needs to be, and if any of it had been
// awkward to write the seam would be cut in the wrong place.
//
// TOPOLOGY, because none of it makes sense otherwise: the joystick is physically part of
// the PANEL. Pressing it makes the panel encode and transmit a key frame; the radio
// receives it and draws the next screen. Here we are impersonating the panel at a REAL
// radio, which is the one situation KeySource::Wire exists for.
//
// KeySource::Wire PUTS PHANTOM BUTTON PRESSES ON THE BUS. Harmless on a bench. In a car it
// is injecting input other modules may act on — do not ship this unless you know precisely
// which module is listening and what it will do.

#include <Arduino.h>
#include <AffaDisplay.h>
#include <cstring>

#if !AFFA_PANEL_CARMINAT || !AFFA_ENABLE_MENU || AFFA_MAX_SUBSCRIPTIONS < 1
#  error "08_radio_mitm needs AFFA_PANEL_CARMINAT=1, AFFA_ENABLE_MENU=1, subscriptions > 0"
#endif

namespace {

struct ArduinoClock final : affa::IClock {
  uint32_t millis() const override { return ::millis(); }
};

constexpr affa::CanPins kPins{ .rx = GPIO_NUM_4, .tx = GPIO_NUM_3 };

affa::Esp32CanLink    g_link;
ArduinoClock          g_clock;
affa::CarminatDisplay g_display(g_link, g_clock);

// ---------------------------------------------------------------------------
// (a) The radio password sequence
// ---------------------------------------------------------------------------
// The PIN and its pacing are RADIO policy: the library has no opinion about either, which
// is precisely why this is an example and not a feature. The extracted version was
// delay(1000) followed by a delay(200) after every press, inside the receive path.
constexpr uint8_t  kCode[]    = {5, 3, 2, 1};   // RollUp x5, Load, x3, Load, x2, Load, x1,
constexpr uint32_t kArmMs     = 1000;           // hold-Load — the last one is a HOLD
constexpr uint32_t kSpacingMs = 200;

// The trigger: the second ISO-TP frame of the radio's PIN screen, seen on the bench as
// exactly `21 20 20 B0 30 30 30 20` on 0x151 — two spaces, the masked-digit glyph, `000 `.
constexpr uint8_t kPrompt[8] = {0x21, 0x20, 0x20, 0xB0, 0x30, 0x30, 0x30, 0x20};

class PinEntry {
 public:
  PinEntry(affa::AffaDisplayBase& d, affa::IClock& c) : _d(d), _c(c) {}

  // Layer 1 callback, fired from inside poll(). It ARMS and returns — it does not press.
  // A callback that waited here would be the old delay(1000) with extra steps, and it
  // would stall the very poll() that has to deliver the ACKs for what it sends.
  static void onPrompt(const affa::Frame&, void* ctx) {
    auto* self = static_cast<PinEntry*>(ctx);
    if (!self->_done) return;                    // already running
    Serial.println("[mitm] PIN prompt matched — arming");
    self->_done   = false;
    self->_digit  = 0;
    self->_detent = 0;
    self->_nextMs = self->_c.millis() + kArmMs;  // the radio is still drawing
  }

  // Called from loop(), beside poll(). One press per due deadline, and nothing else.
  void tick() {
    if (_done) return;
    if (!affa::expired(_c.millis(), _nextMs)) return;
    _nextMs = _c.millis() + kSpacingMs;

    if (_detent < kCode[_digit]) {                              // one wheel detent
      (void)_d.pressKey(affa::Key::RollUp, affa::KeyEdge::Click, affa::KeySource::Wire);
      ++_detent;
      return;
    }
    const bool last = (_digit + 1u == sizeof kCode);             // the final Load is a HOLD
    (void)_d.pressKey(affa::Key::Load, last ? affa::KeyEdge::Hold : affa::KeyEdge::Click,
                affa::KeySource::Wire);
    _detent = 0;
    if (last) { _done = true; Serial.println("[mitm] sequence complete"); return; }
    ++_digit;
  }

 private:
  affa::AffaDisplayBase& _d;
  affa::IClock&          _c;
  uint32_t _nextMs = 0;
  uint8_t  _digit  = 0, _detent = 0;
  bool     _done   = true;
};

PinEntry g_pin(g_display, g_clock);

// ---------------------------------------------------------------------------
// (b) AUX-source detection, in a dozen lines
// ---------------------------------------------------------------------------
bool g_inAux = false;

void onEvent(const affa::Event& ev, void*) {
  if (ev.kind != affa::EventKind::RadioText) return;
  // ev.text.text is NUL-terminated and already transliterated; ev.text.raw is the
  // reassembled payload for byte-level heuristics. BOTH DIE WHEN THIS RETURNS.
  const bool aux = (std::strstr(ev.text.text, "AUX") != nullptr);
  if (aux == g_inAux) return;
  g_inAux = aux;
  Serial.printf("[aux ] source is now %s (\"%s\")\n", aux ? "AUX" : "not AUX",
                ev.text.text);
}

// ---------------------------------------------------------------------------
// (c) A custom menu-open gesture: double-click Load
// ---------------------------------------------------------------------------
// clearMenuHotkey() first, and it is not optional: with the default hotkey live, hold-Load
// would still open the menu behind our back — and hold-Load is what the PIN sequence ends
// with. A closed menu consumes nothing, so every key reaches this callback while it is
// shut, which is what makes an application-defined gesture possible at all.
uint32_t g_lastLoadMs = 0;

void onKey(affa::Key k, affa::KeyEdge e, void*) {
  if (k == affa::Key::Load && e == affa::KeyEdge::Click && !g_display.getMenu().isOpen()) {
    const uint32_t now = ::millis();
    if (now - g_lastLoadMs < 400) {
      Serial.println("[app ] double-click -> nav(Open)");
      (void)g_display.nav(affa::NavCommand::Open);     // from the task that owns poll()
    }
    g_lastLoadMs = now;
    return;
  }
  Serial.printf("[key ] 0x%04X %s\n", static_cast<unsigned>(k),
                e == affa::KeyEdge::Hold ? "hold" : "click");
}

void buildMenu() {
  affa::Menu& m = g_display.getMenu();
  m.setHeader("MITM");
  affa::MenuItem aux{};
  aux.label      = "AUX";
  aux.fields[0]  = affa::readOnlyField(0);
  aux.fieldCount = 1;
  aux.editable   = false;
  m.addItem(aux);                                // an EMPTY menu never opens
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(200);
  if (!g_link.begin(kPins, 500000)) { Serial.println("CAN did not come up"); return; }

  affa::FrameMatch m{};
  m.id     = 0x151;
  m.idMask = 0x7FF;
  m.dir    = affa::Direction::Rx;          // what the RADIO sent, never our own echo
  memcpy(m.data,     kPrompt, 8);
  memset(m.dataMask, 0xFF,    8);          // all eight bytes must match exactly
  m.len    = 8;
  if (!g_display.subscribe(m, &PinEntry::onPrompt, &g_pin).valid())
    Serial.println("[mitm] subscription table full");   // never ignore this

  g_display.onEvent(&onEvent, nullptr);
  g_display.onKey(&onKey, nullptr);
  g_display.clearMenuHotkey();             // (c): nav(Open) becomes the only way in
  g_display.begin();
  buildMenu();

  // Honest reporting rather than a promise: (b) is written against the event the library
  // publishes, and that event needs the reassembler. With AFFA_ENABLE_ISOTP_RX=0 — the
  // default on target — supports() answers false and onEvent() will simply never see a
  // RadioText. (a) and (c) do not depend on it.
  Serial.printf("RadioText supported=%d (ISOTP_RX=%d), menu hotkey cleared\n",
                g_display.supports(affa::Feature::RadioText), AFFA_ENABLE_ISOTP_RX);
}

void loop() {
  g_display.poll();
  g_pin.tick();        // the only "timer" involved, and it belongs to the application
}
