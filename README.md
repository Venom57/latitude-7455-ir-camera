# HM1092 IR camera on the Dell Latitude 7455 (Snapdragon X Elite)

Linux support for the Windows Hello IR camera of the Dell Latitude 7455 (X1E-80-100, `x1e80100`): a V4L2 driver for the Himax HM1092 sensor, device-tree overlays, the IR emitter, a relay that makes it an ordinary webcam, and the research that got there (decoders for Qualcomm's Windows camera files and notes from the Windows drivers).

Tested on Omarchy (Arch Linux ARM), kernel `linux-omarchy 7.3.0-75`. Discussion and background: https://github.com/omacom/omarchy/discussions/19382

## Status

| Part | State |
| --- | --- |
| Sensor driver (`driver/hm1092.c`) | works: 560x360 Y10, 30 fps, CSIPHY0, 1 lane |
| IR emitter | works: PM8550 GPIO9, pulsed two frames on / two off like Windows |
| Webcam relay (`relay/`) | works: `/dev/video43` "IR Camera", on demand, emitter-lit frames only |
| Face unlock (Howdy) | not done; `howdy/install-howdy.sh` is an untested starting point |

## Requirements

1. The colour-camera overlay from `colour-camera/`, installed first: copy the folder to `~/7455-camera`, build the overlay (`dtc -@ -I dts -O dtb -o latitude-7455-camera.dtbo latitude-7455-camera.dtso`) and run `sudo bash ~/7455-camera/install-camera.sh`. It keeps the stock device tree as `/boot/dtbs/linux-omarchy/qcom/x1e80100-dell-latitude-7455.dtb.stock`, which the IR scripts build on.
2. `linux-omarchy-headers` (or the headers for your kernel), `dkms`, `dtc`, `v4l-utils` (`media-ctl`, `v4l2-ctl`), and for the relay `v4l2loopback-dkms`.

## Install

```
bash driver/install.sh      # driver via DKMS + colour/IR overlays + pacman hook + boot image
bash relay/install.sh       # /dev/video43 "IR Camera" + ir-relay.service (needs v4l2loopback-dkms)
# reboot
```

Kernel updates are handled automatically: DKMS rebuilds `hm1092` for the new kernel, and the pacman hook `85-latitude-7455-cameras.hook` re-applies both overlays (`/usr/local/lib/latitude-7455-cameras/reapply`) when the kernel package replaces the device tree, before `90-mkinitcpio-install` builds the boot image. Without that, an update installs the stock device tree and both cameras disappear (this happened going from 7.3-75 to 7.3-78). Running `bash driver/install.sh` again repairs an installation either way.

`bash driver/install.sh --undo` and `bash relay/install.sh --undo` revert. CAMSS registers its cameras only when every sensor in the device tree has bound, so if `hm1092` is missing the colour camera disappears too.

Module parameter `emitter_mode`: 0 off, 1 on while streaming, 2 pulsed (default).

## Not included

- Dell's Windows driver pack (`Latitude 7455-Win11-A01-W394W.CAB`, 809 MB, MD5 `2f3700b5768bdae95dda8e710b755142`, from https://downloads.dell.com/FOLDER14914706M/1/Latitude%207455-Win11-A01-W394W.CAB). The notes refer to files extracted from it under `driverpack/` (camera package `PVFJN_A00-00`, chipset package `Y9XHN_A00-00`).
- The laptop's DSDT (published in https://github.com/aarch64-laptops/build under `misc/dell-latitude-7455`) and the reference kernel sources (`ov02e10.c`, `pinctrl-x1e80100.c`, Linaro's `x1-dell-thena.dtsi`).
- Captured images (the notes mention some by file name).

## Layout

- `driver/`: the sensor driver, overlay, installer and test scripts (`capture.sh`, `check-*.sh`, `emitter-diag.sh`).
- `relay/`: `ir-relay` (C), its setup helper, systemd unit and installer.
- `probe/`: the first read-only presence test (module and overlay).
- `tools/`: `aeob.py` (Qualcomm AeoB resource files), `chromatix.py` (Chromatix sensor modules), `pe_xref.py` (cross-references in ARM64 PE disassembly), `y10p.py` and `framestats.py` (raw frames).
- `notes-emitter-analysis.md`, `notes-hm1092-regs.txt`, `notes-hm1092-fields.txt`: research notes and dumps.
- `colour-camera/`: the OV02E10 colour-camera overlay this builds on.

License: GPL-2.0 (see `LICENSE`).

# Research log

