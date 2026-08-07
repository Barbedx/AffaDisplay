#include "AffaTask.h"

// THE ENTIRE BODY IS INSIDE THE GATE, exactly like every other optional .cpp in this
// library: the Library Dependency Finder compiles every .cpp under a lib_deps library and
// the preprocessor is the only mechanism that can remove a translation unit. With
// AFFA_ENABLE_TASK=0 this file is an empty object and costs nothing.
#if AFFA_ENABLE_TASK

#include "../util/AffaLog.h"
#include <esp_timer.h>

namespace affa {
namespace rtos {
namespace {

constexpr const char* kTag = "AFFA-T";

inline uint32_t nowUs() { return static_cast<uint32_t>(esp_timer_get_time()); }
inline uint32_t nowMs() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }

// Set in start() BEFORE the task exists, so there is no window in which an application's
// leftover poll() call is accepted because the owner has not been claimed yet. No task
// handle is ever 1, so every poll() is refused until the owned task claims ownership as
// its first action.
//
// It has a second effect since 2.0 and it is the safe one: renders called during that
// window are POSTED to the dispatch ring rather than applied, because AffaDisplayBase sees
// an owner it is not. They drain on the owned task's first iteration.
void* const kOwnerClaiming = reinterpret_cast<void*>(1);

}  // namespace

void* AffaTask::currentTaskId() {
  return static_cast<void*>(xTaskGetCurrentTaskHandle());
}

bool AffaTask::onOwnedTask() const {
  return _handle != nullptr && xTaskGetCurrentTaskHandle() == _handle;
}

AffaTask::~AffaTask() { stop(); }

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

bool AffaTask::start(AffaDisplayBase& d, const TaskOptions& opt) {
  // A second task polling the same display is the same corruption as an application that
  // never stopped calling poll(). Refuse, loudly.
  if (_handle) {
    AFFA_LOGE(kTag, "start() called twice — the task is already running");
    return false;
  }
  // begin() resets the FSMs and arms the deadlines. Polling a display that was never begun
  // transmits nothing and reports no reason, which is precisely the silent failure this
  // whole mode exists to remove.
  if (!d.begun()) {
    AFFA_LOGE(kTag, "start() before begin() — call display.begin() first");
    return false;
  }

  _opt = opt;
  if (_opt.periodMs == 0) _opt.periodMs = 1;

  _d             = &d;
  _stop          = false;
  _iterations    = 0;
  _pollLateMaxUs = 0;
  _pollLateAtMs  = 0;

  // NO onComplete TAKEOVER. The pre-2.0 version installed its own here so it could map
  // TxTicket onto a second handle space; there is one handle space now, so the
  // application's completion callback is the application's and start() does not touch it.
  d.setPollOwner(kOwnerClaiming, &AffaTask::currentTaskId);

  // PinnedToCore unconditionally: ESP-IDF provides it on unicore targets too, where
  // tskNO_AFFINITY is the only meaningful answer anyway. One call site beats an #if on a
  // macro whose spelling changed between IDF 4 and 5.
  const BaseType_t ok =
      xTaskCreatePinnedToCore(&AffaTask::trampoline, _opt.name, _opt.stack, this,
                              _opt.priority, &_handle,
                              _opt.core < 0 ? tskNO_AFFINITY
                                            : static_cast<BaseType_t>(_opt.core));

  // A heap failure here used to be indistinguishable from success: the flag was set,
  // nothing polled, and the panel simply never drew.
  if (ok != pdPASS || !_handle) {
    AFFA_LOGE(kTag, "task creation failed (stack %u, prio %u) — heap exhausted",
              static_cast<unsigned>(_opt.stack), static_cast<unsigned>(_opt.priority));
    _handle = nullptr;
    d.setPollOwner(nullptr, nullptr);
    _d = nullptr;
    return false;
  }

  AFFA_LOGI(kTag, "owned task up: period %u ms, prio %u, stack %u, dispatch %u",
            static_cast<unsigned>(_opt.periodMs), static_cast<unsigned>(_opt.priority),
            static_cast<unsigned>(_opt.stack),
            static_cast<unsigned>(AFFA_DISPATCH_DEPTH));
  return true;
}

void AffaTask::stop() {
  if (!_handle) return;
  _stop = true;
  // stop() from a callback is on the owned task, which cannot join itself. Request it and
  // return; the iteration that is running will not start another.
  if (onOwnedTask()) return;
  while (_handle) vTaskDelay(1);
}

// ---------------------------------------------------------------------------
// The task
// ---------------------------------------------------------------------------

void AffaTask::trampoline(void* self) { static_cast<AffaTask*>(self)->run(); }

void AffaTask::run() {
  // Claim ownership FIRST. From here poll() refuses every other task, and every render from
  // every other task crosses through the dispatch ring instead of racing this one.
  _d->setPollOwner(currentTaskId(), &AffaTask::currentTaskId);

  const TickType_t period = pdMS_TO_TICKS(_opt.periodMs) ? pdMS_TO_TICKS(_opt.periodMs)
                                                         : 1;
  TickType_t last = xTaskGetTickCount();

  while (!_stop) {
    const uint32_t t0 = nowUs();

    // ONE ITERATION, EXACTLY. The command drain that used to sit here is inside poll() now
    // — between pumpSync() and pumpTx(), which is where a posted render is judged against
    // this poll's sync state and still leaves during this period.
    _d->poll();

    ++_iterations;
    publish(nowUs() - t0);

    vTaskDelayUntil(&last, period);
  }

  // ---- teardown -----------------------------------------------------------
  // Unstarted renders are dropped; a message already on the wire is allowed to finish, so
  // the panel is never left holding half an ISO-TP transfer. Bounded: a panel that has
  // stopped acknowledging must not stop us from shutting down.
  _d->abortPending();
  const uint32_t deadline = nowMs() + 2u * AFFA_ACK_TIMEOUT_MS;
  while (_d->busy() && !expired(nowMs(), deadline)) {
    _d->poll();
    vTaskDelay(period);
  }

  _d->setPollOwner(nullptr, nullptr);

  AFFA_LOGI(kTag, "owned task stopped after %lu iterations",
            static_cast<unsigned long>(_iterations));

  _handle = nullptr;              // stop() is waiting on exactly this
  vTaskDelete(nullptr);
}

// ---------------------------------------------------------------------------
// Status
// ---------------------------------------------------------------------------

void AffaTask::publish(uint32_t iterUs) {
  const uint32_t ms = nowMs();
  if (iterUs > _pollLateMaxUs) { _pollLateMaxUs = iterUs; _pollLateAtMs = ms; }

  // A user callback that blocks cannot be prevented, only made visible. One line per second
  // at most: a callback that blocks every iteration would otherwise produce the log storm
  // that hides the first one.
  const uint32_t lateUs = static_cast<uint32_t>(AFFA_TASK_LATE_FACTOR) *
                          static_cast<uint32_t>(_opt.periodMs) * 1000u;
  if (iterUs > lateUs && expired(ms, _lateLogMs)) {
    _lateLogMs = ms + 1000;
    AFFA_LOGW(kTag, "iteration took %lu us (period %u ms) — a callback is blocking the "
              "owned task", static_cast<unsigned long>(iterUs),
              static_cast<unsigned>(_opt.periodMs));
  }

  const uint32_t dropped = _d->dispatchDropped();
  if (dropped != _pub.postDropped && expired(ms, _lateLogMs)) {
    _lateLogMs = ms + 1000;
    AFFA_LOGW(kTag, "dispatch ring full: %lu renders refused (depth %u)",
              static_cast<unsigned long>(dropped),
              static_cast<unsigned>(AFFA_DISPATCH_DEPTH));
  }

  Status s;
  s.sync              = _d->syncState();
  s.phase             = _d->phase();
  s.sessionsLost      = _d->sessionsLost();
  s.lastSessionLossMs = _d->lastSessionLossMs();
  s.lastLossReason    = _d->lastLossReason();
  s.running        = !_stop;
  s.registered     = _d->registered();
  s.busy           = _d->busy();
  s.queued         = _d->queued();
  s.posted         = _d->dispatchQueued();
  s.stats          = _d->stats();
  s.lastResult     = _d->lastResult();
  s.stampMs        = ms;
  s.iterations     = _iterations;
  s.pollLateMaxUs  = _pollLateMaxUs;
  s.pollLateAtMs   = _pollLateAtMs;
  s.postDropped    = dropped;
  s.slowestCb      = _d->slowestCallback();
  s.slowestCbMs    = _d->slowestCallbackMs();
  s.slowestCbAtMs  = _d->slowestCallbackAtMs();
  s.cbOverruns     = _d->callbackOverruns();
  s.foreignPolls   = _d->foreignPolls();
  s.stackFreeBytes = uxTaskGetStackHighWaterMark(nullptr);

  __atomic_add_fetch(&_seq, 1u, __ATOMIC_RELAXED);   // odd: mid-write
  __sync_synchronize();
  _pub = s;
  __sync_synchronize();
  __atomic_add_fetch(&_seq, 1u, __ATOMIC_RELAXED);   // even: stable
}

Status AffaTask::status() const {
  Status out;
  // Bounded retry, not a spin: the publisher runs for microseconds every period, so two
  // attempts is already generous. Past that, return what we have rather than block the
  // caller — a status read must never be able to hang an HTTP handler.
  for (uint8_t i = 0; i < 8; ++i) {
    const uint32_t s1 = __atomic_load_n(&_seq, __ATOMIC_RELAXED);
    if (s1 & 1u) continue;
    __sync_synchronize();
    out = _pub;
    __sync_synchronize();
    if (__atomic_load_n(&_seq, __ATOMIC_RELAXED) == s1) break;
  }
  return out;
}

void AffaTask::resetPeaks() {
  _pollLateMaxUs = 0;
  _pollLateAtMs  = 0;
  if (_d) _d->resetCallbackPeak();
}

}  // namespace rtos
}  // namespace affa

#endif  // AFFA_ENABLE_TASK
