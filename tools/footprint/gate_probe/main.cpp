// The gate-measurement probe. NOT an example and never flashed to a panel — it is the
// instrument behind the README's "what each gate is actually worth" table, driven by
// platformio_footprint.ini (see tools/footprint/README.md). It lives outside src/, so the
// Library Dependency Finder never pulls it into a consumer's build.
//
// Instantiates every panel the build selected and calls every optional render, so that
// --gc-sections cannot remove a feature the gate was supposed to remove. Flipping one
// AFFA_* gate and re-measuring therefore reports the real flash cost of that gate.
#include <Arduino.h>
#include <AffaDisplay.h>

namespace {

struct ArduinoClock final : affa::IClock {
  uint32_t millis() const override { return ::millis(); }
};

affa::LoopbackLink<32> g_link;
ArduinoClock           g_clock;

volatile uint32_t g_sink = 0;

void bite(affa::Result r) { g_sink += static_cast<uint32_t>(r); }
void bite(bool b)         { g_sink += b ? 1u : 0u; }

void exercise(affa::AffaDisplayBase& d) {
  d.begin();
  d.poll();
  bite(d.setText("HELLO", 0));
  bite(d.setTime("1234"));
  bite(d.setPower(true));
  bite(d.showMenu("HDR", "row0", "row1"));
  bite(d.highlightItem(1));
  bite(d.showPopupText("VOL 28"));
  bite(d.hidePopup());
  bite(d.showFullscreenText("a", "b", "c"));
  bite(d.hideFullscreenText());
  bite(d.showConfirmBox("CAP", "r0", "r1"));
  bite(d.showInfoPopup("AUX", "AUTO", "SPEED"));
  bite(d.hideInfoPopup());
  bite(d.pressKey(affa::Key::Load, affa::KeyEdge::Click));
  bite(d.pressKey(affa::Key::Load, affa::KeyEdge::Click, affa::KeySource::Wire));
  bite(d.nav(affa::NavCommand::Next));
  bite(d.supports(affa::Feature::Menu));
  bite(d.synced());
  g_sink += d.abortPending();
  bite(d.abortAll());
  bite(d.pending(affa::RenderSlot::Text));
  affa::FrameMatch m{};
  m.id = 0x151;
  bite(d.subscribe(m, nullptr, nullptr).valid());
  g_sink += d.subscriptions();
  g_sink += d.stats().rxFrames;
  g_sink += d.queued();
  d.setPassive(true);
  d.setSelfAck(true);
  bite(d.passive());
}

#if AFFA_PANEL_CARMINAT
affa::CarminatDisplay g_carminat(g_link, g_clock);
const char* const     kList[] = {"OFF", "ON"};
#endif
#if AFFA_PANEL_UPDATELIST
affa::UpdateListDisplay g_seg(g_link, g_clock);
#endif
#if AFFA_PANEL_UPDATELIST_MENU
affa::UpdateListMenuDisplay g_lcd(g_link, g_clock);
#endif

}  // namespace

void setup() {
  Serial.begin(115200);
#if AFFA_PANEL_CARMINAT
  exercise(g_carminat);
  bite(g_carminat.showInfoMenu("a", "b", "c"));
#  if AFFA_ENABLE_MENU
  affa::Menu& mn = g_carminat.getMenu();
  affa::MenuItem it{};
  it.label      = "Item";
  it.fields[0]  = affa::integerField(3, 0, 10);
  it.fields[1]  = affa::listField(kList, 2, 0);
  it.fields[2]  = affa::readOnlyField(7, "V");
  it.fieldCount = 3;
  g_sink += static_cast<uint32_t>(mn.addItem(it));
  mn.setHeader("H");
  mn.open();
  // The model has no key vocabulary any more; the six intents are what the key map calls.
  bite(mn.next());
  bite(mn.prev());
  bite(mn.increase());
  bite(mn.decrease());
  bite(mn.select());
  bite(mn.setFieldValue(0, 0, 5));
  mn.render();
  bite(g_carminat.menuRenderer().lastResult());
  bite(mn.back());
  mn.close();
  mn.clear();
  g_sink += mn.count();
  g_carminat.pushPage(nullptr);
  g_carminat.popPage();
  bite(g_carminat.currentPage() != nullptr);
  g_carminat.setMenuHotkey(affa::Key::Pause, affa::KeyEdge::Hold);
  g_carminat.clearMenuHotkey();
#  endif
#endif
#if AFFA_PANEL_UPDATELIST
  exercise(g_seg);
  g_seg.setScrollText("A LONG TITLE TO SCROLL");
  g_seg.setScrollActive(true);
  g_seg.reassert();
  bite(g_seg.scrollActive());
#endif
#if AFFA_PANEL_UPDATELIST_MENU
  exercise(g_lcd);
#endif
#if AFFA_ENABLE_ESP32CAN_LINK
  static affa::Esp32CanLink real;
  bite(real.begin(affa::CanPins{.rx = GPIO_NUM_4, .tx = GPIO_NUM_3}, 500000));
  bite(real.isLive());
#endif
  // The vpanel twin probe was here. src/vpanel/ is gone; what AFFA_ENABLE_ISOTP_RX buys
  // now is the reassembler, the screen decoder and the onText() path, and touching
  // onText() is what keeps that whole chain out of --gc-sections' reach.
#if AFFA_ENABLE_ISOTP_RX && AFFA_PANEL_CARMINAT
  g_carminat.onText([](const char* t, void*) { g_sink += static_cast<uint32_t>(t[0]); },
                    nullptr);
  affa::Frame tf{};
  tf.id  = 0x151;
  tf.len = 8;
  tf.data[0] = 0x10;
  tf.data[1] = 0x0E;
  tf.data[2] = 0x77;
  g_sink += static_cast<uint32_t>(affa::isotp::frameCount(tf.data[1]));
#endif
  char buf[16];
  g_sink += static_cast<uint32_t>(affa::toAscii("\xC3\x84\xC3\x96", buf, sizeof(buf)));
  Serial.println(static_cast<unsigned>(g_sink));
}

void loop() {
#if AFFA_PANEL_CARMINAT
  g_carminat.poll();
#endif
#if AFFA_PANEL_UPDATELIST
  g_seg.poll();
#endif
#if AFFA_PANEL_UPDATELIST_MENU
  g_lcd.poll();
#endif
}
