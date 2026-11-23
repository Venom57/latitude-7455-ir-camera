#!/bin/bash
# Dell Latitude 7455: install the HM1092 IR camera driver and its device-tree
# overlay (on top of the colour camera overlay), replacing the presence-test
# overlay.
#   bash install.sh                   install the module and the overlay
#   bash install.sh --emitter-test    also expose the PMIC flash channels as
#                                     ir:flash-1..4 (400 mA flash cap), to find the emitter
#   bash install.sh --undo            remove both; back to the colour camera only
#
# CAMSS waits for every sensor in the device tree before it registers any of
# them, so if hm1092 fails to probe the colour camera disappears too. --undo
# puts it back.
set -euo pipefail
[ "$(id -u)" -eq 0 ] || exec sudo bash "$0" "$@"

here="$(cd "$(dirname "$(readlink -f "$0")")" && pwd)"
# Colour-camera overlay folder (latitude-7455-camera.dtso/.dtbo), in the home
# directory of the user who ran this script.
cam="$(getent passwd "${SUDO_USER:-$USER}" | cut -d: -f6)/7455-camera"
kver="$(uname -r)"
moddir="/usr/lib/modules/$kver/updates"
dtb=/boot/dtbs/linux-omarchy/qcom/x1e80100-dell-latitude-7455.dtb
stock="$dtb.stock"

[ -f "$stock" ] || { echo "No stock device tree at $stock; run $cam/install-camera.sh first."; exit 1; }
if fdtget -t s "$stock" /soc@0/cci@ac16000/i2c-bus@1/camera@10 compatible >/dev/null 2>&1; then
    echo "$stock already has a camera node; refusing."; exit 1
fi

overlays=("$cam/latitude-7455-camera.dtbo")
if [ "${1:-}" != "--undo" ]; then
    [ -f "$here/hm1092.ko" ] || { echo "Build the module first: make -C $here"; exit 1; }
    modinfo -F vermagic "$here/hm1092.ko" | grep -q "^$kver " ||
        { echo "hm1092.ko was built for another kernel; rebuild it: make -C $here clean all"; exit 1; }
    dtc -q -@ -I dts -O dtb -o "$here/hm1092.dtbo" "$here/hm1092.dtso"
    overlays+=("$here/hm1092.dtbo")
    if [ "${1:-}" = "--emitter-test" ]; then
        dtc -q -@ -I dts -O dtb -o "$here/ir-emitter-test.dtbo" "$here/ir-emitter-test.dtso"
        overlays+=("$here/ir-emitter-test.dtbo")
    fi
fi

tmp="$(mktemp)"
fdtoverlay -i "$stock" -o "$tmp" "${overlays[@]}"
[ "$(fdtget -t s "$tmp" /soc@0/cci@ac16000/i2c-bus@1/camera@10 compatible)" = "ovti,ov02e10" ] ||
    { echo "Merged tree check failed (colour camera)"; rm -f "$tmp"; exit 1; }
if [ "${1:-}" != "--undo" ]; then
    [ "$(fdtget -t s "$tmp" /soc@0/cci@ac15000/i2c-bus@0/camera@24 compatible)" = "himax,hm1092" ] ||
        { echo "Merged tree check failed (hm1092)"; rm -f "$tmp"; exit 1; }
fi
if [ "${1:-}" = "--emitter-test" ]; then
    flash=/soc@0/arbiter@c400000/spmi@c42d000/pmic@1/led-controller@ee00
    [ "$(fdtget -t s "$tmp" $flash status)" = okay ] &&
    [ "$(fdtget "$tmp" $flash/led-0 led-max-microamp)" = 100000 ] ||
        { echo "Merged tree check failed (flash LEDs)"; rm -f "$tmp"; exit 1; }
fi

if [ "${1:-}" = "--undo" ]; then
    rm -f "$moddir/hm1092.ko"
else
    install -D -m 644 "$here/hm1092.ko" "$moddir/hm1092.ko"
fi
depmod "$kver"

install -m 600 "$tmp" "$dtb"
rm -f "$tmp"
echo "Rebuilding the boot image ..."
limine-mkinitcpio
sync

if [ "${1:-}" = "--undo" ]; then
    echo "IR camera removed. Reboot to finish."
else
    echo "IR camera driver installed. Reboot, then: sudo dmesg | grep -iE 'hm1092|camss'"
fi
