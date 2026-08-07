// The library owns the poll task.
//
//   affa::CarminatDisplay display(link, clock);
//   display.onKey(&onKey, nullptr);          // callbacks FIRST
//   display.onComplete(&onDone, nullptr);
//   display.begin();                         // then begin()
//   affa::rtos::AffaTask task;
//   task.start(display);                     // then start() — and never call poll() again
//
//   // …and from ANY task, at any time, through the DISPLAY:
//   display.setTextStyled("HELLO", icon, src, fmt);
//
// THAT LAST LINE IS WHAT CHANGED IN 2.0, and it is the whole reason this class shrank.
//
// Until 2.0 AffaTask published its own render surface — setText, setTime, showMenu and
// eleven more — copied into a command queue with its own handle space (TxRequest) and its
// own translation table back to TxTicket. It covered ten of CarminatDisplay's twenty-two
// calls. Twelve renders were added to the panels over the following releases and NONE were
// mirrored, because mirroring one meant editing four places nobody was looking at. So an
// application that wanted a styled main line or the nav pane had to take the display
// directly — and then the owned task looked like an extra object with a second vocabulary,
// which is why thirteen of nineteen shipped examples turned it off (docs/REFACTOR-2.0.md
// §1.1).
//
// The boundary moved into AffaDisplayBase::enqueue() instead, where every render in every
// panel already funnels. So the display IS the thread-safe surface, for calls that exist now
// and calls written later, and this class is left with the one job its name claims: run
// poll() on a task of its own.
//
// WHAT IT DOES NOT CHANGE, AND MUST NOT. KeyCb still fires SYNCHRONOUSLY inside poll(),
// before any TX pumping, so key latency is still bounded by the poll period alone — it is
// NOT routed back to an application task through a queue. That is the acceptance criterion
// this design lives or dies by. What changes is only WHICH task the callback runs on, and it
// changes for the better: the owned task is not shared with a web server.
//
// THE PRICE, STATED PLAINLY: your callbacks run on the library's task, at priority
// AFFA_TASK_PRIO, on a AFFA_TASK_STACK-byte stack. A callback that blocks blocks the
// library. It cannot be prevented, so it is made visible — Status::pollLateMaxUs.
#pragma once

#include "../AffaConfig.h"

#if AFFA_ENABLE_TASK

#include "../core/AffaDisplayBase.h"

// The library builds with -Wundef so that a misspelled AFFA_* gate is a diagnostic. The
// IDF's own FreeRTOSConfig.h tests two dozen CONFIG_* macros it does not define, and this
// is the ONE translation unit in the library that includes it: without the push/pop, every
// build of src/rtos/ buries its own warnings under thirty lines of somebody else's.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wundef"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#pragma GCC diagnostic pop

namespace affa {
namespace rtos {

struct TaskOptions {
  uint16_t periodMs   = AFFA_TASK_PERIOD_MS;     // 2 — key latency is bounded by this
  uint16_t stack      = AFFA_TASK_STACK;         // 4096
  uint8_t  priority   = AFFA_TASK_PRIO;          // 2 — above the Arduino loop task
  int8_t   core       = AFFA_TASK_CORE;          // -1 = tskNO_AFFINITY
  const char* name    = "affa";
};

// A SNAPSHOT, published once per iteration by the owned task.
//
// The direct accessors (synced(), busy(), stats() …) still exist and are still correct
// from the owned task's own callbacks, but off-task they are reads racing a writer: Stats
// is a seven-field struct and reading it field by field can return a mixture of two
// moments. status() is the answer for every reader that is not the owned task.
struct Status {
  SyncState sync       = SyncState::Failed;
  // WHERE THE OPENING HAS GOT TO — print this one. `sync` answers the application's
  // question; this answers the person's. See the Phase comment in AffaTypes.h.
  Phase     phase      = Phase::Silent;
  // Times the panel has taken the session away since start(), and when it last did. A soak
  // that reports neither is a soak that cannot see fourteen deauthorizations.
  uint32_t  sessionsLost      = 0;
  uint32_t  lastSessionLossMs = 0;
  LossReason lastLossReason   = LossReason::None;
  bool      running    = false;
  bool      registered = false;
  bool      busy       = false;
  uint8_t   queued     = 0;   // transmit jobs waiting behind the active one
  uint8_t   posted     = 0;   // renders posted from another task, not yet admitted
  Stats     stats{};
  Result    lastResult = Result::Ok;   // acceptance verdict of the last render admitted

