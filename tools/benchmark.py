#!/usr/bin/env python3
"""Run both logger designs across CAN bus loads and plot how much gets logged.

usage: python tools/benchmark.py [--binary build/rtos_logger] [--duration 2]
writes docs/benchmark.csv and docs/benchmark.png
"""
import argparse
import csv
import json
import subprocess
import tempfile
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
RATES = [250, 500, 1000, 2000, 3000, 4000]          # CAN frames per second
MODES = {"sequential": ("Sequential (prototype)", "#eb6834", "s"),
         "rtos": ("FreeRTOS producer/consumer", "#2a78d6", "o")}
SURFACE, INK, INK2, MUTED, GRID, AXIS = "#fcfcfb", "#0b0b0b", "#52514e", "#898781", "#e1e0d9", "#c3c2b7"


def run(binary: Path, mode: str, rate: int, duration: float) -> dict:
    with tempfile.TemporaryDirectory() as out:
        subprocess.run([str(binary), "--mode", mode, "--can-rate", str(rate),
                        "--duration", str(duration), "--out", out],
                       check=True, capture_output=True, timeout=120)
        return json.loads((Path(out) / "stats.json").read_text())


def plot(rows: list[dict], path: Path) -> None:
    fig, ax = plt.subplots(figsize=(8, 4.4), dpi=120)
    fig.patch.set_facecolor(SURFACE)
    ax.set_facecolor(SURFACE)
    for mode, (label, color, marker) in MODES.items():
        pts = [(r["can_rate"], r["can_logged_pct"]) for r in rows if r["mode"] == mode]
        xs, ys = zip(*pts)
        ax.plot(xs, ys, color=color, lw=2, marker=marker, ms=7,
                markeredgecolor=SURFACE, markeredgewidth=1.2, label=label)
        ax.annotate(f"{label}: {ys[-1]:.0f} %", (xs[-1], ys[-1]), xytext=(-6, 10),
                    textcoords="offset points", ha="right", fontsize=9, color=INK2)
    ax.set_ylim(0, 105)
    ax.set_xlim(0, max(RATES) * 1.04)
    ax.set_xlabel("CAN bus load (frames per second, K-Line running in parallel)", fontsize=9, color=MUTED)
    ax.set_ylabel("CAN frames logged (%)", fontsize=9, color=MUTED)
    ax.set_title("Share of CAN frames written to the SD card", loc="left", fontsize=12, color=INK)
    ax.grid(axis="y", color=GRID, linewidth=0.6)
    ax.set_axisbelow(True)
    for side in ("top", "right"):
        ax.spines[side].set_visible(False)
    for side in ("left", "bottom"):
        ax.spines[side].set_color(AXIS)
    ax.tick_params(colors=MUTED, labelsize=9, length=0)
    ax.legend(frameon=False, fontsize=9, loc="center right")
    fig.tight_layout()
    fig.savefig(path, facecolor=SURFACE)
    plt.close(fig)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", type=Path, default=ROOT / "build" / "rtos_logger")
    ap.add_argument("--duration", type=float, default=2.0)
    ap.add_argument("--out", type=Path, default=ROOT / "docs")
    a = ap.parse_args()
    rows = []
    for rate in RATES:
        for mode in MODES:
            s = run(a.binary, mode, rate, a.duration)
            rows.append(s)
            print(f"{mode:10s} CAN {rate:5d}/s  logged {s['can_logged_pct']:6.1f} %   "
                  f"K-Line logged {s['kline_logged_pct']:6.1f} %")
    a.out.mkdir(exist_ok=True)
    keys = ["mode", "can_rate", "can_received", "can_logged", "can_logged_pct", "can_fifo_overruns",
            "can_buffer_drops", "kline_received", "kline_logged", "kline_logged_pct", "kline_overruns",
            "sd_writes", "sd_syncs", "bytes_written"]
    with open(a.out / "benchmark.csv", "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=keys, extrasaction="ignore")
        w.writeheader()
        w.writerows(rows)
    plot(rows, a.out / "benchmark.png")
    print(f"wrote {a.out / 'benchmark.csv'} and {a.out / 'benchmark.png'}")


if __name__ == "__main__":
    main()