Goal: get the IR camera (Himax HM1092) and its emitter working on Linux, for face unlock. Kept in the order things were found, so early sections contain guesses that later sections correct.

## What is known (from Dell's Windows driver pack)

Source: `Latitude 7455-Win11-A01-W394W.CAB` (Dell driver pack catalog, MD5 `2f3700b5…`), camera package `PVFJN_A00-00`, extracted to `driverpack/camera/`. The Dell board uses the `QRD` set of files (`SUBSYS_10280C83`–`86`).

| Item | Value | Source |
|---|---|---|
| Power enable | GPIO 90, high = on | `CAMI_RES_QRD.bin` (`tools/aeob.py`) |
| Reset | GPIO 109, active low, released after MCLK | `CAMI_RES_QRD.bin` |
| Clock | `cam_cc_mclk0_clk`, 24 MHz (`CAM_CC_MCLK0_CLK` = 71, pin GPIO 96 `cam_mclk`) | `CAMI_RES_QRD.bin`, pinctrl-x1e80100.c |
| I2C address | 0x24 (0x48 8-bit) | `SCFG_AUX_QRD.bin` |
| Chip ID | 0x1091 at 0x0000-0x0001; 0x0002-0x0003 read 00 10. No address auto-increment: read and write one byte at a time | `SCFG_AUX_QRD.bin`, probe test |
| Register addresses | 16-bit; data 8-bit | sensor module |
| Output | 560x360 RAW10 (CSI DT 0x2b), VC 0, 30 fps | sensor module `streamConfiguration`, regs 0x034c-0x034f |
| Timing | line length 0x0650, frame length 0x02ee, pixel clock 36 MHz | sensor module `resolutionData` |
| Init sequence | 184 register writes | `notes-hm1092-regs.txt` |
| I2C bus | **CCI0 bus 0** (`/soc@0/cci@ac15000/i2c-bus@0`, Linux `i2c-8`) | probe test 2026-11-22: answered at 0x24, reg 0x0000 = 0x10 |
| CSI PHY and lanes | CSIPHY0 (camss port@0), 1 lane, 180 MHz link (inferred, untested) | MCLK0/CCI0 pairing; `C1PG.bin` lists PHYs 0 and 4 only; PLL 24 MHz / 0x0c * 0x5a = 180 MHz = 36 MHz * 10 bit / 2 / 1 lane |
| Emitter | separate Windows driver `qccamflash8380`; wiring unknown | |

Checked against the colour camera: the same decoding of `CAMF_RES_QRD.bin` gives reset GPIO 237, power GPIO 91 and MCLK4 at 19.2 MHz, and `tools/chromatix.py` reproduces Linux's `ov02e10` init list (15 of 16 writes identical).

## Tools

- `tools/aeob.py FILE`: decodes Qualcomm `AeoB` files (`*_RES_*.bin`, `SCFG_*.bin`).
- `tools/chromatix.py FILE fields|regs`: reads `com.qti.sensormodule.*.bin` field tables and register writes.

## Step 1: presence test (`probe/`)

`hm1092_probe.ko` powers the sensor as Windows does, reads registers 0x0000-0x0003 and powers it off. It writes nothing to the sensor. `hm1092-probe.dtso` puts a test node on all four camera I2C buses.

```
bash probe/apply-probe-overlay.sh          # adds the test nodes, rebuilds the boot image (sudo)
# reboot
sudo insmod probe/hm1092_probe.ko
sudo dmesg | grep -i hm1092
sudo rmmod hm1092_probe
bash probe/apply-probe-overlay.sh --undo   # back to the colour camera overlay only
```

### Result (2026-11-22)

`hm1092-probe 8-0024: ANSWERED at 0x24 on i2c-8: regs 0x0000-0x0003 = 10 ff ff ff`

The first version of the module crashed the kernel after this line (the regulator array was on the stack while devm held a pointer to it); fixed. The `ff` bytes are probably the sensor not auto-incrementing on reads; the fixed module also reads each byte on its own.

Second run (fixed module): regs 0x0000-0x0003 = `10 91 00 10` read one at a time; no answer on the other three buses.

## Register map (SMIA-style, from the sensor module)

| Register | Use |
|---|---|
| 0x0100 | mode select: 1 = stream (startStream), 0 = standby |
| 0x0104 | group hold (groupHoldOn/Off) |
| 0x0202-0x0203 | coarse exposure, lines (init 0x00be) |
| 0x0340-0x0341 | frame length (0x02ee) |
| 0x0342-0x0343 | line length (0x0650) |
| 0x034c-0x034f | output size 560x360 |
| 0x0601 | test pattern 0-4 (testPatternData) |
| analogue gain | unknown (maxAnalogGain 15.5) |

