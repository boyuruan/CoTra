#!/usr/bin/env python3
"""Sum scala_v3 COMM_PROFILE blocks across machines, per ef.

usage: comm_table.py LOG_DIR N [--types] [--chan] [--sections]
                     [--at R1,R2,...]

Reads LOG_DIR/search.m{0..N-1}.log. Each machine prints one COMM block and
then its ef row; machine 0's row carries the all-machine recall. --sections
prints QUERY bytes by purpose (fork, migrate) and section, SYNC bytes by
section, and the summed work counters. --at interpolates the summed
counters linearly in recall between the two ef rows that bracket each
target, so runs with different recall-per-ef compare at the same recall.
"""
import argparse
import re
import sys

ROW = re.compile(r"^(\d+)\s*\t\s*([0-9.]+)\s*\t")
TOTAL = re.compile(
    r"ReadBytes: (\d+) ReadCnt: (\d+) WriteBytes: (\d+) WriteCnt: (\d+)")
WTYPE = re.compile(r"WriteType: (\w+) Bytes: (\d+) Cnt: (\d+)")
CHAN = re.compile(
    r"Chan: (\w+) Deliv: (\d+) Dup: (\d+) Pass: (\d+) Gated: (\d+)")
QUERY = re.compile(
    r"Query: (\w+) Cnt: (\d+) Head: (\d+) Top: (\d+) Cand: (\d+) "
    r"Result: (\d+)")
SYNC = re.compile(
    r"Sync: Cnt: (\d+) Head: (\d+) Cand: (\d+) Target: (\d+) Other: (\d+) "
    r"Token: (\d+)")
WORK = re.compile(
    r"Work: Expand: (\d+) NodeTask: (\d+) Dist: (\d+) PqDist: (\d+)")
QUERY_COLS = ("cnt", "head", "top", "cand", "result")
SYNC_COLS = ("cnt", "head", "cand", "target", "other", "token")
WORK_COLS = ("expand", "node_task", "dist", "pq_dist")


def parse(path):
    rows = []
    cur = {}
    with open(path) as f:
        for line in f:
            m = TOTAL.search(line)
            if m:
                cur["rb"], cur["rc"], cur["wb"], cur["wc"] = map(int, m.groups())
                continue
            m = WTYPE.search(line)
            if m:
                cur.setdefault("types", {})[m.group(1)] = (
                    int(m.group(2)), int(m.group(3)))
                continue
            m = CHAN.search(line)
            if m:
                cur.setdefault("chan", {})[m.group(1)] = tuple(
                    int(x) for x in m.groups()[1:])
                continue
            m = QUERY.search(line)
            if m:
                cur.setdefault("query", {})[m.group(1)] = tuple(
                    int(x) for x in m.groups()[1:])
                continue
            m = SYNC.search(line)
            if m:
                cur["sync"] = tuple(int(x) for x in m.groups())
                continue
            m = WORK.search(line)
            if m:
                cur["work"] = tuple(int(x) for x in m.groups())
                continue
            m = ROW.match(line)
            if m and "rb" in cur:
                cur["ef"] = int(m.group(1))
                cur["recall"] = float(m.group(2))
                rows.append(cur)
                cur = {}
    return rows


