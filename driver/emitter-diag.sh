#!/bin/bash
# Why does ir:emitter (gpio-leds on PM8550 gpio9) not light the emitter when
# raw sysfs GPIO did? Shows the pin's PMIC configuration and level, switches
# the LED on by hand (driver pulsing disabled meanwhile), captures, restores.
set -uo pipefail
here="$(cd "$(dirname "$(readlink -f "$0")")" && pwd)"
raw="$here/../captures/frames.y10p"
led=/sys/class/leds/ir:emitter
pc="/sys/kernel/debug/pinctrl/c42d000.spmi:pmic@1:gpio@8800/pinconf-pins"
sudo -v

pin() {  # PMIC config of gpio9 and the gpiolib view
    sudo grep -E '^pin 8 |gpio9' "$pc" 2>/dev/null | head -2
    sudo grep -A13 'pmic@1:gpio@8800' /sys/kernel/debug/gpio 2>/dev/null | grep -E 'gpio9|-765 ' | head -2
}
stats() { bash "$here/capture.sh" >/dev/null 2>&1; python3 -I "$here/../tools/framestats.py" "$raw" 560 360 704; }

old_mode="$(cat /sys/module/hm1092/parameters/emitter_mode)"
restore() {
    echo 0 | sudo tee "$led/brightness" >/dev/null
    echo "$old_mode" | sudo tee /sys/module/hm1092/parameters/emitter_mode >/dev/null
}
trap restore EXIT
echo 0 | sudo tee /sys/module/hm1092/parameters/emitter_mode >/dev/null

echo "== LED: max_brightness $(cat $led/max_brightness), trigger: $(grep -o '\[[^]]*\]' $led/trigger)"
echo "== pin, LED off:"; pin
echo "LED off:  $(stats)"

echo 1 | sudo tee "$led/brightness" >/dev/null
echo "== pin, LED on:"; pin
echo "LED on:   $(stats)"
echo 0 | sudo tee "$led/brightness" >/dev/null

echo "== driver pulsing (emitter_mode 2):"
echo 2 | sudo tee /sys/module/hm1092/parameters/emitter_mode >/dev/null
echo "mode 2:   $(stats)"
echo "== driver steady (emitter_mode 1):"
echo 1 | sudo tee /sys/module/hm1092/parameters/emitter_mode >/dev/null
echo "mode 1:   $(stats)"
