#!/usr/bin/env python3
"""Plot compression speed against ratio from bench/curve TSV output.

    python3 bench/plot_curve.py out.png "title" label1=run1.tsv label2=run2.tsv ...

Each TSV is what bench/curve (or a tool writing the same columns) prints: one row
per level with at least the columns mode, level, ratio (compressed/original) and
c_MBps. The x axis is compression speed on a log scale, fastest on the left, as
in the graph fast-lzma2 was designed against; the y axis is original/compressed,
so the top left is the better corner. Every point is labelled with its level.
"""
import sys

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.ticker import FuncFormatter, LogLocator, NullFormatter


def read_tsv(path):
    header, rows = None, []
    for line in open(path):
        line = line.rstrip("\n")
        if not line or line.startswith("#"):
            continue
        fields = line.split("\t")
        if header is None:
            header = fields
            continue
        rows.append(dict(zip(header, fields)))
    return rows


def main():
    if len(sys.argv) < 4:
        sys.exit(__doc__)
    out, title, series = sys.argv[1], sys.argv[2], sys.argv[3:]
    colors = ["#e8742a", "#3a6fc4", "#4a9d5b", "#9b59b6"]

    fig, ax = plt.subplots(figsize=(9.2, 5.8), dpi=100)
    for i, spec in enumerate(series):
        label, path = spec.split("=", 1)
        rows = read_tsv(path)
        x = [float(r["c_MBps"]) for r in rows]
        y = [1.0 / float(r["ratio"]) for r in rows]
        color = colors[i % len(colors)]
        ax.plot(x, y, "-o", color=color, label=label, linewidth=2, markersize=5)
        for r, xi, yi in zip(rows, x, y):
            ax.annotate(r["level"], (xi, yi), textcoords="offset points", xytext=(4, 5),
                        fontsize=8, color=color)

    ax.set_xscale("log")
    ax.invert_xaxis()
    ax.xaxis.set_major_locator(LogLocator(base=10, subs=(1.0, 2.0, 5.0)))
    ax.xaxis.set_minor_formatter(NullFormatter())
    ax.xaxis.set_major_formatter(FuncFormatter(lambda v, _: f"{v:g}"))
    ax.set_xlabel("compression, MB/s (log scale)")
    ax.set_ylabel("ratio (original / compressed)")
    ax.set_title(title)
    ax.grid(True, which="major", color="#dddddd")
    ax.legend(loc="lower right")
    fig.tight_layout()
    fig.savefig(out)


if __name__ == "__main__":
    main()