## Step 2: driver (`driver/`)

- `hm1092.c`: V4L2 sensor driver, Y10_1X10 560x360, controls for exposure, vblank and test pattern. `hm1092_mode_560x360.h` is generated from the Windows register list.
- `hm1092.dtso`: the sensor on CCI0 bus 0, connected to CAMSS port@0.
- `install.sh`: installs the module to `/usr/lib/modules/$(uname -r)/updates` and the overlay (replacing the probe overlay); `--undo` removes both.
- `capture.sh [--pattern N]`: links hm1092 -> csiphy0 -> csid1 -> vfe1_rdi0 and saves a PNG in `captures/`. `tools/y10p.py` converts the raw frames.

CAMSS registers its sensors only after every sensor in the device tree has bound, so if hm1092 fails to probe, or its module is missing after a kernel update, the colour camera disappears too. `install.sh --undo` restores it; the module must be rebuilt (`make -C driver clean all`) after kernel updates.

```
make -C driver
bash driver/install.sh
# reboot
sudo dmesg | grep -iE 'hm1092|camss'
bash driver/capture.sh --pattern 2   # test pattern: checks the CSI link
bash driver/capture.sh               # real image
```

### Result (2026-11-22, step 2)

- The driver probes ("HM1092 found, chip ID 0x1091"), and the colour camera still registers next to it.
- Test pattern 2 (`--pattern 2`) streams at 30 fps over CSIPHY0 with 1 lane at 180 MHz and shows clean colour bars (alternate rows differ because the pattern generator is Bayer-patterned). This confirms the PHY, lane and link-frequency guesses.
- A real capture with no IR light is black-level noise (8-bit mean 7.8). Exposure 190 -> 746 lines gave the same mean, so whether 0x0202 controls exposure is still unconfirmed and needs an IR source.

A TV remote pointed at the camera shows up as a saturated spot with lens flare, on the frames where the remote is sending (`captures/ir-204619.png`, not published). The optical path works.

## Step 3: IR emitter (`driver/ir-emitter-test.dtso`, `driver/find-emitter.sh`)

Windows drives the emitter through Qualcomm's "WhiteLEDFlashDriverSoC" (`qccamflash8380.sys`), i.e. the PMIC flash block, `pm8550_flash` at 0xee00 (disabled in the stock tree; Linux driver `leds-qcom-flash`), with `IrLedCurrentMilliampere = 700` (`qccamflash_ext8380.inf`). The `FLSH` resource file is empty, so there is no GPIO involved. The channel is unknown: the test overlay exposes all four as `ir:torch-1`..`4`, each torch-only with the PMIC clamp at 100 mA.

```
bash driver/install.sh --emitter-test   # reboot afterwards
bash driver/check-emitter.sh
```

### Result (2026-11-22, step 3)

Not on the PM8550 flash channels: with each of `ir:torch-1`..`4` at 100 mA, the IR image stayed at mean 7.8 (same as all off) and a phone camera showed no glow. BOB rails are on. Clues still open, from `qccamflash8380.sys`: `CameraWhiteLEDFlashPMIC_Fire`/`SetTargetCurrent`, `CameraIRLED_Trigger`, `IRLED_CCI_Trigger_Callback`, `CCI Timer Index`, `CameraSensorGPIO_SyncWriteWait`. The FLSH ACPI `_CRS` is empty and its PEP resources are empty; the driver reaches the hardware through `PoFxPowerControl`. CCI timer outputs are GPIO 109-113 (timer0 = GPIO 109, which Windows toggles in the sensor power-up; GPIO 110 is the white camera indicator LED).

### Prior work found (2026-11-22)

