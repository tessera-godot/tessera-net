"""Generates the benchmark charts used in the documentation as plain SVG files (standard library only).

Run from the repository root:  python tools/make_charts.py
Edit the numbers below when new measurements are recorded in docs/benchmarks.md.
"""
import math
from pathlib import Path
from xml.sax.saxutils import escape

OUT = Path(__file__).resolve().parent.parent / "docs" / "images"

FONT = "font-family=\"Segoe UI, Helvetica, Arial, sans-serif\""
INK = "#1f2937"
MUTED = "#6b7280"
GRID = "#e5e7eb"
BLUE = "#2563eb"
RED = "#dc2626"
AMBER = "#d97706"
GREEN = "#16a34a"


def text(x, y, content, size=13, fill=INK, anchor="start", weight="normal"):
    return (f'<text x="{x}" y="{y}" font-size="{size}" fill="{fill}" text-anchor="{anchor}" '
            f'font-weight="{weight}" {FONT}>{escape(content)}</text>')


def card(width, height, title, subtitle, body):
    # A white card keeps the chart readable on both light and dark pages.
    return "\n".join([
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" viewBox="0 0 {width} {height}" role="img" aria-label="{escape(title)}">',
        f'<title>{escape(title)}</title>',
        f'<rect x="0.5" y="0.5" width="{width - 1}" height="{height - 1}" rx="10" fill="#ffffff" stroke="{GRID}"/>',
        text(24, 34, title, 17, INK, weight="bold"),
        text(24, 54, subtitle, 12, MUTED),
        body,
        "</svg>",
    ]) + "\n"


def baseline_chart():
    """Godot's built-in replication: server frame rate as the entity count grows."""
    data = [("500", 60.0, 10.5), ("2,000", 59.4, 36.6), ("5,000", 26.8, 89.2), ("10,000", 7.4, 97.7)]
    width, height = 720, 380
    left, bottom, top, plot_w = 70, 310, 90, 600
    plot_h = bottom - top
    parts = []
    for fps in (0, 15, 30, 45, 60):
        y = bottom - plot_h * fps / 60.0
        parts.append(f'<line x1="{left}" y1="{y:.1f}" x2="{left + plot_w}" y2="{y:.1f}" stroke="{GRID}"/>')
        parts.append(text(left - 8, y + 4, str(fps), 11, MUTED, "end"))
    cy = top + plot_h / 2
    parts.append(f'<text transform="rotate(-90 18 {cy})" x="18" y="{cy}" font-size="11" fill="{MUTED}" '
                 f'text-anchor="middle" {FONT}>server frame rate (fps)</text>')
    slot = plot_w / len(data)
    for i, (label, fps, cpu) in enumerate(data):
        h = plot_h * fps / 60.0
        x = left + slot * i + slot * 0.2
        w = slot * 0.6
        color = BLUE if fps >= 55 else (AMBER if fps >= 20 else RED)
        parts.append(f'<rect x="{x:.1f}" y="{bottom - h:.1f}" width="{w:.1f}" height="{h:.1f}" rx="3" fill="{color}"/>')
        parts.append(text(x + w / 2, bottom - h - 8, f"{fps:g} fps", 13, INK, "middle", "bold"))
        parts.append(text(x + w / 2, bottom + 20, f"{label} entities", 12, INK, "middle"))
        parts.append(text(x + w / 2, bottom + 37, f"server CPU {cpu:g}% of one core", 11, MUTED, "middle"))
    parts.append(f'<line x1="{left}" y1="{bottom}" x2="{left + plot_w}" y2="{bottom}" stroke="{MUTED}"/>')
    return card(width, height, "Godot's built-in replication: where it stops scaling",
                "8 clients see every entity, one position per entity at 20 Hz, one machine (localhost). Target: 60 fps.",
                "\n".join(parts))


