#!/usr/bin/env bash
# Photograph the app on a simulator, one frame per screen, for the store.
#
#   tool/capture-screenshots.sh <path-to-DOSDeck.app>
#
# A script rather than a `run:` block in the workflow, because this logic has
# a python helper and a heredoc in it, and inside a YAML block scalar those
# have to start at column 0 -- which ends the block and breaks the file. It is
# also testable here and unrunnable there.
#
# HOW A SCREEN IS REACHED. simctl has no tap API, so there is no way to drive
# the UI. RETRODOS_SHOT names the screen to open on and the frontend honours
# it at startup; SIMCTL_CHILD_ is how an environment variable reaches the app.
# It is inert in the shipped app because nothing can set one for an App Store
# app on a device.
#
# TWO WAYS A CAPTURE LIES, both seen for real:
#
#   1. simctl reports a successful launch for a process that crashes a moment
#      later, and the screenshot is then of the home screen.
#   2. The app is still starting, and the screenshot is iOS's launch screen --
#      plain white, and completely convincing as a file. One of these reached
#      a finished store-ready set before anybody looked at it.
#
# Both are checked below rather than trusted: the process must still be alive,
# and the frame must be dark, because this interface is near-black
# (SDL_SetRenderDrawColor(ren, 12, 12, 16)) and the launch screen is white.
set -euo pipefail

APP_PATH="${1:?usage: capture-screenshots.sh <path-to-app-bundle>}"
BUNDLE_ID="${BUNDLE_ID:-com.crownparkcomputing.dosdeck}"
HERE="$(cd "$(dirname "$0")/.." && pwd)"

# The screens to photograph, in the order they are numbered. The first is the
# one App Store search results show, so it leads with what the app is.
SHOTS=(library demo run about wizard)

[ -d "$APP_PATH" ] || { echo "no app bundle at $APP_PATH" >&2; exit 1; }

# Device types are named differently in every Xcode, so match rather than
# hardcode: a missing "iPhone 17 Pro Max" would otherwise fail the job on an
# Xcode bump with a message about an unknown device. Highest match wins, so
# this follows Xcode forward.
devtype() {
    xcrun simctl list devicetypes -j | python3 -c "import json,re,sys; m=[t for t in json.load(sys.stdin)['devicetypes'] if re.search(sys.argv[1],t['name'])]; print(sorted(m,key=lambda t:t['name'])[-1]['identifier']) if m else sys.exit('no simulator device type matching '+sys.argv[1])" "$1"
}

# Shoot until the frame is the app rather than the launch screen.
shoot() {   # <udid> <outfile>
    local udid="$1" out="$2" attempt
    for attempt in 1 2 3 4 5 6; do
        xcrun simctl io "$udid" screenshot "$out" >/dev/null 2>&1 || true
        if [ -s "$out" ] && python3 "$HERE/tool/capture-is-drawn.py" "$out"; then
            return 0
        fi
        echo "      (not drawn yet, attempt $attempt)"
        sleep 5
    done
    echo "::error::$out never stopped showing the launch screen" >&2
    return 1
}

capture() {   # <device-type regex> <output dir>
    local udid outdir="$2" i=0 shot pid
    udid=$(xcrun simctl create "dosdeck-shots" "$(devtype "$1")")
    # shellcheck disable=SC2064
    trap "xcrun simctl delete '$udid' >/dev/null 2>&1 || true" RETURN
    xcrun simctl boot "$udid"
    xcrun simctl bootstatus "$udid" -b
    # A clean status bar, and no "<- Back to ..." breadcrumb from a previous app.
    xcrun simctl status_bar "$udid" override \
        --time "09:41" --batteryState charged --batteryLevel 100 \
        --wifiBars 3 --cellularBars 4
    xcrun simctl install "$udid" "$APP_PATH"
    mkdir -p "$outdir"

    for shot in "${SHOTS[@]}"; do
        i=$((i + 1))
        xcrun simctl terminate "$udid" "$BUNDLE_ID" >/dev/null 2>&1 || true
        pid=$(SIMCTL_CHILD_RETRODOS_SHOT="$shot" \
              xcrun simctl launch "$udid" "$BUNDLE_ID" | awk -F': ' '{print $2}')
        # "run" boots DOS and starts the demo; the rest are just a frame.
        if [ "$shot" = run ]; then sleep 25; else sleep 10; fi
        # Plain ps on the RUNNER, not `simctl spawn ... ps`: a simulator app is
        # an ordinary host process, and spawn runs a host binary inside the
        # simulator runtime, where it fails -- so that version of this check
        # called every launch dead and failed the job on a working app.
        if [ -z "$pid" ] || ! ps -p "$pid" >/dev/null 2>&1; then
            echo "::error::the app died before the '$shot' capture (pid ${pid:-none})" >&2
            exit 1
        fi
        shoot "$udid" "$outdir/$i-$shot.png"
        echo "    $outdir/$i-$shot.png"
    done
    xcrun simctl shutdown "$udid" >/dev/null 2>&1 || true
}

echo "==> iPhone"
capture 'iPhone .*Pro Max' "$HERE/store/screenshots-raw-iphone"
echo "==> iPad"
capture 'iPad Pro 13-inch' "$HERE/store/screenshots-raw-ipad"
echo "==> done"
