#!/bin/bash
# Dell Latitude 7455: install the HM1092 IR camera so it survives kernel
# updates.
#   - driver: through DKMS, so it is rebuilt for every new kernel
#   - device tree: the colour-camera and IR overlays applied to the stock tree
#   - pacman hook 85-latitude-7455-cameras.hook: re-applies both overlays when
#     a kernel update replaces the device tree, before the boot image is built
#
#   bash install.sh          install, or repair after a kernel update
#   bash install.sh --undo   remove the IR camera (driver, hook); colour
#                            camera overlay only
#
# CAMSS waits for every sensor in the device tree before it registers any of
# them, so if hm1092 is missing or fails to probe the colour camera disappears
# too. --undo puts the colour camera back on its own.
set -euo pipefail
[ "$(id -u)" -eq 0 ] || exec sudo bash "$0" "$@"

here="$(cd "$(dirname "$(readlink -f "$0")")" && pwd)"
# Colour-camera overlay folder (latitude-7455-camera.dtso), in the home
# directory of the user who ran this script.
cam="$(getent passwd "${SUDO_USER:-$USER}" | cut -d: -f6)/7455-camera"
ver="$(sed -n 's/^PACKAGE_VERSION="\(.*\)"/\1/p' "$here/dkms.conf")"
lib=/usr/local/lib/latitude-7455-cameras
hook=/etc/pacman.d/hooks/85-latitude-7455-cameras.hook
dtb=/boot/dtbs/linux-omarchy/qcom/x1e80100-dell-latitude-7455.dtb
stock="$dtb.stock"
uki=/boot/EFI/Linux/omarchy_linux-omarchy.efi
fallback_uki=/boot/EFI/stock-dtb/omarchy-stock-dtb.efi
colour_node=/soc@0/cci@ac16000/i2c-bus@1/camera@10

has_camera() { fdtget -t s "$1" "$colour_node" compatible >/dev/null 2>&1; }

remove_driver() {
    local v
    for v in $(dkms status hm1092 2>/dev/null | sed -n 's|^hm1092/\([^,]*\),.*|\1|p' | sort -u); do
        dkms remove "hm1092/$v" --all >/dev/null 2>&1 || true
        rm -rf "/usr/src/hm1092-$v"
    done
    # Copies installed by hand by earlier versions of this script.
    rm -f /usr/lib/modules/*/updates/hm1092.ko /usr/lib/modules/*/updates/hm1092.ko.*
}

[ -f "$cam/latitude-7455-camera.dtso" ] || { echo "No colour-camera overlay at $cam (see colour-camera/ in this repository)."; exit 1; }
[ -f "$dtb" ] || { echo "Missing $dtb"; exit 1; }

# The device tree without a camera node is the kernel package's stock copy.
if ! has_camera "$dtb"; then
    cp -f "$dtb" "$stock"
    echo "Saved the stock device tree for this kernel."
    # The current boot image was built from it: keep it as the fallback entry.
    if [ -f "$uki" ] && [ -d "$(dirname "$fallback_uki")" ]; then
        cp -f "$uki" "$fallback_uki"
        echo "Refreshed the fallback boot image (stock device tree, this kernel)."
    fi
fi
[ -f "$stock" ] || { echo "No stock device tree at $stock; run $cam/install-camera.sh first."; exit 1; }

if [ "${1:-}" = "--undo" ]; then
    remove_driver
    rm -f "$hook"
    rm -rf "$lib"
    depmod "$(uname -r)"
    tmp="$(mktemp)"
    dtc -q -@ -I dts -O dtb -o "$tmp.dtbo" "$cam/latitude-7455-camera.dtso"
    fdtoverlay -i "$stock" -o "$tmp" "$tmp.dtbo"
    has_camera "$tmp" || { echo "Merged tree check failed (colour camera)"; rm -f "$tmp" "$tmp.dtbo"; exit 1; }
    install -m 600 "$tmp" "$dtb"
    rm -f "$tmp" "$tmp.dtbo"
    limine-mkinitcpio
    echo "IR camera removed; colour camera overlay only. Reboot to finish."
    echo "Note: without the hook, a kernel update drops the colour overlay again (rerun $cam/install-camera.sh)."
    exit 0
fi

echo "== 1/3 driver (DKMS hm1092/$ver)"
remove_driver
mkdir -p "/usr/src/hm1092-$ver"
install -m 644 "$here/hm1092.c" "$here/hm1092_mode_560x360.h" "$here/Makefile" "$here/dkms.conf" \
    "/usr/src/hm1092-$ver/"
dkms install "hm1092/$ver" -k "$(uname -r)"
modinfo -k "$(uname -r)" -n hm1092

echo "== 2/3 overlays and pacman hook"
install -d -m 755 "$lib"
dtc -q -@ -I dts -O dtb -o "$lib/latitude-7455-camera.dtbo" "$cam/latitude-7455-camera.dtso"
dtc -q -@ -I dts -O dtb -o "$lib/hm1092.dtbo" "$here/hm1092.dtso"
install -m 755 "$here/reapply-overlays.sh" "$lib/reapply"
install -D -m 644 "$here/85-latitude-7455-cameras.hook" "$hook"

echo "== 3/3 device tree and boot image"
"$lib/reapply" --rebuild
sync
echo
echo "Done. Reboot; both cameras should come back."
echo "Kernel updates now rebuild the driver (DKMS) and re-apply the overlays (pacman hook) on their own."