def acceptance_chart():
    """Data core results: measured value as a share of its limit (shorter is better)."""
    rows = [
        ("Create 1 M entities", 11.75, 32.0, "ms"),
        ("Destroy 1 M entities", 5.69, 12.0, "ms"),
        ("Churn: replace 10% ten times", 36.7, 420.0, "ms"),
        ("Iterate 1 M entities (one pass)", 1.37, 3.0, "ms"),
        ("Bookkeeping per entity", 4.25, 8.0, "bytes"),
        ("Tick jitter at 30 Hz (p99)", 0.06, 2.0, "ms"),
    ]
    width, height = 720, 380
    left, plot_w, top, row_h = 250, 340, 92, 40
    parts = []
    for pct in (0, 25, 50, 75, 100):
        x = left + plot_w * pct / 100.0
        parts.append(f'<line x1="{x:.1f}" y1="{top - 8}" x2="{x:.1f}" y2="{top + row_h * len(rows)}" stroke="{GRID}"/>')
        parts.append(text(x, top - 14, f"{pct}%", 11, MUTED, "middle"))
    for i, (label, value, limit, unit) in enumerate(rows):
        y = top + row_h * i
        share = value / limit
        parts.append(text(left - 12, y + 22, label, 13, INK, "end"))
        parts.append(f'<rect x="{left}" y="{y + 8}" width="{plot_w * share:.1f}" height="20" rx="3" fill="{BLUE}"/>')
        parts.append(text(left + plot_w + 14, y + 23, f"{value:g} of {limit:g} {unit}", 12, MUTED))
    parts.append(f'<line x1="{left + plot_w}" y1="{top - 8}" x2="{left + plot_w}" y2="{top + row_h * len(rows)}" stroke="{RED}" stroke-width="1.5"/>')
    parts.append(text(left + plot_w, top + row_h * len(rows) + 22, "limit", 11, RED, "middle"))
    parts.append(text(24, height - 22, "Also: 91 to 95% parallel efficiency with 8 workers, and identical results with 1 and 8 workers.", 12, MUTED))
    return card(width, height, "Data core: measured value vs. its limit",
                "1,000,000 entities, Release build, median of 5 runs, desktop CPU with 12 cores. Bars shorter than the red line pass.",
                "\n".join(parts))


def nodes_chart():
    """One million entities kept as Godot nodes versus in the entity table (same process, log scale)."""
    rows = [
        ("Create 1 M entities", 615.0, 12.4),
        ("Destroy 1 M entities", 557.0, 6.2),
        ("Update 1 M positions (one pass)", 168.0, 1.0),
    ]
    width, height = 720, 380
    left, plot_w, top, row_h = 250, 400, 100, 66
    lo, hi = 0.5, 1000.0

    def x_of(value):
        return left + plot_w * (math.log10(value) - math.log10(lo)) / (math.log10(hi) - math.log10(lo))

    parts = []
    for tick in (1, 10, 100, 1000):
        x = x_of(tick)
        parts.append(f'<line x1="{x:.1f}" y1="{top - 8}" x2="{x:.1f}" y2="{top + row_h * len(rows)}" stroke="{GRID}"/>')
        parts.append(text(x, top - 14, f"{tick} ms", 11, MUTED, "middle"))
    for i, (label, node_ms, table_ms) in enumerate(rows):
        y = top + row_h * i
        parts.append(text(left - 12, y + 30, label, 13, INK, "end"))
        parts.append(f'<rect x="{left}" y="{y + 6}" width="{x_of(node_ms) - left:.1f}" height="20" rx="3" fill="{AMBER}"/>')
        parts.append(text(x_of(node_ms) + 8, y + 21, f"{node_ms:g} ms", 12, INK))
        parts.append(f'<rect x="{left}" y="{y + 32}" width="{x_of(table_ms) - left:.1f}" height="20" rx="3" fill="{BLUE}"/>')
        parts.append(text(x_of(table_ms) + 8, y + 47, f"{table_ms:g} ms  ({node_ms / table_ms:.0f}x faster)", 12, INK))
    legend_y = top + row_h * len(rows) + 28
    parts.append(f'<rect x="{left}" y="{legend_y - 11}" width="14" height="14" rx="3" fill="{AMBER}"/>')
    parts.append(text(left + 22, legend_y, "Godot Node3D objects", 12, INK))
    parts.append(f'<rect x="{left + 190}" y="{legend_y - 11}" width="14" height="14" rx="3" fill="{BLUE}"/>')
    parts.append(text(left + 212, legend_y, "Tessera Net entity table", 12, INK))
    parts.append(text(24, height - 18, "Log scale. Nodes are created and updated through the extension API, so engine-internal code would be faster.", 11, MUTED))
    return card(width, height, "Entities as Godot nodes vs. the entity table",
                "1,000,000 entities, same process, same arithmetic, median of 5 runs, one thread. Shorter is better.",
                "\n".join(parts))


