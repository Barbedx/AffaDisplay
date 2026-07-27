#include "AuxModeTracker.h"

// The ENTIRE body is gated, so with AFFA_ENABLE_AUX_TRACKER = 0 (the default) neither the
// code nor its pattern strings reach flash.
#if AFFA_ENABLE_AUX_TRACKER

#include "../util/AffaLog.h"
#include "../core/AffaConstants.h"

namespace affa {
namespace {

constexpr const char* kTag = "AUX";

// Local, because <cctype>'s isdigit() takes an int and is locale-dependent, and the
// extracted code used Arduino's isDigit(), which is neither of those and is not available
// off-target.
inline bool digit(uint8_t c) { return c >= '0' && c <= '9'; }

}  // namespace

void AuxModeTracker::onFrame(const Frame& f) {
  // Short DLCs are real on this bus. Everything below indexes data[0..7].
  if (f.id != kAuxWatchId || f.len < kPacketLength) return;

  const uint8_t first = f.data[0];

  if (first == 0x10) {
    // The setText/screen first frame. Byte 6 is the format byte and is half the
    // discriminator, so the header has to be kept, not just its arrival time.
    for (uint8_t i = 0; i < kPacketLength; ++i) _header[i] = f.data[i];
    _headerMs   = _clock.millis();
    _haveHeader = true;
    return;
  }

  if (first != 0x21) return;
  if (!_haveHeader) return;
  if (expired(_clock.millis(), _headerMs + kAuxPairWindowMs)) return;

  const bool now = classify(_header, f.data);
  if (now == _aux) return;
  _aux = now;
  AFFA_LOGI(kTag, "AUX mode changed: %s", _aux ? "ENTERED" : "EXITED");
  if (_cb) _cb(_aux, _ctx);
}

// The reverse-engineered pattern table, preserved verbatim in behaviour and in intent.
// text[0] is skipped throughout: it is the last byte of the header region, not text.
// head[6] is the setText format byte — 0x19-0x3F is the radio digit style, 0x59-0x7F is
// plain ASCII — which is why the 0x59 threshold appears in two of the tests.
bool AuxModeTracker::classify(const uint8_t* head, const uint8_t* text) const {
  AFFA_LOGT(kTag, "head %02X %02X %02X %02X %02X %02X %02X %02X",
            head[0], head[1], head[2], head[3], head[4], head[5], head[6], head[7]);
  AFFA_LOGT(kTag, "text %02X %02X %02X %02X %02X %02X %02X %02X",
            text[0], text[1], text[2], text[3], text[4], text[5], text[6], text[7]);

  // Just the three letters: the trailing content varies by radio.
  if (text[1] == 'A' && text[2] == 'U' && text[3] == 'X') {
    AFFA_LOGD(kTag, "definitely AUX");
    return true;
  }

  // The idle/radio banner.
  if (text[1] == 'R' && text[2] == 'E' && text[3] == 'N' && text[4] == 'A' &&
      text[5] == 'U' && text[6] == 'L' && text[7] == 'T') {
    AFFA_LOGD(kTag, "definitely radio (RENAULT)");
    return false;
  }

  // CD: "TR <d> <d> C.."
  if (text[1] == 'T' && text[2] == 'R' && text[3] == ' ' &&
      (text[4] == ' ' || digit(text[4])) && digit(text[5]) && text[6] == ' ' &&
      text[7] == 'C') {
    AFFA_LOGD(kTag, "definitely CD");
    return false;
  }

  // Radio, short form: "> X" with the plain-ASCII format byte.
  if (text[1] == '>' && text[2] == ' ' && text[3] != ' ' && head[6] >= 0x59) {
    AFFA_LOGD(kTag, "definitely radio (short)");
    return false;
  }

  // Radio, manual preset: "M <ddd> "
  if (text[1] == 'M' && text[2] == ' ' && (text[3] == ' ' || digit(text[3])) &&
      digit(text[4]) && digit(text[5]) && digit(text[6]) && text[7] == ' ') {
    AFFA_LOGD(kTag, "definitely radio (M)");
    return false;
  }

  // Radio, list preset: "L  <ddd> "
  if (text[1] == 'L' && text[2] == ' ' && text[3] == ' ' && digit(text[4]) &&
      digit(text[5]) && digit(text[6]) && text[7] == ' ') {
    AFFA_LOGD(kTag, "definitely radio (L)");
    return false;
  }

  // Radio, bare frequency: "   1056" in the radio-digit format.
  if (text[1] == ' ' && text[2] == ' ' && text[3] == ' ' && digit(text[4]) &&
      digit(text[5]) && digit(text[6]) && digit(text[7]) && head[6] < 0x59) {
    AFFA_LOGD(kTag, "definitely radio (mode 1)");
    return false;
  }

  // No pattern matched. RETAIN the previous verdict rather than guessing — a flip here
  // would toggle the application's source state on every unrecognised screen.
  AFFA_LOGD(kTag, "undetermined — retaining last state");
  return _aux;
}

}  // namespace affa

#endif  // AFFA_ENABLE_AUX_TRACKER
