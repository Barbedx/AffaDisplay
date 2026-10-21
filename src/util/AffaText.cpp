#include "../AffaConfig.h"          // the ONLY thing outside the gate
#if AFFA_ENABLE_TRANSLITERATION

#include "AffaText.h"
#include <cstdint>

namespace affa {
namespace {

// Sink that refuses to write a partial replacement. `put()` returns false once the
// output is full, which is the signal to stop consuming input — that, plus decoding a
// whole codepoint before emitting anything, is what makes truncation safe at both ends.
struct Sink {
  char*  out;
  size_t cap;      // total buffer size, including room for the NUL
  size_t n = 0;

  bool put(const char* s) {
    size_t l = 0;
    while (s[l]) ++l;
    if (n + l + 1 > cap) return false;    // whole replacement or nothing
    for (size_t i = 0; i < l; ++i) out[n + i] = s[i];
    n += l;
    return true;
  }
  bool put(char c) {
    if (n + 2 > cap) return false;
    out[n++] = c;
    return true;
  }
  size_t finish() {
    if (cap) out[n] = '\0';
    return n;
  }
};

// Decode one UTF-8 sequence starting at s[i]. Advances i past the whole sequence.
// Returns false on a malformed lead byte, having consumed exactly that byte, so a
// corrupt stream cannot stall the walk.
bool decodeUtf8(const uint8_t* s, size_t& i, uint32_t& cp) {
  const uint8_t b = s[i];
  int extra;
  if ((b & 0xE0) == 0xC0)      { cp = b & 0x1Fu; extra = 1; }
  else if ((b & 0xF0) == 0xE0) { cp = b & 0x0Fu; extra = 2; }
  else if ((b & 0xF8) == 0xF0) { cp = b & 0x07u; extra = 3; }
  else                         { ++i; return false; }
  ++i;
  for (int j = 0; j < extra && s[i]; ++j, ++i) cp = (cp << 6) | (s[i] & 0x3Fu);
  return true;
}

// The mapping table, ported verbatim from MeganeCAN/src/utils/TextUtils.cpp. Written as
// a switch rather than a sorted array so that adding a codepoint cannot silently break
// the ordering a binary search would depend on; the compiler builds whatever lookup it
// prefers. A null return means "drop this codepoint entirely" (the Cyrillic hard and
// soft signs, which have no Latin form and are not worth a '?').
const char* mapCodepoint(uint32_t cp) {
  switch (cp) {
    // --- Polish ---
    case 0x0104: return "A";  // A-ogonek
    case 0x0105: return "a";
    case 0x0106: return "C";  // C-acute
    case 0x0107: return "c";
    case 0x0118: return "E";  // E-ogonek
    case 0x0119: return "e";
    case 0x0141: return "L";  // L-stroke
    case 0x0142: return "l";
    case 0x0143: return "N";  // N-acute
    case 0x0144: return "n";
    case 0x00D3: return "O";  // O-acute
    case 0x00F3: return "o";
    case 0x015A: return "S";  // S-acute
    case 0x015B: return "s";
    case 0x0179: return "Z";  // Z-acute
    case 0x017A: return "z";
    case 0x017B: return "Z";  // Z-dot
    case 0x017C: return "z";

    // --- Cyrillic ---
    case 0x0410: return "A";
    case 0x0430: return "a";
    case 0x0411: return "B";
    case 0x0431: return "b";
    case 0x0412: return "V";
    case 0x0432: return "v";
    case 0x0413: return "G";
    case 0x0433: return "g";
    case 0x0414: return "D";
    case 0x0434: return "d";
    case 0x0415: return "E";
    case 0x0435: return "e";
    case 0x0401: return "E";
    case 0x0451: return "e";
    case 0x0416: return "Zh";
    case 0x0436: return "zh";
    case 0x0417: return "Z";
    case 0x0437: return "z";
    case 0x0418: return "I";
    case 0x0438: return "i";
    case 0x0419: return "J";
    case 0x0439: return "j";
    case 0x041A: return "K";
    case 0x043A: return "k";
    case 0x041B: return "L";
    case 0x043B: return "l";
    case 0x041C: return "M";
    case 0x043C: return "m";
    case 0x041D: return "N";
    case 0x043D: return "n";
    case 0x041E: return "O";
    case 0x043E: return "o";
    case 0x041F: return "P";
    case 0x043F: return "p";
    case 0x0420: return "R";
    case 0x0440: return "r";
    case 0x0421: return "S";
    case 0x0441: return "s";
    case 0x0422: return "T";
    case 0x0442: return "t";
    case 0x0423: return "U";
    case 0x0443: return "u";
    case 0x0424: return "F";
    case 0x0444: return "f";
    case 0x0425: return "Kh";
    case 0x0445: return "kh";
    case 0x0426: return "Ts";
    case 0x0446: return "ts";
    case 0x0427: return "Ch";
    case 0x0447: return "ch";
    case 0x0428: return "Sh";
    case 0x0448: return "sh";
    case 0x0429: return "Sch";
    case 0x0449: return "sch";
    case 0x042A: return "";   // hard sign — dropped, as in the original
    case 0x044A: return "";
    case 0x042B: return "Y";
    case 0x044B: return "y";
    case 0x042C: return "";   // soft sign — dropped
    case 0x044C: return "";
    case 0x042D: return "E";
    case 0x044D: return "e";
    case 0x042E: return "Yu";
    case 0x044E: return "yu";
    case 0x042F: return "Ya";
    case 0x044F: return "ya";

    // --- Ukrainian extras ---
    case 0x0404: return "Ye";
    case 0x0454: return "ye";
    case 0x0406: return "I";
    case 0x0456: return "i";
    case 0x0407: return "Yi";
    case 0x0457: return "yi";
    case 0x0490: return "G";
    case 0x0491: return "g";

    default: return nullptr;  // -> '?'
  }
}

// Apple Music annotations, as raw UTF-8 bytes so that this file's own encoding cannot
// change what is matched. Longest first: " * Ye video" contains "* Ye video", and a
// shorter match first would leave a stray separator behind.
//   U+2022 BULLET      = E2 80 A2
//   U+0404 CYRILLIC YE = D0 84
//   "video" (uk)       = D0 B2 D1 96 D0 B4 D0 B5 D0 BE
const char* const kAnnotations[] = {
  " \xE2\x80\xA2 \xD0\x84 \xD0\xB2\xD1\x96\xD0\xB4\xD0\xB5\xD0\xBE",
  "\xE2\x80\xA2 \xD0\x84 \xD0\xB2\xD1\x96\xD0\xB4\xD0\xB5\xD0\xBE",
  "\xE2\x80\xA2\xD0\x84\xD0\xB2\xD1\x96\xD0\xB4\xD0\xB5\xD0\xBE",
  "\xE2\x80\xA2 Video",
};

bool isSpace(uint8_t c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

// Core walk. `strip` selects the normalizeTitle behaviour.
size_t convert(const char* in, char* out, size_t outSize, bool strip) {
  Sink sink{out, outSize};
  if (!out || outSize == 0) return 0;
  if (!in) return sink.finish();

  const uint8_t* s = reinterpret_cast<const uint8_t*>(in);
  size_t i = 0;

  if (strip) while (s[i] && isSpace(s[i])) ++i;

  while (s[i]) {
    if (strip) {
      bool skipped = false;
      for (const char* pat : kAnnotations) {
        const size_t l = std::strlen(pat);
        if (std::strncmp(reinterpret_cast<const char*>(s + i), pat, l) == 0) {
          i += l;
          skipped = true;
          break;
        }
      }
      if (skipped) continue;
    }

    const uint8_t b = s[i];
    if (b < 0x80) {
      if (!sink.put(static_cast<char>(b))) break;
      ++i;
      continue;
    }

    uint32_t cp = 0;
    if (!decodeUtf8(s, i, cp)) continue;   // malformed lead byte, already consumed

    const char* rep = mapCodepoint(cp);
    if (!rep) {
      if (!sink.put('?')) break;
    } else if (*rep) {
      if (!sink.put(rep)) break;
    }
  }

  const size_t n = sink.finish();
  if (!strip) return n;

  size_t t = n;
  while (t > 0 && isSpace(static_cast<uint8_t>(out[t - 1]))) --t;
  out[t] = '\0';
  return t;
}

} // namespace

size_t toAscii(const char* in, char* out, size_t outSize) {
  return convert(in, out, outSize, /*strip=*/false);
}

size_t normalizeTitle(const char* in, char* out, size_t outSize) {
  return convert(in, out, outSize, /*strip=*/true);
}

} // namespace affa

#endif // AFFA_ENABLE_TRANSLITERATION
