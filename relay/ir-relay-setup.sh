#!/bin/bash
# Prepare the CAMSS path for the HM1092 IR camera and print its capture node.
# Path: hm1092 -> msm_csiphy0 -> msm_csid1 -> msm_vfe1_rdi0 -> msm_vfe1_video0,
# kept apart from the colour camera (csiphy4 -> csid0 -> vfe0).
# Called by ir-relay each time an application starts streaming.
set -euo pipefail

md=
for m in /dev/media*; do
    media-ctl -d "$m" -p 2>/dev/null | grep -q '^- entity [0-9]*: hm1092 ' && { md="$m"; break; }
done
[ -n "$md" ] || { echo "no hm1092 in any media graph" >&2; exit 1; }
sensor="$(media-ctl -d "$md" -p | sed -n 's/^- entity [0-9]*: \(hm1092 [0-9a-f-]*\) .*/\1/p')"

fmt='Y10_1X10/560x360 field:none'
media-ctl -d "$md" -l '"msm_csiphy0":1->"msm_csid1":0[1]'
media-ctl -d "$md" -l '"msm_csid1":1->"msm_vfe1_rdi0":0[1]'
for pad in "\"$sensor\":0" '"msm_csiphy0":0' '"msm_csid1":0' '"msm_csid1":1' '"msm_vfe1_rdi0":0'; do
    media-ctl -d "$md" -V "$pad[fmt:$fmt]"
done
v4l2-ctl -d "$(media-ctl -d "$md" -e "$sensor")" -c test_pattern=0 >/dev/null

media-ctl -d "$md" -e msm_vfe1_video0
