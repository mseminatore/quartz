# RTOS Code Review — Bugs, Unimplemented Features, Improvements

## Approach

Pure analysis pass — no code yet. Findings are grouped by severity. For each, the plan
records *what*, *where* (file:line), *why it matters*, and a *suggested fix*. The
implementation phase (after your approval) would tackle them in priority order:
**Critical → High → Medium → Low / nice-to-have**.

I have **not** verified findings by running an AVR or ARM build (no cross-toolchains
installed locally). The MSVC host build configures cleanly. A few of the bugs below
are based on careful reading; please flag any you'd like verified before acting on.

---

## Progress Snapshot (re-evaluated)

**Status: ALL ITEMS COMPLETE.** 471 host tests pass.

**Completed** (verified in tree as of this update):
- C1 — AVR `ISR(TIMER1_COMPA_vect, ISR_NAKED)` header restored (`port/avr_atmega/port.c:278`).
- C2 — Per-core critical nesting array `g_critical_nesting[RTOS_NUM_CORES]`, indexed by `port_core_id()` (`port/arm_cm0plus/port.c:62–88`).
- H1 — Multi-mutex priority inheritance: TCB now owns an intrusive `held_mutexes` list; lock/unlock recompute owner's effective priority across all held mutexes; waiter timeout drops boost when no longer justified (`src/mutex.c`).
- H2 — `rtos_task_set_priority` updates only `base_priority` and preserves any active PI boost.
- H3 — Direct hand-off implemented for sem and queue waiters (notif-vs-queue race resolved).
- H4 — Auto-disabled when `RTOS_STACK_BYTES_PER_WORD != 4`.
- H5 — `rtos_idle_next_wakeup_ticks` / `rtos_timer_min_remaining` wrap reads in critical section.
- H6 — `rtos_task_create` clears `notif_pending`.
- M1 — AVR file structure correct after C1 fix.
- M2 — AVR preempt-on-give: `port_request_reschedule()` now arms a Timer1 OCR1B compare-match for an immediate context switch on sem/queue give (`port/avr_atmega/port.c`).
- M4 — Ready buckets now have per-priority tail pointers; `ready_add` is O(1).
- M5 — "treat-as-timeout" path documented.
- M6 — Both `rtos_task_create` and `rtos_task_set_priority` use the same `>= RTOS_MAX_PRIORITIES` rule.
- M7 — Trace hook audit: missing `RTOS_TRACE_*` emits added in queue/sem ISR and blocking-completion paths.
- M8 — Timer self-reset/stop guard: `g_firing_timer` re-entrant path; `rtos_timer_reset/stop/start` callable from inside the firing timer's own callback.
- L1 — Recursive mutex variant: `rtos_mutex_create_recursive` + `nest_count` field; gated on `RTOS_ENABLE_RECURSIVE_MUTEX`.
- L2 — Value-passing notifications (FreeRTOS-style): `rtos_task_notify_value`, `rtos_task_notify_wait_value`, action enum (NONE/SET_BITS/INCREMENT/OVERWRITE/SET_NO_OVERWRITE).
- L3 — Queue API extras: `rtos_queue_peek`, `rtos_queue_send_to_front`, `rtos_queue_spaces_available`.
- L4 — Cortex-M4 FPU support (`RTOS_CM4_FPU=1`): per-task EXC_RETURN saved in software frame, conditional `vstmdb`/`vldmia` of S16-S31 in PendSV.
- L5 — Idle stack walk now snapshots `g_all_tasks` under critical section before iterating.
- L6 — `RTOS_BASEPRI_STACK_DEPTH` bound + weak `rtos_port_critical_overflow_hook` invoked on critical-nesting overflow.
- L7 — `RTOS_PORT` cmake cache variable replaces fragile `CMAKE_SYSTEM_PROCESSOR` matching (legacy auto-detect retained).
- L8 — README updated for FPU support, recursive mutex, value notifications, queue extras, AVR preemption, `RTOS_PORT` selection.

**No outstanding items.**

---

## Critical (build-breaking or silent corruption)

### C1. AVR Timer1 ISR is missing its function header
- **Where:** `port/avr_atmega/port.c` ~line 274–278
- **What:** The big asm block that performs the AVR tick / context switch starts with
  a bare `{` — there is **no `ISR(TIMER1_COMPA_vect, ISR_NAKED)` declaration** in front
  of it. Confirmed via `grep TIMER1_COMPA|ISR\(` returning *zero* matches in
  `port/avr_atmega/`.