def add(acc, row):
    for k in ("rb", "rc", "wb", "wc"):
        acc[k] = acc.get(k, 0) + row[k]
    for t, (b, c) in row.get("types", {}).items():
        ob, oc = acc.setdefault("types", {}).get(t, (0, 0))
        acc["types"][t] = (ob + b, oc + c)
    for ch, v in row.get("chan", {}).items():
        old = acc.setdefault("chan", {}).get(ch, (0, 0, 0, 0))
        acc["chan"][ch] = tuple(a + b for a, b in zip(old, v))
    for p, v in row.get("query", {}).items():
        old = acc.setdefault("query", {}).get(p, (0,) * len(QUERY_COLS))
        acc["query"][p] = tuple(a + b for a, b in zip(old, v))
    for k, cols in (("sync", SYNC_COLS), ("work", WORK_COLS)):
        if k in row:
            old = acc.get(k, (0,) * len(cols))
            acc[k] = tuple(a + b for a, b in zip(old, row[k]))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("log_dir")
    ap.add_argument("n", type=int)
    ap.add_argument("--types", action="store_true")
    ap.add_argument("--chan", action="store_true")
    ap.add_argument("--sections", action="store_true")
    ap.add_argument("--at", default="")
    a = ap.parse_args()

    per = [parse(f"{a.log_dir}/search.m{m}.log") for m in range(a.n)]
    nrow = min(len(p) for p in per)
    if nrow == 0:
        sys.exit("no COMM blocks")
    table = []
    for i in range(nrow):
        acc = {"ef": per[0][i]["ef"], "recall": per[0][i]["recall"]}
        for m in range(a.n):
            if per[m][i]["ef"] != acc["ef"]:
                sys.exit(f"ef mismatch at row {i} on m{m}")
            add(acc, per[m][i])
        table.append(acc)

    print("ef\trecall\tread_bytes\tread_cnt\twrite_bytes\twrite_cnt")
    for r in table:
        print(f"{r['ef']}\t{r['recall']:.5f}\t{r['rb']}\t{r['rc']}"
              f"\t{r['wb']}\t{r['wc']}")

    if a.types:
        names = sorted({t for r in table for t in r.get("types", {})})
        print("\nWRITE by type, bytes/cnt")
        print("ef\t" + "\t".join(names))
        for r in table:
            cells = [f"{r['types'].get(t, (0, 0))[0]}/"
                     f"{r['types'].get(t, (0, 0))[1]}" for t in names]
            print(f"{r['ef']}\t" + "\t".join(cells))

    if a.chan:
        names = ["READ", "MICRO", "RESULT", "NODE_RESULT"]
        print("\nchannel deliv/dup/pass/gated")
        print("ef\t" + "\t".join(names))
        for r in table:
            cells = ["/".join(str(x) for x in r.get("chan", {}).get(
                c, (0, 0, 0, 0))) for c in names]
            print(f"{r['ef']}\t" + "\t".join(cells))

    if a.sections:
        for p in ("FORK", "MIGRATE"):
            print(f"\nQUERY {p} bytes by section")
            print("ef\t" + "\t".join(QUERY_COLS))
            for r in table:
                v = r.get("query", {}).get(p, (0,) * len(QUERY_COLS))
                print(f"{r['ef']}\t" + "\t".join(str(x) for x in v))
        print("\nSYNC bytes by section; target is the receiver's own lists")
        print("ef\t" + "\t".join(SYNC_COLS))
        for r in table:
            v = r.get("sync", (0,) * len(SYNC_COLS))
            print(f"{r['ef']}\t" + "\t".join(str(x) for x in v))
        print("\nwork summed over machines")
        print("ef\t" + "\t".join(WORK_COLS))
        for r in table:
            v = r.get("work", (0,) * len(WORK_COLS))
            print(f"{r['ef']}\t" + "\t".join(str(x) for x in v))

    if a.at:
        print("\nat recall (linear in recall between bracketing ef rows)")
        print("recall\tef_lo\tef_hi\tread_bytes\tread_cnt\twrite_bytes"
              "\twrite_cnt")
        # The ef<k rows all run at ef=k; keep the last of equal-ef rows.
        rows = [r for r in table if r["ef"] >= 10]
        for target in (float(x) for x in a.at.split(",")):
            hit = None
            for lo, hi in zip(rows, rows[1:]):
                if lo["recall"] <= target <= hi["recall"]:
                    hit = (lo, hi)
                    break
            if hit is None:
                print(f"{target:.3f}\tout of range")
                continue
            lo, hi = hit
            span = hi["recall"] - lo["recall"]
            w = 0.0 if span == 0 else (target - lo["recall"]) / span
            vals = [lo[k] + w * (hi[k] - lo[k]) for k in ("rb", "rc", "wb",
                                                          "wc")]
            print(f"{target:.3f}\t{lo['ef']}\t{hi['ef']}\t" +
                  "\t".join(f"{v:.0f}" for v in vals))


if __name__ == "__main__":
    main()
