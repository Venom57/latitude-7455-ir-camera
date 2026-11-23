#!/bin/bash
# Take one IR picture with the emitter on (PM8550 gpio9 high for about a
# second), until the driver does this by itself. Saves a PNG in captures/.
set -euo pipefail
here="$(cd "$(dirname "$(readlink -f "$0")")" && pwd)"
sudo -v

chip="$(dirname "$(grep -l 'pmic@1:gpio@8800' /sys/class/gpio/gpiochip*/label)")"
n=$(( $(cat "$chip/base") + 8 ))
g=/sys/class/gpio/gpio$n
off() { echo 0 | sudo tee "$g/value" >/dev/null 2>&1 || true; echo $n | sudo tee /sys/class/gpio/unexport >/dev/null 2>&1 || true; }
trap off EXIT
[ -e "$g" ] || echo $n | sudo tee /sys/class/gpio/export >/dev/null
echo out | sudo tee "$g/direction" >/dev/null
echo 1 | sudo tee "$g/value" >/dev/null

bash "$here/capture.sh" | tail -1
