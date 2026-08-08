// HOW BIG EACH SURFACE IS, in one struct the panel answers for itself.
//
// WHY THIS EXISTS. supports(Feature) says a panel CAN draw a list; it does not say the list
// holds six items of twenty-six characters. Those numbers were scattered across
// carminat/CarminatConstants.h, CarminatMenuRenderer::kChars and
// updatelist/UpdateListConstants.h, and every consumer that needed one copied it.
//
// A COPIED CONSTANT IS A PLACE TO CHANGE. The application this was added for composes screens
// from content it did not write — an agent, a weather service, a home-automation feed — and
// the fitting has to happen somewhere. If the fitter hard-codes "eight characters", adding a
// panel family means editing the fitter. If it asks the panel, adding a family costs nothing.
//
// EVERY FIELD IS WHAT REACHES THE GLASS, not what the builder accepts. Carminat's setText
// carries fourteen cells and the panel shows about eight of them; the number here is eight,
// because a fitter that believes fourteen produces text that is cut off on hardware and
// correct in every test.
//
// A ZERO MEANS THE SURFACE DOES NOT EXIST on this family. Check it, or check
// supports(Feature) — they agree, and a zero-sized surface is the same answer as an
// unsupported one.
#pragma once
#include <cstdint>

namespace affa {

struct PanelGeometry {
  // The main text line — setText().
  uint8_t mainChars      = 0;

  // The two-row window — showMenu(). `menuRows` is the VIEWPORT, not the list length.
  uint8_t menuRows       = 0;
  uint8_t menuRowChars   = 0;

  // The info rows — showInfoMenu(). Narrower than a menu row and there are three of them.
  uint8_t infoRows       = 0;
  uint8_t infoRowChars   = 0;

  // The scrolling list — showMenuN(). The panel tracks `listMaxItems` and draws `menuRows`
  // of them, scrolling itself as the selection moves.
  uint8_t listMaxItems   = 0;
  uint8_t listItemChars  = 0;

  // The three-line fullscreen — showFullscreenText().
  uint8_t fullRows       = 0;
  uint8_t fullRowChars   = 0;

  // The monochrome pane — showNavBitmap(). An INDEPENDENT LAYER, not a screen mode.
  uint8_t imageWidth     = 0;
  uint8_t imageHeight    = 0;

  bool hasImage() const { return imageWidth && imageHeight; }
};

}  // namespace affa
