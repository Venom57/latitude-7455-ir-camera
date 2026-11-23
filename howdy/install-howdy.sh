#!/bin/bash
# Install Howdy (face authentication) on the Latitude 7455 (Arch Linux ARM)
# and point it at the IR camera relay (/dev/video43).
#
# Run as your user (not with sudo); it asks for the password when installing.
# The AUR packages are marked x86_64 only, so they are built with
# --ignorearch; python-dlib also has its CUDA variant switched off.
# This installs Howdy only. Nothing is added to PAM (sudo, lock screen) yet.
set -euo pipefail
[ "$(id -u)" -ne 0 ] || { echo "Run as your user, not root."; exit 1; }

work="${XDG_CACHE_HOME:-$HOME/.cache}/ir-camera-howdy"
mkdir -p "$work"
sudo -v

echo "== 1/4 dependencies from the repositories"
sudo pacman -S --needed --noconfirm \
    base-devel git meson ninja cmake boost python-setuptools sqlite \
    dlib python-opencv python-numpy libinih libevdev cblas lapack libx11

build_aur() {  # $1 package, $2 optional sed expression for the PKGBUILD
    local p="$1"
    rm -rf "$work/$p"
    git clone --depth 1 "https://aur.archlinux.org/$p.git" "$work/$p"
    if [ -n "${2:-}" ]; then
        sed -i "$2" "$work/$p/PKGBUILD"
    fi
    (cd "$work/$p" && makepkg -si --ignorearch --noconfirm --needed)
}

echo "== 2/4 python-dlib (CPU only; this compile takes a while)"
if pacman -Q python-dlib >/dev/null 2>&1; then
    echo "already installed"
else
    build_aur python-dlib 's/^_build_cuda=1/_build_cuda=0/'
fi

echo "== 3/4 howdy-git"
if pacman -Q howdy-git >/dev/null 2>&1; then
    echo "already installed"
else
    build_aur howdy-git
fi

echo "== 4/4 configure /etc/howdy/config.ini for the IR camera"
cfg=/etc/howdy/config.ini
[ -f "$cfg" ] || { echo "$cfg not found; check the howdy-git install"; exit 1; }
sudo cp -n "$cfg" "$cfg.orig"
set_opt() {  # key value: replace "key = ..." wherever it is
    sudo sed -i "s|^\s*$1\s*=.*|$1 = $2|" "$cfg"
    grep -q "^$1 = $2\$" "$cfg" || echo "warning: $1 not found in $cfg"
}
set_opt device_path /dev/video43
# The relay passes on only emitter-lit frames, but the room behind the face
# is dark; Howdy skips frames where too many pixels fall in the darkest bin.
set_opt dark_threshold 90
grep -nE '^(device_path|dark_threshold|certainty|timeout|use_cnn) ' "$cfg"

echo
echo "Howdy installed. Next:"
echo "  sudo howdy add        # enrol your face (do it 2-3 times: with/without glasses)"
echo "  sudo howdy test       # live view with recognition scores (needs a window)"
echo "Nothing uses it for login yet; that is the PAM step."
