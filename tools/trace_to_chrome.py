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


def emit_chrome(names, records):
    pid = 1
    events = []

    # Assign each known task its own thread id (= lane in chrome://tracing).
    # A separate lane "events" hosts IPC instants so they don't visually clip
    # the task slices.  Lower tid = higher row in the viewer.
    task_tid = {}
    next_tid = 1
    for hid, info in names.items():
        if info["is_task"]:
            task_tid[info["name"]] = next_tid
            next_tid += 1
    EVENTS_TID = 100

    events.append({"name": "process_name", "ph": "M", "pid": pid, "tid": 1,
                   "args": {"name": "rtos"}})
    for tname, tid in task_tid.items():
        events.append({"name": "thread_name", "ph": "M", "pid": pid, "tid": tid,
                       "args": {"name": tname}})
        # sort_index orders the lanes; tasks first, "events" last.
        events.append({"name": "thread_sort_index", "ph": "M", "pid": pid,
                       "tid": tid, "args": {"sort_index": tid}})
    # The "events" lane is added lazily — only if at least one IPC instant
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
            events.append({"name": "thread_name", "ph": "M", "pid": pid,
                           "tid": next_tid, "args": {"name": name}})
            next_tid += 1
        return task_tid[name]

    current = None      # name of currently-running task
    current_tid = None
    for r in records:
        t = r["type"]
        if t == EV_TASK_SWITCH_IN:
            # Close any prior open slice (defensive — paired SWITCH_OUT is
            # the normal case but ports may emit back-to-back SWITCH_INs).
            if current is not None:
                events.append({"ph": "E", "pid": pid, "tid": current_tid,
                               "ts": r["ts"], "name": current})
            current = hname(r["hid"])
            current_tid = tid_for_task(current)
            events.append({"ph": "B", "pid": pid, "tid": current_tid,
                           "ts": r["ts"], "name": "running",
                           "args": {"task": current, "prio": r["aux1"]}})
        elif t == EV_TASK_SWITCH_OUT:
            if current is not None:
                events.append({"ph": "E", "pid": pid, "tid": current_tid,
                               "ts": r["ts"], "name": current})
                current = None
                current_tid = None
        else:
            label = EVENT_NAMES.get(t, f"ev{t}")
            target = hname(r["hid"])
            # Put the instant on the current task's lane so it's visually
            # associated with whoever caused it; fall back to the events lane
            # if no task is running yet.
            if current_tid is not None:
                tid = current_tid
            else:
                tid = EVENTS_TID
                events_lane_used = True
            events.append({"ph": "i", "s": "t", "pid": pid, "tid": tid,
                           "ts": r["ts"], "name": f"{label}({target})",
                           "args": {"handle": target, "by_task": current,
                                    "aux1": r["aux1"], "aux2": r["aux2"]}})

    if events_lane_used:
        events.append({"name": "thread_name", "ph": "M", "pid": pid,
                       "tid": EVENTS_TID, "args": {"name": "events"}})
        events.append({"name": "thread_sort_index", "ph": "M", "pid": pid,
                       "tid": EVENTS_TID, "args": {"sort_index": EVENTS_TID}})

    # Chrome Trace Event Format only accepts "ms" (default) or "ns" for
    # displayTimeUnit.  Our `ts` values are integer microseconds (the format's
    # required unit regardless of displayTimeUnit), so "ns" keeps full
    # sub-millisecond resolution in the viewer.
    return {"traceEvents": events, "displayTimeUnit": "ns"}

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
