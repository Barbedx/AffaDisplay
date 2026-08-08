// The semantic display contract — and the ONE place that knows how it becomes CAN.
//
// THE RULE THIS FILE EXISTS TO ENFORCE:
//
//     A producer may compose SCREENS. A producer may never compose AffaDisplay commands.
//
// "Producer" is an LLM here, but deliberately not only: the same document is what a weather
// service, a home-automation rule, a phone or the car's own telemetry would send. Nothing
// downstream knows or cares which produced it, which is why this is a DisplayDocument and
// not an AiMessage.
//
// WHAT A PRODUCER NEVER SEES: payload[3] = 0x30, payload[4] = the thumb, payload[8] = the
// arrow mask, ISO-TP, RenderSlot, ticket handles, the 400 ms quiet interval after
// registration. Those are renderer implementation details, and a producer that could reach
// them could brick the glass or wedge the bus. It gets six nouns and some text.
//
// ── the layering ─────────────────────────────────────────────────────────────
//   producer (LLM / service / phone)
//        │  DisplayDocument
//   normalise()      <- clamps to what THIS panel can actually show
//        │
//   render()         <- the only function in the project that maps intent to protocol
//        │
//   AffaDisplay → CAN
//
// ── why the type set is CLOSED ───────────────────────────────────────────────
// Six values, and new content does NOT get a seventh — it gets fitted into one of these.
//
// That rule is not stylistic. This library shipped an enum that mapped one value onto one
// render call (rtos::Op), and keeping it in step meant editing four places; twelve renders
// were added and NONE were mirrored, until the whole thing was deleted in 2.0
// (docs/API.md §7). What saves DocType from the same rot is that it is SEMANTIC —
// it says what the content IS, not which builder to call — and that the mapping below is
// allowed to be LOSSY. A `Question` on a panel with no message box degrades to two lines and
// a hint; it does not add a case.
#pragma once
#include <AffaDisplay.h>
#include <cstring>

namespace aidoc {

// What the content IS. Not what it is drawn with.
enum class DocType : uint8_t {
  Text,      // one short line. The always-available surface: every family has it
  Info,      // a few short rows — a fact, a reading, a status
  List,      // a set of choices or headlines the user can walk
  Message,   // something that wants acknowledging
  Question,  // a choice between two answers
  Image,     // a picture carries it; the text is the caption
};

inline DocType docTypeFrom(const char* s) {
  if (!s) return DocType::Text;
  if (!strcmp(s, "info"))     return DocType::Info;
  if (!strcmp(s, "list"))     return DocType::List;
  if (!strcmp(s, "message"))  return DocType::Message;
  if (!strcmp(s, "question")) return DocType::Question;
  if (!strcmp(s, "image"))    return DocType::Image;
  return DocType::Text;
}
inline const char* docTypeName(DocType t) {
  switch (t) {
    case DocType::Text:     return "text";
    case DocType::Info:     return "info";
    case DocType::List:     return "list";
    case DocType::Message:  return "message";
    case DocType::Question: return "question";
    case DocType::Image:    return "image";
  }
  return "text";
}

// ── icons ───────────────────────────────────────────────────────────────────
// A NAME, RESOLVED ON THE DEVICE — never bytes from the producer.
//
// AND IT IS ONE BYTE ON THE WIRE. The gutter glyph rides inside the list message we are
// already sending; the 48x48 pane is 44 CAN frames at ISO-TP BlockSize 1, about 50 ms of
// round trips each. So an icon is a glyph, and the pane is for an actual picture. That
// distinction is worth more than it looks: it is the difference between "every card can have
// an icon for free" and "an icon costs a quarter of the link".
//
// Adding one is one row of this table. That is the whole point of a name.
struct IconName { const char* name; uint8_t glyph; };
inline constexpr IconName kIcons[] = {
  { "none",      affa::carminat::kMenuIconOemBlank },
  { "book",      affa::carminat::kMenuIconBookOpen },
  { "search",    affa::carminat::kMenuIconBookGlass },
  { "bluetooth", affa::carminat::kMenuIconBluetooth },
  { "gps",       affa::carminat::kMenuIconGps },
  { "plane",     affa::carminat::kMenuIconPlane },
  { "nav",       affa::carminat::kMenuIconOemNav },
  { "settings",  affa::carminat::kMenuIconOemSet },
};
inline constexpr uint8_t kIconCount = sizeof(kIcons) / sizeof(kIcons[0]);

// UNKNOWN NAMES RESOLVE TO "none", they do not fail. A producer that invents an icon gets a
// blank gutter and a screen that still reads; rejecting the whole document over a decorative
// byte would be the wrong trade.
inline uint8_t iconGlyph(const char* name) {
  if (name && *name)
    for (uint8_t i = 0; i < kIconCount; ++i)
      if (!strcmp(kIcons[i].name, name)) return kIcons[i].glyph;
  return affa::carminat::kMenuIconOemBlank;
}

constexpr uint8_t kMaxLines = 3;
constexpr uint8_t kMaxItems = 6;      // the OEM corpus shows 2, 4 and 6; six is what it uses
constexpr uint8_t kTextMax  = 32;

// ONE DOCUMENT. Fixed-size and copyable: it crosses from an HTTP handler to the render path
// and is held in a ring, so nothing here may be a pointer into somebody's request buffer.
struct DisplayDocument {
  char     id[16]        = {0};   // the producer's handle, echoed back with a key event
  DocType  type          = DocType::Text;
  char     title[kTextMax] = {0};
  char     lines[kMaxLines][kTextMax] = {{0}};
  uint8_t  lineCount     = 0;
  char     items[kMaxItems][kTextMax] = {{0}};
  uint8_t  itemCount     = 0;
  uint8_t  selected      = 0;
  char     icon[12]      = {0};
  // A NAMED DEVICE-SIDE ANIMATION, resolved here exactly like an icon is — "boom", "stars",
  // "clock", "globe". Empty leaves whatever the pane is already showing.
  //
  // WHY A NAME AND NOT PIXELS. A 48x48 frame is 288 bytes and 44 CAN frames; an animation is
  // that every 250 ms. Sending frames from a producer over a phone hotspot is not a slow
  // version of this, it is a different thing that does not work in a tunnel. The device
  // already draws these; the producer picks one.
  char     scene[12]     = {0};
  uint32_t ttlMs         = 30000;