def relevancy_chart():
    """Relevancy update cost per tick (log scale): measured value, regression guard and goal."""
    # (label, measured ms, regression guard ms or None, goal ms or None)
    rows = [
        ("128 players, 1 M entities, 1 thread", 1.03, 2.0, 1.5),
        ("128 players, 1 M entities, 8 workers", 0.33, 0.7, 0.4),
        ("32 players, 250 k entities, 1 thread", 0.29, None, None),
        ("32 players, 250 k entities, 8 workers", 0.10, None, None),
    ]
    width, height = 720, 400
    left, plot_w, top, row_h = 270, 380, 100, 54
    lo, hi = 0.1, 100.0

    def x_of(value):
        return left + plot_w * (math.log10(value) - math.log10(lo)) / (math.log10(hi) - math.log10(lo))

    parts = []
    for tick in (0.1, 1, 10, 100):
        x = x_of(tick)
        parts.append(f'<line x1="{x:.1f}" y1="{top - 8}" x2="{x:.1f}" y2="{top + row_h * len(rows)}" stroke="{GRID}"/>')
        parts.append(text(x, top - 14, f"{tick:g} ms", 11, MUTED, "middle"))
    for i, (label, value, guard, goal) in enumerate(rows):
        y = top + row_h * i
        parts.append(text(left - 12, y + 28, label, 13, INK, "end"))
        parts.append(f'<rect x="{left}" y="{y + 12}" width="{x_of(value) - left:.1f}" height="22" rx="3" fill="{BLUE}"/>')
        # The value sits inside the bar so it never collides with the limit and goal marks.
        parts.append(text(left + 10, y + 28, f"{value:.3g} ms" if value >= 1 else f"{value:.2f} ms", 12, "#ffffff", weight="bold"))
        if guard is not None:
            parts.append(f'<line x1="{x_of(guard):.1f}" y1="{y + 6}" x2="{x_of(guard):.1f}" y2="{y + 40}" stroke="{RED}" stroke-width="2"/>')
        if goal is not None:
            parts.append(f'<line x1="{x_of(goal):.1f}" y1="{y + 6}" x2="{x_of(goal):.1f}" y2="{y + 40}" stroke="{GREEN}" stroke-width="2"/>')
    legend_y = top + row_h * len(rows) + 28
    parts.append(f'<rect x="{left}" y="{legend_y - 11}" width="14" height="14" rx="3" fill="{BLUE}"/>')
    parts.append(text(left + 22, legend_y, "measured", 12, INK))
    parts.append(f'<rect x="{left + 100}" y="{legend_y - 11}" width="4" height="14" fill="{RED}"/>')
    parts.append(text(left + 112, legend_y, "regression limit", 12, INK))
    parts.append(f'<rect x="{left + 230}" y="{legend_y - 11}" width="4" height="14" fill="{GREEN}"/>')
    parts.append(text(left + 242, legend_y, "goal", 12, INK))
    parts.append(text(24, height - 18, "Log scale. Both goals are met in the main scenario; the limits only guard against regressions.", 11, MUTED))
    return card(width, height, "Relevancy: time per tick",
                "Median of 4 runs, desktop CPU with 12 cores, Release build. Shorter is better.",
                "\n".join(parts))


