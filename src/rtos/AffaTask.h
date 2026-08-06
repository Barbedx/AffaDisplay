// The library owns the poll task.
//
//   affa::CarminatDisplay display(link, clock);
//   display.onKey(&onKey, nullptr);          // callbacks FIRST
//   display.begin();                         // then begin()
//   affa::rtos::AffaTask task;
//   task.start(display);                     // then start() — and never call poll() again
//
//   // from ANY task, at any time:
//   task.setText("HELLO");
//
// WHAT THIS BUYS. The threading contract used to be one sentence — "call poll() from one
// task and never block that task" — and it is violated by ADDITION: by the next feature
// somebody puts in loop(), not by the code that was reviewed. Three such additions in one
// day cost one consumer 401 timed-out renders, a latched BUS_OFF and two lost registrations
// (docs/CR-0.3.0-OWNED-TASK.md §2). The owned task removes the whole class.
//
// WHAT IT DOES NOT CHANGE, AND MUST NOT. KeyCb still fires SYNCHRONOUSLY inside poll(),
// before any TX pumping, so key latency is still bounded by the poll period alone — it is
// NOT routed back to an application task through a queue. That is the acceptance criterion
// this design lives or dies by; test/test_owned_task and examples/19_owned_task both assert
// it. What changes is only WHICH task the callback runs on, and it changes for the better:
// the owned task is not shared with a web server.
//
// THE PRICE, STATED PLAINLY: your callbacks now run on the library's task, at priority
// AFFA_TASK_PRIO, on a AFFA_TASK_STACK-byte stack. A callback that blocks blocks the
// library. It cannot be prevented, so it is made visible — Status::pollLateMaxUs.
#pragma once

#include "../AffaConfig.h"

#if AFFA_ENABLE_TASK

#include "AffaCommand.h"
#include "../core/AffaDisplayBase.h"

// The library builds with -Wundef so that a misspelled AFFA_* gate is a diagnostic. The
// IDF's own FreeRTOSConfig.h tests two dozen CONFIG_* macros it does not define, and this
// is the ONE translation unit in the library that includes it: without the push/pop, every
// build of src/rtos/ buries its own warnings under thirty lines of somebody else's.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wundef"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#pragma GCC diagnostic pop

namespace affa {
namespace rtos {

struct TaskOptions {
  uint16_t periodMs   = AFFA_TASK_PERIOD_MS;     // 2 — key latency is bounded by this
  uint16_t stack      = AFFA_TASK_STACK;         // 4096
  uint8_t  priority   = AFFA_TASK_PRIO;          // 2 — above the Arduino loop task
  uint8_t  queueDepth = AFFA_TASK_QUEUE_DEPTH;   // 8
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
  uint8_t   cmdQueued  = 0;   // commands posted and not yet drained
  Stats     stats{};
  Result    lastResult = Result::Ok;   // acceptance verdict of the last drained render

  uint32_t  stampMs        = 0;   // when the owned task published this
  uint32_t  iterations     = 0;   // poll cycles since start()
  uint32_t  pollLateMaxUs  = 0;   // worst iteration seen — a blocking callback shows HERE
  uint32_t  pollLateAtMs   = 0;   // ...and WHEN, which is what identifies the cause. A peak
                                  // at t=0 is WiFi associating; a peak that keeps moving is
                                  // a callback. Without the timestamp both look identical.
  uint32_t  queueDropped   = 0;   // commands refused because the queue was full
  uint32_t  foreignPolls   = 0;   // poll() called by a task that is not the owner
  uint32_t  stackFreeBytes = 0;   // uxTaskGetStackHighWaterMark, for sizing AFFA_TASK_STACK
};

class AffaTask {
 public:
  // Reported for every completed render, on the OWNED TASK, with the handle the caller was
  // given. `req == kNoRequest` means the render was made by the library itself (a menu
  // redraw, the close banner) — real, and not something the application asked for.
  using CompleteCb = void (*)(TxRequest req, Result r, void* ctx);

  AffaTask() = default;
  ~AffaTask();
  AffaTask(const AffaTask&)            = delete;
  AffaTask& operator=(const AffaTask&) = delete;

