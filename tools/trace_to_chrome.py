#!/usr/bin/env python3
"""Convert an RTOS chrome-trace binary capture to Chrome Trace Event JSON.

See include/rtos_trace_chrome.h for the on-wire format. Input may be either
a raw binary file or ASCII hex (as produced by rtos_trace_chrome_dump_hex).

Usage:
    python3 trace_to_chrome.py capture.bin > trace.json
    python3 trace_to_chrome.py capture.hex --hex > trace.json
    cat capture.hex | python3 trace_to_chrome.py - --hex > trace.json
"""

import argparse
import json
import struct
import sys

EV_TASK_SWITCH_IN  = 1
EV_TASK_SWITCH_OUT = 2
EV_TASK_CREATE     = 3
EV_TASK_DELETE     = 4
EV_SEM_TAKE        = 5
EV_SEM_GIVE        = 6
EV_MUTEX_LOCK      = 7
EV_MUTEX_UNLOCK    = 8
EV_QUEUE_SEND      = 9
EV_QUEUE_RECEIVE   = 10
EV_TIMER_FIRE      = 11
EV_EG_SET          = 12
EV_EG_WAIT         = 13

EVENT_NAMES = {
    EV_TASK_SWITCH_IN:  "switch_in",
    EV_TASK_SWITCH_OUT: "switch_out",
    EV_TASK_CREATE:     "task_create",
    EV_TASK_DELETE:     "task_delete",
    EV_SEM_TAKE:        "sem_take",
    EV_SEM_GIVE:        "sem_give",
    EV_MUTEX_LOCK:      "mutex_lock",
    EV_MUTEX_UNLOCK:    "mutex_unlock",
    EV_QUEUE_SEND:      "queue_send",
    EV_QUEUE_RECEIVE:   "queue_recv",
    EV_TIMER_FIRE:      "timer_fire",
    EV_EG_SET:          "eg_set",
    EV_EG_WAIT:         "eg_wait",
}


def read_input(path: str, is_hex: bool) -> bytes:
    raw = sys.stdin.buffer.read() if path == "-" else open(path, "rb").read()
    if not is_hex:
        return raw
    text = raw.decode("ascii", errors="ignore")
    end = text.find("---END---")
    if end >= 0:
        text = text[:end]
    text = "".join(c for c in text if c in "0123456789abcdefABCDEF")
    if len(text) % 2:
        text = text[:-1]
    return bytes.fromhex(text)


def parse(buf: bytes):
    if len(buf) < 16 or buf[:4] != b"RTRC":
        raise ValueError("not an RTRC capture (missing magic)")
    version, name_len = struct.unpack_from("<HH", buf, 4)
    if version != 1:
        raise ValueError(f"unsupported version {version}")
    names_n, records_n = struct.unpack_from("<II", buf, 8)
    off = 16

    names = {}
    name_entry = 4 + name_len
    for _ in range(names_n):
        if off + name_entry > len(buf):
            raise ValueError("truncated name table")
        hid, is_task, _resv = struct.unpack_from("<HBB", buf, off)
        raw_name = buf[off + 4 : off + 4 + name_len]
        nul = raw_name.find(b"\x00")
        name = raw_name[:nul if nul >= 0 else name_len].decode("utf-8", "replace")
        names[hid] = {"name": name, "is_task": bool(is_task)}
        off += name_entry

    records = []
    rec_size = 16
    for _ in range(records_n):
        if off + rec_size > len(buf):
            raise ValueError("truncated record")
        ts, hid, typ, flags, aux1, aux2 = struct.unpack_from("<IHBBII", buf, off)
        records.append({
            "ts": ts, "hid": hid, "type": typ,
            "flags": flags, "aux1": aux1, "aux2": aux2,
        })
        off += rec_size

    return names, records


# Width (microseconds) of the synthetic IPC bar.  Big enough to be selectable
# in chrome://tracing without dominating realistic kernel-scale time spans.
IPC_BAR_US = 50

# Minimum slot width assigned to members of a tight cluster after equalization.
# Without this, a cluster spanning 1 µs (post-nudge) collapses to 1-µs bars
# that are essentially invisible at default zoom.  Re-spacing cluster members
# at this granularity keeps each bar clickable while only nudging timestamps
# by up to IPC_MIN_VISIBLE_US * cluster_size µs (well below the µs→ms gap to
# the next IPC iteration on real workloads).
IPC_MIN_VISIBLE_US = 20

