# Notes — the three things that are not bytes

`docs/WIRE.md` is the wire, generated from the golden vectors CI asserts. This file is the
rest: what is **not** known, what went **wrong** and why, and the incidents that shaped the
threading model. Every claim here is either a thing that happened or a thing we admit we
cannot answer.

Everything else — ~8 000 lines across nine documents — was deleted on 2026-08-08. It was
inference, third-party source reconstruction, and prose transcription of captures we still
have. Raw evidence lives in `docs/captures/`; what has been seen on real
glass lives in `docs/BENCH-VERIFIED.md`.

---

## 1. What we do not know

### 1.1 The cluster's opening cannot complete  [BLOCKER]

Both `queueHello()` call sites live inside one branch of `handleSyncFrame()`:

```cpp
if (f.data[0] == 0x61 && f.data[1] == 0x11) { … queueHello(now); … }
```

**There is no `61 11` anywhere in the cluster capture.** The cluster sends only `3CF 1 69`,
so the burst is never queued, registration never follows, and no `SyncProfile` field can
change that — the trigger lives in the FSM, not in the data.

What the one capture shows is the radio driving throughout:

```
3AF 2  59 00                     radio alive
3AF 2  5A 01                     radio sync request
3CF 1  69                        cluster answers
3AF 8  50 29 00 23 00 00 00 69   radio hello
```

So the trigger is either the `69` or nothing at all — our own request being enough.
**Which of the two is not decidable from one sample.** It is recorded rather than guessed,
**and as of 2026-08-08 nothing guards it.** The test that deliberately passed on the broken
behaviour went with the rest of the behavioural suite; this paragraph is now the only record.

Closing it needs a cluster on a bench, or an owner's ruling.

### 1.2 Ten of the fourteen nav-header bytes

`carminat::kNavHeader` is sent verbatim because verbatim is what renders. `[0] [1]` are the
command, `[12] [13]` are the 48 × 48 geometry, and the other ten are unmeasured. Bytes
`[4..10]` held `"ABCDEF\0"` in the capture and are **confirmed not to be text** — ASCII
written there puts nothing on the glass.

`examples/17_mediascreen` has the sweep card: `/api/cmd?op=navhdr&bN=0xVV`, one byte at a
time, logging what went out. There is no automated sweep, because **the panel ACKs a screen
it never lights** — 256 scripted steps produce 256 rows of `ok` and no information. The
oracle is a person looking at glass.

### 1.3 The `0x7F` text-plus-icons flavour

Documented in `docs/WIRE.md` prose only, never emitted. Our reconstructed copy contradicted
the only real capture at two bytes. If icons are implemented, use the captured bytes and a
bench — not the archive source. See §2, entry six.

---

## 2. How this project gets things wrong

Worth reading before writing code here. **Every protocol bug found so far has been the same
shape: a special case standing in for a general rule.**

| what was written | what the captures actually say |
|---|---|
| `61 11 01` is discovery-only | any complete `61 11 xx` is the same request |
| tear down only on `61 11 01` | any `61 11` while registered voids the session |
| registration happens on the first render | registration is part of the opening |
| the hello answers the first request | our `BA` first; the *next* request draws it |

Four times, the same error: encoding the case in front of me instead of the law the data
states. The captures were unambiguous each time.

**A fifth was found in a TEST, 2026-08-04, which is worse.**
`test_carminat_ignores_unknown_full_auth_until_00` asserted that `61 11 5A` produced nothing
until a `61 11 00` arrived. No capture contains `5A`, or says byte 2 is read at all. The
special case had been promoted from code into a regression test, where it looked like a
measurement and would have outlived the code that made it true. It was renamed to say what the captures say, and later deleted with the rest of the
behavioural suite — the fix it guarded is still in the FSM.

The lesson generalises: **a test that pins a flag's VALUE is weaker than one that pins the
wire.** Four assertions of the form `TEST_ASSERT_FALSE(kSync.someFlag)` went with the flags
they named, and every one would have gone on passing while the FSM did something else.

**And the counters lie by omission.** `rx 0` with zero errors fits *three* states — a silent
bus, a bus we cannot decode, and a controller that never started. Telling them apart needs
`msgs_to_tx`; without it a dead ESP32-C3 receive path looked exactly like a sleeping display
for hours, and an oscilloscope was right where the firmware was wrong. **Expose queue depths
on any diagnostic surface.**

**A sixth, 2026-08-08, the same shape one level up.** `UpdateListMenuDisplay` existed because
two captured `0x121` headers differed, and "two headers" was read as "two panels". Byte `[2]`
is a command *flavour* — `0x76`, `0x7E` and `0x7F` all appear there, and independent projects
have driven both `0x76` and `0x7F` into the same family of glass. This time the special case
had been promoted into a class, a build gate **and** a golden vector.

**A seventh, same day, about documents rather than bytes.** Source comments were pointing at
`docs/PROTOCOL-NOTES.md §17` (that file had ten sections) and `docs/DISPLAY-INIT-SPEC.md` (a
different repository). Prose about bytes drifts from the bytes because nothing checks it,
which is why `docs/WIRE.md` is now generated from the assertions instead of written.

---

## 3. Why the library owns the poll task

`AFFA_ENABLE_TASK` defaults to `1` on ESP32. The reason is three incidents from one
consumer, in one day, each somewhere different, each with the same symptom on the glass:

| what went on the poll task | how it presented |
| --- | --- |
| a BLE service call, allowed to block on GATT | sync stuck at `0x08`, **401 of 644 renders `Timeout`**, every error counter zero |
| a retry site that advanced its backoff only on success | **21 261 frames transmitted against 9 625 received**, `BUS_OFF` latched, registration lost |
| a blocking WebSocket write | *"it froze again — because I opened the web interface?"* Two browser tabs, 115 failed renders |

In all three the frames arrived correctly and on time; the consumer was not awake to consume
them. **The caller-owned contract is one sentence and it is violated by ADDITION** — by the
next feature somebody puts in `loop()`, not by the code that was reviewed.

The owned task removes the class, because the task that polls is no longer a task anyone else
can put anything on. `docs/API.md` §4.7 is the mechanism, §4b the contract.
