#!/bin/bash
# Install the IR camera relay: a second v4l2loopback device /dev/video43
# ("IR Camera", next to the colour camera's /dev/video42) and a system service
# that feeds it from the HM1092 only while an application is streaming.
#   bash install.sh          install (reboot afterwards to recreate the devices)
#   bash install.sh --undo   remove; back to /dev/video42 only
set -euo pipefail
[ "$(id -u)" -eq 0 ] || exec sudo bash "$0" "$@"
here="$(cd "$(dirname "$(readlink -f "$0")")" && pwd)"

conf=/etc/modprobe.d/v4l2loopback.conf
if [ "${1:-}" = "--undo" ]; then
    systemctl disable --now ir-relay.service 2>/dev/null || true
    rm -f /etc/systemd/system/ir-relay.service
    rm -rf /usr/lib/ir-relay
    cat >"$conf" <<'CONF'
# Virtual webcam for the Latitude 7455's built-in camera (fed by a relay service).
options v4l2loopback video_nr=42 card_label="Built-in Camera" exclusive_caps=1 max_buffers=4
CONF
    systemctl daemon-reload
    echo "IR relay removed. Reboot to drop /dev/video43."
    exit 0
fi

make -C "$here" >/dev/null
install -D -m 755 "$here/ir-relay" /usr/lib/ir-relay/ir-relay
install -D -m 755 "$here/ir-relay-setup.sh" /usr/lib/ir-relay/ir-relay-setup.sh
install -D -m 644 "$here/ir-relay.service" /etc/systemd/system/ir-relay.service

cat >"$conf" <<'CONF'
# Virtual webcams for the Latitude 7455: the colour camera (fed by the user's
# camera-relay service) and the IR camera (fed by ir-relay.service on demand).
options v4l2loopback devices=2 video_nr=42,43 card_label="Built-in Camera","IR Camera" exclusive_caps=1,1 max_buffers=4
CONF

systemctl daemon-reload
systemctl enable ir-relay.service
echo "Installed. Reboot so v4l2loopback creates /dev/video43; ir-relay.service then starts on its own."
