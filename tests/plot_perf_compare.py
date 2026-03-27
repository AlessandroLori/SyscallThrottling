import csv
import math
import sys
from collections import defaultdict

csv_path = sys.argv[1] if len(sys.argv) > 1 else "perf_compare.csv"

rows = []
with open(csv_path, newline="") as f:
    reader = csv.DictReader(f)
    for row in reader:
        def parse_int_or_none(x):
            x = (x or "").strip()
            return None if x == "" else int(x)

        def parse_float_or_none(x):
            x = (x or "").strip()
            return None if x == "" else float(x)

        rows.append({
            "policy": row["policy"],
            "run": int(row["run"]),
            "threads": int(row["threads"]),
            "calls": int(row["calls"]),
            "max_per_epoch": int(row["max_per_epoch"]),
            "cycles": parse_int_or_none(row["cycles"]),
            "task_clock_ms": parse_float_or_none(row["task_clock_ms"]),
            "context_switches": parse_int_or_none(row["context_switches"]),
        })

if not rows:
    print("CSV vuoto")
    sys.exit(1)

group = defaultdict(list)
for r in rows:
    group[r["policy"]].append(r)

policies = ["FIFO", "WAKE_RACE"]

def mean(vals):
    vals = [v for v in vals if v is not None]
    return sum(vals) / len(vals) if vals else 0.0

def stdev(vals):
    vals = [v for v in vals if v is not None]
    if len(vals) < 2:
        return 0.0
    m = mean(vals)
    return math.sqrt(sum((x - m) ** 2 for x in vals) / (len(vals) - 1))

def svg_bar_chart(filename, title, ylabel, series_name):
    width = 900
    height = 520
    margin_left = 90
    margin_right = 40
    margin_top = 60
    margin_bottom = 80

    values = [mean([r[series_name] for r in group[p]]) for p in policies]
    stds = [stdev([r[series_name] for r in group[p]]) for p in policies]
    raw_vals = {p: [r[series_name] for r in group[p] if r[series_name] is not None] for p in policies}

    ymax = max([v + s for v, s in zip(values, stds)] + [1.0])
    ymax *= 1.15

    plot_w = width - margin_left - margin_right
    plot_h = height - margin_top - margin_bottom

    def ymap(v):
        return margin_top + plot_h - (v / ymax) * plot_h

    bar_w = 140
    centers = [margin_left + plot_w * 0.30, margin_left + plot_w * 0.70]

    out = []
    out.append(f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}">')
    out.append(f'<rect width="100%" height="100%" fill="white"/>')
    out.append(f'<text x="{width/2}" y="30" text-anchor="middle" font-size="24">{title}</text>')

    out.append(f'<line x1="{margin_left}" y1="{margin_top}" x2="{margin_left}" y2="{margin_top+plot_h}" stroke="black"/>')
    out.append(f'<line x1="{margin_left}" y1="{margin_top+plot_h}" x2="{margin_left+plot_w}" y2="{margin_top+plot_h}" stroke="black"/>')

    ticks = 5
    for i in range(ticks + 1):
        v = ymax * i / ticks
        y = ymap(v)
        out.append(f'<line x1="{margin_left-5}" y1="{y}" x2="{margin_left}" y2="{y}" stroke="black"/>')
        out.append(f'<text x="{margin_left-10}" y="{y+5}" text-anchor="end" font-size="12">{v:.0f}</text>')

    out.append(f'<text x="25" y="{margin_top + plot_h/2}" transform="rotate(-90 25,{margin_top + plot_h/2})" text-anchor="middle" font-size="16">{ylabel}</text>')

    for i, p in enumerate(policies):
        cx = centers[i]
        v = values[i]
        s = stds[i]
        x = cx - bar_w / 2
        y = ymap(v)
        h = margin_top + plot_h - y

        fill = "#4C78A8" if p == "FIFO" else "#F58518"
        out.append(f'<rect x="{x}" y="{y}" width="{bar_w}" height="{h}" fill="{fill}" stroke="black"/>')

        y1 = ymap(max(v - s, 0))
        y2 = ymap(v + s)
        out.append(f'<line x1="{cx}" y1="{y1}" x2="{cx}" y2="{y2}" stroke="black"/>')
        out.append(f'<line x1="{cx-10}" y1="{y1}" x2="{cx+10}" y2="{y1}" stroke="black"/>')
        out.append(f'<line x1="{cx-10}" y1="{y2}" x2="{cx+10}" y2="{y2}" stroke="black"/>')

        out.append(f'<text x="{cx}" y="{margin_top+plot_h+30}" text-anchor="middle" font-size="16">{p}</text>')

        out.append(f'<text x="{cx}" y="{y-10}" text-anchor="middle" font-size="13">mean={v:.2f}</text>')

    out.append('</svg>')

    with open(filename, "w") as f:
        f.write("\n".join(out))

svg_bar_chart("task_clock_compare.svg",
              "Confronto task-clock: FIFO vs WAKE_RACE",
              "Task clock (ms)",
              "task_clock_ms")

svg_bar_chart("context_switches_compare.svg",
              "Confronto context-switches: FIFO vs WAKE_RACE",
              "Context switches",
              "context_switches")

has_cycles = any(r["cycles"] is not None for r in rows)
if has_cycles:
    svg_bar_chart("cycles_compare.svg",
                  "Confronto cycles: FIFO vs WAKE_RACE",
                  "Cycles",
                  "cycles")
    print("Creato: cycles_compare.svg")
else:
    print("Cycles hardware non supportati in questo ambiente: grafico cycles saltato.")

print("Creati:")
print("  task_clock_compare.svg")
print("  context_switches_compare.svg")