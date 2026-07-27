// AffaDisplay — the only header a consumer includes.
//
//   #include <AffaDisplay.h>
//
// Declares no types of its own. Everything below is behind the gate that owns it, so a
// build that selected one panel does not even parse the other's declarations.
//
// The __has_include guards on the panel, protocol and twin folders are deliberate, not a
// workaround: this umbrella must stay valid for a consumer who vendors only the parts
// they use, and for the phased build-out of this repository. The AFFA_* gate is still
// what decides whether a present file is compiled — __has_include only stops a missing
// one from being a parse error at a call site that was never going to use it.
#pragma once

#include "AffaConfig.h"

#include "core/AffaTypes.h"
#include "core/AffaConstants.h"
#include "core/AffaSyncProfile.h"
#include "core/AffaRing.h"
#include "core/IClock.h"
#include "core/ICanLink.h"
#include "core/IPanel.h"
#include "core/IDisplay.h"
#include "core/AffaDisplayBase.h"

#include "util/AffaLog.h"
#include "util/AffaText.h"

#include "link/LoopbackLink.h"
#if AFFA_ENABLE_ESP32CAN_LINK
#  include "link/Esp32CanLink.h"
#endif

#if AFFA_ENABLE_ISOTP_RX
#  if __has_include("proto/IsoTp.h")
#    include "proto/IsoTp.h"
#    include "proto/ScreenModel.h"
#    include "proto/ScreenDecode.h"
#  endif
#endif

#if AFFA_ENABLE_VIRTUAL_PANEL
#  if __has_include("vpanel/VirtualPanelBase.h")
#    include "vpanel/IVirtualPanel.h"
#    include "vpanel/VirtualPanelBase.h"
#    if AFFA_PANEL_CARMINAT && __has_include("vpanel/CarminatVirtualPanel.h")
#      include "vpanel/CarminatVirtualPanel.h"
#    endif
#    if AFFA_PANEL_UPDATELIST && __has_include("vpanel/UpdateListSegVirtualPanel.h")
#      include "vpanel/UpdateListSegVirtualPanel.h"
#    endif
#    if AFFA_PANEL_UPDATELIST_MENU && __has_include("vpanel/UpdateListLcdVirtualPanel.h")
#      include "vpanel/UpdateListLcdVirtualPanel.h"
#    endif
#  endif
#endif

#if AFFA_PANEL_CARMINAT
#  if __has_include("carminat/CarminatDisplay.h")
#    include "carminat/CarminatConstants.h"
#    include "carminat/CarminatDisplay.h"
#    if AFFA_ENABLE_MENU
#      include "carminat/Menu/Menu.h"
#      include "carminat/MenuController.h"
#      include "carminat/IPage.h"
#    endif
#    if AFFA_ENABLE_AUX_TRACKER
#      include "carminat/AuxModeTracker.h"
#    endif
#  endif
#endif

#if AFFA_PANEL_UPDATELIST
#  if __has_include("updatelist/UpdateListDisplay.h")
#    include "updatelist/UpdateListConstants.h"
#    include "updatelist/UpdateListBase.h"
#    include "updatelist/UpdateListDisplay.h"
#    if AFFA_PANEL_UPDATELIST_MENU && __has_include("updatelist/UpdateListMenuDisplay.h")
#      include "updatelist/UpdateListMenuDisplay.h"
#    endif
#  endif
#endif
