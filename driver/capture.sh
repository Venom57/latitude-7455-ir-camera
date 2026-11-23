#!/bin/bash
# Capture frames from the HM1092 IR camera through CAMSS and save the last one
# as a PNG.
#   bash capture.sh               real image (dark unless something lights it in IR)
#   bash capture.sh --pattern 2   sensor test pattern (2 = colour bars), which
#                                 tests the CSI link without needing IR light
# Path: hm1092 -> msm_csiphy0 -> msm_csid1 -> msm_vfe1_rdi0 -> video node,
# kept apart from the colour camera (csiphy4 -> csid0 -> vfe0).
set -euo pipefail

here="$(cd "$(dirname "$(readlink -f "$0")")" && pwd)"
out="$here/../captures"
mkdir -p "$out"
pattern=0
[ "${1:-}" = "--pattern" ] && pattern="${2:?pattern number 0-4}"

md=
for m in /dev/media*; do
    media-ctl -d "$m" -p 2>/dev/null | grep -q '^- entity [0-9]*: hm1092 ' && { md="$m"; break; }
done
[ -n "$md" ] || { echo "No hm1092 entity in any media graph; is the driver loaded? (sudo dmesg | grep -i hm1092)"; exit 1; }
sensor="$(media-ctl -d "$md" -p | sed -n 's/^- entity [0-9]*: \(hm1092 [0-9a-f-]*\) .*/\1/p')"
echo "media device $md, sensor \"$sensor\""

fmt='Y10_1X10/560x360 field:none'
media-ctl -d "$md" -l '"msm_csiphy0":1->"msm_csid1":0[1]'
media-ctl -d "$md" -l '"msm_csid1":1->"msm_vfe1_rdi0":0[1]'
media-ctl -d "$md" -V "\"$sensor\":0[fmt:$fmt]"
media-ctl -d "$md" -V "\"msm_csiphy0\":0[fmt:$fmt]"
media-ctl -d "$md" -V "\"msm_csid1\":0[fmt:$fmt]"
media-ctl -d "$md" -V "\"msm_csid1\":1[fmt:$fmt]"
media-ctl -d "$md" -V "\"msm_vfe1_rdi0\":0[fmt:$fmt]"

sd="$(media-ctl -d "$md" -e "$sensor")"
vid="$(media-ctl -d "$md" -e msm_vfe1_video0)"
v4l2-ctl -d "$sd" -c test_pattern="$pattern"
v4l2-ctl -d "$vid" --set-fmt-video=width=560,height=360,pixelformat=Y10P
stride="$(v4l2-ctl -d "$vid" --get-fmt-video | sed -n 's/.*Bytes per Line *: *\([0-9]*\).*/\1/p')"

raw="$out/frames.y10p"
rm -f "$raw"
echo "Streaming 15 frames from $vid (stride $stride) ..."
timeout 10 v4l2-ctl -d "$vid" --stream-mmap --stream-count=15 --stream-to="$raw" --verbose 2>&1 | tail -5 || true
[ -s "$raw" ] || { echo "No frames arrived. Kernel log:"; sudo dmesg | grep -iE 'hm1092|camss|csid|csiphy|vfe' | tail -20; exit 1; }

name="ir-$(date +%H%M%S)"
if [ "$pattern" != 0 ]; then name="$name-pattern$pattern"; fi
python3 -I "$here/../tools/y10p.py" "$raw" 560 360 "$stride" "$out/$name.pgm"
if command -v ffmpeg >/dev/null; then
    ffmpeg -loglevel error -y -i "$out/$name.pgm" "$out/$name.png" && rm -f "$out/$name.pgm"
    echo "Saved $out/$name.png"
else
    echo "Saved $out/$name.pgm"
fi