# Per-event-type color names (Chrome Trace Format `cname` palette).  Both
# chrome://tracing and Perfetto honor these — handy for telling apart
# closely-spaced IPC events that would otherwise be visually indistinguishable.
IPC_CNAME = {
    EV_SEM_TAKE:      "good",            # green
    EV_SEM_GIVE:      "olive",
    EV_MUTEX_LOCK:    "bad",             # red
    EV_MUTEX_UNLOCK:  "yellow",
    EV_QUEUE_SEND:    "rail_response",   # lavender
    EV_QUEUE_RECEIVE: "rail_animation",  # orange
    EV_TIMER_FIRE:    "white",
    EV_EG_SET:        "cq_build_passed", # cyan
    EV_EG_WAIT:       "cq_build_attempt_runnning",  # light blue
}

# Per-event-type labels for the (aux1, aux2) record fields.  See trace_chrome.c
# call sites for the kernel-side meaning.
AUX_LABELS = {
    EV_SEM_TAKE:      ("count", "blocked"),
    EV_SEM_GIVE:      ("count", "woke_waiter"),
    EV_MUTEX_LOCK:    ("nest_count", "blocked"),
    EV_MUTEX_UNLOCK:  ("nest_count", "woke_waiter"),
    EV_QUEUE_SEND:    ("count", "woke_receiver"),
    EV_QUEUE_RECEIVE: ("count", "woke_sender"),
    EV_EG_SET:        ("bits_set", "tasks_woken"),
    EV_EG_WAIT:       ("bits_satisfied", "timed_out"),
}


