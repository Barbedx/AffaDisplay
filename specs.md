# AffaDisplay — Specification Index

Spec-driven development for this repository runs on [OpenSpec](https://github.com/Fission-AI/OpenSpec).
The specs live under `openspec/specs/<capability>/spec.md`; this file is the map.

```
openspec/
  config.yaml              project context and authoring rules, shown to the agent
  specs/<capability>/      the CURRENT truth — what the system does today
  changes/<name>/          proposed changes, with delta specs, until archived
  changes/archive/         changes that have landed
```

## Workflow

1. **Propose** — `/opsx:propose` writes a change under `openspec/changes/<name>/`: a
   proposal, a design, delta specs (`## ADDED` / `## MODIFIED` / `## REMOVED` /
   `## RENAMED` requirements), and tasks.
2. **Apply** — `/opsx:apply` implements the tasks. Code follows the spec, not the reverse.
3. **Sync or archive** — `/opsx:sync` folds the deltas into the main specs;
   `/opsx:archive` does that and retires the change.

Validate at any point:

```
openspec spec validate --strict     # every capability spec
openspec list --specs               # what capabilities exist
openspec view                       # dashboard
```

## Capabilities

| Capability | What it owns |
| --- | --- |
| [can-link](openspec/specs/can-link/spec.md) | The only seam to a CAN driver: bring-up, the receive ring, the software transmit gate, bus-off recovery, and health that is counted rather than sampled. |
| [panel-sync](openspec/specs/panel-sync/spec.md) | The AFFA handshake as data plus one shared state machine: heartbeat, sync request, hello, peer timeout, lazy function registration, passive mode. |
| [render-queue](openspec/specs/render-queue/spec.md) | Admission, latest-value-wins coalescing per render slot, priority, retry and backoff, preemption, and the two-verdict contract. |
| [owned-task](openspec/specs/owned-task/spec.md) | The library owning its own poll task: single-pumper enforcement, RX-before-TX ordering, renders from any task, and making blocking visible. |
| [carminat-panel](openspec/specs/carminat-panel/spec.md) | The Carminat / AFFA3 family: frame builders, capability answers, menu geometry, key guards, and what is bench-verified versus inferred. |
| [observation](openspec/specs/observation/spec.md) | Three layers of visibility — raw frame tap, filtered subscriptions, decoded events — plus inbound text behind its gate. |
| [remote-serviceability](openspec/specs/remote-serviceability/spec.md) | Keeping a board with no cable reachable: update-before-CAN ordering, the socket-table lockout, the self-probe, and handler stack sizing. |

## The rule these specs exist to hold

**The library owns the protocol and nothing else.** Wire format, sync, queueing and
delivery verdicts are its job. Scrolling, animation, menus, screen layout, reconnect policy
and clock sources are application policy or an opt-in widget under `src/widget/`.

When a feature is proposed, the first question is which capability it belongs to. If the
answer is "none of them", it probably belongs in an example.

## Two things the specs deliberately record

**Measurements carry their date.** `forceRecoveryMs = 250`, the 1371 µs mean panel
acknowledgment turnaround, the 13-versus-14 frame difference between hardware and self-ack —
each is quoted with when it was measured, because a number without provenance gets
"corrected" by the next person who finds it surprising.

**Rules carry the failure that produced them.** A requirement that says only what to do
gets tidied away; one that also says what broke does not. Examples living in the specs:

- `LinkMode::ListenOnly` is refused rather than faked, because entering it before `begin()`
  starts tasks that block on queues which do not exist yet — a board dead before OTA.
- `send()` must never test `sendFrame()`'s return value, because it is a literal
  `return true` on every path including "transmitted nothing".
- The give-up path disables retry, because routing it through the normal path re-armed the
  job forever — `LinkDown` is exactly the result the retry branch forgives.
- The HTTP server needs LRU purge on, because without it a full socket table is permanent
  and takes OTA with it while ping and mDNS keep answering.

## Status

The specs describe the library as it stands. They are written to be falsifiable: every
requirement should be checkable by a host test under `test/`, or by a bench measurement
through the console in `examples/`.

Host tests and the docs tree were removed from the working tree before these specs were
written and have not been restored — `openspec/specs/` is currently the authoritative
description of intended behaviour.
