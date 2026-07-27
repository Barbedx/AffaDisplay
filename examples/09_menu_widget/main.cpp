// 09_menu_widget — one menu, three displays.
//
// The same MenuModel, the same three items, the same key stream, rendered in turn onto:
//
//   1. the Carminat two-row menu screen   (2 x 26, showMenu + highlightItem)
//   2. the Carminat info-row screen       (3 x 8,  showInfoPopup, no highlight, no arrows)
//   3. a serial "OLED"                    (6 x 20, no AFFA protocol at all)
//
// Nothing in src/widget/ changes between them. What changes is a MenuGeometry and about
// twenty lines of adapter — which is the claim this example exists to make good on.
//
// WHY THE MODEL IS RE-CONSTRUCTED WHEN THE DISPLAY CHANGES. The geometry is injected at
// construction and is deliberately immutable: rows is the loop bound and the divisor of the
// window arithmetic, and a window cannot change height under a live selection without a rule
// for where the selection lands. Switching displays therefore destroys and rebuilds the model
// in a STATIC buffer — placement new, no heap, exactly the constraint src/widget/ holds
// itself to — and the item table is rebuilt by the same buildItems() that built it the first
// time. The selection resets to the top, which is why the scripted key stream restarts with
// it and every display gets the identical sequence.
//
// CarminatDisplay'S OWN MENU IS THE SAME MODEL. Since the migration there is no second menu
// implementation anywhere: getMenu() hands out a widget::MenuModel driven through
// affa::CarminatMenuRenderer, which is the very adapter this example uses for target 1. What
// this file adds is a SECOND, application-owned model on the same panel — so setup() clears
// the hotkey, the library's (empty) menu stays out of the way, and every key arrives in
// onKey() to be routed into OUR model.

#include <Arduino.h>
#include <AffaDisplay.h>
#include <widget/MenuModel.h>
#include <new>

// The Carminat adapter is NOT in this folder: it ships in the library, as
// src/carminat/CarminatMenuRenderer.h, because CarminatDisplay's own menu is built on it.
// Copying it here so the example could own one would be the second implementation this
// library just finished deleting. The other two adapters below stay local — they are what
// writing your own looks like.
#include "InfoMenuRenderer.h"
#include "TextPanelRenderer.h"

#if !AFFA_PANEL_CARMINAT || !AFFA_ENABLE_MENU
#  error "09_menu_widget needs -D AFFA_PANEL_CARMINAT=1 -D AFFA_ENABLE_MENU=1"
#endif

