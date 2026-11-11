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
        ("128 players, 1 M entities, 1 thread", 0.71, 1.4, 1.5),
        ("128 players, 1 M entities, 8 workers", 0.24, 0.5, 0.4),
        ("32 players, 250 k entities, 1 thread", 0.22, None, None),
        ("32 players, 250 k entities, 8 workers", 0.06, None, None),
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
    # The guard and the goal are for 192,000 records (every visible entity changing): 2.9 and 10 million per second.
    rows = [
        ("200,000 entities changing, 1 thread", 33.2, 66.2, 19.2),
        ("200,000 entities changing, 8 workers", 9.6, None, None),
        ("5 Hz updates, 1 thread, with confirmations", 24.6, None, None),
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
                "128 players seeing 1,500 of 200,000 entities each. Median of 6 runs, desktop CPU, Release build. Shorter is better.",
                chr(10).join(parts))


def network_chart():
    """The whole network phase per tick (log scale): measured value, regression guard and goal."""
    # (label, measured ms, regression guard ms, goal ms)
    rows = [
        ("128 players, 1 thread", 31.6, 50.0, 3.3),
        ("128 players, 8 workers", 8.7, 16.0, 1.0),
    ]
    width, height = 720, 310
    left, plot_w, top, row_h = 230, 420, 100, 54
    lo, hi = 0.5, 200.0

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
        parts.append(f'<line x1="{x_of(guard):.1f}" y1="{y + 6}" x2="{x_of(guard):.1f}" y2="{y + 40}" stroke="{RED}" stroke-width="2"/>')
        parts.append(f'<line x1="{x_of(goal):.1f}" y1="{y + 6}" x2="{x_of(goal):.1f}" y2="{y + 40}" stroke="{GREEN}" stroke-width="2"/>')
    legend_y = top + row_h * len(rows) + 28
    parts.append(f'<rect x="{left}" y="{legend_y - 11}" width="14" height="14" rx="3" fill="{BLUE}"/>')
    parts.append(text(left + 22, legend_y, "measured", 12, INK))
    parts.append(f'<rect x="{left + 100}" y="{legend_y - 11}" width="4" height="14" fill="{RED}"/>')
    parts.append(text(left + 112, legend_y, "regression limit", 12, INK))
    parts.append(f'<rect x="{left + 230}" y="{legend_y - 11}" width="4" height="14" fill="{GREEN}"/>')
    parts.append(text(left + 242, legend_y, "goal", 12, INK))
    parts.append(text(24, height - 18, "Log scale. Neither goal is met yet. A player receives 98 kbps on average, within the goal of 100.", 11, MUTED))
    return card(width, height, "Network phase: time per tick",
                "128 players, 200,000 entities, 5% packet loss. Median of 4 runs, desktop CPU, Release build. Shorter is better.",
                chr(10).join(parts))