  // Install this BEFORE start(). It replaces AffaDisplayBase::onComplete, which AffaTask
  // takes over — in owned-task mode there is one handle space (TxRequest) and this is how
  // you observe it.
  void onComplete(CompleteCb cb, void* ctx);

  // Creates the task and takes ownership of poll(). The display must already have been
  // begun and had its callbacks installed.
  //
  // false, with a log line saying which: display not begun, already started, queue or task
  // creation failed. NEVER a silent success that never polls — that is the exact failure
  // the old unconditional #error existed to prevent.
  bool start(AffaDisplayBase& d, const TaskOptions& opt = TaskOptions{});

  // Stops and joins. Pending-and-unstarted renders complete Cancelled; a message already on
  // the wire is allowed to finish, so no torn ISO-TP transfer is left behind. Safe to call
  // twice, and safe from a callback (i.e. from the owned task) — where it cannot join
  // itself, so it requests the stop and returns.
  void stop();

  bool running() const { return _handle != nullptr && !_stop; }

  // ---- renders, callable FROM ANY TASK ------------------------------------
  // Each returns a TxRequest: non-zero if accepted, kNoRequest if refused. NEVER blocks,
  // and never silently drops — a refusal increments Status::queueDropped and is logged.
  //
  // Called from the owned task (i.e. from inside your own KeyCb, SyncCb or CompleteCb) they
  // take the DIRECT path instead of the queue. That is not an optimisation: self-enqueueing
  // from a callback would deadlock against a full queue, and docs/API.md §4.3's documented
  // pattern — poll() -> KeyCb -> abortPending() -> CompleteCb — is exactly that shape.
  TxRequest setText(const char* text, uint8_t digit = 255);
  TxRequest setTime(const char* hhmm);
  TxRequest setPower(bool on);
  TxRequest showMenu(const char* header, const char* row0, const char* row1,
                     uint8_t scroll = 0x0B);
  TxRequest highlightItem(uint8_t row);
  TxRequest showPopupText(const char* text, uint8_t icon = 0x09, uint8_t srcIcon = 0xFF,
                          uint8_t fmt = 0x60);
  TxRequest hidePopup();
  TxRequest showFullscreenText(const char* l1, const char* l2, const char* l3);
  TxRequest showConfirmBox(const char* caption, const char* row0, const char* row1);
  TxRequest showInfoPopup(const char* l1, const char* l2, const char* l3);

  // The Local half fires KeyCb on the owned task, exactly as a key off the wire does.
  TxRequest pressKey(Key k, KeyEdge e, KeySource src = KeySource::Local);

  TxRequest abortPending();
  TxRequest abortAll();
  TxRequest resync();          // re-runs begin() on the owned task

  // ---- observation --------------------------------------------------------
  Status status() const;
  void   resetPeaks();         // clears pollLateMaxUs and queueDropped

 private:
  static void  trampoline(void* self);
  static void* currentTaskId();
  static void  onDisplayComplete(TxTicket t, Result r, void* ctx);

  void      run();
  void      drainCommands();
  void      publish(uint32_t iterUs);
  bool      onOwnedTask() const;
  TxRequest nextRequest();
  TxRequest submit(Command& c);
  TxRequest applyHere(Command& c);      // the direct path; owned task only

  AffaDisplayBase* _d      = nullptr;
  TaskHandle_t     _handle = nullptr;
  QueueHandle_t    _q      = nullptr;
  TaskOptions      _opt{};
  volatile bool    _stop   = false;

  CompleteCb _cb  = nullptr;
  void*      _ctx = nullptr;

  RequestTable<AFFA_TX_QUEUE_DEPTH + 2> _map;
  volatile uint32_t _nextReq      = 1;
  volatile uint32_t _dropped      = 0;
  uint32_t          _iterations   = 0;
  uint32_t          _pollLateMaxUs = 0;
  uint32_t          _pollLateAtMs  = 0;
  uint32_t          _lateLogMs    = 0;
  Result            _lastResult   = Result::Ok;

  // Seqlock, not a mutex: the publisher is one task and every reader is a copy. An odd
  // sequence means "mid-write, try again" — which is why a reader can be lock-free and
  // still never observe half of two different moments.
  volatile uint32_t _seq = 0;
  Status            _pub{};
};

}  // namespace rtos
}  // namespace affa

#endif  // AFFA_ENABLE_TASK
