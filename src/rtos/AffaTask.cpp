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
void* const kOwnerClaiming = reinterpret_cast<void*>(1);

}  // namespace

void* AffaTask::currentTaskId() {
  return static_cast<void*>(xTaskGetCurrentTaskHandle());
}

bool AffaTask::onOwnedTask() const {
  return _handle != nullptr && xTaskGetCurrentTaskHandle() == _handle;
}

AffaTask::~AffaTask() { stop(); }

void AffaTask::onComplete(CompleteCb cb, void* ctx) { _cb = cb; _ctx = ctx; }

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

bool AffaTask::start(AffaDisplayBase& d, const TaskOptions& opt) {
  // 6.5 — a second task polling the same display is the same corruption as an application
  // that never stopped calling poll(). Refuse, loudly.
  if (_handle) {
    AFFA_LOGE(kTag, "start() called twice — the task is already running");
    return false;
  }
  // 6.1 — begin() resets the FSMs and arms the deadlines. Polling a display that was never
  // begun transmits nothing and reports no reason, which is precisely the silent failure
  // this whole mode exists to remove.
  if (!d.begun()) {
    AFFA_LOGE(kTag, "start() before begin() — call display.begin() first");
    return false;
  }

  _opt = opt;
  if (_opt.periodMs == 0) _opt.periodMs = 1;
  if (_opt.queueDepth == 0) _opt.queueDepth = 1;

  _d           = &d;
  _stop        = false;
  _iterations  = 0;
  _pollLateMaxUs = 0;
  _dropped     = 0;
  _lastResult  = Result::Ok;
  _map.clear();

  _q = xQueueCreate(_opt.queueDepth, sizeof(Command));
  if (!_q) {
    AFFA_LOGE(kTag, "command queue (%u x %u B) would not allocate",
              static_cast<unsigned>(_opt.queueDepth),
              static_cast<unsigned>(sizeof(Command)));
    _d = nullptr;
    return false;
  }

  // AffaTask owns the completion callback: in owned-task mode the application's handle is
  // a TxRequest, so the TxTicket -> TxRequest translation has to happen here or the two
  // spaces leak into each other.
  d.onComplete(&AffaTask::onDisplayComplete, this);
  d.setPollOwner(kOwnerClaiming, &AffaTask::currentTaskId);

  // PinnedToCore unconditionally: ESP-IDF provides it on unicore targets too, where
  // tskNO_AFFINITY is the only meaningful answer anyway. One call site beats an #if on a
  // macro whose spelling changed between IDF 4 and 5.
  const BaseType_t ok =
      xTaskCreatePinnedToCore(&AffaTask::trampoline, _opt.name, _opt.stack, this,
                              _opt.priority, &_handle,
                              _opt.core < 0 ? tskNO_AFFINITY
                                            : static_cast<BaseType_t>(_opt.core));

  // 6.7 — a heap failure here used to be indistinguishable from success: the flag was set,
  // nothing polled, and the panel simply never drew.
  if (ok != pdPASS || !_handle) {
    AFFA_LOGE(kTag, "task creation failed (stack %u, prio %u) — heap exhausted",
              static_cast<unsigned>(_opt.stack), static_cast<unsigned>(_opt.priority));
    _handle = nullptr;
    d.setPollOwner(nullptr, nullptr);
    d.onComplete(nullptr, nullptr);
    vQueueDelete(_q);
    _q = nullptr;
    _d = nullptr;
    return false;
  }

  AFFA_LOGI(kTag, "owned task up: period %u ms, prio %u, queue %u, stack %u",
            static_cast<unsigned>(_opt.periodMs), static_cast<unsigned>(_opt.priority),
            static_cast<unsigned>(_opt.queueDepth), static_cast<unsigned>(_opt.stack));
  return true;
}

void AffaTask::stop() {
  if (!_handle) return;
  _stop = true;
  // 11.3 — stop() from a callback is on the owned task, which cannot join itself. Request
  // it and return; the iteration that is running will not start another.
  if (onOwnedTask()) return;
  while (_handle) vTaskDelay(1);
}

// ---------------------------------------------------------------------------
// The task
// ---------------------------------------------------------------------------

void AffaTask::trampoline(void* self) { static_cast<AffaTask*>(self)->run(); }

