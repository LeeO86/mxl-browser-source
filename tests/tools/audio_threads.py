#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""What Chromium's audio threads do while the page's audio stalls (docs/performance.md, M7).

    audio_threads.py sample CONTAINER SECONDS OUT.jsonl
        Every 5 ms, from the host's /proc (same uid as the container, no ptrace needed): state,
        wchan, schedstat (run time, run-queue wait, switches) and CPU of the audio-path threads of
        the container's processes: AudioInputDevic (the browser process's capture thread that
        calls OnAudioStreamPacket), the audio service's AudioWorkerThre and Loopback Worker, and
        the renderer's AudioOutputDevi and Media threads.
    audio_threads.py stalls AUDIOTRACE.json OUT.jsonl [--stall 60]
        For each pause between packets in a cadence_bench.py audio trace, what each thread did:
        run time, run-queue wait, its states (S sleeping, R running, D waiting in the kernel) and
        kernel wait channels. A capture thread in D with do_get_write_access or __lock_buffer
        waits for the ext4 journal.

Standard library only.
"""
import argparse
import json
import os
import subprocess
import time

NAMES = {"AudioInputDevic", "AudioWorkerThre", "Loopback Worker", "AudioOutputDevi", "Media"}
TAI_UTC = 37


def find(cid):
    threads = {}
    for p in os.listdir("/proc"):
        if not p.isdigit():
            continue
        try:
            with open(f"/proc/{p}/cgroup") as f:
                if cid not in f.read():
                    continue
            for t in os.listdir(f"/proc/{p}/task"):
                with open(f"/proc/{p}/task/{t}/comm") as f:
                    comm = f.read().strip()
                if comm in NAMES:
                    threads[int(t)] = (int(p), comm)
        except OSError:
            pass
    return threads


def sample(container, seconds, out):
    cid = subprocess.run(["docker", "inspect", "-f", "{{.Id}}", container], capture_output=True, text=True).stdout.strip()
    threads = find(cid)
    end = time.time() + seconds
    last_scan = time.time()
    with open(out, "w") as f:
        f.write(json.dumps({"threads": {str(k): v for k, v in threads.items()}}) + "\n")
        while time.time() < end:
            now = time.time()
            if now - last_scan > 2:  # streams come and go: their threads too
                new = find(cid)
                if new != threads:
                    threads = new
                    f.write(json.dumps({"threads": {str(k): v for k, v in threads.items()}}) + "\n")
                last_scan = now
            row = [round(now, 4)]
            for tid, (pid, _) in threads.items():
                try:
                    with open(f"/proc/{pid}/task/{tid}/stat") as s:
                        stat = s.read()
                    rest = stat[stat.rindex(")") + 2:].split()
                    with open(f"/proc/{pid}/task/{tid}/schedstat") as s:
                        sched = s.read().split()
                    with open(f"/proc/{pid}/task/{tid}/wchan") as s:
                        wchan = s.read()
                    row.append([tid, rest[0], wchan, int(sched[0]), int(sched[1]), int(sched[2]), int(rest[36])])
                except OSError:
                    pass
            f.write(json.dumps(row) + "\n")
            time.sleep(max(0.0, 0.005 - (time.time() - now)))


def stalls(trace, samples, stall_ms):
    with open(trace) as f:
        packets = [e for e in json.load(f)["events"] if e[1] == "packet"]
    names, rows = {}, []
    with open(samples) as f:
        for line in f:
            r = json.loads(line)
            if isinstance(r, dict):
                names.update({int(k): v[1] for k, v in r["threads"].items()})
            else:
                rows.append(r)
    for i in range(1, len(packets)):
        gap = (packets[i][0] - packets[i - 1][0]) / 1e6
        if gap < stall_ms:
            continue
        a = packets[i - 1][0] / 1e9 - TAI_UTC
        b = packets[i][0] / 1e9 - TAI_UTC
        print(f"stall {gap:.0f} ms at {time.strftime('%H:%M:%S', time.gmtime(a))} UTC")
        win = [r for r in rows if a - 0.02 <= r[0] <= b + 0.01]
        if not win:
            print("  no samples")
            continue

        def of(r, tid):
            return next((t for t in r[1:] if t[0] == tid), None)
        for tid in sorted({t[0] for t in win[0][1:]}):
            first, last = of(win[0], tid), of(win[-1], tid)
            if not first or not last:
                continue
            states = "".join((of(r, tid) or [0, "?"])[1] for r in win)
            wchans = sorted({(of(r, tid) or [0, 0, ""])[2] for r in win} - {"", "0"})
            cpus = sorted({(of(r, tid) or [0] * 7)[6] for r in win})
            print(f"  {names.get(tid, tid):16s} run +{(last[3] - first[3]) / 1e6:6.1f} ms  runq-wait +{(last[4] - first[4]) / 1e6:6.1f} ms  "
                  f"switches +{last[5] - first[5]:4d}  cpus {cpus}  states {states[:80]}  wchan {wchans[:4]}")


def main():
    p = argparse.ArgumentParser()
    sub = p.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("sample")
    s.add_argument("container")
    s.add_argument("seconds", type=float)
    s.add_argument("out")
    t = sub.add_parser("stalls")
    t.add_argument("trace")
    t.add_argument("samples")
    t.add_argument("--stall", type=float, default=60.0)
    args = p.parse_args()
    if args.cmd == "sample":
        sample(args.container, args.seconds, args.out)
    else:
        stalls(args.trace, args.samples, args.stall)


if __name__ == "__main__":
    main()
