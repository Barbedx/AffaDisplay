#pragma once
#include "AffaTypes.h"
#include "PanelGeometry.h"

namespace affa {

// The panel-agnostic surface. An application that wants to work across Carminat and
// UpdateList holds an IDisplay&; AffaDisplayBase implements it.
//
// Every render call ENQUEUES and returns immediately. `Submitted` is an ACCEPTANCE
// verdict, never a delivery verdict — "was it queued?", not "did the panel show it?".
// The delivery verdict arrives later through onComplete(), keyed by Submitted::ticket.
//
// CALLABLE FROM ANY TASK. The bytes are built on the calling task's own stack — every
// render in this library is a pure function of its arguments — and then cross into the
// task that owns poll(). See docs/REFACTOR-2.0.md §3.4 for the one honest consequence:
// a refusal that the direct path reports in `result` is reported through onComplete()
// instead when the call came from another task, because nothing has been enqueued yet at
// the moment of return.
struct IDisplay {
  virtual ~IDisplay() = default;

  virtual bool      begin() = 0;
  virtual void      poll()  = 0;
  virtual bool      supports(Feature f) const = 0;
  virtual SyncState syncState() const = 0;

  // HOW BIG EACH SURFACE IS. supports() says a panel can draw a list; this says the list
  // holds six items of twenty-six characters. See PanelGeometry.h for why a consumer that
  // copies those numbers is a place to change every time a panel family is added.
  //
  // Not pure: a panel that has not filled it in returns all zeros, which reads as "no
  // surface" everywhere and is the same answer supports() gives.
  virtual PanelGeometry panelGeometry() const { return PanelGeometry{}; }

  // The [[nodiscard]] that used to be spelled out on every one of these now lives on
  // `Submitted` itself, so it cannot be forgotten on a render added later. Same guarantee,
  // one place: a render whose verdict is dropped is a screen that silently never appears.
  // To ignore one on purpose, say so: `(void)display.setText(...);`.
  virtual Submitted setText(const char* text, uint8_t digit = 255) = 0;
  virtual Submitted setTime(const char* hhmm)                      = 0;
  virtual Submitted setPower(bool on)                              = 0;

  virtual Submitted showMenu(const char* header, const char* row0,
                             const char* row1,
                             uint8_t scrollIndicator = 0x0B)       = 0;
  virtual Submitted highlightItem(uint8_t row)                     = 0;

  virtual Submitted showPopupText(const char* text, uint8_t icon = 0x09,
                                  uint8_t srcIcon = 0xFF,
                                  uint8_t fmt = 0x60)              = 0;
  virtual Submitted hidePopup()                                    = 0;
  // THE POPUP IS THE ONLY SCREEN WITH A HIDE, and that is a protocol fact rather than an
  // omission. It is the one true overlay: the screen underneath keeps redrawing and
  // reappears when it is cleared, so something has to clear it. Every other screen —
  // fullscreen, menu, message box, info rows — is REPLACED by the next render, so
  // "closing" one means drawing the next thing.
  //
  // `hideFullscreenText()` was removed on 2026-08-06. It emitted `02 54 03`, byte for byte
  // the same three bytes as hidePopup(), so it was a second name for one command and it
  // implied a teardown callers do not owe. If you want that command, call hidePopup(); if
  // you want the fullscreen gone, draw something else. `hideInfoPopup()` went with it for a
  // worse reason: it was never a close command at all, just setText("RENAULT") wearing one.
  virtual Submitted showFullscreenText(const char* l1, const char* l2,
                                       const char* l3)             = 0;
  virtual Submitted showConfirmBox(const char* caption, const char* row0,
                                   const char* row1)               = 0;
  virtual Submitted showInfoPopup(const char* l1, const char* l2,
                                  const char* l3)                  = 0;
};

} // namespace affa
