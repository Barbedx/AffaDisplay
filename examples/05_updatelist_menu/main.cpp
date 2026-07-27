// 05_updatelist_menu — a menu on the mono LCD variant of the AFFA2 panel.
//
// THE LIBRARY HAS NO MENU FOR THIS FAMILY, and supports(Feature::Menu) says so. The
// Carminat Menu class is a two-row sliding window over a 96-byte screen payload; this
// panel has one text field and no window geometry, so a menu here is not a wire operation
// at all — it is a list the APPLICATION keeps and draws one line at a time through
// setText(). That is the boundary principle deciding a case where the honest answer is
// "not ours": rendering geometry the panel does not have cannot be library-owned.
//
// Everything else IS the library's: the 0x121 `10 1C 7F ..` LCD encoding, the handshake,
// the key decode on 0x0A9, the 0x4A9 auto-ACK (COMPUTED — 0x0A9 | 0x400 is 0x4A9, not
// 0x5A9), and the marquee, which this file borrows to scroll a label too long for the
// screen.
//
// clearAmsHotkey() is called deliberately: hold-Load is the family's AMS toggle by
// default, and this application wants that gesture for "go back" instead. Replacing a
// default is public API; the default existing at all is the OEM convention.

#include <Arduino.h>
#include <AffaDisplay.h>
#include <cstring>

#if !AFFA_PANEL_UPDATELIST_MENU
#  error "05_updatelist_menu needs -D AFFA_PANEL_UPDATELIST_MENU=1"
#endif

namespace {

struct ArduinoClock final : affa::IClock {
  uint32_t millis() const override { return ::millis(); }
};

constexpr affa::CanPins kPins{ .rx = GPIO_NUM_4, .tx = GPIO_NUM_3 };

affa::Esp32CanLink          g_link;
ArduinoClock                g_clock;
affa::UpdateListMenuDisplay g_display(g_link, g_clock);

// The application's list. Fixed, file-scope, caller-owned — no heap, exactly as the
// library's own containers are.
struct Entry { const char* label; const char* const* values; uint8_t count; uint8_t index; };
const char* const kOnOff[]  = { "OFF", "ON" };
const char* const kSources[] = { "TUNER", "CD", "AUX", "BT" };
const char* const kLevels[] = { "LOW", "MID", "HIGH" };

Entry g_items[] = {
  { "SOURCE", kSources, 4, 2 },
  { "LOUD",   kOnOff,   2, 1 },
  { "BASS",   kLevels,  3, 1 },
  { "AMS",    kOnOff,   2, 1 },
};
constexpr uint8_t kItemCount = sizeof(g_items) / sizeof(g_items[0]);

uint8_t g_sel     = 0;
bool    g_editing = false;

// One line of "menu": "LABEL VALUE", scrolled only if it does not fit the eight cells.
// The marquee is the library's — reusing it is why this file has no scroll code at all.
void draw() {
  char line[32];
  snprintf(line, sizeof(line), "%s%s %s%s", g_editing ? "*" : "",
           g_items[g_sel].label, g_items[g_sel].values[g_items[g_sel].index],
           g_editing ? "<" : "");

  if (std::strlen(line) <= affa::updatelist::kScrollWidth) {
    g_display.setScrollText(nullptr);   // marquee off: transmits NOTHING, so the static
    g_display.setText(line);            // render below owns the screen
  } else {
    g_display.setScrollText(line);      // new content -> redraws from position 0
    g_display.setScrollActive(true);
  }
  Serial.printf("[lcd ] %s\n", line);
}

void onKey(affa::Key k, affa::KeyEdge e, void*) {
  switch (k) {
    case affa::Key::RollUp:
      if (g_editing) {
        Entry& it = g_items[g_sel];
        it.index = static_cast<uint8_t>((it.index + it.count - 1) % it.count);
      } else {
        g_sel = static_cast<uint8_t>((g_sel + kItemCount - 1) % kItemCount);
      }
      break;

    case affa::Key::RollDown:
      if (g_editing) {
        Entry& it = g_items[g_sel];
        it.index = static_cast<uint8_t>((it.index + 1) % it.count);
      } else {
        g_sel = static_cast<uint8_t>((g_sel + 1) % kItemCount);
      }
      break;

    case affa::Key::Load:
      if (e == affa::KeyEdge::Hold) {          // our own "back", because we took the
        g_editing = false;                     // gesture off the AMS toggle in setup()
        Serial.println("[app ] back");
      } else {
        g_editing = !g_editing;
        if (!g_editing && g_sel == 3)          // the one item that changes library state
          g_display.setAmsKeysEnabled(g_items[3].index != 0);
      }
      break;

    default:
      return;                                   // nothing to redraw
  }
  draw();
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(200);
  if (!g_link.begin(kPins, 500000)) { Serial.println("CAN did not come up"); return; }

  // Ask, do not assume. Menu=0 here and 1 on Carminat, from the identical call.
  Serial.printf("supports Text=%d Menu=%d Popup=%d Power=%d KeyTx=%d\n",
                g_display.supports(affa::Feature::Text),
                g_display.supports(affa::Feature::Menu),
                g_display.supports(affa::Feature::Popup),
                g_display.supports(affa::Feature::Power),
                g_display.supports(affa::Feature::KeyTx));

  // Take hold-Load for the application. Without this the panel would toggle AMS key
  // forwarding on the same gesture — and while forwarding is off, no key reaches onKey().
  g_display.clearAmsHotkey();

  g_display.onKey(&onKey, nullptr);
  g_display.begin();
  g_display.setPower(true);
  draw();
}

void loop() {
  g_display.poll();     // the marquee step and every deadline live in here
}