def replication_chart():
    """Replicated state: time to write the buffers of one tick (log scale), measured value, guard and goal."""
    # (label, measured ms, regression guard ms or None, goal ms or None)
    # The guard and the goal are for 192,000 records (every visible entity changing): 3.2 and 10 million per second.
    rows = [
        ("200,000 entities changing, 1 thread", 29.6, 60.0, 19.2),
        ("200,000 entities changing, 8 workers", 8.3, None, None),
        ("5 Hz updates, 1 thread, with confirmations", 22.7, None, None),
    ]
    width, height = 720, 350
    left, plot_w, top, row_h = 270, 380, 100, 54
    lo, hi = 1.0, 100.0

    def x_of(value):
        return left + plot_w * (math.log10(value) - math.log10(lo)) / (math.log10(hi) - math.log10(lo))

    parts = []
    for tick in (1, 10, 100):
        x = x_of(tick)
        parts.append(f'<line x1="{x:.1f}" y1="{top - 8}" x2="{x:.1f}" y2="{top + row_h * len(rows)}" stroke="{GRID}"/>')
        parts.append(text(x, top - 14, f"{tick:g} ms", 11, MUTED, "middle"))
    for i, (label, value, guard, goal) in enumerate(rows):
        y = top + row_h * i
        parts.append(text(left - 12, y + 28, label, 13, INK, "end"))
        parts.append(f'<rect x="{left}" y="{y + 12}" width="{x_of(value) - left:.1f}" height="22" rx="3" fill="{BLUE}"/>')
        parts.append(text(left + 10, y + 28, f"{value:.3g} ms", 12, "#ffffff", weight="bold"))
        if guard is not None:
            parts.append(f'<line x1="{x_of(guard):.1f}" y1="{y + 6}" x2="{x_of(guard):.1f}" y2="{y + 40}" stroke="{RED}" stroke-width="2"/>')
        if goal is not None:
            parts.append(f'<line x1="{x_of(goal):.1f}" y1="{y + 6}" x2="{x_of(goal):.1f}" y2="{y + 40}" stroke="{GREEN}" stroke-width="2"/>')
    legend_y = top + row_h * len(rows) + 28
    parts.append(f'<rect x="{left}" y="{legend_y - 11}" width="14" height="14" rx="3" fill="{BLUE}"/>')
    parts.append(text(left + 22, legend_y, "measured", 12, INK))
    parts.append(f'<rect x="{left + 100}" y="{legend_y - 11}" width="4" height="14" fill="{RED}"/>')
    parts.append(text(left + 112, legend_y, "regression limit", 12, INK))
    parts.append(f'<rect x="{left + 230}" y="{legend_y - 11}" width="4" height="14" fill="{GREEN}"/>')
    parts.append(text(left + 242, legend_y, "goal", 12, INK))
    parts.append(text(24, height - 18, "Log scale. The first version is faster than the limit but has not reached its goal of 10 million updates per second.", 11, MUTED))
    return card(width, height, "Replicated state: time per tick",
                "128 players seeing 1,500 of 200,000 entities each. Median of 4 runs, desktop CPU, Release build. Shorter is better.",
                chr(10).join(parts))


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    (OUT / "baseline-godot-replication.svg").write_text(baseline_chart(), encoding="utf-8")
    (OUT / "data-core-acceptance.svg").write_text(acceptance_chart(), encoding="utf-8")
    (OUT / "entities-vs-godot-nodes.svg").write_text(nodes_chart(), encoding="utf-8")
    (OUT / "relevancy-update.svg").write_text(relevancy_chart(), encoding="utf-8")
    (OUT / "replication-update.svg").write_text(replication_chart(), encoding="utf-8")
    print("wrote", OUT)


if __name__ == "__main__":
    main()
