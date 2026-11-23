#!/usr/bin/env python3
"""T7: whole-game differential check. Runs the game twice with the same scripted input (a Cross press every 150 vblanks) and
compares frames captured at fixed vblanks: the reference (the x86-64 build under Rosetta 2) against the recompiled arm64 build.
Needs your own dump, both dcp_run binaries, python3 with Pillow; not part of ctest (see docs/MAC.md, "Checking the recompiler").

  python3 tests/t7_compare.py --dump /path/to/CUSA10454 --arm path/to/arm64/dcp_run --ref path/to/x86_64/dcp_run [--work DIR]

Frames up to --strict-until must match (every channel within --tolerance). Later frames are only reported: from the character
creation screen on, the game draws random things (a random default name) and pulsing glows run on real time, so even two runs of
the reference differ there (--noise measures that with a second reference run). --selftest checks the comparison itself."""
import argparse, os, subprocess, sys, tempfile

SHOTS = (600, 1200, 1800, 2400, 3000, 3600, 4300)


def diff_stats(path_a, path_b):
    """(share of pixels that differ, share that differ by more than 16, largest channel difference) or None when sizes differ."""
    from PIL import Image, ImageChops
    a, b = Image.open(path_a).convert("RGB"), Image.open(path_b).convert("RGB")
    if a.size != b.size:
        return None
    r, g, bl = ImageChops.difference(a, b).split()
    h = ImageChops.lighter(ImageChops.lighter(r, g), bl).histogram()  # per pixel, its largest channel difference
    total = a.size[0] * a.size[1]
    return sum(h[1:]) / total, sum(h[17:]) / total, max((i for i, v in enumerate(h) if v), default=0)


def press_script(last_vblank):
    lines, f = [], 200
    while f < last_vblank:
        lines += [f"{f} CROSS", f"{f + 6} 0"]
        f += 150
    return "\n".join(lines) + "\n"


def run_game(binary, dump, work, name, shots, env_extra):
    out, user = os.path.join(work, name), os.path.join(work, "user_" + name)
    for d in (out, user):
        os.makedirs(d, exist_ok=True)
    script = os.path.join(work, "press.txt")
    with open(script, "w") as f:
        f.write(press_script(max(shots)))
    arg = ",".join(f"{v}={out}/v{v}.png" for v in shots)
    env = dict(os.environ, DCP_USER_DIR=user, **env_extra)  # a fresh user dir: saves change what the game shows
    r = subprocess.run([binary, dump, "--input", script, "--shots", arg], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True, timeout=1800)
    if r.returncode != 0:
        sys.exit(f"t7: {name} run failed ({r.returncode}): {r.stderr.strip().splitlines()[-1:]}")
    return out


def selftest():
    from PIL import Image
    with tempfile.TemporaryDirectory() as d:
        a, b, c, e = (os.path.join(d, n) for n in ("a.png", "b.png", "c.png", "e.png"))
        Image.new("RGB", (10, 10), (10, 20, 30)).save(a)
        Image.new("RGB", (10, 10), (10, 20, 30)).save(b)
        im = Image.new("RGB", (10, 10), (10, 20, 30)); im.putpixel((0, 0), (110, 20, 30)); im.save(c)
        Image.new("RGB", (11, 10)).save(e)
        assert diff_stats(a, b) == (0.0, 0.0, 0)
        assert diff_stats(a, c) == (0.01, 0.01, 100)
        assert diff_stats(a, e) is None
        assert press_script(400) == "200 CROSS\n206 0\n350 CROSS\n356 0\n"
    print("t7_compare: self-test passed")


def main():
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    p.add_argument("--dump"), p.add_argument("--arm"), p.add_argument("--ref"), p.add_argument("--work")
    p.add_argument("--strict-until", type=int, default=1200)
    p.add_argument("--tolerance", type=int, default=2)
    p.add_argument("--noise", action="store_true", help="also run the reference a second time to show how much it varies by itself")
    p.add_argument("--selftest", action="store_true")
    o = p.parse_args()
    if o.selftest:
        return selftest()
    if not (o.dump and o.arm and o.ref):
        p.error("--dump, --arm and --ref are required")
    dump = o.dump if o.dump.endswith(".bin") else os.path.join(o.dump, "eboot.bin")
    work = o.work or tempfile.mkdtemp(prefix="t7_")
    if subprocess.run(["git", "-C", work, "rev-parse"], capture_output=True).returncode == 0:
        sys.exit("t7: --work is inside a git repository; frames come from your game, keep them out of it")
    rosetta = {"ROSETTA_ADVERTISE_AVX": "1"}  # Rosetta hides AVX unless asked; the game needs it
    ref = run_game(o.ref, dump, work, "ref", SHOTS, rosetta)
    arm = run_game(o.arm, dump, work, "arm", SHOTS, {})
    noise = run_game(o.ref, dump, work, "ref2", SHOTS, rosetta) if o.noise else None
    failed = False
    print(f"frames in {work}")
    for v in SHOTS:
        s = diff_stats(f"{ref}/v{v}.png", f"{arm}/v{v}.png")
        strict = v <= o.strict_until
        ok = s is not None and s[2] <= o.tolerance
        failed |= strict and not ok
        line = "size differs" if s is None else f"{100 * s[0]:6.3f}% of pixels differ, {100 * s[1]:6.3f}% by >16, max {s[2]:3d}"
        if noise:
            n = diff_stats(f"{ref}/v{v}.png", f"{noise}/v{v}.png")
            line += "  | reference vs itself: " + ("size differs" if n is None else f"{100 * n[0]:6.3f}%, max {n[2]:3d}")
        print(f"vblank {v:5d} {'STRICT' if strict else 'info  '} {'ok  ' if ok else 'DIFF'} {line}")
    print("t7: FAILED" if failed else "t7: strict frames match")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
