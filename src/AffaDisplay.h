// AffaDisplay — the only header a consumer includes.
//
//   #include <AffaDisplay.h>
//
// Declares no types of its own. Everything below is behind the gate that owns it, so a
// build that selected one panel does not even parse the other's declarations.
//
// The __has_include guards on the panel and protocol folders are deliberate, not a
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

// The display-agnostic menu. It is not panel code — MenuModel drives any IMenuRenderer, and
// the Carminat adapter is one of them (carminat/CarminatMenuRenderer.h, below) — so it sits
// above the panel gates and only the feature gate applies.
#if AFFA_ENABLE_MENU
#  if __has_include("widget/MenuModel.h")
#    include "widget/MenuGeometry.h"
#    include "widget/IMenuRenderer.h"
#    include "widget/MenuModel.h"
#  endif
#endif

// Same story, same layer: a scrolling text window that knows nothing about a panel, and
// three of them arranged as a live screen. Both transmit nothing and hold no display — the
// application samples them and calls the render primitive itself, exactly as MenuModel is
// sampled through an IMenuRenderer.
#if AFFA_ENABLE_MARQUEE
#  if __has_include("widget/Marquee.h")
#    include "widget/Marquee.h"
#  endif
#  if __has_include("widget/RowScreen.h")
#    include "widget/RowScreen.h"
#  endif
#endif

#include "link/LoopbackLink.h"
#if AFFA_ENABLE_ESP32CAN_LINK
#  include "link/Esp32CanLink.h"
#endif

// The owned poll task. THE ONLY PART OF THIS LIBRARY THAT IS NOT PORTABLE, and the one
// directory a non-FreeRTOS port omits: everything above this line compiles on the host
// against nothing but C++17. docs/API.md §4b.
#if AFFA_ENABLE_TASK
#  if __has_include("rtos/AffaTask.h")
#    include "rtos/AffaCommand.h"
#    include "rtos/AffaTask.h"
#  endif
#endif

#if AFFA_ENABLE_ISOTP_RX
#  if __has_include("proto/IsoTp.h")
#    include "proto/IsoTp.h"
#    include "proto/ScreenModel.h"
#    include "proto/ScreenDecode.h"
#  endif
#endif

#if AFFA_PANEL_CARMINAT
#  if __has_include("carminat/CarminatDisplay.h")
#    include "carminat/CarminatConstants.h"
#    include "carminat/CarminatDisplay.h"
#    if AFFA_ENABLE_MENU
#      include "carminat/CarminatMenuRenderer.h"
#      include "carminat/MenuController.h"
#      include "carminat/IPage.h"
#    endif
#  endif
#endif

#if AFFA_PANEL_CLUSTER
#  if __has_include("cluster/ClusterDisplay.h")
#    include "cluster/ClusterConstants.h"
#    include "cluster/ClusterDisplay.h"
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
