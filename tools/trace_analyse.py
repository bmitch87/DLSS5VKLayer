#!/usr/bin/env python3
"""Read a dlssnr frame trace and say what the session looked like.

Standard library only, on purpose: this runs wherever the trace was produced, including a machine
that has nothing installed but a Python, and a diagnostic that needs its own dependencies is one
more thing to be wrong when you are already debugging something.

    tools/trace_analyse.py <trace.csv> [more.csv ...]

Writes <trace>.analysis.md and <trace>.analysis.json beside each input and prints the markdown.

What it will NOT do, and why:

  * It refuses a file that does not end with `#complete,1`. A trace is written in one pass when the
    process stops; a file without that line was truncated -- the process was killed, the disk filled,
    the copy was taken mid-write -- and half a ring read as a whole one is worse than no answer. Pass
    --allow-truncated if you know what you are looking at.

  * It never sums durations across nesting levels. `Present` contains `Leg1Wait` and `RoundTrip`;
    `Frame` contains `Evaluate` and `Readback`. Adding them together counts the same microseconds
    twice and produces a total larger than the session. Each tag is summarised on its own.

  * It says nothing about frame rate. These are CPU wall-clock spans around calls we make, not
    on-screen frames, and a short submission call does not mean the GPU work was short. The GPU
    timestamps in the header answer that question and this file does not.

This reads the trace and nothing else. The capture pairs the layer writes are the other half of the
measurement story and are read by tools/capture_metrics, which is C++ because it does per-pixel
arithmetic over raw frames -- work this file would be the wrong tool for. Two readers, one for each
kind of evidence.

The aggregate rows are the authority on counts, totals and maxima: they are kept outside the rings
and so cover the whole session, while the ring rows cover only what did not get overwritten. When
the two disagree the ring is the one that is incomplete -- which is what the `overwritten` counts
are for.
"""

import json
import os
import sys
import time


class Trace:
    def __init__(self, path):
        self.path = path
        self.clock = {}
        self.process = "unknown"
        self.capacity = {}
        self.overwritten = {}
        self.caveats = []
        self.aggregates = []   # dicts: tag, count, total_us, max_us, slow_count
        self.events = []       # dicts: ring, tag, start_us, duration_us, a, b
        self.complete = False


def parse(path, allow_truncated=False):
    t = Trace(path)
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.rstrip("\n")
            if not line:
                continue
            if line.startswith("#"):
                parts = line[1:].split(",")
                kind = parts[0]
                if kind == "clock":
                    # clock,ticks_per_second,N,origin_ticks,N,unix_utc,N
                    for i in range(1, len(parts) - 1, 2):
                        t.clock[parts[i]] = _int(parts[i + 1])
                elif kind == "process":
                    t.process = parts[1] if len(parts) > 1 else "unknown"
                elif kind == "capacity":
                    for i in range(1, len(parts) - 1, 2):
                        t.capacity[parts[i]] = _int(parts[i + 1])
                elif kind == "overwritten":
                    for i in range(1, len(parts) - 1, 2):
                        t.overwritten[parts[i]] = _int(parts[i + 1])
                elif kind == "caveat":
                    t.caveats.append(",".join(parts[1:]))
                elif kind == "agg" and len(parts) >= 6:
                    t.aggregates.append({
                        "tag": parts[1],
                        "count": _int(parts[2]),
                        "total_us": _int(parts[3]),
                        "max_us": _int(parts[4]),
                        "slow_count": _int(parts[5]),
                    })
                elif kind == "complete":
                    t.complete = True
                continue
            if line.startswith("ring,"):
                continue  # the column header
            parts = line.split(",")
            if len(parts) < 6:
                continue
            t.events.append({
                "ring": parts[0],
                "tag": parts[1],
                "start_us": _int(parts[2]),
                "duration_us": _int(parts[3]),
                "a": _int(parts[4]),
                "b": _int(parts[5]),
            })
    if not t.complete and not allow_truncated:
        raise ValueError(
            "%s has no #complete marker: it was truncated. Re-run with --allow-truncated only if "
            "you understand that the tail is missing." % path)
    return t


def _int(s):
    try:
        return int(s)
    except (TypeError, ValueError):
        return 0


def percentile(sorted_values, q):
    """Nearest-rank, which is what a small sample deserves: no interpolation between two
    measurements that were never taken."""
    if not sorted_values:
        return 0
    k = max(1, min(len(sorted_values), int(round(q * len(sorted_values) + 0.5))))
    return sorted_values[k - 1]


