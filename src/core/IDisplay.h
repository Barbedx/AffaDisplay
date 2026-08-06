#pragma once
#include "AffaTypes.h"

namespace affa {

// The panel-agnostic surface. An application that wants to work across Carminat and
// UpdateList holds an IDisplay&; AffaDisplayBase implements it.
//
// Every render call ENQUEUES and returns immediately. The Result is an ACCEPTANCE
// verdict, never a delivery verdict — "was it queued?", not "did the panel show it?".
// The delivery verdict arrives later through onComplete().
struct IDisplay {
  virtual ~IDisplay() = default;

  virtual bool      begin() = 0;
  virtual void      poll()  = 0;
  virtual bool      supports(Feature f) const = 0;
  virtual SyncState syncState() const = 0;

  // [[nodiscard]] ON EVERY ONE OF THESE, and it is not decoration. The Result is the only
  // thing that distinguishes "queued" from NoSync / QueueFull / TooLong / NotSupported,
  // and a render whose Result is dropped is a screen that silently never appears — the
  // legacy failure this library was extracted to stop repeating. If you mean to ignore
  // one, say so: `(void)display.setText(...);`.
  [[nodiscard]] virtual Result setText(const char* text, uint8_t digit = 255) = 0;
  [[nodiscard]] virtual Result setTime(const char* hhmm)                      = 0;
  [[nodiscard]] virtual Result setPower(bool on)                              = 0;

  [[nodiscard]] virtual Result showMenu(const char* header, const char* row0,
                                        const char* row1,
                                        uint8_t scrollIndicator = 0x0B)       = 0;
  [[nodiscard]] virtual Result highlightItem(uint8_t row)                     = 0;

  [[nodiscard]] virtual Result showPopupText(const char* text, uint8_t icon = 0x09,
                                             uint8_t srcIcon = 0xFF,
                                             uint8_t fmt = 0x60)              = 0;
  [[nodiscard]] virtual Result hidePopup()                                    = 0;
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
  [[nodiscard]] virtual Result showFullscreenText(const char* l1, const char* l2,
                                                  const char* l3)             = 0;
  [[nodiscard]] virtual Result showConfirmBox(const char* caption, const char* row0,
                                              const char* row1)               = 0;
  [[nodiscard]] virtual Result showInfoPopup(const char* l1, const char* l2,
                                             const char* l3)                  = 0;
};

} // namespace affa