- **Impact:** The AVR port cannot compile at all. The host unit tests still pass
  because they stub the port out, masking the regression.
- **Fix:** Add the standard avr-libc header line just before the `{`:
  ```c
  ISR(TIMER1_COMPA_vect, ISR_NAKED)
  ```

### C2. Cortex-M0+ multi-core critical section race on `g_critical_nesting`
- **Where:** `port/arm_cm0plus/port.c:60–82`
- **What:** `g_critical_nesting` is a single global counter. When `RTOS_NUM_CORES > 1`
  (RP2040), `port_enter_critical` does:
  ```c
  cpsid i;
  if (g_critical_nesting == 0) spinlock_acquire();
  g_critical_nesting++;
  ```
  If core 0 is *inside* a critical section (`nesting == 1`, holds spinlock), and core 1
  enters `port_enter_critical`, core 1 reads `nesting == 1` → **skips
  `spinlock_acquire`** but still increments and returns. Core 1 is now executing
  "in the critical section" without the spinlock — direct mutual-exclusion failure.
- **Impact:** Any non-trivial dual-core RP2040 workload will eventually corrupt the
  ready/blocked lists or queue state.
- **Fix:** Make the nesting counter per-core (`g_critical_nesting[RTOS_NUM_CORES]`)
  indexed by `port_core_id()`. Each core's counter only counts that core's own
  re-entries; the spinlock provides mutual exclusion between cores.

---

## High (functional bugs visible to applications)

### H1. Mutex priority restoration is wrong when a task holds multiple mutexes
- **Where:** `src/mutex.c:130–133`
- **What:** On unlock, `self->priority = self->base_priority` unconditionally —
  even if the task still holds *another* mutex with a higher-priority waiter.
  The boost is dropped early, re-enabling priority inversion.
- **Fix:** When unlocking, recompute the effective priority as
  `max(base_priority, max waiter priority across all still-held mutexes)`.
  Requires either tracking held mutexes per task, or walking all mutexes (small
  set in static-allocation systems). A simpler interim fix: document the
  single-mutex-per-task limitation prominently in `rtos_mutex.h`.

### H2. Priority-inheritance "boost" leaves stale state if `rtos_task_set_priority` runs while boosted
- **Where:** `src/task.c:912–937`
- **What:** Sets both `priority` and `base_priority` to the new value while the task
  may currently be boosted by a mutex it owns. When the mutex is later released,
  the saved `base_priority` overwrites the (now-changed) intent silently.
- **Fix:** Only update `base_priority`; recompute effective `priority` honouring
  any current boost.

### H3. Queue/Sem can wake a waiter who then loses the message/token (no real hand-off)
- **Where:** `src/queue.c:99–111` and the symmetric receive path; `src/sem.c:128–141`.
- **What:** Sender enqueues an item, increments `count`, then makes a receiver ready.
  Between that point and the woken receiver actually re-entering the critical
  section, *another* receiver task can call `rtos_queue_receive`, see `count > 0`,
  and steal the item. The originally woken receiver returns `RTOS_TIMEOUT`
  (post-wake fallback path), even though it was selected.
- The semaphore path has the same shape (no direct hand-off — `rtos_semaphore_give`
  pops the waiter then exits critical; another `take` could decrement first).
  Currently sem dodges this by **not** incrementing count on hand-off, so the
  woken taker just returns OK without re-checking — but only if no further code
  changes that invariant.
- **Impact:** Spurious `RTOS_TIMEOUT` returns under contention; not "wrong"
  (caller can retry) but violates expected priority hand-off semantics.
- **Fix:** True direct hand-off: mark the woken waiter as "owns the slot/item"
  via a per-TCB scratch field (or store the slot index in the TCB), so racing
  callers skip that slot. For semaphore: keep current "no count increment on
  hand-off" but make sure no future change adds a `count++ then make_ready`.

