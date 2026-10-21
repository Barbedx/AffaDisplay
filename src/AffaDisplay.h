// SPDX-License-Identifier: MIT
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

// The display needs a panel; the link alone (AffaTwai.h) does not. See AffaConfig.h.
#if !AFFA_PANEL_CARMINAT && !AFFA_PANEL_UPDATELIST && !AFFA_PANEL_CLUSTER
#  error "AffaDisplay: no panel selected. Add -D AFFA_PANEL_CARMINAT=1 and/or -D AFFA_PANEL_UPDATELIST=1, or -D AFFA_PANEL_DEFAULT_ALL=1 for both. Check your spelling: a typo'd AFFA_PANEL_* flag lands here. Only want the CAN link? Include <AffaTwai.h> instead."
#endif

#include "core/AffaTypes.h"
#include "core/AffaConstants.h"
#include "core/AffaSyncProfile.h"
#include "core/AffaRing.h"
#include "core/IClock.h"
#include "core/ICanLink.h"
#include "core/WakePacer.h"
#include "core/IPanel.h"
#include "core/IDisplay.h"
#include "core/AffaDisplayBase.h"

#include "util/AffaLog.h"
#include "util/AffaText.h"

#include "link/LoopbackLink.h"
// The ESP32 link, on the IDF TWAI driver. Empty on a target without <driver/twai.h>.
#include "link/TxGate.h"
#include "link/TwaiLink.h"

// The owned poll task. THE ONLY PART OF THIS LIBRARY THAT IS NOT PORTABLE, and the one
// directory a non-FreeRTOS port omits: everything above this line compiles on the host
// against nothing but C++17. docs/API.md §4.4.
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
