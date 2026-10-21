"""Graphiques SVG statiques (sans dépendance) à partir de results/*/metrics.csv et results/scale/summary.csv."""
import csv
import math
import os

ROOT = os.path.join(os.path.dirname(__file__), "..", "results")
# palette catégorielle de référence (ordre fixe), surface claire
SERIES = ["#2a78d6", "#eb6834", "#1baf7a", "#eda100", "#e87ba4", "#008300"]
SURFACE, INK, INK2, GRID = "#fcfcfb", "#0b0b0b", "#52514e", "#e4e3df"
EVENTS = [(40, "la boîte bouge"), (64, "boîte immobile"), (80, "lampe B allumée"), (100, "lampe A éteinte")]


def read_csv(path):
    with open(path, newline="") as f:
        return list(csv.DictReader(f))


def frames(method, col, seeds=5):
    """Moyenne sur les graines disponibles de la colonne `col`, par frame."""
    runs = []
    for s in range(seeds):
        name = "metrics.csv" if s == 0 else f"metrics_s{s}.csv"
        p = os.path.join(ROOT, method, name)
        if os.path.exists(p):
            runs.append([float(r[col]) for r in read_csv(p)])
    return [sum(v) / len(v) for v in zip(*runs)]


def esc(t):
    return t.replace("&", "&amp;").replace("<", "&lt;")


def line_chart(path, title, subtitle, series, ylabel, logy=False, events=True, xlabel="frame", xs=None):
    W, H, L, R, T, B = 760, 380, 70, 170, 58, 46
    pw, ph = W - L - R, H - T - B
    allx = xs or list(range(len(series[0][1])))
    ys = [y for _, v in series for y in v if (y > 0 or not logy)]
    lo, hi = (min(ys), max(ys))
    if logy:
        lo, hi = 10 ** math.floor(math.log10(lo)), 10 ** math.ceil(math.log10(hi))
        f = lambda y: T + ph * (1 - (math.log10(max(y, lo)) - math.log10(lo)) / (math.log10(hi) - math.log10(lo)))
        ticks = [10 ** e for e in range(int(math.log10(lo)), int(math.log10(hi)) + 1)]
        fmt = lambda t: f"1e{int(math.log10(t))}"
    else:
        lo = 0
        step = 10 ** math.floor(math.log10(hi / 4))
        step *= 2 if hi / step > 10 else 1
        hi = math.ceil(hi / step) * step
        f = lambda y: T + ph * (1 - y / hi)
        ticks = [i * step for i in range(int(hi / step) + 1)]
        fmt = lambda t: f"{t:,.0f}".replace(",", " ")
    x0, x1 = min(allx), max(allx)
    gx = lambda x: L + pw * (x - x0) / (x1 - x0)
    o = [f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {W} {H}" font-family="system-ui, sans-serif" font-size="12">',
         f'<rect width="{W}" height="{H}" fill="{SURFACE}"/>',
         f'<text x="{L}" y="22" font-size="15" font-weight="600" fill="{INK}">{esc(title)}</text>',
         f'<text x="{L}" y="40" fill="{INK2}">{esc(subtitle)}</text>']
    for t in ticks:
        y = f(t)
        o.append(f'<line x1="{L}" x2="{L + pw}" y1="{y:.1f}" y2="{y:.1f}" stroke="{GRID}"/>')
        o.append(f'<text x="{L - 8}" y="{y + 4:.1f}" text-anchor="end" fill="{INK2}">{fmt(t)}</text>')
    if events:
        for k, (fx, lab) in enumerate(EVENTS):
            x = gx(fx)
            o.append(f'<line x1="{x:.1f}" x2="{x:.1f}" y1="{T}" y2="{T + ph}" stroke="{INK2}" stroke-dasharray="3 3" opacity="0.6"/>')
            o.append(f'<text x="{x + 3:.1f}" y="{T + 11 + 12 * (k % 2)}" fill="{INK2}" font-size="10">{esc(lab)}</text>')
    xt = allx if xs else list(range(0, x1 + 1, 20))
    for t in xt:
        o.append(f'<text x="{gx(t):.1f}" y="{T + ph + 18}" text-anchor="middle" fill="{INK2}">{t}</text>')
    o.append(f'<text x="{L + pw / 2}" y="{H - 8}" text-anchor="middle" fill="{INK2}">{esc(xlabel)}</text>')
    o.append(f'<text transform="translate(16 {T + ph / 2}) rotate(-90)" text-anchor="middle" fill="{INK2}">{esc(ylabel)}</text>')
    ends = []
    for i, (name, v) in enumerate(series):
        c = SERIES[i]
        pts = " ".join(f"{gx(x):.1f},{f(y):.1f}" for x, y in zip(allx, v))
        o.append(f'<polyline points="{pts}" fill="none" stroke="{c}" stroke-width="2" stroke-linejoin="round"/>')
        if xs:
            for x, y in zip(allx, v):
                o.append(f'<circle cx="{gx(x):.1f}" cy="{f(y):.1f}" r="4" fill="{c}" stroke="{SURFACE}" stroke-width="2"/>')
        ends.append([f(v[-1]), name, c])
    # étiquettes directes en fin de courbe, décollées pour éviter les chevauchements
    ends.sort()
    for k in range(1, len(ends)):
        ends[k][0] = max(ends[k][0], ends[k - 1][0] + 14)
    for y, name, c in ends:
        o.append(f'<rect x="{L + pw + 8}" y="{y - 5:.1f}" width="10" height="10" rx="2" fill="{c}"/>')
        o.append(f'<text x="{L + pw + 22}" y="{y + 4:.1f}" fill="{INK}">{esc(name)}</text>')
    o.append("</svg>")
    with open(path, "w", encoding="utf8") as fh:
        fh.write("\n".join(o))


