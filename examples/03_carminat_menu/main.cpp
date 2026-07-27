// 03_carminat_menu — the two-row menu, the highlight, and keys arriving from the panel.
//
// Two things are demonstrated side by side, and they are NOT the same thing:
//
//   * the library's Menu — the application supplies items and fields, and the panel's
//     wheel drives it. Menu::render() is what calls showMenu() + highlightItem(); the
//     application never does. Hold-Load opens it (the OEM gesture, and a replaceable
//     default), the wheel scrolls, Load enters edit, hold-Load leaves.
//   * showMenu() / highlightItem() called DIRECTLY, on VolUp, to draw a screen that is
//     not a menu at all. Same wire operation, no state machine attached.
//
// Popups and the fullscreen screen are behind supports(). Every one of them is a compile
// gate (AFFA_ENABLE_POPUP, AFFA_ENABLE_FULLSCREEN), so on a size-reduced build the call
// returns NotSupported instead of silently doing nothing — ask first.

#include <Arduino.h>
#include <AffaDisplay.h>

#if !AFFA_PANEL_CARMINAT || !AFFA_ENABLE_MENU
#  error "03_carminat_menu needs -D AFFA_PANEL_CARMINAT=1 -D AFFA_ENABLE_MENU=1"
#endif

namespace {

struct ArduinoClock final : affa::IClock {
  uint32_t millis() const override { return ::millis(); }
};

constexpr affa::CanPins kPins{ .rx = GPIO_NUM_4, .tx = GPIO_NUM_3 };

affa::Esp32CanLink    g_link;
ArduinoClock          g_clock;
affa::CarminatDisplay g_display(g_link, g_clock);

// Item content is CALLER-OWNED and pointed at, never copied: these must outlive the Menu,
// which is why they are file-scope constants and not stack strings.
const char* const kBtModes[] = { "OFF", "AUTO", "ON" };

uint8_t  g_manualRow = 0;      // which row the DIRECT showMenu demo highlights
uint32_t g_nextVoltsMs = 0;

// Fired after a field changed, from the task that called poll(). NVS writes belong here —
// persistence is the application's, which is why the library never touches Preferences.
void onBrightness(const affa::MenuItem& it, uint8_t field, void*) {
  Serial.printf("[menu] brightness -> %ld%s\n",
                static_cast<long>(it.fields[field].value),
                it.fields[field].unit ? it.fields[field].unit : "");
}

void onBtMode(const affa::MenuItem& it, uint8_t field, void*) {
  const affa::Field& f = it.fields[field];
  Serial.printf("[menu] bluetooth -> %s\n", f.list[f.value]);
}

void onAbout(void*) { Serial.println("[menu] About activated (onActivate, not edit)"); }

void buildMenu() {
  affa::Menu& m = g_display.getMenu();
  m.setHeader("SETTINGS");

  affa::MenuItem bright{};
  bright.label      = "Bright";
  bright.fields[0]  = affa::integerField(50, 0, 100, /*step*/1, /*stepMultiplier*/10, "%");
  bright.fieldCount = 1;
  bright.onChange   = &onBrightness;
  m.addItem(bright);

  affa::MenuItem bt{};
  bt.label      = "BT";
  bt.fields[0]  = affa::listField(kBtModes, 3, /*index*/1);
  bt.fieldCount = 1;
  bt.onChange   = &onBtMode;
  m.addItem(bt);

  affa::MenuItem volts{};
  volts.label      = "Battery";
  volts.fields[0]  = affa::readOnlyField(138, "dV");   // a live reading: never edited
  volts.fieldCount = 1;
  volts.editable   = false;
  m.addItem(volts);

  affa::MenuItem about{};
  about.label      = "About";
  about.fieldCount = 0;
  about.onActivate = &onAbout;
  m.addItem(about);

  Serial.printf("[menu] %u items, hotkey = hold-Load\n", m.count());
}

// Keys the menu did NOT consume reach here. While the menu is open it consumes the wheel
// and Load; VolUp/VolDown/Pause/SrcNext reach the application either way, because the
// menu has no opinion about them.
void onKey(affa::Key k, affa::KeyEdge e, void*) {
  Serial.printf("[key ] 0x%04X %s (menu %s)\n", static_cast<unsigned>(k),
                e == affa::KeyEdge::Hold ? "hold" : "click",
                g_display.getMenu().isOpen() ? "open" : "closed");

  switch (k) {
    case affa::Key::VolUp: {   // showMenu() + highlightItem() with no Menu behind them.
      // Guarded on the menu being SHUT, deliberately: VolUp reaches the application even
      // while the menu is open (the menu has no opinion about it), and drawing over the
      // menu's screen would leave the Menu's idea of the glass and the glass disagreeing.
      if (g_display.getMenu().isOpen()) break;
      g_manualRow ^= 1;
      (void)g_display.showMenu("DIRECT", "row zero", "row one", affa::carminat::kScrollBoth);
      (void)g_display.highlightItem(g_manualRow);   // a DIFFERENT single frame, not a redraw
      break;
    }
    case affa::Key::Pause:
      if (g_display.supports(affa::Feature::Popup)) (void)g_display.showPopupText("VOL 28");
      else Serial.println("[app ] popup not compiled in");
      break;
    case affa::Key::VolDown:
      if (g_display.supports(affa::Feature::Popup)) (void)g_display.hidePopup();
      break;
    case affa::Key::SrcNext:
      if (g_display.supports(affa::Feature::Fullscreen))
        (void)g_display.showFullscreenText("AffaDisplay", "fullscreen", "demo");
      else Serial.println("[app ] fullscreen not compiled in");
      break;
    case affa::Key::SrcPrev:
      if (g_display.supports(affa::Feature::Fullscreen)) (void)g_display.hideFullscreenText();
      break;
    default: break;
  }
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(200);
  if (!g_link.begin(kPins, 500000)) { Serial.println("CAN did not come up"); return; }

  g_display.onKey(&onKey, nullptr);
  g_display.begin();
  buildMenu();                       // AFTER begin(); the menu renders when it opens
  (void)g_display.setText("RENAULT");
}

void loop() {
  g_display.poll();

  // A live reading pushed into a menu field from outside. It re-renders ONLY if that item
  // is in the visible two-row window — which is why this is safe to call at 1 Hz.
  const uint32_t now = ::millis();
  if (affa::expired(now, g_nextVoltsMs)) {
    g_nextVoltsMs = now + 1000;
    g_display.getMenu().setFieldValue(/*item*/2, /*field*/0, 130 + (now / 1000) % 12);
  }
}
