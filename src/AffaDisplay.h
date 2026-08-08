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

// THE MENU STATE MACHINE IS GONE, 2.0. MenuModel, MenuController, IPage, IMenuRenderer and
// CarminatMenu were 1090 lines of UI policy — which item is selected, which field is being
// edited, what a hold-Load gesture means — inside a CAN driver, and no shipping build ever
// compiled them: the widget gates defaulted to 0 and only the host test env turned them on.
//
// WHAT REMAINS IS THE PROTOCOL, and it is all an application needs to draw a menu:
// showMenu(header, row0, row1, scroll), showMenuN() for a list the panel scrolls itself,
// highlightItem() and selectMenuItem(). Deciding what is IN the list, and what a key press
// means, is the application's — which is where it was always going to end up.
//
// src/widget/ went with it, for the same reason one layer down: a scrolling title and a
// three-row live screen are decisions about a product, and this library implements a
// transport. An application that wants a marquee calls setText with a different window every
// 400 ms, on its own task, where a slow repaint cannot stall the protocol.
#include "link/LoopbackLink.h"
// The same seam over collin80's esp32_can instead of raw TWAI, for applications that
// already own that stack. Off unless the build asks: it needs an external library.
#if AFFA_ENABLE_CANCOMMON_LINK
#  include "link/CanCommonLink.h"
#endif

// The owned poll task. THE ONLY PART OF THIS LIBRARY THAT IS NOT PORTABLE, and the one
// directory a non-FreeRTOS port omits: everything above this line compiles on the host
// against nothing but C++17. docs/API.md §4b.
#if AFFA_ENABLE_TASK
#  if __has_include("rtos/AffaTask.h")
#    include "rtos/AffaTask.h"
#  endif
#endif

#if AFFA_PANEL_CARMINAT
#  if __has_include("carminat/CarminatDisplay.h")
#    include "carminat/CarminatConstants.h"
#    include "carminat/CarminatDisplay.h"
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
#  endif
#endif
