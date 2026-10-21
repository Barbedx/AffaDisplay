// AffaTwai — the CAN link without the display.
//
//   #include <AffaTwai.h>
//
// For a build with no panel at all: a bus sniffer, a listen-only tap, a second bus. It needs
// no AFFA_PANEL_* flag, and nothing here knows the AFFA protocol.
//
// Standalone, nobody runs the display's poll(), so the owner of the link does its two jobs:
//
//   link.begin(cfg);                       // cfg.listenOnly = true for a sniffer
//   for (;;) {
//     link.waitRx(10);                     // sleep until a frame or 10 ms
//     affa::Frame f;
//     while (link.recv(f)) { ... }
//     link.service(millis());              // TxGate; a no-op in listen-only
//     if (!link.healthy()) link.recover(); // bus-off / stopped
//   }
#pragma once

#include "AffaConfig.h"
#include "core/AffaTypes.h"
#include "core/ICanLink.h"
#include "link/TxGate.h"
#include "link/TwaiLink.h"
#include "util/AffaLog.h"               // affa::setLogSink(), for the link's own log lines