  uint32_t  stampMs        = 0;   // when the owned task published this
  uint32_t  iterations     = 0;   // poll cycles since start()
  uint32_t  pollLateMaxUs  = 0;   // worst iteration seen — a blocking callback shows HERE
  uint32_t  pollLateAtMs   = 0;   // ...and WHEN, which is what identifies the cause. A peak
                                  // at t=0 is WiFi associating; a peak that keeps moving is
                                  // a callback. Without the timestamp both look identical.
  uint32_t  postDropped    = 0;   // renders refused because the dispatch ring was full
  // WHICH CALLBACK BLOCKED, and how long for. `pollLateMaxUs` above says an iteration was
  // slow; these say whose fault it was. Print both.
  CbKind    slowestCb      = CbKind::None;
  uint32_t  slowestCbMs    = 0;
  uint32_t  slowestCbAtMs  = 0;
  uint32_t  cbOverruns     = 0;
  uint32_t  foreignPolls   = 0;   // poll() called by a task that is not the owner
  uint32_t  stackFreeBytes = 0;   // uxTaskGetStackHighWaterMark, for sizing AFFA_TASK_STACK
};

class AffaTask {
 public:
  AffaTask() = default;
  ~AffaTask();
  AffaTask(const AffaTask&)            = delete;
  AffaTask& operator=(const AffaTask&) = delete;

  // Creates the task and takes ownership of poll(). The display must already have been
  // begun and had its callbacks installed.
  //
  // false, with a log line saying which: display not begun, already started, or task
  // creation failed. NEVER a silent success that never polls — that is the exact failure
  // the old unconditional #error existed to prevent.
  //
  // IT DOES NOT TOUCH YOUR CALLBACKS. The pre-2.0 version took over
  // AffaDisplayBase::onComplete so it could translate TxTicket into its own TxRequest
  // handle. There is one handle space now, so there is nothing to translate and nothing to
  // take over: install onComplete on the display, before or after start(), and it is yours.
  bool start(AffaDisplayBase& d, const TaskOptions& opt = TaskOptions{});

  // Stops and joins. Pending-and-unstarted renders complete Cancelled; a message already on
  // the wire is allowed to finish, so no torn ISO-TP transfer is left behind. Safe to call
  // twice, and safe from a callback (i.e. from the owned task) — where it cannot join
  // itself, so it requests the stop and returns.
  void stop();

  bool running() const { return _handle != nullptr && !_stop; }

  // ---- observation --------------------------------------------------------
  Status status() const;
  void   resetPeaks();         // clears pollLateMaxUs

 private:
  static void  trampoline(void* self);
  static void* currentTaskId();

  void run();
  void publish(uint32_t iterUs);
  bool onOwnedTask() const;

  AffaDisplayBase* _d      = nullptr;
  TaskHandle_t     _handle = nullptr;
  TaskOptions      _opt{};
  volatile bool    _stop   = false;

  uint32_t          _iterations    = 0;
  uint32_t          _pollLateMaxUs = 0;
  uint32_t          _pollLateAtMs  = 0;
  uint32_t          _lateLogMs     = 0;

  // Seqlock, not a mutex: the publisher is one task and every reader is a copy. An odd
  // sequence means "mid-write, try again" — which is why a reader can be lock-free and
  // still never observe half of two different moments.
  volatile uint32_t _seq = 0;
  Status            _pub{};
};

}  // namespace rtos
}  // namespace affa

#endif  // AFFA_ENABLE_TASK
