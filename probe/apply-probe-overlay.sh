#!/bin/bash
# Dell Latitude 7455: add the HM1092 presence-test nodes to the boot device
# tree, on top of the colour camera overlay. Nothing runs at boot; the test
# only happens when hm1092_probe.ko is loaded by hand.
#   bash apply-probe-overlay.sh          add the test nodes
#   bash apply-probe-overlay.sh --undo   back to the colour camera overlay only
set -euo pipefail
[ "$(id -u)" -eq 0 ] || exec sudo bash "$0" "$@"

here="$(cd "$(dirname "$(readlink -f "$0")")" && pwd)"
# Colour-camera overlay folder (latitude-7455-camera.dtso/.dtbo), in the home
# directory of the user who ran this script.
cam="$(getent passwd "${SUDO_USER:-$USER}" | cut -d: -f6)/7455-camera"
dtb=/boot/dtbs/linux-omarchy/qcom/x1e80100-dell-latitude-7455.dtb
stock="$dtb.stock"

[ -f "$stock" ] || { echo "No stock device tree at $stock; run $cam/install-camera.sh first."; exit 1; }
# The stock copy must be this kernel's tree, without the camera node.
if fdtget -t s "$stock" /soc@0/cci@ac16000/i2c-bus@1/camera@10 compatible >/dev/null 2>&1; then
    echo "$stock already has a camera node; refusing."; exit 1
fi

overlays=("$cam/latitude-7455-camera.dtbo")
if [ "${1:-}" != "--undo" ]; then
    dtc -q -@ -I dts -O dtb -o "$here/hm1092-probe.dtbo" "$here/hm1092-probe.dtso"
    overlays+=("$here/hm1092-probe.dtbo")
fi

tmp="$(mktemp)"
fdtoverlay -i "$stock" -o "$tmp" "${overlays[@]}"
# Check the merged tree before installing it.
[ "$(fdtget -t s "$tmp" /soc@0/cci@ac16000/i2c-bus@1/camera@10 compatible)" = "ovti,ov02e10" ] ||
    { echo "Merged tree check failed (colour camera)"; rm -f "$tmp"; exit 1; }
if [ "${1:-}" != "--undo" ]; then
    [ "$(fdtget -t s "$tmp" /soc@0/cci@ac15000 status)" = okay ] ||
        { echo "Merged tree check failed (cci0)"; rm -f "$tmp"; exit 1; }
fi
install -m 600 "$tmp" "$dtb"
rm -f "$tmp"

echo "Rebuilding the boot image ..."
limine-mkinitcpio
sync
if [ "${1:-}" = "--undo" ]; then
    echo "Back to the colour camera overlay only. Reboot to finish."
else
    echo "Test nodes added. Reboot, then: sudo insmod $here/hm1092_probe.ko && sudo dmesg | grep -i hm1092"
fi
