// The Carminat (AFFA3) twin — the monochrome graphical panel.
//
// EVERY Carminat screen (menu, now-playing box, notification, popup, info row) arrives on
// 0x151, so one decode() covers all of them; the highlight arrives separately as the
// single frame `07 29 01 <rowTag>`.
#pragma once

#include "../AffaConfig.h"
#if AFFA_ENABLE_VIRTUAL_PANEL && AFFA_PANEL_CARMINAT

#include "VirtualPanelBase.h"

// Drift guard, not a dependency. The identifiers below stay hand-written so that the twin
// CAN disagree with the driver — that is the whole point of an independent witness — and
// this include only turns an ACCIDENTAL divergence into a compile error instead of a
// silent one. __has_include is what keeps the twin legal for a consumer who vendors only
// vpanel/ and supplies their own panel.
#if __has_include("../carminat/CarminatConstants.h")
#  include "../carminat/CarminatConstants.h"
#  define AFFA_TWIN_HAVE_CARMINAT_CONSTANTS 1
#else
#  define AFFA_TWIN_HAVE_CARMINAT_CONSTANTS 0
#endif

namespace affa {

// The Carminat identifiers, from docs/WIRE-SPEC.md §4 (the id table) and §5 (sync).
//
// SPELLED OUT HERE rather than included from carminat/CarminatConstants.h, which is a
// later phase of this repository — and deliberately kept spelled out even once it lands,
// because the twin is supposed to be an INDEPENDENT WITNESS. A twin that derives its
// identifiers from the driver's header can no longer disagree with the driver, which is
// the one thing it exists to be able to do. When carminat/ arrives, reconcile the two with
// a static_assert; do not replace these with an #include.
namespace carminat_twin {
inline constexpr uint16_t kIdSetText     = 0x151;  // text / menu / control channel
inline constexpr uint16_t kIdBulk        = 0x1F1;  // second registered function id
inline constexpr uint16_t kIdKeyPressed  = 0x1C1;  // panel -> radio
inline constexpr uint16_t kIdSync        = 0x3AF;  // radio -> panel
inline constexpr uint16_t kIdSyncReply   = 0x3CF;  // panel -> radio
inline constexpr uint8_t  kFiller        = 0x00;
inline constexpr uint8_t  kAliveByte     = 0xB9;   // the RADIO's heartbeat
inline constexpr uint8_t  kRequestByte   = 0xBA;   // the RADIO's sync request

// The function table, IN THE ORDER THE DRIVER REGISTERS IT. The order is on the wire, and
// the twin has to acknowledge both entries or the very first send after a resync stalls
// on the 0x1F1 probe for AFFA_ACK_TIMEOUT_MS and then fails the payload behind it.
inline constexpr uint16_t kFuncIds[] = {kIdSetText, kIdBulk};

#if AFFA_TWIN_HAVE_CARMINAT_CONSTANTS
static_assert(kIdSetText    == carminat::kIdSetText,     "twin/driver 0x151 drift");
static_assert(kIdBulk       == carminat::kIdNav,         "twin/driver 0x1F1 drift");
static_assert(kIdKeyPressed == carminat::kIdKeyPressed,  "twin/driver 0x1C1 drift");
static_assert(kIdSync       == carminat::kIdSync,        "twin/driver 0x3AF drift");
static_assert(kIdSyncReply  == carminat::kIdSyncReply,   "twin/driver 0x3CF drift");
static_assert(kFiller       == carminat::kFiller,        "twin/driver filler drift");
#endif

inline constexpr VirtualPanelProfile kProfile{
    /*ctrlId     */ kIdSetText,
    /*textId     */ kIdSetText,   // same channel; there is no second text id
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
}  // namespace carminat_twin

class CarminatVirtualPanel final : public VirtualPanelBase {
 public:
  CarminatVirtualPanel() : VirtualPanelBase(carminat_twin::kProfile) {}

 protected:
  void decode(const Frame& f) override;
};

}  // namespace affa
#endif  // AFFA_ENABLE_VIRTUAL_PANEL && AFFA_PANEL_CARMINAT
