// SPDX-License-Identifier: MIT
#pragma once
#include "ICanLink.h"

namespace affa {

// How the owner task sleeps between polls, given what ICanLink::waitRx() did. Portable and
// host-tested; src/rtos/AffaTask.cpp is the one caller.
//
//   Unsupported  the link cannot wait: sleep the period, exactly as before.
//   Timeout      the link slept the whole period itself: poll now.
//   Ready        a frame woke us early — a panel key, an ACK. Poll NOW, which is the point.
//
// A STORM MUST NOT HOLD THE TASK AWAKE. Under an unanswered panel's retransmits every wait
// returns Ready at once, and a priority-2 task that never blocks starves everything below it,
// the idle task's watchdog included. So only kEarlyPolls early wakes in a row poll at once;
// the next yields one tick first. A quiet bus still wakes on its first frame instantly.
enum class WakeAction : uint8_t { SleepPeriod, PollNow, YieldThenPoll };

class WakePacer {
 public:
  static constexpr uint8_t kEarlyPolls = 1;

  WakeAction after(RxWait w) {
    switch (w) {
      case RxWait::Unsupported: _early = 0; return WakeAction::SleepPeriod;
      case RxWait::Timeout:     _early = 0; return WakeAction::PollNow;
      case RxWait::Ready:
        if (_early < kEarlyPolls) { ++_early; return WakeAction::PollNow; }
        _early = 0;
        return WakeAction::YieldThenPoll;
    }
    return WakeAction::SleepPeriod;
  }

 private:
  uint8_t _early = 0;
};

}  // namespace affa