  bool empty() const { return !title[0] && !lineCount && !itemCount && !scene[0]; }
};

// ── context: the other direction ────────────────────────────────────────────
// WHAT THE CAR KNOWS, sent UP with every fetch so a producer can be relevant rather than
// generic — weather where the car actually is, a fact about the artist actually playing.
//
// It is a bag of named strings on purpose. Every field added here is a field the producer
// MAY use and nothing breaks if it does not; a typed struct shared between firmware and a
// server is two places to change for every new signal, which is the thing this whole design
// is avoiding. The device fills what it has and omits what it does not.
//
// Nothing here is required. A producer that gets no position falls back to somewhere
// sensible, which is its decision to make and not the firmware's.
constexpr uint8_t kMaxContext = 8;
struct Context {
  struct KV { char k[12]; char v[40]; };
  KV      kv[kMaxContext];
  uint8_t count = 0;

  void set(const char* k, const char* v) {
    if (!k || !v || !*v) return;
    for (uint8_t i = 0; i < count; ++i)
      if (!strcmp(kv[i].k, k)) { snprintf(kv[i].v, sizeof(kv[i].v), "%s", v); return; }
    if (count >= kMaxContext) return;
    snprintf(kv[count].k, sizeof(kv[count].k), "%s", k);
    snprintf(kv[count].v, sizeof(kv[count].v), "%s", v);
    ++count;
  }
  void clear() { count = 0; }
};

// ── normalise ───────────────────────────────────────────────────────────────
// THE PANEL SAYS HOW BIG, NOT THE PRODUCER. Everything below is clamped against
// panelGeometry(), so adding a panel family changes nothing here and nothing upstream.
//
// It NORMALISES rather than rejects. A document with seven items and a forty-character title
// is not an attack, it is a model that overshot; cutting it to fit puts something on the
// glass, and rejecting it puts nothing there and no explanation either. The one thing that
// IS rejected is a document with no content at all, because that has nothing to show.
//
// `selected` is clamped last, after itemCount, or a producer could point the highlight past
// the end of a list it also shortened.
inline bool normalise(DisplayDocument& d, const affa::PanelGeometry& g) {
  auto clamp = [](char* s, uint8_t n) {
    if (n == 0) { s[0] = '\0'; return; }
    if (n >= kTextMax) n = kTextMax - 1;
    s[n] = '\0';
  };

  if (d.lineCount > kMaxLines) d.lineCount = kMaxLines;
  if (d.itemCount > kMaxItems) d.itemCount = kMaxItems;
  if (g.listMaxItems && d.itemCount > g.listMaxItems) d.itemCount = g.listMaxItems;

  // A surface this panel does not have is a document that has to become another type. The
  // UpdateList family is eight segment cells and nothing else, so every type collapses to
  // Text there — which is exactly the case that would need editing upstream if the sizes
  // were hard-coded on the server instead of asked for here.
  if (d.type == DocType::List && !g.listMaxItems)     d.type = DocType::Info;
  if (d.type == DocType::Question && !g.menuRows)     d.type = DocType::Info;
  if (d.type == DocType::Info && !g.infoRows)         d.type = DocType::Text;
  if (d.type == DocType::Message && !g.menuRows)      d.type = DocType::Text;
  if (d.type == DocType::Image && !g.hasImage())      d.type = DocType::Info;

  clamp(d.title, d.type == DocType::Text ? g.mainChars : g.menuRowChars);
  for (uint8_t i = 0; i < d.lineCount; ++i)
    clamp(d.lines[i], d.type == DocType::Info ? g.infoRowChars : g.menuRowChars);
  for (uint8_t i = 0; i < d.itemCount; ++i) clamp(d.items[i], g.listItemChars);

  if (d.itemCount && d.selected >= d.itemCount) d.selected = 0;
  if (d.ttlMs && d.ttlMs < 2000) d.ttlMs = 2000;     // a screen nobody can read is a flicker

  // ── THE COMPOSITION MATRIX, measured on the bench 2026-08-08 ────────────────
  // The 48x48 pane is an independent LAYER, but it is not independent of everything: it
  // stays visible under the plain text line and under the three info rows, and it is NOT
  // visible under a list (`21 01`), a message box or a fullscreen screen. Those take the
  // whole glass.
  //
  // SO A SCENE ON ONE OF THOSE TYPES IS A PROMISE THE PANEL DOES NOT KEEP, and clearing it
  // here is the difference between a producer that learns the rule and one that keeps
  // asking for a picture nobody can see. It costs 44 CAN frames to send an image that is
  // then covered up — about a quarter of this link — so this is not only a correctness fix.
  //
  // Text and Info keep theirs. Image IS the pairing, and normalise() has already turned it
  // into Info above when the family has no pane at all.
  if (d.type == DocType::List || d.type == DocType::Message || d.type == DocType::Question)
    d.scene[0] = '\0';

  return !d.empty();
}

// ── render ──────────────────────────────────────────────────────────────────
// THE ONLY FUNCTION IN THIS PROJECT THAT TURNS INTENT INTO PROTOCOL. Everything above is
// content; everything below is AffaDisplay. If a screen looks wrong, it is wrong here.
//
// The mapping is deliberately lossy where the panel is poorer than the intent — see the
// header note on why that is what keeps the type set closed.
inline affa::Submitted render(affa::CarminatDisplay& d, const DisplayDocument& doc,
                             uint8_t* scratch, uint16_t scratchLen) {
  const uint8_t glyph = iconGlyph(doc.icon);

  switch (doc.type) {
    case DocType::Text:
      return d.setText(doc.title[0] ? doc.title : doc.lines[0]);

    case DocType::Info:
      // Three narrow rows. The title is not drawn — showInfoMenu has no header — so a
      // producer that put the payload in the title still gets it on the glass.
      return d.showInfoMenu(doc.lineCount > 0 ? doc.lines[0] : doc.title,
                            doc.lineCount > 1 ? doc.lines[1] : "",
                            doc.lineCount > 2 ? doc.lines[2] : "");

    case DocType::List: {
      const char* items[kMaxItems];
      for (uint8_t i = 0; i < doc.itemCount; ++i) items[i] = doc.items[i];
      // scrollMask 0x03 is both arrows — the OEM byte, not the origin's 0x0C.
      // The thumb sits at the top: a position, not a proportion.
      return d.showMenuN(scratch, scratchLen, doc.title, items, doc.itemCount,
                         /*firstVisible=*/0, doc.selected, affa::carminat::kScrollBoth,
                         glyph, affa::carminat::kMenuThumbMin);
    }

    case DocType::Message: {
      // One button, because a message wants acknowledging rather than answering. The box has
      // TWO rows and no header of its own, so the title becomes row 0 when there is room —
      // losing it entirely would drop the part a producer thinks is the headline.
      const char* ok[1] = { doc.itemCount > 0 ? doc.items[0] : "OK" };
      const char* r0 = doc.title[0] ? doc.title : (doc.lineCount > 0 ? doc.lines[0] : "");
      const char* r1 = doc.title[0] ? (doc.lineCount > 0 ? doc.lines[0] : "")
                                    : (doc.lineCount > 1 ? doc.lines[1] : "");
      return d.showMessageBox(r0, r1, ok, 1, 0);
    }

    case DocType::Question: {
      // The two-button box IS the question primitive. Labels come from the first two items,
      // which is what a producer naturally fills in for a choice.
      const char* labels[2] = { doc.itemCount > 0 ? doc.items[0] : "YES",
                                doc.itemCount > 1 ? doc.items[1] : "NO" };
      const char* r0 = doc.title[0] ? doc.title : (doc.lineCount > 0 ? doc.lines[0] : "");
      const char* r1 = doc.title[0] ? (doc.lineCount > 0 ? doc.lines[0] : "")
                                    : (doc.lineCount > 1 ? doc.lines[1] : "");
      return d.showMessageBox(r0, r1, labels, 2, doc.selected < 2 ? doc.selected : 0);
    }

    case DocType::Image:
      // THE CAPTION, NOT THE PICTURE. The 48x48 pane is an independent layer the application
      // already drives on its own cadence; a document that says "Image" is saying the
      // picture carries the meaning, so all this does is put the caption under it.
      return d.showInfoMenu(doc.title, doc.lineCount > 0 ? doc.lines[0] : "",
                            doc.lineCount > 1 ? doc.lines[1] : "");
  }
  return affa::Submitted::refused(affa::Result::BadArgument);
}

}  // namespace aidoc
