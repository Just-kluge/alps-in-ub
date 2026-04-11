#!/usr/bin/env python3
"""计算任务的平均 FCT、P99 FCT 和最大 FCT。

用法示例:
  python3 compute_fct.py /path/to/task_statistics.csv

默认使用列 `taskStartTime(us)` 和 `taskCompletesTime(us)`。
可通过 `--mode packet` 改为使用 `firstPacketSends(us)` 和 `lastPacketACKs(us)`，
或用 `--start-col`/`--end-col` 指定自定义列名。


python3 /app/ns-3-ub/scratch/ns-3-ub-tools/ALPS_tools/compute_fct.py /app/ns-3-ub/scratch/tp_UB_Mesh_V0002/output/task_statistics.csv
"""

import argparse
import csv
import math
import sys
from typing import List, Optional


def percentile(values: List[float], p: float) -> Optional[float]:
    if not values:
        return None
    vals = sorted(values)
    n = len(vals)
    # Nearest-rank method
    k = math.ceil(p / 100.0 * n)
    k = max(1, min(k, n))
    return vals[k - 1]


def parse_args():
    parser = argparse.ArgumentParser(description="Compute FCT statistics from CSV")
    parser.add_argument("csvfile", help="Path to task_statistics.csv")
    parser.add_argument("--mode", choices=["task", "packet"], default="task",
                        help="Which timestamp pair to use: 'task' uses taskStartTime/taskCompletesTime (default); 'packet' uses firstPacketSends/lastPacketACKs")
    parser.add_argument("--start-col", help="Override start timestamp column name")
    parser.add_argument("--end-col", help="Override end timestamp column name")
    return parser.parse_args()


def read_durations(path: str, start_col: str, end_col: str) -> List[float]:
    durations = []
    skipped = 0
    with open(path, newline='') as f:
        reader = csv.DictReader(f)
        for row in reader:
            s = row.get(start_col)
            e = row.get(end_col)
            if s is None or e is None:
                skipped += 1
                continue
            try:
                s_val = float(s)
                e_val = float(e)
            except Exception:
                skipped += 1
                continue
            dur = e_val - s_val
            if dur >= 0:
                durations.append(dur)  # keep in microseconds
            else:
                skipped += 1
    if skipped:
        print(f"Note: skipped {skipped} rows due to missing/invalid timestamps.")
    return durations


def fmt(us: float) -> str:
    # Format microseconds and seconds
    s = us / 1e6
    if us >= 1000:
        return f"{us:,.1f} µs ({s:.6f} s)"
    else:
        return f"{us:.1f} µs ({s:.6f} s)"


def main():
    args = parse_args()
    if args.start_col and args.end_col:
        start_col = args.start_col
        end_col = args.end_col
    else:
        if args.mode == 'task':
            start_col = 'taskStartTime(us)'
            end_col = 'taskCompletesTime(us)'
        else:
            start_col = 'firstPacketSends(us)'
            end_col = 'lastPacketACKs(us)'

    durations = read_durations(args.csvfile, start_col, end_col)
    n = len(durations)
    if n == 0:
        print("No valid durations found. Check column names and CSV format.")
        sys.exit(2)

    total = sum(durations)
    mean_us = total / n
    p99_us = percentile(durations, 99)
    max_us = max(durations)

    print(f"Input file: {args.csvfile}")
    print(f"Using columns: start='{start_col}', end='{end_col}'")
    print(f"Samples: {n}")
    print("")
    print(f"Average FCT: {fmt(mean_us)}")
    print(f"P99 FCT:     {fmt(p99_us) if p99_us is not None else 'N/A'}")
    print(f"Max FCT:     {fmt(max_us)}")


if __name__ == '__main__':
    main()
