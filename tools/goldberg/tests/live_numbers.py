#!/usr/bin/env python3
# Save to frame, live (docs/PRDs/Krome-Slice-6-PRD.md, KL22): a `goldberg watch` on kromescreens'
# roots, kromescreens connected to it by itself (asset_live, through <tree>/.goldberg-port), and a
# screen, a prelude and an icon each saved N times. Each save is timed from the write to the
# line the program logs when it has the change: the screen built again, or the shape read again.
# Then arcfire under a watch of its roots, a pixel of one of its textures changed each save.
# Every file is put back as it was. Run from the repository's root after a dev build:
#
#   python3 tools/goldberg/tests/live_numbers.py [--saves N] [--target linux-dev]
import argparse, os, statistics, subprocess, sys, time

ap = argparse.ArgumentParser()
ap.add_argument("--saves", type=int, default=10)
ap.add_argument("--target", default="linux-dev")
args = ap.parse_args()
root = os.getcwd()
bin_dir = os.path.join(root, "build", args.target, "bin")
roots = os.path.join(root, "build", args.target, "goldberg", "kromescreens.roots")
tree = os.path.join(root, "build", "assets", "kromescreens")
logs = os.path.join(root, "build", args.target, "bench", "live")
os.makedirs(logs, exist_ok=True)

cases = [
    # name, source, (old, new) to swap in turn, what the program says once it has it
    ("screen .kim", "examples/kromescreens/assets/screens.kim", ('heading "goldberg"', 'heading "goldberg {n}"'), "screens.kimb#screens' rebuilt"),
    ("prelude .kim", "modules/krome/assets/std.kim", ("page=#10131a", "page=#10131{n}"), "screens.kimb#screens' rebuilt"),
    ("icon .svg", "modules/krome/assets/icons/arrow_drop_down.svg", ('M480-384 288-576', 'M480-39{n} 288-576'), "arrow_drop_down.kshape' reloaded"),
]

originals = {c[1]: open(os.path.join(root, c[1])).read() for c in cases}
gl = open(os.path.join(logs, "goldberg.log"), "w")
kl_path = os.path.join(logs, "program.log")
kl = open(kl_path, "w")
g = subprocess.Popen([os.path.join(bin_dir, "goldberg"), "watch", "--roots", roots], stdout=gl, stderr=subprocess.STDOUT, stdin=subprocess.PIPE)
k = None
try:
    port = os.path.join(tree, ".goldberg-port")
    for _ in range(400):
        if os.path.exists(port): break
        time.sleep(0.025)
    else:
        sys.exit("no port file: the watch did not listen")
    k = subprocess.Popen([os.path.join(bin_dir, "kromescreens"), "--run-seconds", "600"], stdout=kl, stderr=subprocess.STDOUT)
    for _ in range(400):
        if "goldberg on port" in open(kl_path).read(): break
        time.sleep(0.025)
    else:
        sys.exit("the program did not connect")
    time.sleep(1.0)

    def count(needle): return open(kl_path).read().count(needle)

    for name, src, (old, new), needle in cases:
        path = os.path.join(root, src)
        text = originals[src]
        if old not in text: sys.exit(f"{src}: '{old}' is not there to change")
        times = []
        for n in range(args.saves):
            before = count(needle)
            body = text.replace(old, new.format(n=n % 10), 1) if n % 2 == 0 else text
            t0 = time.perf_counter()
            with open(path, "w") as f: f.write(body)
            while count(needle) <= before:
                if time.perf_counter() - t0 > 10: sys.exit(f"{name}: save {n} not seen in 10 s")
                time.sleep(0.001)
            times.append((time.perf_counter() - t0) * 1000)
            time.sleep(0.3)
        with open(path, "w") as f: f.write(text)
        print(f"{name:14} {args.saves} saves: median {statistics.median(times):6.1f} ms, min {min(times):6.1f}, max {max(times):6.1f}")
        time.sleep(0.5)
    # A texture in a game: arcfire under a watch of its own roots, a pixel of a PNG changed.
    k.terminate(); k.wait(timeout=20); k = None
    g.stdin.write(b"quit\n"); g.stdin.flush(); g.wait(timeout=30)
    png = os.path.join(root, "examples/arcfire/assets/space/stars.png")
    png_bytes = open(png, "rb").read()
    try:
        from PIL import Image
    except ImportError:
        Image = None
    if Image:
        g = subprocess.Popen([os.path.join(bin_dir, "goldberg"), "watch", "--roots", os.path.join(root, "build", args.target, "goldberg", "arcfire.roots")],
                             stdout=gl, stderr=subprocess.STDOUT, stdin=subprocess.PIPE)
        al_path = os.path.join(logs, "arcfire.log")
        al = open(al_path, "w")
        port = os.path.join(root, "build", "assets", "arcfire", ".goldberg-port")
        for _ in range(400):
            if os.path.exists(port): break
            time.sleep(0.025)
        k = subprocess.Popen([os.path.join(bin_dir, "arcfire"), "--run-seconds", "600"], stdout=al, stderr=subprocess.STDOUT)
        for _ in range(800):
            if "goldberg on port" in open(al_path).read(): break
            time.sleep(0.025)
        time.sleep(1.0)
        times = []
        try:
            base = Image.open(png).convert("RGBA")
            for n in range(args.saves):
                needle = "'arcfire/space/stars.png' reloaded"
                before = open(al_path).read().count(needle)
                im = base.copy()
                im.putpixel((0, 0), (n * 20 % 256, 0, 0, 255))
                t0 = time.perf_counter()
                im.save(png)
                while open(al_path).read().count(needle) <= before:
                    if time.perf_counter() - t0 > 10: sys.exit(f"texture: save {n} not seen in 10 s")
                    time.sleep(0.001)
                times.append((time.perf_counter() - t0) * 1000)
                time.sleep(0.3)
        finally:
            with open(png, "wb") as f: f.write(png_bytes)
        print(f"{'texture .png':14} {args.saves} saves: median {statistics.median(times):6.1f} ms, min {min(times):6.1f}, max {max(times):6.1f}  ({base.width}x{base.height}, arcfire)")
finally:
    for src, text in originals.items():
        with open(os.path.join(root, src), "w") as f: f.write(text)
    if k: k.terminate(); k.wait(timeout=20)
    if g.poll() is None: g.stdin.write(b"quit\n"); g.stdin.flush(); g.wait(timeout=30)
