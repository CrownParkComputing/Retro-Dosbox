#!/usr/bin/env python3
"""Upload a screenshot set to App Store Connect and wait until Apple has it.

    tool/upload-screenshots.py <version-localization-id> <dir> <display-type> [--replace]

    tool/upload-screenshots.py 5ab05fa7-... store/screenshots/ios-ipad-13-landscape \\
        APP_IPAD_PRO_3GEN_129 --replace

WHY THIS EXISTS, rather than one `asc screenshots upload --path <dir>`.

Uploading an asset is two things: the bytes go up, and then Apple processes
them and the asset moves UPLOAD_COMPLETE -> COMPLETE. `asc` polls for that
second part and gives up after its own timeout. When it gives up it reports
the file as FAILED -- but the bytes have already arrived, so the asset is
left half-registered: present in the set, `assetDeliveryState` stuck at
UPLOAD_COMPLETE, width and height 0. App Store Connect draws exactly that as
a broken image, which is what "error occurred loading images" means.

Uploading a directory makes it worse: the first timeout aborts the batch, so
of five files one lands cleanly, one lands broken, and three never go at all.

So: one file at a time, treat a delivery-poll timeout as "probably fine, we
will check", and then poll the SET until every asset really is COMPLETE. The
truth is the set's state, never the uploader's exit code.
"""
from __future__ import annotations

import json
import pathlib
import subprocess
import sys
import time

POLL_TIMEOUT = 900          # Apple is usually seconds, occasionally minutes.
POLL_INTERVAL = 15


def asc(*args: str) -> tuple[int, str]:
    p = subprocess.run(["asc", *args], capture_output=True, text=True)
    return p.returncode, (p.stdout or "") + (p.stderr or "")


# Apple normalises some display types on the way in. APP_IPHONE_67 and
# APP_IPHONE_69 accept exactly the same six dimensions, so a set created as
# _69 comes back as _67 -- and polling for the name you sent then finds no
# set at all, reports "0/N complete" for ever and fails a run whose uploads
# all succeeded. Treat the pair as one.
ALIASES = {
    "APP_IPHONE_69": {"APP_IPHONE_69", "APP_IPHONE_67"},
    "APP_IPHONE_67": {"APP_IPHONE_67", "APP_IPHONE_69"},
}


def set_state(loc: str, display_type: str) -> list[tuple[str, str]] | None:
    """[(fileName, deliveryState)] for the set, or None if there is no set."""
    rc, out = asc("screenshots", "list", "--version-localization", loc)
    if rc != 0:
        return None
    try:
        data = json.loads(out)
    except json.JSONDecodeError:
        return None
    wanted = ALIASES.get(display_type, {display_type})
    for s in data.get("sets", []):
        if s["set"]["attributes"]["screenshotDisplayType"] not in wanted:
            continue
        rows = []
        for sh in s.get("screenshots", []):
            a = sh.get("attributes", {})
            state = (a.get("assetDeliveryState") or {}).get("state")
            rows.append((a.get("fileName", "?"), state))
        return rows
    return None


def main(argv: list[str]) -> int:
    if len(argv) < 4:
        print(__doc__.strip().splitlines()[2].strip(), file=sys.stderr)
        return 2
    loc, directory, display_type = argv[1], argv[2], argv[3]
    replace = "--replace" in argv[4:]

    shots = sorted(pathlib.Path(directory).glob("*.png"))
    if not shots:
        print(f"no PNGs in {directory}", file=sys.stderr)
        return 1
    if len(shots) > 10:
        print(f"{len(shots)} screenshots; Apple's limit per set is 10", file=sys.stderr)
        return 1

    for n, shot in enumerate(shots):
        # --replace clears the set, so it goes on the FIRST upload only --
        # otherwise each file would wipe the ones before it.
        extra = ["--replace", "--confirm"] if (replace and n == 0) else []
        rc, out = asc("screenshots", "upload",
                      "--version-localization", loc,
                      "--path", str(shot),
                      "--device-type", display_type, *extra)
        if rc == 0:
            print(f"  uploaded  {shot.name}")
        elif "timed out waiting for asset" in out or "context deadline exceeded" in out:
            # The bytes went; only the wait-for-processing gave up. Verified
            # below against the set, which is the thing that actually knows.
            print(f"  sent      {shot.name}  (delivery poll timed out, will verify)")
        else:
            print(f"  FAILED    {shot.name}", file=sys.stderr)
            print(out.strip()[-600:], file=sys.stderr)
            return 1

    print(f"waiting for Apple to process {len(shots)} assets...")
    deadline = time.time() + POLL_TIMEOUT
    while True:
        rows = set_state(loc, display_type)
        if rows is None:
            print(f"  (no {display_type} set visible yet)")
            if time.time() > deadline:
                print(f"no {display_type} set ever appeared", file=sys.stderr)
                return 1
            time.sleep(POLL_INTERVAL)
            continue
        done = [f for f, st in rows if st == "COMPLETE"]
        stuck = [(f, st) for f, st in rows if st != "COMPLETE"]
        if len(done) == len(shots) and not stuck:
            print(f"all {len(done)} COMPLETE in {display_type}")
            return 0
        if time.time() > deadline:
            print(f"timed out: {len(done)}/{len(shots)} COMPLETE", file=sys.stderr)
            for f, st in stuck:
                print(f"  {f}: {st}", file=sys.stderr)
            return 1
        print(f"  {len(done)}/{len(shots)} complete"
              + (f"; waiting on {', '.join(f for f, _ in stuck)}" if stuck else ""))
        time.sleep(POLL_INTERVAL)


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