def emit_chrome(names, records):
    pid = 1
    events = []

    # Each known task gets two adjacent lanes:
    #   running lane (tid)         — `running` slices, sort_index = 2*N
    #   IPC sub-lane (tid + 1000)  — `ph:"X"` IPC bars,  sort_index = 2*N + 1
    # The +1000 offset gives plenty of room to keep them distinct from the
    # running tids.  An auxiliary "events" lane catches IPC events that fire
    # before any task is on-CPU.
    task_tid = {}
    next_tid = 1
    for hid, info in names.items():
        if info["is_task"]:
            task_tid[info["name"]] = next_tid
            next_tid += 1
    EVENTS_TID = 100
    IPC_TID_OFFSET = 1000  # IPC sub-lane tid = task tid + 1000

    events.append({"name": "process_name", "ph": "M", "pid": pid, "tid": 1,
                   "args": {"name": "rtos"}})

    def add_task_lane_metadata(tname: str, tid: int):
        # running lane
        events.append({"name": "thread_name", "ph": "M", "pid": pid, "tid": tid,
                       "args": {"name": tname}})
        events.append({"name": "thread_sort_index", "ph": "M", "pid": pid,
                       "tid": tid, "args": {"sort_index": tid * 2}})
        # adjacent IPC sub-lane (sorted right under its task's running lane)
        ipc_tid = tid + IPC_TID_OFFSET
        events.append({"name": "thread_name", "ph": "M", "pid": pid,
                       "tid": ipc_tid, "args": {"name": f"{tname}/IPC"}})
        events.append({"name": "thread_sort_index", "ph": "M", "pid": pid,
                       "tid": ipc_tid, "args": {"sort_index": tid * 2 + 1}})

    for tname, tid in task_tid.items():
        add_task_lane_metadata(tname, tid)
    # The "events" lane is added lazily — only if at least one IPC event
    # actually lands there (i.e. fired before any task was running).
    events_lane_used = False

    def hname(hid: int) -> str:
        n = names.get(hid)
        if n and n["name"]:
            return n["name"]
        return f"h{hid:#x}"

    def tid_for_task(name: str) -> int:
        if name not in task_tid:
            nonlocal next_tid
            task_tid[name] = next_tid
            add_task_lane_metadata(name, next_tid)
            next_tid += 1
        return task_tid[name]

    current = None      # name of currently-running task
    current_tid = None
    for r in records:
        t = r["type"]
        if t == EV_TASK_SWITCH_IN:
            # Close any prior open slice (defensive — paired SWITCH_OUT is
            # the normal case but ports may emit back-to-back SWITCH_INs).
            # NOTE: the E event's `name` MUST match its B's name — chrome://
            # tracing is lenient but Perfetto strictly matches B/E by name.
            if current is not None:
                events.append({"ph": "E", "pid": pid, "tid": current_tid,
                               "ts": r["ts"], "name": "running"})
            current = hname(r["hid"])
            current_tid = tid_for_task(current)
            events.append({"ph": "B", "pid": pid, "tid": current_tid,
                           "ts": r["ts"], "name": "running",
                           "args": {"task": current, "prio": r["aux1"]}})
        elif t == EV_TASK_SWITCH_OUT:
            if current is not None:
                events.append({"ph": "E", "pid": pid, "tid": current_tid,
                               "ts": r["ts"], "name": "running"})
                current = None
                current_tid = None
        else:
            label = EVENT_NAMES.get(t, f"ev{t}")
            target = hname(r["hid"])
            # IPC events emit as ph:"X" complete events on the originating
            # task's IPC sub-lane (selectable bars in both viewers).  If no
            # task is running yet the event lands on the shared events lane.
            if current_tid is not None:
                tid = current_tid + IPC_TID_OFFSET
            else:
                tid = EVENTS_TID
                events_lane_used = True
            a1_lbl, a2_lbl = AUX_LABELS.get(t, ("aux1", "aux2"))
            ev = {"ph": "X", "pid": pid, "tid": tid,
                  "ts": r["ts"], "dur": IPC_BAR_US,
                  "name": f"{label}({target})",
                  "args": {"handle": target, "by_task": current,
                           a1_lbl: r["aux1"], a2_lbl: r["aux2"]}}
            cname = IPC_CNAME.get(t)
            if cname is not None:
                ev["cname"] = cname
            events.append(ev)

    if events_lane_used:
        events.append({"name": "thread_name", "ph": "M", "pid": pid,
                       "tid": EVENTS_TID, "args": {"name": "events"}})
        events.append({"name": "thread_sort_index", "ph": "M", "pid": pid,
                       "tid": EVENTS_TID, "args": {"sort_index": EVENTS_TID}})

    # Per-IPC-sub-lane post-processing:
    #   1. Nudge simultaneous events forward by 1 µs each so they don't stack
    #      (host ports have µs-resolution timestamps, so back-to-back kernel
    #      calls inside one task body can land in the same microsecond — which
    #      would force chrome://tracing to render them on stacked sub-tracks
    #      and add an inconsistent collapse arrow only on the affected lane).
    #   2. Clamp each bar's dur so it never overlaps the next bar on the same
    #      lane (target IPC_BAR_US, floor 1 µs).
    #   3. Equalize widths within tight clusters: a cluster is a contiguous
    #      run of events where each is within IPC_BAR_US of the next.  Within
    #      a cluster, give every event the cluster's smallest dur — otherwise
    #      the trailing event of a cluster (whose gap to the next iteration is
    #      large) keeps the full 50 µs and visually dwarfs its peers.
    by_tid = {}
    for e in events:
        if e.get("ph") == "X":
            by_tid.setdefault(e["tid"], []).append(e)
    for tid_evs in by_tid.values():
        tid_evs.sort(key=lambda e: e["ts"])
        for i in range(1, len(tid_evs)):
            if tid_evs[i]["ts"] <= tid_evs[i - 1]["ts"]:
                tid_evs[i]["ts"] = tid_evs[i - 1]["ts"] + 1
        for i, e in enumerate(tid_evs):
            if i + 1 < len(tid_evs):
                gap = tid_evs[i + 1]["ts"] - e["ts"]
                if gap < e["dur"]:
                    e["dur"] = max(1, gap)
        # Cluster equalization + re-spacing.  A cluster is a contiguous run of
        # events where each is within IPC_BAR_US of the next.  Distribute the
        # cluster across uniform slots wide enough to remain clickable.
        i = 0
        while i < len(tid_evs):
            j = i
            while (j + 1 < len(tid_evs)
                   and (tid_evs[j + 1]["ts"] - tid_evs[j]["ts"]) <= IPC_BAR_US):
                j += 1
            n = j - i + 1
            if n > 1:  # cluster of >= 2 events
                first_ts = tid_evs[i]["ts"]
                natural_span = tid_evs[j]["ts"] - first_ts
                slot = max(IPC_MIN_VISIBLE_US, (natural_span + n - 1) // n)
                # Don't run past the next standalone event (if any).
                if j + 1 < len(tid_evs):
                    available = tid_evs[j + 1]["ts"] - first_ts - 1
                    if available > 0:
                        slot = min(slot, max(1, available // n))
                for k, e in enumerate(tid_evs[i:j + 1]):
                    e["ts"] = first_ts + k * slot
                    e["dur"] = max(1, slot - 1)
            i = j + 1

    # Chrome Trace Event Format only accepts "ms" (default) or "ns" for
    # displayTimeUnit.  Our `ts` values are integer microseconds (the format's
    # required unit regardless of displayTimeUnit), so "ns" keeps full
    # sub-millisecond resolution in the viewer.
    return {"traceEvents": events, "displayTimeUnit": "ns"}


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("input", help="capture file (or '-' for stdin)")
    ap.add_argument("--hex", action="store_true",
                    help="input is ASCII hex (rtos_trace_chrome_dump_hex output)")
    ap.add_argument("-o", "--output", default="-", help="output JSON path (default: stdout)")
    args = ap.parse_args()

    buf = read_input(args.input, args.hex)
    names, records = parse(buf)
    js = emit_chrome(names, records)
    out = json.dumps(js, indent=2) + "\n"
    if args.output == "-":
        sys.stdout.write(out)
    else:
        with open(args.output, "w") as f:
            f.write(out)


if __name__ == "__main__":
    main()
