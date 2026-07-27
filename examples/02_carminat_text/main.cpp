// 02_carminat_text — the goal, in one file: come up, stay up, and put text on the glass.
//
// A CarminatDisplay, the CAN link, a clock, and a counter that re-renders once a second.
// Every [sync] transition is printed, so the handshake is visible rather than implied.
//
// THE RETURN VALUE IS AN ACCEPTANCE VERDICT, NOT A DELIVERY VERDICT. setText() enqueues
// and returns; "did the panel draw it" arrives later through onComplete() carrying the
// same TxTicket. That is why this file prints both, side by side — before sync is up
// setText() returns NoSync and no ticket is ever issued, and the two lines make the
// difference obvious.
//
// BEGIN() TRANSMITS: the first heartbeat leaves on the first poll(), on 0x3AF.

#include <Arduino.h>
#include <AffaDisplay.h>

#if !AFFA_PANEL_CARMINAT
#  error "02_carminat_text needs -D AFFA_PANEL_CARMINAT=1"
#endif

namespace {

struct ArduinoClock final : affa::IClock {
  uint32_t millis() const override { return ::millis(); }
};

constexpr affa::CanPins kPins{ .rx = GPIO_NUM_4, .tx = GPIO_NUM_3 };

affa::Esp32CanLink    g_link;
ArduinoClock          g_clock;
affa::CarminatDisplay g_display(g_link, g_clock);

uint32_t g_nextRenderMs = 0;
uint32_t g_counter      = 0;
affa::TxTicket g_lastTicket = affa::kNoTicket;

const char* resultName(affa::Result r) {
  switch (r) {
    case affa::Result::Ok:           return "Ok";
    case affa::Result::NoSync:       return "NoSync";
    case affa::Result::UnknownFunc:  return "UnknownFunc";
    case affa::Result::SendFailed:   return "SendFailed";
    case affa::Result::Timeout:      return "Timeout";
    case affa::Result::TooLong:      return "TooLong";
    case affa::Result::QueueFull:    return "QueueFull";
    case affa::Result::NotSupported: return "NotSupported";
    case affa::Result::BadArgument:  return "BadArgument";
    case affa::Result::LinkDown:     return "LinkDown";
    case affa::Result::Cancelled:    return "Cancelled";
    case affa::Result::Busy:         return "Busy";
    case affa::Result::Aborted:      return "Aborted";
  }
  return "?";
}

// Fires only on an ACTUAL state change, so this is a transition log and not a poll dump.
void onSync(affa::SyncState s, void*) {
  Serial.printf("[sync] 0x%02X  %s%s%s%s  synced=%d registered=%d\n",
                static_cast<unsigned>(s),
                affa::hasFlag(s, affa::SyncState::Failed)    ? "FAILED "    : "",
                affa::hasFlag(s, affa::SyncState::PeerAlive) ? "PEERALIVE " : "",
                affa::hasFlag(s, affa::SyncState::Start)     ? "START "     : "",
                affa::hasFlag(s, affa::SyncState::FuncsReg)  ? "FUNCSREG"   : "",
                g_display.synced(), g_display.registered());
}

// The delivery verdict. Superseded renders complete Result::Aborted and that is normal:
// setText() coalesces on RenderSlot::Text, so a re-render replaces a queued one that has
// not started yet rather than stacking behind it.
void onComplete(affa::TxTicket t, affa::Result r, void*) {
  Serial.printf("[tx  ] ticket %u -> %s\n", static_cast<unsigned>(t), resultName(r));
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(200);                       // the application may sleep; the library may not

  if (!g_link.begin(kPins, 500000)) {
    Serial.println("CAN did not come up — check pins, transceiver and termination");
    return;
  }

  // Ask before you call. On this panel Text is always true; on an UpdateList panel
  // supports(Feature::Time) is false and setTime() would return NotSupported rather than
  // silently doing nothing, which is the whole reason Feature exists.
  Serial.printf("supports Text=%d Time=%d Power=%d\n",
                g_display.supports(affa::Feature::Text),
                g_display.supports(affa::Feature::Time),
                g_display.supports(affa::Feature::Power));

  g_display.onSync(&onSync, nullptr);
  g_display.onComplete(&onComplete, nullptr);
  g_display.begin();

  g_display.setPower(true);         // 03 52 09 FF FF — enqueued like everything else
  g_display.setText("RENAULT");
}

void loop() {
  g_display.poll();                 // that is the whole integration

  const uint32_t now = ::millis();
  if (!affa::expired(now, g_nextRenderMs)) return;
  g_nextRenderMs = now + 1000;      // `= now + period`, never `+=`: a stalled loop must
                                    // not produce a catch-up burst

  char buf[16];
  snprintf(buf, sizeof(buf), "CNT %lu", static_cast<unsigned long>(++g_counter));

  const affa::Result r = g_display.setText(buf);
  g_lastTicket = g_display.lastEnqueued();   // read it IMMEDIATELY: the next enqueue,
                                             // including one a menu makes for you,
                                             // overwrites it
  Serial.printf("[app ] setText(\"%s\") -> %s, ticket %u\n", buf, resultName(r),
                static_cast<unsigned>(g_lastTicket));
}