def bar_chart(path, title, subtitle, rows, xlabel):
    """Barres horizontales, une seule série (magnitude), échelle log."""
    W, rowh, L, R, T = 760, 26, 150, 90, 58
    H = T + rowh * len(rows) + 40
    pw = W - L - R
    lo = 10 ** math.floor(math.log10(min(v for _, v, _ in rows)))
    hi = 10 ** math.ceil(math.log10(max(v for _, v, _ in rows)))
    gx = lambda v: L + pw * (math.log10(v) - math.log10(lo)) / (math.log10(hi) - math.log10(lo))
    o = [f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {W} {H}" font-family="system-ui, sans-serif" font-size="12">',
         f'<rect width="{W}" height="{H}" fill="{SURFACE}"/>',
         f'<text x="20" y="22" font-size="15" font-weight="600" fill="{INK}">{esc(title)}</text>',
         f'<text x="20" y="40" fill="{INK2}">{esc(subtitle)}</text>']
    e = int(math.log10(lo))
    while 10 ** e <= hi:
        x = gx(10 ** e)
        o.append(f'<line x1="{x:.1f}" x2="{x:.1f}" y1="{T}" y2="{T + rowh * len(rows)}" stroke="{GRID}"/>')
        o.append(f'<text x="{x:.1f}" y="{T + rowh * len(rows) + 16}" text-anchor="middle" fill="{INK2}">1e{e}</text>')
        e += 1
    for i, (name, v, sd) in enumerate(rows):
        y = T + i * rowh + 4
        hl = name == "krc"
        o.append(f'<text x="{L - 8}" y="{y + 13}" text-anchor="end" fill="{INK}" font-weight="{600 if hl else 400}">{esc(name)}</text>')
        o.append(f'<rect x="{L}" y="{y}" width="{gx(v) - L:.1f}" height="{rowh - 8}" rx="4" fill="{SERIES[0] if hl else "#9fb9d9"}"/>')
        o.append(f'<text x="{gx(v) + 6:.1f}" y="{y + 13}" fill="{INK2}">{v:.2e} ± {sd:.0e}</text>')
    o.append(f'<text x="{L + pw / 2}" y="{H - 6}" text-anchor="middle" fill="{INK2}">{esc(xlabel)}</text>')
    o.append("</svg>")
    with open(path, "w", encoding="utf8") as fh:
        fh.write("\n".join(o))


