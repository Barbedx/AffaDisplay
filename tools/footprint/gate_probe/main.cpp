// The gate-measurement probe. NOT an example and never flashed to a panel — it is the
// instrument behind the README's "what each gate is actually worth" table, driven by
// platformio_footprint.ini (see tools/footprint/README.md). It lives outside src/, so the
// Library Dependency Finder never pulls it into a consumer's build.
//
// Instantiates every panel the build selected and calls every optional render, so that
// --gc-sections cannot remove a feature the gate was supposed to remove. Flipping one
// AFFA_* gate and re-measuring therefore reports the real flash cost of that gate.
//
// IT ONLY MEASURES GATES THAT EXIST. Half of what this file used to touch — the menu
// widget, the marquee, the subscription table, the ISO-TP reassembler, Esp32CanLink,
// UpdateListMenuDisplay — was deleted in 2.0, and a probe that names them does not compile,
// which means the table they filled in was never going to be re-measured. docs/API.md §7
// says where each one went.
#include <Arduino.h>
#include <AffaDisplay.h>

namespace {

struct ArduinoClock final : affa::IClock {
  uint32_t millis() const override { return ::millis(); }
};

affa::LoopbackLink<32> g_link;
ArduinoClock           g_clock;

volatile uint32_t g_sink = 0;

void bite(affa::Result r)          { g_sink += static_cast<uint32_t>(r); }
void bite(bool b)                  { g_sink += b ? 1u : 0u; }
void bite(affa::Submitted s)       { g_sink += static_cast<uint32_t>(s.result) + s.ticket; }

// Everything on the panel-agnostic surface. Called once per selected panel, so a gate that
// is supposed to remove a builder has nothing left holding a reference to it.
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
  bite(d.showConfirmBox("CAP", "r0", "r1"));
  bite(d.showInfoPopup("AUX", "AUTO", "SPEED"));
  bite(d.pressKey(affa::Key::Load, affa::KeyEdge::Click));
  bite(d.pressKey(affa::Key::Load, affa::KeyEdge::Click, affa::KeySource::Wire));
  bite(d.supports(affa::Feature::Text));
  bite(d.synced());
  g_sink += d.abortPending();
  bite(d.abortAll());
  bite(d.pending(affa::RenderSlot::Text));
  g_sink += d.stats().rxFrames;
  g_sink += d.queued();
  // The observation seam, all of what is left of it — Layer 0. Touching each callback is
  // what keeps the dispatch out of --gc-sections' reach.
  d.onFrame([](const affa::Frame& f, affa::Direction, void*) { g_sink += f.id; }, nullptr);
  d.onKey([](affa::Key k, affa::KeyEdge, void*) { g_sink += static_cast<uint32_t>(k); }, nullptr);
  d.onComplete([](affa::TxTicket t, affa::Result, void*) { g_sink += t; }, nullptr);
  d.onSync([](affa::SyncState s, void*) { g_sink += static_cast<uint32_t>(s); }, nullptr);
  // Geometry is a virtual per panel; asking for it is what keeps each override linked.
  const affa::PanelGeometry g = d.panelGeometry();
  g_sink += g.mainChars + g.menuRows + g.infoRows + g.listMaxItems;
  d.setPassive(true);
  d.setSelfAck(true);
  bite(d.passive());
}

#if AFFA_PANEL_CARMINAT
affa::CarminatDisplay g_carminat(g_link, g_clock);
#  if AFFA_ENABLE_BIGMENU
uint8_t               g_menuBuf[affa::CarminatDisplay::menuScreenBytes(6)];
const char* const     kItems[] = {"ONE", "TWO", "THREE", "FOUR", "FIVE", "SIX"};
#  endif
#endif
#if AFFA_PANEL_UPDATELIST
affa::UpdateListDisplay g_seg(g_link, g_clock);
#endif
#if AFFA_PANEL_CLUSTER
affa::ClusterDisplay g_cluster(g_link, g_clock);
#endif
#if AFFA_ENABLE_TASK
// Instantiated so --gc-sections cannot remove src/rtos/. Never started: the probe is
// linked and measured, never flashed.
affa::rtos::AffaTask g_task;
#endif

}  // namespace

void setup() {
  Serial.begin(115200);
#if AFFA_PANEL_CARMINAT
  exercise(g_carminat);
  bite(g_carminat.showInfoMenu("a", "b", "c"));
  bite(g_carminat.selectBoxButton(1));
#  if AFFA_ENABLE_BIGMENU
  bite(g_carminat.showMenuN(g_menuBuf, sizeof(g_menuBuf), "HDR", kItems, 6));
  bite(g_carminat.selectMenuItem(1));   // declared inside the BIGMENU guard, so gated here too
#  endif
#  if AFFA_ENABLE_NAV
  static const uint8_t kBmp[affa::carminat::kNavBitmapBytes] = {0};
  bite(g_carminat.showNavBitmap(kBmp));
  bite(g_carminat.showNavBitmapWithHeader(affa::carminat::kNavHeader, kBmp));
  bite(g_carminat.navTick(true));
#  endif
#endif
#if AFFA_PANEL_UPDATELIST
  exercise(g_seg);
#endif
#if AFFA_PANEL_CLUSTER
  exercise(g_cluster);
#endif
#if AFFA_ENABLE_CANCOMMON_LINK
  static affa::CanCommonLink real;
  bite(real.begin(GPIO_NUM_5, GPIO_NUM_4, 500000));
  bite(real.isLive());
#endif
#if AFFA_ENABLE_TASK
  bite(g_task.running());
  g_sink += g_task.status().iterations;
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
#if AFFA_PANEL_CLUSTER
  g_cluster.poll();
#endif
}