def summarise(t):
    by_tag = {}
    for e in t.events:
        if e["ring"] != "recent":
            continue
        by_tag.setdefault(e["tag"], []).append(e["duration_us"])

    rows = []
    aggs = {a["tag"]: a for a in t.aggregates}
    for tag in sorted(set(list(by_tag.keys()) + list(aggs.keys()))):
        ds = sorted(by_tag.get(tag, []))
        a = aggs.get(tag, {})
        rows.append({
            "tag": tag,
            "session_count": a.get("count", 0),
            "session_mean_us": (a.get("total_us", 0) // a["count"]) if a.get("count") else 0,
            "session_max_us": a.get("max_us", 0),
            "session_slow_count": a.get("slow_count", 0),
            "ring_count": len(ds),
            "ring_p50_us": percentile(ds, 0.50),
            "ring_p95_us": percentile(ds, 0.95),
            "ring_p99_us": percentile(ds, 0.99),
        })

    slow = [e for e in t.events if e["ring"] == "slow"]
    slow.sort(key=lambda e: e["duration_us"], reverse=True)

    span_us = 0
    recent = [e for e in t.events if e["ring"] == "recent"]
    if recent:
        span_us = max(e["start_us"] + e["duration_us"] for e in recent) - \
                  min(e["start_us"] for e in recent)

    return {
        "path": t.path,
        "process": t.process,
        "complete": t.complete,
        "clock": t.clock,
        "capacity": t.capacity,
        "overwritten": t.overwritten,
        "caveats": t.caveats,
        "ring_span_us": span_us,
        "tags": rows,
        "slowest": slow[:20],
    }


def markdown(s):
    out = []
    out.append("# dlssnr frame trace -- %s" % os.path.basename(s["path"]))
    out.append("")
    out.append("Process: **%s**" % s["process"])
    unix = s["clock"].get("unix_utc", 0)
    if unix:
        out.append("Session started: %s UTC (unix %d)"
                   % (time.strftime("%Y-%m-%d %H:%M:%S", time.gmtime(unix)), unix))
    out.append("Ring span: %.2f s of events retained" % (s["ring_span_us"] / 1e6))

    lost_recent = s["overwritten"].get("recent", 0)
    lost_slow = s["overwritten"].get("slow", 0)
    if lost_recent or lost_slow:
        out.append("")
        out.append("> **This ring wrapped.** %d recent and %d slow events were overwritten and are "
                   "not in the file. The per-session columns below still cover the whole run; the "
                   "ring columns cover only the retained tail."
                   % (lost_recent, lost_slow))
    for c in s["caveats"]:
        out.append("")
        out.append("> Caveat: %s" % c)

    out.append("")
    out.append("## Per tag")
    out.append("")
    out.append("Whole-session columns come from counters kept outside the ring; ring columns come "
               "from the retained events only. Durations nest -- never add these together.")
    out.append("")
    out.append("| tag | session n | session mean | session max | session slow | ring n | p50 | p95 | p99 |")
    out.append("|---|---:|---:|---:|---:|---:|---:|---:|---:|")
    for r in s["tags"]:
        out.append("| `%s` | %d | %s | %s | %d | %d | %s | %s | %s |" % (
            r["tag"], r["session_count"], _ms(r["session_mean_us"]), _ms(r["session_max_us"]),
            r["session_slow_count"], r["ring_count"], _ms(r["ring_p50_us"]),
            _ms(r["ring_p95_us"]), _ms(r["ring_p99_us"])))

    if s["slowest"]:
        out.append("")
        out.append("## Slowest retained events")
        out.append("")
        out.append("| tag | at | duration | a | b |")
        out.append("|---|---:|---:|---:|---:|")
        for e in s["slowest"]:
            out.append("| `%s` | %.3f s | %s | %d | %d |"
                       % (e["tag"], e["start_us"] / 1e6, _ms(e["duration_us"]), e["a"], e["b"]))
    else:
        out.append("")
        out.append("No event crossed the slow threshold. That is the result, not a missing section.")
    out.append("")
    return "\n".join(out)


def _ms(us):
    return "%.2f ms" % (us / 1000.0)


def main(argv):
    allow_truncated = "--allow-truncated" in argv
    paths = [a for a in argv[1:] if not a.startswith("--")]
    if not paths:
        sys.stderr.write(__doc__)
        return 2
    rc = 0
    for p in paths:
        try:
            t = parse(p, allow_truncated)
        except (OSError, ValueError) as exc:
            sys.stderr.write("%s\n" % exc)
            rc = 1
            continue
        s = summarise(t)
        md = markdown(s)
        base = p[:-4] if p.endswith(".csv") else p
        with open(base + ".analysis.md", "w", encoding="utf-8") as f:
            f.write(md)
        with open(base + ".analysis.json", "w", encoding="utf-8") as f:
            json.dump(s, f, indent=2, sort_keys=True)
        print(md)
    return rc


if __name__ == "__main__":
    sys.exit(main(sys.argv))