void AffaTask::run() {
  // Claim ownership FIRST. From here poll() refuses every other task.
  _d->setPollOwner(currentTaskId(), &AffaTask::currentTaskId);

  const TickType_t period = pdMS_TO_TICKS(_opt.periodMs) ? pdMS_TO_TICKS(_opt.periodMs)
                                                         : 1;
  TickType_t last = xTaskGetTickCount();

  while (!_stop) {
    const uint32_t t0 = nowUs();

    // ONE ITERATION, EXACTLY. Commands drain BEFORE poll(), so a render posted during this
    // period is pumped during this period rather than the next one. Key latency is
    // unaffected either way — keys arrive in poll()'s RX phase regardless — but render
    // latency improves by one period for free.
    drainCommands();
    _d->poll();

    ++_iterations;
    publish(nowUs() - t0);

    vTaskDelayUntil(&last, period);
  }

  // ---- teardown (6.6) -----------------------------------------------------
  // Unstarted renders are dropped; a message already on the wire is allowed to finish, so
  // the panel is never left holding half an ISO-TP transfer. Bounded: a panel that has
  // stopped acknowledging must not stop us from shutting down.
  _d->abortPending();
  const uint32_t deadline = nowMs() + 2u * AFFA_ACK_TIMEOUT_MS;
  while (_d->busy() && !expired(nowMs(), deadline)) {
    _d->poll();
    vTaskDelay(period);
  }

  Command c;
  while (xQueueReceive(_q, &c, 0) == pdTRUE)
    if (_cb) _cb(c.req, Result::Cancelled, _ctx);

  _d->setPollOwner(nullptr, nullptr);
  _d->onComplete(nullptr, nullptr);
  QueueHandle_t q = _q;
  _q = nullptr;
  vQueueDelete(q);

  AFFA_LOGI(kTag, "owned task stopped after %lu iterations",
            static_cast<unsigned long>(_iterations));

  _handle = nullptr;              // stop() is waiting on exactly this
  vTaskDelete(nullptr);
}

void AffaTask::drainCommands() {
  // Bounded by the queue depth, not by "until empty": a producer that posts faster than we
  // drain must not be able to hold this task inside the drain loop for ever, because the
  // poll() below it is what delivers keys.
  Command c;
  for (uint8_t i = 0; i < _opt.queueDepth; ++i) {
    if (xQueueReceive(_q, &c, 0) != pdTRUE) return;
    applyHere(c);
  }
}

TxRequest AffaTask::applyHere(Command& c) {
  TxTicket     t = kNoTicket;
  const Result r = applyCommand(*_d, c, t);
  _lastResult    = r;

  if (r != Result::Ok) {
    // A render the panel refused NOW (NoSync, QueueFull, NotSupported) is reported through
    // the completion callback with its reason, so a posted render always ends in exactly
    // one verdict — the caller cannot see the acceptance answer of a queued call.
    if (_cb && c.req != kNoRequest) _cb(c.req, r, _ctx);
    return kNoRequest;
  }
  if (t != kNoTicket) _map.bind(t, c.req);
  return c.req;
}

// ---------------------------------------------------------------------------
// Completion
// ---------------------------------------------------------------------------

void AffaTask::onDisplayComplete(TxTicket t, Result r, void* ctx) {
  AffaTask* self = static_cast<AffaTask*>(ctx);
  // kNoRequest for a render the LIBRARY made — a menu redraw, the close banner. Real, and
  // not something the application asked for; forwarded rather than swallowed so a consumer
  // can see its panel working even when it did not ask.
  const TxRequest req = self->_map.take(t);
  if (self->_cb) self->_cb(req, r, self->_ctx);
}

// ---------------------------------------------------------------------------
// Posting
// ---------------------------------------------------------------------------

TxRequest AffaTask::nextRequest() {
  uint32_t v = __atomic_add_fetch(&_nextReq, 1, __ATOMIC_RELAXED);
  v &= 0xFFFFu;
  if (v == 0) v = __atomic_add_fetch(&_nextReq, 1, __ATOMIC_RELAXED) & 0xFFFFu;
  return static_cast<TxRequest>(v ? v : 1);
}

TxRequest AffaTask::submit(Command& c) {
  if (!_d || !_handle || _stop) return kNoRequest;
  c.req = nextRequest();

  // 6.8 — ALREADY ON THE OWNED TASK. A render from inside a KeyCb takes the direct path;
  // self-enqueueing would deadlock the library's own documented preemption pattern against
  // a full queue.
  if (onOwnedTask()) return applyHere(c);

  // 6.3 — never blocks (timeout 0) and never silently drops. The counter is surfaced in
  // Status::queueDropped and logged by the owned task, not from here: a log sink is
  // application code and this is an arbitrary caller's task.
  if (xQueueSend(_q, &c, 0) != pdTRUE) {
    __atomic_add_fetch(&_dropped, 1u, __ATOMIC_RELAXED);
    return kNoRequest;
  }
  return c.req;
}

// ---------------------------------------------------------------------------
// The render surface
// ---------------------------------------------------------------------------

