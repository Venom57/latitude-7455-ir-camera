#!/bin/bash
# Test the Windows emitter switch: PM8550 GPIO9 (SPMI SID 1, pm8550_gpios pin
# 9). qccamflash8380.sys fires the emitter with a qcpmic GPIO write (IOCTL
# 0x80028fa8: PMIC 1, GPIO index 8 = gpio9, output high/low), after setting
# the flash current to LED1 = 700 mA = channels 1 + 4 at 350 mA each.
# See notes-emitter-analysis.md.
#
# While the IR camera streams, this tries:
#   GPIO9 high alone; GPIO9 high + flash pulses on ir:flash-4, ir:flash-1;
#   GPIO9 high + torch on ir:flash-4, ir:flash-1
# GPIO9 is high only during each capture (about a second), then low again.
# Needs the overlay from: bash install.sh --emitter-test (for ir:flash-*).
set -euo pipefail

here="$(cd "$(dirname "$(readlink -f "$0")")" && pwd)"
raw="$here/../captures/frames.y10p"
current_ua="${CURRENT_UA:-300000}"
sudo -v

chip="$(dirname "$(grep -l 'pmic@1:gpio@8800' /sys/class/gpio/gpiochip*/label)")"
n=$(( $(cat "$chip/base") + 8 ))
g=/sys/class/gpio/gpio$n
echo "PM8550 gpio9 = Linux GPIO $n"
[ -e "$g" ] || echo $n | sudo tee /sys/class/gpio/export >/dev/null
echo out | sudo tee "$g/direction" >/dev/null    # starts low

pulser=
stop_all() {
    [ -n "$pulser" ] && sudo kill "$pulser" 2>/dev/null || true
    pulser=
    for l in /sys/class/leds/ir:flash-*; do
        echo 0 | sudo tee "$l/flash_strobe" >/dev/null 2>&1 || true
        echo 0 | sudo tee "$l/brightness" >/dev/null 2>&1 || true
    done
    [ -e "$g/value" ] && echo 0 | sudo tee "$g/value" >/dev/null || true
}
cleanup() { stop_all; [ -e "$g" ] && echo $n | sudo tee /sys/class/gpio/unexport >/dev/null || true; }
trap cleanup EXIT

pulse() {
    sudo bash -c "while :; do
        echo 0 > '$1/flash_strobe'; echo $current_ua > '$1/flash_brightness'
        echo 50000 > '$1/flash_timeout'; echo 1 > '$1/flash_strobe'; sleep 0.1
    done" &
    pulser=$!
}

sd=
for m in /dev/media*; do
    e="$(media-ctl -d "$m" -e "hm1092 8-0024" 2>/dev/null)" && { sd="$e"; break; }
done
[ -n "$sd" ] && v4l2-ctl -d "$sd" -c exposure=746

shot() {
    bash "$here/capture.sh" >/dev/null 2>&1 || true
    python3 -I "$here/../tools/framestats.py" "$raw" 560 360 704
}

run() {  # $1 label, then the setup command
    local label="$1"; shift
    "$@"
    sleep 0.2
    echo "$label: $(shot)"
    stop_all
    sleep 0.5
}

gpio_high() { echo 1 | sudo tee "$g/value" >/dev/null; }
nothing() { :; }

run "baseline (all off)       " nothing
run "GPIO9 high               " gpio_high
if [ -e /sys/class/leds/ir:flash-4 ]; then
    gpio_high; pulse /sys/class/leds/ir:flash-4; sleep 0.2; echo "GPIO9 + flash ch4 pulses : $(shot)"; stop_all; sleep 0.5
    gpio_high; pulse /sys/class/leds/ir:flash-1; sleep 0.2; echo "GPIO9 + flash ch1 pulses : $(shot)"; stop_all; sleep 0.5
    gpio_high; for c in 1 4; do echo 255 | sudo tee /sys/class/leds/ir:flash-$c/brightness >/dev/null; done
    sleep 0.2; echo "GPIO9 + torch ch1+ch4    : $(shot)"; stop_all
else
    echo "(no ir:flash-* LEDs: only the GPIO9-alone test ran)"
fi
