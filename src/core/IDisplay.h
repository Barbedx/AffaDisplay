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

  virtual Result setText(const char* text, uint8_t digit = 255) = 0;
  virtual Result setTime(const char* hhmm)                      = 0;
  virtual Result setPower(bool on)                              = 0;
  virtual Result setAuxMode(bool on)                            = 0;

  virtual Result showMenu(const char* header, const char* row0, const char* row1,
                          uint8_t scrollIndicator = 0x0B)       = 0;
  virtual Result highlightItem(uint8_t row)                     = 0;

  virtual Result showPopupText(const char* text, uint8_t icon = 0x09,
                               uint8_t srcIcon = 0xFF, uint8_t fmt = 0x60) = 0;
  virtual Result hidePopup()                                    = 0;
  virtual Result showFullscreenText(const char* l1, const char* l2, const char* l3) = 0;
  virtual Result hideFullscreenText()                           = 0;
  virtual Result showConfirmBox(const char* caption, const char* row0, const char* row1) = 0;
  virtual Result showInfoPopup(const char* l1, const char* l2, const char* l3) = 0;
  virtual Result hideInfoPopup()                                = 0;
};

} // namespace affa
