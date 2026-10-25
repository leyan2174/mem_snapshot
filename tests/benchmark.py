#!/usr/bin/env python3
import json
import os
import platform
import statistics
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
result = {"platform": platform.platform(), "libc": platform.libc_ver(),
          "iterations_per_thread": 2000000, "repeats": 5, "rows": []}
with tempfile.TemporaryDirectory(prefix="mem-snapshot-bench-") as temp:
    for threads, live in [(1, 1), (4, 1), (1, 65536), (4, 16384)]:
        baseline = None
        for depth in (None, 0, 16):
            times = []
            for repetition in range(5):
                env = dict(os.environ)
                env.pop("LD_PRELOAD", None)
                if depth is not None:
                    env.update(LD_PRELOAD=str(ROOT / "build/libmem_snapshot.so"),
                               MS_DEPTH=str(depth),
                               MS_OUTPUT=f"{temp}/{threads}-{live}-{depth}-{repetition}.log")
                elapsed = float(subprocess.check_output(
                    [str(ROOT / "build/bench"), str(threads), "2000000", str(live)],
                    env=env, text=True, timeout=120))
                times.append(elapsed)
            median = statistics.median(times)
            if depth is None: baseline = median
            row = {"threads": threads, "live_per_thread": live, "depth": depth,
                   "seconds": median, "million_pairs_per_second": threads * 2.0 / median,
                   "relative_time": median / baseline, "runs": times}
            result["rows"].append(row)
            print(json.dumps(row), flush=True)
(ROOT / "build/benchmark.json").write_text(json.dumps(result, indent=2) + "\n")
