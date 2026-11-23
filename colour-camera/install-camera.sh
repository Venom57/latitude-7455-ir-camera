#!/bin/bash
# Dell Latitude 7455: apply the camera overlay to the boot device tree.
#   sudo bash ~/7455-camera/install-camera.sh          install
#   sudo bash ~/7455-camera/install-camera.sh --undo   put the stock device tree back
# Keeps the stock device tree and the current boot image as fallbacks.
set -euo pipefail
[ "$(id -u)" -eq 0 ] || exec sudo bash "$0" "$@"

here="$(cd "$(dirname "$(readlink -f "$0")")" && pwd)"
dtb=/boot/dtbs/linux-omarchy/qcom/x1e80100-dell-latitude-7455.dtb
stock="$dtb.stock"
uki=/boot/EFI/Linux/omarchy_linux-omarchy.efi
fallback_dir=/boot/EFI/stock-dtb
fallback_uki="$fallback_dir/omarchy-stock-dtb.efi"
conf=/boot/limine.conf
marker="/Omarchy (stock device tree, no camera)"

rebuild() {
    echo "Rebuilding the boot image ..."
    limine-mkinitcpio
}

ensure_fallback_entry() {
    grep -qF "$marker" "$conf" && return 0
    local cmdline
    cmdline="$(grep -m1 '^[[:space:]]*cmdline:' "$conf" | sed 's/^[[:space:]]*cmdline:[[:space:]]*//')"
    [ -n "$cmdline" ] || { echo "Could not read the kernel command line from $conf"; return 1; }
    cp -a "$conf" "$conf.before-camera"
    {
        echo
        echo "$marker"
        echo "comment: Boot image saved before the camera overlay was applied"
        echo "protocol: efi"
        echo "path: boot():/EFI/stock-dtb/omarchy-stock-dtb.efi"
        echo "cmdline: $cmdline"
    } >>"$conf"
    echo "Added boot menu entry: ${marker#/}"
}

if [ "${1:-}" = "--undo" ]; then
    [ -f "$stock" ] || { echo "No saved stock device tree at $stock - nothing to undo."; exit 1; }
    cp -f "$stock" "$dtb"
    rebuild
    echo "Stock device tree restored. Reboot to finish."
    exit 0
fi

command -v fdtoverlay >/dev/null || { echo "fdtoverlay missing: sudo pacman -S dtc"; exit 1; }
[ -f "$dtb" ] || { echo "Missing $dtb"; exit 1; }

# 1. Save the stock device tree once. If a kernel update replaced the file,
#    the camera node is gone and the new file becomes the new stock copy.
if fdtget -t s "$dtb" /soc@0/cci@ac16000/i2c-bus@1/camera@10 compatible >/dev/null 2>&1; then
    echo "Camera is already in the boot device tree."
    [ -f "$stock" ] || { echo "...but no stock copy exists; stopping."; exit 1; }
else
    cp -f "$dtb" "$stock"
    echo "Saved stock device tree: $stock"
fi
cp -f "$stock" "$here/x1e80100-dell-latitude-7455.dtb.stock"

# 2. Save the current boot image as a fallback (only the first time).
mkdir -p "$fallback_dir"
if [ ! -f "$fallback_uki" ]; then
    cp -f "$uki" "$fallback_uki"
    echo "Saved current boot image: $fallback_uki"
fi

# 3. Build and check the merged device tree before installing it.
dtc -q -@ -I dts -O dtb -o "$here/latitude-7455-camera.dtbo" "$here/latitude-7455-camera.dtso" 2>/dev/null
tmp="$(mktemp)"
fdtoverlay -i "$stock" -o "$tmp" "$here/latitude-7455-camera.dtbo"
[ "$(fdtget -t s "$tmp" /soc@0/isp@acb7000 status)" = okay ] || { echo "Merged tree check failed (isp)"; exit 1; }
[ "$(fdtget -t s "$tmp" /soc@0/cci@ac16000 status)" = okay ] || { echo "Merged tree check failed (cci)"; exit 1; }
[ "$(fdtget -t s "$tmp" /soc@0/cci@ac16000/i2c-bus@1/camera@10 compatible)" = "ovti,ov02e10" ] || { echo "Merged tree check failed (sensor)"; exit 1; }
[ "$(fdtget -t s "$tmp" / compatible | cut -d' ' -f1)" = "$(fdtget -t s "$stock" / compatible | cut -d' ' -f1)" ] || { echo "Merged tree check failed (model)"; exit 1; }
install -m 600 "$tmp" "$dtb"
rm -f "$tmp"
echo "Installed merged device tree."

# 4. Rebuild the boot image, then make sure the fallback menu entry exists.
rebuild
ensure_fallback_entry
chown -R "$(stat -c %U "$here")": "$here" 2>/dev/null || true
sync
echo
echo "Done. Reboot to try the camera."
echo "If the laptop does not start: in the boot menu pick"
echo "   ${marker#/}"
echo "then run:  sudo bash $0 --undo"
