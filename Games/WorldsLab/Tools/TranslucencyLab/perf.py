#!/usr/bin/env python3
"""
perf.py -- TranslucencyLab's GPU spans from perf.ps1's CSV, summarised
(Engine/Modules/RenderCore/LIT_TRANSLUCENCY_PERF.md).

    py -3 perf.py <csv> [--base <config>] [--spans Forward Shadows Total]

Per backend, view and span: each config's median over every sample (runs x
samples), its spread (min-max), and with --base its difference from the base
config's median, as a Markdown table.
"""
import argparse
import csv
import statistics
from collections import defaultdict

VIEWS = {"0": "lab", "1": "glass", "2": "plume", "3": "water"}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("csv")
    ap.add_argument("--base")
    ap.add_argument("--spans", nargs="*", default=["Shadows", "Lighting", "Forward", "Post", "Total"])
    args = ap.parse_args()

    data = defaultdict(list)   # (config, backend, view, span) -> [ms]
    configs = []
    with open(args.csv, newline="") as f:
        for row in csv.DictReader(f):
            data[(row["config"], row["backend"], row["view"], row["span"])].append(float(row["ms"]))
            if row["config"] not in configs:
                configs.append(row["config"])
    backends = sorted({k[1] for k in data})
    views = sorted({k[2] for k in data})
    for backend in backends:
        print(f"\n### {backend}\n")
        head = "| View | Config | " + " | ".join(args.spans) + " |"
        print(head)
        print("|" + "---|" * (2 + len(args.spans)))
        for view in views:
            for config in configs:
                cells = []
                for span in args.spans:
                    values = data.get((config, backend, view, span))
                    if not values:
                        cells.append("-")
                        continue
                    median = statistics.median(values)
                    cell = f"{median:.3f}"
                    if args.base and config != args.base and data.get((args.base, backend, view, span)):
                        delta = median - statistics.median(data[(args.base, backend, view, span)])
                        cell += f" ({delta:+.3f})"
                    else:
                        cell += f" [{min(values):.2f}-{max(values):.2f}]"
                    cells.append(cell)
                print(f"| {VIEWS.get(view, view)} | {config} | " + " | ".join(cells) + " |")


if __name__ == "__main__":
    main()
