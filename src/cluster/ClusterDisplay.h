// The dashboard cluster — UNTESTED, and the header says so because nothing else can.
//
// This class exists to be the counter-example that proves SyncProfile: a third family,
// transcribed from ONE capture (docs/PROTOCOL-NOTES.md §9), needed a header of data and
// these forty lines. No change to core/, no new transport, no new FSM.
//
// WHAT IT CAN DO: complete the handshake, latch the lazy 0x70 registration on 0x121 and
// 0x1B1, answer the cluster's own registration on 0x1C1, and switch the display on and off.
//
// WHAT IT CANNOT DO: render text. The capture contains no text frame at all — the radio
// never draws in the sample — so the cluster's setText encoding is genuinely unknown.
// setText therefore returns NotSupported, and supports(Feature::Text) is FALSE. That is not
// an omission to be filled in by guessing; it is the honest state of the evidence, and a
// capability lie here would be worse than a missing feature.
//
// The clock is NOT here either, and deliberately: `3EF A6 <hh> <mm>` is a raw 3-byte frame
// with no PCI and no SF_DL, so it is not a render call and does not belong on a display
// class. Send it with ICanLink::send(). See §9.4.
#pragma once
#include "../AffaConfig.h"

#if AFFA_PANEL_CLUSTER

#include "../core/AffaDisplayBase.h"
#include "ClusterConstants.h"

namespace affa {

class ClusterDisplay final : public AffaDisplayBase {
 public:
  ClusterDisplay(ICanLink& link, IClock& clock)
      : AffaDisplayBase(link, clock, cluster::kSync, cluster::kFuncIds,
                        cluster::kFuncCount) {}

  using AffaDisplayBase::onFrame;   // name hiding, as in the other two panels

  bool supports(Feature f) const override {
    switch (f) {
      case Feature::Power:     return true;    // [CAP] 1B1 03 52 00 00 observed
      case Feature::KeyTx:     return true;    // the cluster owns 0x1C1
      case Feature::RadioText: return AFFA_ENABLE_ISOTP_RX != 0;
      // Text is NOT supported: the capture contains no text frame, so the encoding is
      // unknown. Saying true here and guessing an encoding would be a capability lie.
      default: return false;
    }
  }

  // `1B1 03 52 <02|00> 00` padded with 0xFF. The OFF value is from the capture; the ON
  // value is inferred from the other two families and is the least certain byte we ship.
  [[nodiscard]] Result setPower(bool on) override {
    const uint8_t d[cluster::kPowerLen] = {
        cluster::kPowerSfDl,
        cluster::kCmdSetState,
        on ? cluster::kPowerOn : cluster::kPowerOff,
        cluster::kPowerTail,
    };
    return enqueue(cluster::kIdDisplayCtrl, d, cluster::kPowerLen,
                   TxOptions{}) == kNoTicket
               ? lastResult()
               : Result::Ok;
  }

 protected:
  uint8_t  packetFiller() const override { return cluster::kFiller; }
  uint16_t keyTxId()      const override { return cluster::kIdKeyPressed; }
};

}  // namespace affa

#endif  // AFFA_PANEL_CLUSTER