def network_curve_chart():
    """The network phase against the entities that move in a tick: measured medians joined by segments."""
    # (entities moving per tick, one thread ms, eight workers ms): medians of 4 runs of `network-curve`
    data = [
        (1, 1.19, 0.64),
        (1000, 3.51, 1.23),
        (2985, 6.90, 2.26),
        (10000, 16.98, 4.67),
        (33333, 38.08, 10.81),
    ]
    stress_goal = (33333, 3.3, 1.0)  # the long-term goals, at the stress point
    reference = (1000, 3.3, 1.0, 3.51, 1.23)  # the goals at the reference load and the medians there
    width, height = 720, 432
    left, right, top, bottom = 70, 676, 90, 320
    x_max, y_max = 35000.0, 40.0

    def x_of(value):
        return left + (right - left) * value / x_max

    def y_of(value):
        return bottom - (bottom - top) * value / y_max

    parts = []
    for ms in (0, 10, 20, 30, 40):
        y = y_of(ms)
        parts.append(f'<line x1="{left}" y1="{y:.1f}" x2="{right}" y2="{y:.1f}" stroke="{GRID}"/>')
        parts.append(text(left - 10, y + 4, f"{ms}", 11, MUTED, "end"))
    for moving in (0, 10000, 20000, 30000):
        x = x_of(moving)
        parts.append(f'<line x1="{x:.1f}" y1="{bottom}" x2="{x:.1f}" y2="{bottom + 5}" stroke="{MUTED}"/>')
        parts.append(text(x, bottom + 20, f"{moving:,}", 11, MUTED, "middle"))
    parts.append(text((left + right) / 2, bottom + 40, "Entities that move in a tick (out of 200,000)", 12, MUTED, "middle"))
    parts.append(text(left - 10, top - 14, "ms per tick", 11, MUTED, "end"))
    for index, color in ((1, BLUE), (2, AMBER)):
        points = " ".join(f"{x_of(row[0]):.1f},{y_of(row[index]):.1f}" for row in data)
        parts.append(f'<polyline points="{points}" fill="none" stroke="{color}" stroke-width="2"/>')
        for row in data:
            parts.append(f'<circle cx="{x_of(row[0]):.1f}" cy="{y_of(row[index]):.1f}" r="4" fill="{color}"/>')
        for row in data[-2:]:
            parts.append(text(x_of(row[0]) - 8, y_of(row[index]) - 10, f"{row[index]:.2f}", 11, color, "end", "bold"))
    gx = x_of(stress_goal[0])
    for goal, label, lift in ((stress_goal[1], "goal 3.3 ms", 7), (stress_goal[2], "goal 1 ms", 7)):
        y = y_of(goal)
        parts.append(f'<line x1="{gx - 8:.1f}" y1="{y:.1f}" x2="{gx + 8:.1f}" y2="{y:.1f}" stroke="{GREEN}" stroke-width="3"/>')
        parts.append(text(gx - 14, y - lift, label, 11, GREEN, "end"))
    # The reference load: the original goals at 1,000 moving entities, next to the measured medians.
    rx = x_of(reference[0])
    for goal in reference[1:3]:
        y = y_of(goal)
        parts.append(f'<line x1="{rx - 8:.1f}" y1="{y:.1f}" x2="{rx + 8:.1f}" y2="{y:.1f}" stroke="{GREEN}" stroke-width="3"/>')
    parts.append(f'<line x1="{rx:.1f}" y1="{top + 40}" x2="{rx:.1f}" y2="{y_of(reference[1]) - 10:.1f}" stroke="{MUTED}" stroke-dasharray="3 3"/>')
    parts.append(text(rx + 8, top + 52, "At 1,000 moving (reference load):", 11, MUTED))
    parts.append(text(rx + 8, top + 68, f"{reference[3]:.2f} ms against a goal of 3.3 ms, one thread", 11, BLUE, "start", "bold"))
    parts.append(text(rx + 8, top + 84, f"{reference[4]:.2f} ms against a goal of 1.0 ms, eight workers", 11, AMBER, "start", "bold"))
    legend_y = 394
    parts.append(f'<rect x="{left}" y="{legend_y - 11}" width="14" height="14" rx="3" fill="{BLUE}"/>')
    parts.append(text(left + 22, legend_y, "one thread", 12, INK))
    parts.append(f'<rect x="{left + 120}" y="{legend_y - 11}" width="14" height="14" rx="3" fill="{AMBER}"/>')
    parts.append(text(left + 142, legend_y, "eight workers", 12, INK))
    parts.append(f'<rect x="{left + 260}" y="{legend_y - 11}" width="14" height="4" fill="{GREEN}"/>')
    parts.append(text(left + 282, legend_y, "goals at 1,000 moving and at the stress point", 12, INK))
    parts.append(text(24, height - 14, "Measured medians joined by straight lines. With one entity moving: 1.19 ms and 0.64 ms.", 11, MUTED))
    return card(width, height, "Network phase: time per tick by entities moving",
                "128 players, 200,000 entities, 5% packet loss. Median of 4 runs, desktop CPU, Release build. Shorter is better.",
                chr(10).join(parts))


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    (OUT / "baseline-godot-replication.svg").write_text(baseline_chart(), encoding="utf-8")
    (OUT / "data-core-acceptance.svg").write_text(acceptance_chart(), encoding="utf-8")
    (OUT / "entities-vs-godot-nodes.svg").write_text(nodes_chart(), encoding="utf-8")
    (OUT / "relevancy-update.svg").write_text(relevancy_chart(), encoding="utf-8")
    (OUT / "replication-update.svg").write_text(replication_chart(), encoding="utf-8")
    (OUT / "network-phase.svg").write_text(network_chart(), encoding="utf-8")
    (OUT / "network-curve.svg").write_text(network_curve_chart(), encoding="utf-8")
    print("wrote", OUT)


if __name__ == "__main__":
    main()
