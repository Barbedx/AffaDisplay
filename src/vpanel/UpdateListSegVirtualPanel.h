// The UpdateList (AFFA2) 8-segment twin — a text-only panel.
//
// The two UpdateList panels share every identifier and differ ONLY in the screen payload
// they decode, so the profile lives here and the LCD twin reuses it.
#pragma once

#include "../AffaConfig.h"
#if AFFA_ENABLE_VIRTUAL_PANEL && AFFA_PANEL_UPDATELIST

#include "VirtualPanelBase.h"

// Drift guard, not a dependency — see the identical note in CarminatVirtualPanel.h.
#if __has_include("../updatelist/UpdateListConstants.h")
#  include "../updatelist/UpdateListConstants.h"
#  define AFFA_TWIN_HAVE_UPDATELIST_CONSTANTS 1
#else
#  define AFFA_TWIN_HAVE_UPDATELIST_CONSTANTS 0
#endif

namespace affa {

// The UpdateList identifiers, from docs/WIRE-SPEC.md §4 and §5. Spelled out here, not
// included from updatelist/UpdateListConstants.h — see the note in CarminatVirtualPanel.h
// for why the twin keeps its own copy on purpose.
namespace updatelist_twin {
inline constexpr uint16_t kIdSetText     = 0x121;  // text channel
inline constexpr uint16_t kIdDisplayCtrl = 0x1B1;  // display enable/disable
inline constexpr uint16_t kIdKeyPressed  = 0x0A9;  // panel -> radio
inline constexpr uint16_t kIdSync        = 0x3DF;  // radio -> panel
inline constexpr uint16_t kIdSyncReply   = 0x3CF;  // panel -> radio (SAME as Carminat)
inline constexpr uint8_t  kFiller        = 0x81;
inline constexpr uint8_t  kAliveByte     = 0x79;   // the RADIO's heartbeat
inline constexpr uint8_t  kRequestByte   = 0x7A;   // the RADIO's sync request

// The ACK id is COMPUTED, never tabulated: 0x0A9 | 0x400 is 0x4A9 and NOT 0x5A9, because
// bit 8 is already clear in 0x0A9 — uniquely in the whole identifier table. Every other
// id in the table has bit 8 set, which is why "add 0x400 to the leading digit" happens to
// work everywhere else and is wrong here.
static_assert((kIdKeyPressed | kReplyFlag) == 0x4A9, "0x0A9 | 0x400 is 0x4A9, not 0x5A9");

inline constexpr uint16_t kFuncIds[] = {kIdSetText, kIdDisplayCtrl};

#if AFFA_TWIN_HAVE_UPDATELIST_CONSTANTS
static_assert(kIdSetText     == updatelist::kIdSetText,     "twin/driver 0x121 drift");
static_assert(kIdDisplayCtrl == updatelist::kIdDisplayCtrl, "twin/driver 0x1B1 drift");
static_assert(kIdKeyPressed  == updatelist::kIdKeyPressed,  "twin/driver 0x0A9 drift");
static_assert(kIdSync        == updatelist::kIdSync,        "twin/driver 0x3DF drift");
static_assert(kIdSyncReply   == updatelist::kIdSyncReply,   "twin/driver 0x3CF drift");
static_assert(kFiller        == updatelist::kFiller,        "twin/driver filler drift");
#endif

inline constexpr VirtualPanelProfile kProfile{
    /*ctrlId     */ kIdDisplayCtrl,
    /*textId     */ kIdSetText,
    /*keyId      */ kIdKeyPressed,
    /*syncId     */ kIdSync,
    /*syncReplyId*/ kIdSyncReply,
    /*replyFlag  */ kReplyFlag,
    /*filler     */ kFiller,
    /*aliveByte  */ kAliveByte,
    /*requestByte*/ kRequestByte,
    /*funcIds    */ kFuncIds,
    /*funcCount  */ 2,
};
}  // namespace updatelist_twin

// Kept as a function as well as a constant, because the reference exposed
// updateListProtocol() and a migrating caller will look for it.
inline constexpr VirtualPanelProfile updateListProfile() { return updatelist_twin::kProfile; }

// Decodes the 0x121 setText payload into header (the "new" text) and row0 (the "old"
// text). Handles BOTH forms of the differential encoding — 0x76 text-only and 0x7F
// text-plus-icons — even though our own 8-segment driver only ever emits 0x76: the twin
// models the PANEL, and the panel accepts both.
class UpdateListSegVirtualPanel final : public VirtualPanelBase {
 public:
  UpdateListSegVirtualPanel() : VirtualPanelBase(updatelist_twin::kProfile) {}

 protected:
  void decode(const Frame& f) override;
};

}  // namespace affa
#endif  // AFFA_ENABLE_VIRTUAL_PANEL && AFFA_PANEL_UPDATELIST