TxRequest AffaTask::setText(const char* text, uint8_t digit) {
  Command c; c.op = Op::SetText; setArg(c.s0, text); c.a = digit;
  return submit(c);
}
TxRequest AffaTask::setTime(const char* hhmm) {
  Command c; c.op = Op::SetTime; setArg(c.s0, hhmm);
  return submit(c);
}
TxRequest AffaTask::setPower(bool on) {
  Command c; c.op = Op::SetPower; c.a = on ? 1 : 0;
  return submit(c);
}
TxRequest AffaTask::showMenu(const char* header, const char* row0, const char* row1,
                             uint8_t scroll) {
  Command c; c.op = Op::ShowMenu;
  setArg(c.s0, header); setArg(c.s1, row0); setArg(c.s2, row1); c.a = scroll;
  return submit(c);
}
TxRequest AffaTask::highlightItem(uint8_t row) {
  Command c; c.op = Op::HighlightItem; c.a = row;
  return submit(c);
}
TxRequest AffaTask::showPopupText(const char* text, uint8_t icon, uint8_t srcIcon,
                                  uint8_t fmt) {
  Command c; c.op = Op::ShowPopupText;
  setArg(c.s0, text); c.a = icon; c.b = srcIcon; c.c = fmt;
  return submit(c);
}
TxRequest AffaTask::hidePopup() {
  Command c; c.op = Op::HidePopup;
  return submit(c);
}
TxRequest AffaTask::showFullscreenText(const char* l1, const char* l2, const char* l3) {
  Command c; c.op = Op::ShowFullscreenText;
  setArg(c.s0, l1); setArg(c.s1, l2); setArg(c.s2, l3);
  return submit(c);
}
TxRequest AffaTask::hideFullscreenText() {
  Command c; c.op = Op::HideFullscreenText;
  return submit(c);
}
TxRequest AffaTask::showConfirmBox(const char* caption, const char* row0, const char* row1) {
  Command c; c.op = Op::ShowConfirmBox;
  setArg(c.s0, caption); setArg(c.s1, row0); setArg(c.s2, row1);
  return submit(c);
}
TxRequest AffaTask::showInfoPopup(const char* l1, const char* l2, const char* l3) {
  Command c; c.op = Op::ShowInfoPopup;
  setArg(c.s0, l1); setArg(c.s1, l2); setArg(c.s2, l3);
  return submit(c);
}
TxRequest AffaTask::hideInfoPopup() {
  Command c; c.op = Op::HideInfoPopup;
  return submit(c);
}
TxRequest AffaTask::pressKey(Key k, KeyEdge e, KeySource src) {
  Command c; c.op = Op::PressKey;
  const uint16_t raw = static_cast<uint16_t>(k);
  c.a = static_cast<uint8_t>(raw >> 8);
  c.b = static_cast<uint8_t>(raw & 0xFF);
  c.c = (e == KeyEdge::Hold) ? 1 : 0;
  c.d = static_cast<uint8_t>(src);
  return submit(c);
}
TxRequest AffaTask::abortPending() { Command c; c.op = Op::AbortPending; return submit(c); }
TxRequest AffaTask::abortAll()     { Command c; c.op = Op::AbortAll;     return submit(c); }
TxRequest AffaTask::resync()       { Command c; c.op = Op::Resync;       return submit(c); }

// ---------------------------------------------------------------------------
// Status
// ---------------------------------------------------------------------------

void AffaTask::publish(uint32_t iterUs) {
  if (iterUs > _pollLateMaxUs) _pollLateMaxUs = iterUs;

  // 6.4 — a user callback that blocks cannot be prevented, only made visible. One line per
  // second at most: a callback that blocks every iteration would otherwise produce the log
  // storm that hides the first one.
  const uint32_t lateUs = static_cast<uint32_t>(AFFA_TASK_LATE_FACTOR) *
                          static_cast<uint32_t>(_opt.periodMs) * 1000u;
  const uint32_t ms = nowMs();
  if (iterUs > lateUs && expired(ms, _lateLogMs)) {
    _lateLogMs = ms + 1000;
    AFFA_LOGW(kTag, "iteration took %lu us (period %u ms) — a callback is blocking the "
              "owned task", static_cast<unsigned long>(iterUs),
              static_cast<unsigned>(_opt.periodMs));
  }

  const uint32_t dropped = __atomic_load_n(&_dropped, __ATOMIC_RELAXED);
  if (dropped != _pub.queueDropped && expired(ms, _lateLogMs)) {
    _lateLogMs = ms + 1000;
    AFFA_LOGW(kTag, "command queue full: %lu renders refused (depth %u)",
              static_cast<unsigned long>(dropped),
              static_cast<unsigned>(_opt.queueDepth));
  }

  Status s;
  s.sync           = _d->syncState();
  s.running        = !_stop;
  s.registered     = _d->registered();
  s.busy           = _d->busy();
  s.queued         = _d->queued();
  s.cmdQueued      = static_cast<uint8_t>(uxQueueMessagesWaiting(_q));
  s.stats          = _d->stats();
  s.lastResult     = _lastResult;
  s.stampMs        = ms;
  s.iterations     = _iterations;
  s.pollLateMaxUs  = _pollLateMaxUs;
  s.queueDropped   = dropped;
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
  __atomic_store_n(&_dropped, 0u, __ATOMIC_RELAXED);
}

}  // namespace rtos
}  // namespace affa

#endif  // AFFA_ENABLE_TASK