def main():
    out = os.path.join(ROOT, "plots")
    os.makedirs(out, exist_ok=True)
    fixed = ["pt_reset", "cache_reset", "krc_hard", "krc"]
    line_chart(os.path.join(out, "erreur_par_frame.svg"), "Erreur par frame à budget égal (1 chemin/pixel/frame)",
               "relMSE moyenne sur 5 graines, échelle log ; plus bas = mieux",
               [(m, frames(m, "relmse")) for m in fixed], "relMSE", logy=True)
    targets = ["cache_reset_target", "krc_hard_target", "krc_target"]
    line_chart(os.path.join(out, "cout_par_frame.svg"), "Coût par frame en mode qualité cible (σ/μ ≤ 3 % par cellule)",
               "chemins tracés par frame (moyenne 5 graines), plafond 4 chemins/pixel",
               [(m, frames(m, "paths")) for m in targets], "chemins / frame")
    summ = [r for r in read_csv(os.path.join(ROOT, "summary.csv")) if r["phase"] == "tout" and not r["method"].endswith("_target")]
    rows = sorted(((r["method"], float(r["relmse_mean"]), float(r["relmse_std"])) for r in summ), key=lambda r: r[1])
    bar_chart(os.path.join(out, "ablation.svg"), "Ablation : erreur moyenne sur la séquence de 120 frames",
              "relMSE moyenne ± écart-type (5 graines), budget égal, échelle log ; plus court = mieux", rows, "relMSE")
    sp = os.path.join(ROOT, "scale", "summary.csv")
    if os.path.exists(sp):
        sc = read_csv(sp)
        rooms = sorted({int(r["rooms"]) for r in sc})
        ms = ["cache_reset_target", "krc_hard_target", "krc_all_target", "krc_target", "krc_energy_target"]

        def val(m, n, phases, col):
            v = [float(r[col]) for r in sc if r["method"] == m and int(r["rooms"]) == n and r["phase"] in phases]
            return sum(v) / len(v)

        ev = ("boite", "lampe_on", "lampe_off")
        line_chart(os.path.join(out, "echelle_cout.svg"), "Passage à l'échelle : coût pendant que la boîte bouge",
                   "chemins/frame en mode qualité cible (cache convergé) ; la boîte reste dans la pièce 0",
                   [(m, [val(m, n, ("boite",), "paths_per_frame") for n in rooms]) for m in ms], "chemins / frame",
                   events=False, xlabel="nombre de pièces (96 px de large par pièce)", xs=rooms)
        def event_cost(m, n, frames_sel):
            vals = []
            d = os.path.join(ROOT, "scale", f"n{n}", m)
            for f in os.listdir(d):
                if f.startswith("metrics"):
                    for r in read_csv(os.path.join(d, f)):
                        if int(r["frame"]) in frames_sel and int(r["delta_paths"]) > 0:
                            vals.append(int(r["delta_paths"]))
            return sum(vals) / max(len(vals), 1)

        dm = ["krc_all", "krc_noage", "krc", "krc_energy"]
        line_chart(os.path.join(out, "echelle_evt_lampe.svg"), "Coût d'un changement de lampe selon la taille de la scène",
                   "chemins corrélés tracés à l'événement (lampe B allumée, lampe A éteinte), moyenne sur graines",
                   [(m, [event_cost(m, n, {80, 100}) for n in rooms]) for m in dm], "chemins / événement",
                   events=False, xlabel="nombre de pièces (96 px de large par pièce)", xs=rooms)
        line_chart(os.path.join(out, "echelle_evt_boite.svg"), "Coût d'un déplacement d'objet selon la taille de la scène",
                   "chemins corrélés tracés par pas de la boîte (pièce 0), moyenne sur graines",
                   [(m, [event_cost(m, n, set(range(40, 64))) for n in rooms]) for m in dm], "chemins / événement",
                   events=False, xlabel="nombre de pièces (96 px de large par pièce)", xs=rooms)
        fixed_s = ["cache_reset", "krc_hard", "krc_all", "krc"]
        line_chart(os.path.join(out, "echelle_erreur.svg"), "Passage à l'échelle : erreur pendant les phases dynamiques",
                   "relMSE à budget égal (1 chemin/pixel/frame), échelle log",
                   [(m, [val(m, n, ev, "relmse_mean") for n in rooms]) for m in fixed_s], "relMSE",
                   logy=True, events=False, xlabel="nombre de pièces (96 px de large par pièce)", xs=rooms)


if __name__ == "__main__":
    main()
