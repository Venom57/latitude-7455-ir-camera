#!/bin/bash
# Re-apply the Latitude 7455 camera overlays (OV02E10 colour camera + HM1092
# IR camera) to the boot device tree.
#
# Installed as /usr/local/lib/latitude-7455-cameras/reapply and run by the
# pacman hook 85-latitude-7455-cameras.hook whenever the kernel package
# replaces the device tree, before 90-mkinitcpio-install builds the boot
# image. Run by hand with --rebuild to also rebuild the boot image.
#
# A device tree without the camera node is a fresh stock copy and is saved as
# the new .stock; the overlays are always applied to .stock.
set -euo pipefail

lib=/usr/local/lib/latitude-7455-cameras
dtb=/boot/dtbs/linux-omarchy/qcom/x1e80100-dell-latitude-7455.dtb
stock="$dtb.stock"
colour=/soc@0/cci@ac16000/i2c-bus@1/camera@10
ir=/soc@0/cci@ac15000/i2c-bus@0/camera@24

compat() { fdtget -t s "$1" "$2" compatible 2>/dev/null || true; }

[ -f "$dtb" ] || { echo "latitude-7455-cameras: $dtb missing"; exit 1; }
if [ -n "$(compat "$dtb" "$colour")" ]; then
    [ -f "$stock" ] || { echo "latitude-7455-cameras: overlaid tree but no $stock; not touching it"; exit 1; }
else
    cp -f "$dtb" "$stock"
    echo "latitude-7455-cameras: saved the new stock device tree"
fi

tmp="$(mktemp)"
trap 'rm -f "$tmp"' EXIT
fdtoverlay -i "$stock" -o "$tmp" "$lib/latitude-7455-camera.dtbo" "$lib/hm1092.dtbo"
[ "$(compat "$tmp" "$colour")" = "ovti,ov02e10" ] &&
[ "$(compat "$tmp" "$ir")" = "himax,hm1092" ] &&
[ "$(fdtget -t s "$tmp" / compatible | cut -d' ' -f1)" = "$(fdtget -t s "$stock" / compatible | cut -d' ' -f1)" ] ||
    { echo "latitude-7455-cameras: merged tree check failed; leaving the stock device tree"; exit 1; }
install -m 600 "$tmp" "$dtb"
echo "latitude-7455-cameras: colour and IR camera overlays applied"

if [ "${1:-}" = "--rebuild" ]; then
    limine-mkinitcpio
fi
