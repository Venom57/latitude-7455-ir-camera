#!/bin/bash
# Find the IR emitter: for each PM8550 flash channel (ir:flash-1..4), keep
# re-firing 50 ms flashes every 100 ms (as the Zenbook A14 driver does) while
# an IR frame is captured, then print the frame's brightness. Also pauses so
# the bezel can be checked through a phone's selfie camera.
# Needs the overlay from: bash install.sh --emitter-test (then a reboot).
set -euo pipefail

here="$(cd "$(dirname "$(readlink -f "$0")")" && pwd)"
current_ua="${CURRENT_UA:-300000}"   # under the overlay's 400 mA cap
leds=(/sys/class/leds/ir:flash-*)
[ -e "${leds[0]}" ] || { echo "No ir:flash-* LEDs; run: bash $here/install.sh --emitter-test, then reboot."; exit 1; }
sudo -v

pulser=
stop_pulses() {
    [ -n "$pulser" ] && sudo kill "$pulser" 2>/dev/null || true
    pulser=
    for l in "${leds[@]}"; do echo 0 | sudo tee "$l/flash_strobe" >/dev/null 2>&1 || true; done
}
trap stop_pulses EXIT

# Fire $1 every 100 ms: strobe off, re-arm current and timeout, strobe on.
start_pulses() {
    sudo bash -c "while :; do
        echo 0 > '$1/flash_strobe'
        echo $current_ua > '$1/flash_brightness'
        echo 50000 > '$1/flash_timeout'
        echo 1 > '$1/flash_strobe'
        sleep 0.1
    done" &
    pulser=$!
}

sd=
for m in /dev/media*; do
    e="$(media-ctl -d "$m" -e "hm1092 8-0024" 2>/dev/null)" && { sd="$e"; break; }
done
[ -n "$sd" ] && v4l2-ctl -d "$sd" -c exposure=746

stats() { bash "$here/capture.sh" 2>&1 | sed -n 's/.*min \([0-9]*\) max \([0-9]*\) mean \([0-9.]*\).*/max \2, mean \3/p'; }

echo "Flash current ${current_ua} uA, 50 ms pulses every 100 ms."
echo "All off:        $(stats)"
for l in "${leds[@]}"; do
    start_pulses "$l"
    sleep 0.3
    echo "$(basename "$l") pulsing: $(stats)   fault: $(cat "$l/flash_fault" 2>/dev/null)"
    read -r -p "  Phone camera on the bezel: any glow? Enter for the next one. " _
    stop_pulses
done
