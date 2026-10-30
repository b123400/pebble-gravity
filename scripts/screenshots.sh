#!/bin/bash
#
# Generates the app store screenshots in store-screenshots/<platform>/.
#
#   scripts/screenshots.sh                 # every platform in package.json
#   scripts/screenshots.sh basalt chalk    # only these platforms
#
# Each scene is built with its time frozen (SCREENSHOT_TIME, see wscript),
# installed on the emulator, given its settings through an app message (the
# same message the phone sends from the settings page), then screenshotted.
# Emulator screenshots are color corrected to look like a real Pebble display.
set -euo pipefail

# One scene per line: <file name> <HH:MM> [setting=value ...]
# Settings left out use the watch face's defaults:
#   bg=CD2231 dot=FFFFFF repel=0 intensity=50 size=2 spacing=8
# bw_bg and bw_dot replace bg and dot on black and white watches
# (aplite, diorite, flint).
SCENES=(
  "1-default-10-10       10:10"
  "2-default-03-40       03:40"
  "3-anti-gravity-08-20  08:20 repel=1"
  "4-big-dots-01-50      01:50 bg=0055AA dot=FFFF00 bw_bg=FFFFFF bw_dot=000000 intensity=70 size=4 spacing=12"
  "5-small-dots-04-35    04:35 bg=000000 dot=55FFFF bw_bg=000000 bw_dot=FFFFFF repel=1 intensity=80 size=1 spacing=5"
)

# Seconds to wait for the watch face to redraw after new settings arrive
SETTLE=3

cd "$(dirname "$0")/.."
OUT=store-screenshots

if [ $# -gt 0 ]; then
  PLATFORMS=("$@")
else
  PLATFORMS=($(python3 -c "import json; print(' '.join(json.load(open('package.json'))['pebble']['targetPlatforms']))"))
fi

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# A normal build, reinstalled at the end so the emulators aren't left frozen
echo "Building..."
pebble build >/dev/null
cp build/*.pbw "$TMP/normal.pbw"

# Message keys are numbered by the SDK, so look them up by name
keys=$(python3 -c "import json; k = json.load(open('build/appinfo.json'))['appKeys']; print(' '.join('{}={}'.format(n, v) for n, v in k.items()))")
key() {
  for kv in $keys; do
    if [ "${kv%%=*}" = "$1" ]; then echo "${kv#*=}"; return; fi
  done
  echo "Message key $1 not found in build/appinfo.json" >&2
  exit 1
}
K_BG=$(key background_color); K_DOT=$(key dot_color); K_REPEL=$(key repel)
K_INTENSITY=$(key intensity); K_SIZE=$(key dot_size); K_SPACING=$(key density)

# One frozen build per scene time
for scene in "${SCENES[@]}"; do
  read -r name time _ <<< "$scene"
  pbw="$TMP/time-${time/:/}.pbw"
  if [ ! -f "$pbw" ]; then
    echo "Building frozen at $time..."
    SCREENSHOT_TIME=$time pebble build >/dev/null
    cp build/*.pbw "$pbw"
  fi
done
# Leave build/ as a normal build
pebble build >/dev/null

send_settings() { # platform bg dot repel intensity size spacing
  pebble send-app-message --emulator "$1" \
    --int "$K_BG=$((16#$2))" "$K_DOT=$((16#$3))" "$K_REPEL=$4" "$K_INTENSITY=$5" "$K_SPACING=$7" \
    --string "$K_SIZE=$6" >/dev/null 2>&1
}

for platform in "${PLATFORMS[@]}"; do
  bw=0
  case $platform in aplite|diorite|flint) bw=1;; esac
  mkdir -p "$OUT/$platform"

  for scene in "${SCENES[@]}"; do
    read -r name time settings <<< "$scene"
    bg=CD2231; dot=FFFFFF; repel=0; intensity=50; size=2; spacing=8; bw_bg=; bw_dot=
    for setting in $settings; do
      case ${setting%%=*} in
        bg|dot|repel|intensity|size|spacing|bw_bg|bw_dot) declare "${setting%%=*}=${setting#*=}";;
        *) echo "Unknown setting in scene $name: $setting" >&2; exit 1;;
      esac
    done
    if [ $bw = 1 ]; then
      bg=${bw_bg:-$bg}; dot=${bw_dot:-$dot}
    fi

    pebble install --emulator "$platform" "$TMP/time-${time/:/}.pbw" >/dev/null 2>&1
    sleep 2
    send_settings "$platform" "$bg" "$dot" "$repel" "$intensity" "$size" "$spacing"
    sleep $SETTLE
    pebble screenshot --emulator "$platform" --no-open "$OUT/$platform/$name.png" >/dev/null 2>&1
    echo "$OUT/$platform/$name.png"
  done

  # Put the normal build back, on default settings
  pebble install --emulator "$platform" "$TMP/normal.pbw" >/dev/null 2>&1
  send_settings "$platform" CD2231 FFFFFF 0 50 2 8
  pebble kill >/dev/null 2>&1 || true
done