namespace {

using affa::CarminatMenuRenderer;
using affa::widget::MenuGeometry;
using affa::widget::MenuItem;
using affa::widget::MenuModel;

struct ArduinoClock final : affa::IClock {
  uint32_t millis() const override { return ::millis(); }
};

constexpr affa::CanPins kPins{ .rx = GPIO_NUM_4, .tx = GPIO_NUM_3 };

affa::Esp32CanLink    g_link;
ArduinoClock          g_clock;
affa::CarminatDisplay g_display(g_link, g_clock);

CarminatMenuRenderer g_carminat(g_display);
InfoMenuRenderer     g_info(g_display);
TextPanelRenderer    g_oled;

// ---------------------------------------------------------------------------
// The content. Caller-owned and pointed at, never copied — file scope, not stack.
// ---------------------------------------------------------------------------
const char* const kBtModes[]    = { "OFF", "AUTO", "ON" };
const char* const kClockFormat[] = { "12h", "24h" };

void onBrightness(const MenuItem& it, uint8_t field, void*) {
  Serial.printf("      onChange: brightness -> %ld%s\n",
                static_cast<long>(it.fields[field].value),
                it.fields[field].unit ? it.fields[field].unit : "");
}

void onBtMode(const MenuItem& it, uint8_t field, void*) {
  const affa::widget::Field& f = it.fields[field];
  Serial.printf("      onChange: bluetooth -> %s\n", f.list[f.value]);
}

// One hook, both roles: the item arrives by reference and the field that moved arrives as an
// index, so "the clock item changed" and "field 1 of the clock item changed" are the same
// callback. An NVS write goes here — persistence is the application's.
void onClock(const MenuItem& it, uint8_t field, void*) {
  Serial.printf("      onChange: clock field %u -> %ld\n", static_cast<unsigned>(field),
                static_cast<long>(it.fields[field].value));
}

// The three-item demo, rebuilt into whichever model is live. An integer field, a list field,
// and an item with three fields so Select can walk 0 -> 1 -> 2 -> out.
void buildItems(MenuModel& m) {
  MenuItem bright{};
  bright.label      = "Bright";
  bright.fields[0]  = affa::widget::integerField(50, 0, 100, /*step*/1, /*coarse*/10, "%");
  bright.fieldCount = 1;
  bright.onChange   = &onBrightness;
  m.addItem(bright);

  MenuItem bt{};
  bt.label      = "BT";
  bt.fields[0]  = affa::widget::listField(kBtModes, 3, /*index*/1);
  bt.fieldCount = 1;
  bt.onChange   = &onBtMode;
  m.addItem(bt);

  MenuItem clock{};
  clock.label      = "Clock";
  clock.fields[0]  = affa::widget::integerField(12, 0, 23, 1, 6);
  clock.fields[1]  = affa::widget::integerField(30, 0, 59, 1, 15);
  clock.fields[2]  = affa::widget::listField(kClockFormat, 2, /*index*/1);
  clock.fieldCount = 3;
  clock.onChange   = &onClock;
  m.addItem(clock);
}

// ---------------------------------------------------------------------------
// The model, and the display it is currently pointed at
// ---------------------------------------------------------------------------
void onMenuClosed(void*);   // defined with the key routing, below

struct Target {
  const char*                   name;
  affa::widget::IMenuRenderer*  renderer;
  MenuGeometry                  geom;
};

const Target kTargets[] = {
  { "carminat", &g_carminat, CarminatMenuRenderer::geometry() },   // 2 x 26
  { "info-row", &g_info,     InfoMenuRenderer::geometry()     },   // 3 x 8
  { "oled",     &g_oled,     TextPanelRenderer::geometry()    },   // 6 x 20
};
constexpr uint8_t kTargetCount = sizeof(kTargets) / sizeof(kTargets[0]);

alignas(MenuModel) uint8_t g_modelStore[sizeof(MenuModel)];
MenuModel* g_model  = nullptr;
uint8_t    g_target = 0;

void useTarget(uint8_t i) {
  g_target = i;
  const Target& t = kTargets[i];

  if (g_model) g_model->~MenuModel();
  g_model = new (g_modelStore) MenuModel(*t.renderer, t.geom, "SETTINGS");
  buildItems(*g_model);
  g_model->onClose(&onMenuClosed, nullptr);   // re-registered: this is a NEW model

  Serial.printf("\n=== %s: %u rows x %u chars, wrap %s =========================\n",
                t.name, static_cast<unsigned>(t.geom.rows),
                static_cast<unsigned>(t.geom.rowChars), t.geom.wrap ? "on" : "off");
  g_model->open();          // the first frame goes out from here
}

// ---------------------------------------------------------------------------
// The scripted key stream — the same one for every display, so the difference on the glass
// is the geometry and nothing else. On a bench with no panel wheel this is what drives it.
// ---------------------------------------------------------------------------
enum class Step : uint8_t { Next, Prev, Select, Increase, Decrease };

const Step kScript[] = {
  Step::Next,                     // Bright -> BT
  Step::Next,                     // BT -> Clock. On 2 rows the window SCROLLS here and the
                                  // mask goes 0x0B -> 0x07; on 3 and 6 rows the whole list
                                  // fits, the mask stays 0x00 and only the highlight moves.
  Step::Select,                   // enter edit on field 0 (hours)
  Step::Increase,                 // coarse step, stepMultiplier 6: 12 -> 18
  Step::Increase,                 // 18 -> 23 and CLAMPED at maxValue, not 24
  Step::Select,                   // field 0 -> field 1 (minutes)
  Step::Decrease,                 // coarse step 15: 30 -> 15
  Step::Select,                   // field 1 -> field 2 (the list)
  Step::Prev,                     // 24h -> 12h
  Step::Next,                     // 12h -> 24h: the LAST list entry is reachable coming back,
                                  // which is the clamp bug the extracted code had
  Step::Select,                   // last field -> leave edit (no wrap to field 0)
  Step::Prev, Step::Prev,         // back up to Bright
  Step::Select,                   // enter edit
  Step::Increase, Step::Increase, // coarse step 10: 50% -> 60% -> 70%
  Step::Select,                   // leave edit
  Step::Prev,                     // already at the top: NOTHING on the two Carminat screens
                                  // (wrap off, and no change means no frames), a wrap to the
                                  // last item on the OLED (wrap on). Same key, same model,
                                  // different geometry.
};
constexpr uint8_t kScriptLen = sizeof(kScript) / sizeof(kScript[0]);

const char* stepName(Step s) {
  switch (s) {
    case Step::Next:     return "Next";
    case Step::Prev:     return "Prev";
    case Step::Select:   return "Select";
    case Step::Increase: return "Increase";
    case Step::Decrease: return "Decrease";
  }
  return "?";
}

uint8_t  g_step     = 0;
uint32_t g_nextStep = 0;
constexpr uint32_t kStepMs  = 700;    // one scripted key
constexpr uint32_t kPauseMs = 2500;   // between displays

void trace(const char* what) {
  char sel[AFFA_MENU_ROW_MAX];
  g_model->rowText(g_model->selectedIndex(), sel, sizeof(sel));
  Serial.printf("[%-8s] %-8s item=%u row=%u %s scroll=0x%02X  \"%s\"\n",
                kTargets[g_target].name, what,
                static_cast<unsigned>(g_model->selectedIndex()),
                static_cast<unsigned>(g_model->selectedRow()),
                g_model->isEditing() ? "edit " : "     ",
                static_cast<unsigned>(g_model->scrollMask()), sel);
}

void runScript(uint32_t now) {
  if (!affa::expired(now, g_nextStep)) return;

  if (g_step >= kScriptLen) {                 // this display has seen the whole stream
    g_step     = 0;
    g_nextStep = now + kPauseMs;
    useTarget(static_cast<uint8_t>((g_target + 1) % kTargetCount));
    return;
  }
  g_nextStep = now + kStepMs;

  // A closed model consumes nothing, so hold-Load on the real panel stops the demo dead.
  // Reopening is this application's policy, not the widget's.
  if (!g_model->isOpen()) { g_model->open(); return; }

  const Step s = kScript[g_step++];
  switch (s) {
    case Step::Next:     (void)g_model->next();     break;
    case Step::Prev:     (void)g_model->prev();     break;
    case Step::Select:   (void)g_model->select();   break;
    case Step::Increase: (void)g_model->increase(); break;
    case Step::Decrease: (void)g_model->decrease(); break;
  }
  trace(stepName(s));
}

// ---------------------------------------------------------------------------
// Real keys from the panel
// ---------------------------------------------------------------------------
//
// MenuModel HAS NO KEY VOCABULARY — it has six intents. Mapping (Key, KeyEdge) onto them is
// the adapter's job now, and so is the `default:` below: the old Menu::handleKey returned
// false for SrcNext/SrcPrev/VolUp/VolDown/Pause so they reached the application even while
// the menu was open. Route every key you receive into the model and you swallow them.
void onKey(affa::Key k, affa::KeyEdge e, void*) {
  if (!g_model) return;                     // before useTarget(0) there is nothing to drive
  const bool hold = (e == affa::KeyEdge::Hold);
  bool consumed = false;

  switch (k) {
    case affa::Key::RollUp:   consumed = hold ? g_model->decrease() : g_model->prev(); break;
    case affa::Key::RollDown: consumed = hold ? g_model->increase() : g_model->next(); break;
    case affa::Key::Load:     consumed = hold ? g_model->back()     : g_model->select(); break;
    default: break;           // the transport keys are the application's, open or not
  }

  if (consumed) { trace("key"); return; }

  Serial.printf("[app     ] key 0x%04X %s reached the application\n",
                static_cast<unsigned>(k), hold ? "hold" : "click");
}

void onMenuClosed(void*) { Serial.println("[app     ] menu closed (CloseCb)"); }

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(200);
  if (!g_link.begin(kPins, 500000)) Serial.println("CAN did not come up — serial demo only");

  // CarminatDisplay's own menu is compiled in, empty, and must stay out of the way: without
  // this the OEM hotkey would try to open it on every hold-Load. It refuses (an empty menu
  // never opens), but saying so explicitly is the point — this example drives its own model.
  g_display.clearMenuHotkey();

  g_display.onKey(&onKey, nullptr);
  g_display.begin();

  useTarget(0);
  g_nextStep = ::millis() + kPauseMs;
}

void loop() {
  g_display.poll();
  runScript(::millis());
}