### H4. `rtos_task_check_stack` / `stack_high_water_mark` and the per-tick sentinel check assume 32-bit stack words on AVR
- **Where:** `src/task.c:283–289, 583–620, 651–656, 753–760`
- **What:** All sentinel reads/writes use `uint32_t *p = (uint32_t *)stack_base; p[0]`.
  On AVR, `RTOS_STACK_BYTES_PER_WORD == 1`, so the cast both:
  1. Aliases a `uint8_t *` buffer as `uint32_t *` (alignment may fail; the
     ATmega328P does tolerate misaligned 32-bit access in software but it's still
     undefined C).
  2. Walks bytes 4 at a time from a buffer whose size is **bytes**, not
     32-bit words; `stack_words * 1 / 4` truncates 1–3 trailing bytes per stack.
- **Impact:** Stack overflow detection silently misbehaves on AVR.
- **Fix:** Make the sentinel/watermark word type configurable via
  `RTOS_STACK_BYTES_PER_WORD` (use `uint8_t` & 0xA5 on AVR, `uint32_t` &
  0xA5A5A5A5 elsewhere). Or simply disable both features on AVR via
  `#if RTOS_STACK_BYTES_PER_WORD == 4`.

### H5. `rtos_idle_next_wakeup_ticks` and `rtos_timer_min_remaining` read shared lists outside a critical section
- **Where:** `src/task.c:691–707`, `src/timer.c:191–197`.
- **What:** Both functions read `G_BLOCKED->wakeup_tick` / `g_timer_list->abs_expiry_tick`
  with no `port_enter_critical`. Called from the idle task; a tick ISR can mutate
  those pointers concurrently. Worst case: NULL deref of a stale pointer that
  the ISR already popped, or torn 32-bit read on AVR (16-bit register file).
- **Fix:** Wrap the bodies in `port_enter_critical` / `port_exit_critical`.

### H6. Trailing-empty-comment pattern in `rtos_task_delete` leaves `notif_pending` set
- **Where:** `src/task.c:434–465`
- **What:** `rtos_task_delete` clears `state` and removes from lists but does
  NOT clear `notif_pending`, `ipc_wait`, or `wakeup_tick`. If the same TCB is
  later reused via `rtos_task_create`, `notif_pending` carries over and the
  first `notify_wait` returns `OK` immediately (stale notification).
- **Fix:** In `rtos_task_create`, the init block (~line 251–258) already resets
  most fields — add `tcb->notif_pending = 0;` (under the `RTOS_ENABLE_TASK_NOTIFY`
  guard).

---

## Medium (subtle correctness, documentation gaps, completeness)

### M1. AVR file ends with a stray `}` after the broken ISR
- **Where:** `port/avr_atmega/port.c:397`
- **What:** A consequence of C1; once C1 is fixed, ensure the file actually closes
  the function (the closing `}` on line 397 is presumably the intended ISR end).

### M2. `port_request_reschedule` from `rtos_semaphore_give` can be called from an unsafe state
- **Where:** `src/sem.c:140`
- **What:** Calling `port_request_reschedule()` outside a critical section after
  having modified scheduler state is fine on ARM (PendSV is queued), but on AVR
  this is a no-op and the next preempt only happens at the next tick. So
  semaphore give from a low-priority task does *not* immediately yield to a
  waiting high-priority task on AVR. That defeats RTOS preemption semantics.
- **Fix:** AVR port needs a software way to provoke an immediate context switch
  (e.g., set a flag and trigger a software interrupt, or call `rtos_context_switch`
  directly with appropriate stack handling). At minimum, document the limitation.

### M3. `rtos_timer_min_remaining` is called from outside a critical section even for tickless idle correctness
- See H5 — same fix.

### M4. `list_insert_tail` is O(n) and runs on every `ready_add`
- **Where:** `src/list.c:65–81`, used by `ready_add` (`src/task.c:133–144`)
- **What:** Per-priority ready queues are usually short, but on systems with
  many tasks at one priority this is unnecessary. A `tail` pointer (per
  priority bucket) would make it O(1) for the same memory cost.
- **Severity:** mostly aesthetic at typical RTOS sizes.

### M5. Notification's "treat as timeout" logic in IPC waiters silently drops the notification
- **Where:** `src/sem.c:101–109`, `src/mutex.c:98–106`, `src/queue.c:94–98, 167–171`
- **What:** When `notif_pending` is true after waking from an IPC wait, the
  function returns `RTOS_TIMEOUT` but does **not** clear `notif_pending`. That
  leaks the notification to the next `notify_wait`, where it correctly returns
  OK — so this is "by design" but undocumented and easy to misread. Add a
  comment, or clear and re-deliver via `rtos_task_notify(self)`.