Patch series "media/arm64: HM1092 IR camera and ASUS Zenbook A14 (X1P42100) camera support" (Ramshouriesh, 10 June 2026, linux-arm-msm; https://ratatoskr.run/linux-arm-msm/2026/06/17114244/t). Same sensor, same wiring as found here (0x24 on CCI0 bus 0, reset GPIO 109, MCLK0 24 MHz, CSIPHY0, 1 lane). Its emitter is the PM8550 flash LED **channel 4**, driven in **flash mode** at 700 mA (`flash-max-microamp = <700000>`, `led-max-microamp = <180000>`), 50 ms strobes re-fired every 100 ms while streaming, via `leds = <&ir_flash>` on the sensor node and the LED flash API. Its driver re-arms brightness and timeout before every strobe because a bare re-strobe fires at 0 mA. Review asked for the driver to be split out and the Chromatix tables dropped; not merged as of this note.

The torch-mode test above lit nothing on any channel, so `ir-emitter-test.dtso` now makes all four channels flash-capable (`ir:flash-1`..`4`, 400 mA cap) and `check-emitter.sh` strobes each one while capturing.

### Independent analysis (2026-11-22): `notes-emitter-analysis.md`

A second pass over the Windows binaries (Fable) found that `CameraWhiteLEDFlashPMIC_Fire` (IOCTL 0x80028fa8 to `qcpmic8380.sys`, not PoFx/PEP) is a **PMIC GPIO write: PM8550 gpio9** (SID 1, base 0x9000), high = on. For trigger type 2 Windows toggles it on start-of-frame (two frames high, two low). `SetTargetCurrent(LED1 = 700)` maps to **channels 1 + 4 at 350 mA each**. Windows never writes the flash block's module/channel enables. Linux never drove gpio9 (unused in the Dell and Linaro DTs). Corrections to the notes above: 0x140004aa0 is an IOCTL to qcpmic; SetSafetyTimer is 0x802f0fac; the CCI-timer path (`CameraIRLED_Trigger`) is used only by trigger type 1, so GPIO 109 is not involved.

Test: `bash driver/check-gpio9.sh` (sudo, no reboot; uses the `--emitter-test` overlay for the flash LEDs). It reports the brightest of the 15 frames via `tools/framestats.py`. The last flash-mode capture on channel 4 was dark in all 15 frames, so the earlier result was not a sampling miss.

### Emitter working (2026-11-22)

`driver/check-gpio9.sh`: PM8550 gpio9 high alone lit the emitter (IR frame mean 7.9 -> 20.0, a face clearly visible at arm's length: `captures/ir-emitter-first.png`, not published); the flash channels made no difference. The emitter regulates its own current; gpio9 is its switch.

Now built in: `hm1092.dtso` adds `ir:emitter` (gpio-leds on `<&pm8550_gpios 9 GPIO_ACTIVE_HIGH>`) and links it to the sensor (`leds`, `led-names = "ir"`). The driver lights it while streaming. Module parameter `emitter_mode`: 0 off, 1 on, **2 = Windows pattern (default)**: two frames lit, two dark. The driver doesn't see start-of-frame, so toggles are scheduled every two frame periods (from line length, frame length and pixel rate, following vblank) against absolute deadlines from stream start; phase is relative to stream start, so a frame can be partly lit. Change at runtime: `echo 1 | sudo tee /sys/module/hm1092/parameters/emitter_mode` (takes effect at the next stream start). `driver/ir-photo.sh` lights it by hand through sysfs for the old setup.

Driver pulsing verified (2026-11-22, after fixing `led_set_brightness_sync()` -> `led_set_brightness()`: the PMIC GPIO's LED has only a non-blocking setter, so `_sync` silently did nothing). Per-frame means at exposure 746: `25.0 21.6 7.9 8.6 | 24.9 21.9 7.9 8.1 | ...`, two lit and two dark, locked over 15 frames. Face clearly lit: `captures/ir-214025.png`, not published. Default exposure is now the full frame (746 lines); `y10p.py` saves the brightest frame.

## Step 4: IR webcam relay (`relay/`)

`ir-relay` turns the IR camera into an ordinary webcam: a second v4l2loopback device, `/dev/video43` "IR Camera" (next to the colour camera's `/dev/video42`), fed by `ir-relay.service` (system service, DynamicUser in group `video`).

- On demand: it waits for v4l2loopback's client-usage event and streams the sensor only while an application is capturing from `/dev/video43`, so the sensor and emitter are otherwise off. `ir-relay-setup.sh` sets up the CAMSS links and formats each time and prints the capture node.
- Only emitter-lit frames are passed on (mean above the recent darkest by 2; if all recent frames are alike, steady or no emitter, all are passed).
- Black level (32 in 10-bit) removed, then a gain that brings the 99th percentile to about 230, adapting slowly. Output YUYV 560x360, grey in Y.
- `ir-relay --test OUT.pgm` runs 30 frames without the loopback device: on 2026-11-22 it kept 16/30 (exactly the lit pairs), gain settled near 3.6, `captures/relay-test.png`, not published.

```
bash relay/install.sh     # second loopback device + service; reboot afterwards
bash relay/install.sh --undo
```