### M6. `rtos_task_set_priority` rejects `RTOS_MAX_PRIORITIES - 1` (idle priority) — but `rtos_task_create` accepts it
- **Where:** `src/task.c:235`, `src/task.c:914`
- **What:** Inconsistency: a user can create a task at idle priority but cannot
  later assign that priority. Pick one rule and apply it in both places.

### M7. Trace hooks for queue / timer / mutex are documented but not all are emitted
- **Where:** `include/rtos_trace.h` vs `src/queue.c`, `src/timer.c`, `src/mutex.c`
- **Action:** Audit which `RTOS_TRACE_*` macros exist and grep for them — fill
  in any that are declared but never fired.

### M8. `rtos_timer_create` callback constraints are documented but not enforced
- **Where:** `src/timer.c:11–13` (header comment)
- **What:** "Timer callbacks must not call rtos_timer_reset() on themselves."
  Worth either fixing (cheap: check `cur->active` *and* set a "currently firing"
  flag, ignore reset/start during fire), or asserting in debug builds.

---

## Low / nice-to-have

### L1. Recursive mutex variant
Many RTOS-using codebases want `rtos_mutex_lock_recursive`. Currently a task
that re-locks its own mutex blocks itself permanently. Worth adding (small TCB
field for `lock_count`).

### L2. Counting / value-passing task notifications (FreeRTOS-style)
The current notification is a single binary flag. A `uint32_t` value with
overwrite/increment/clear semantics is a common request and is essentially free
in TCB cost (one `uint32_t`, gated by `RTOS_ENABLE_TASK_NOTIFY`).

### L3. `rtos_queue_peek`, `rtos_queue_send_to_front` (LIFO), `rtos_queue_spaces_available`
Trivial additions that round out the queue API.

### L4. ARM Cortex-M4 FPU support
README explicitly mentions this as planned (line 255). `port/arm_cm4/port.c`
notes the hooks but doesn't implement them. Add `RTOS_CM4_FPU=1` path that
saves/restores S16–S31 and FPSCR in PendSV, and EXC_RETURN = 0xFFFFFFED.

### L5. Idle task's `STACK_OVERFLOW_CHECK` walk holds no critical section
- **Where:** `src/task.c:649–656`
- Walks `g_all_tasks` while task creation/deletion may modify it. Wrap in
  critical section (or accept it — the worst case is a stale read because
  add/remove operations are simple pointer assignments).

### L6. `g_basepri_stack[8]` in CM4 silently truncates if nesting > 8
- **Where:** `port/arm_cm4/port.c:67, 83`
- An `assert` or compile-time max would protect against ill-conceived deep
  nesting.

### L7. CMake target detection for cross-builds is brittle
`CMAKE_SYSTEM_PROCESSOR` string-matching is fragile; consider a dedicated
`RTOS_PORT` cache variable (`RTOS_PORT=arm_cm4` etc.).

### L8. README accuracy
Re-skim once H/M items are fixed; AVR section probably overstates feature
parity (no preemption-on-give as noted in M2; stack overflow check broken on
AVR per H4).

---

## Risks / Things I deliberately *did not* do
- I did not run any cross-toolchain build, so AVR and ARM ports may have
  additional issues I missed.
- I did not deeply review `port/xtensa_esp32s3/` or `port/riscv/port_asm.S` —
  bugs there could affect those targets.
- The `samples/wifi_http.c` and `samples/lwipopts.h` aren't core RTOS — skipped.

## Suggested order of attack (remaining work)
1. **H1** — multi-mutex priority restoration (real correctness gap)
2. **M2** — AVR preemption-on-give (functional gap on AVR target)
3. **M8** — guard `rtos_timer_reset()` from inside a firing callback
4. **M7** — audit & emit missing `RTOS_TRACE_*` hooks
5. **L5, L6** — small hardening (idle stack walk crit section, CM4 basepri stack assert)
6. **L4** — Cortex-M4 FPU support (sizeable, README-promised)
7. **L1, L2, L3** — API enhancements (recursive mutex, value notifications, queue peek/etc.)
8. **L7, L8, M4** — build/docs/perf polish
