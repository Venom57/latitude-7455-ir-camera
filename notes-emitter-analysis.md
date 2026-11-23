# IR emitter: independent analysis (2026-11-22)

Read-only analysis of `qccamflash8380.sys`, `qcpmic8380.sys`, `qccamplatform8380.sys`, the AeoB files, the DSDT and upstream sources. Nothing on the system was changed.
Scratch disassemblies were not kept; regenerate them with `objdump -d` and `tools/pe_xref.py`.

## Main finding

On Windows the "on" switch for the emitter is **PM8550 GPIO9** (SPMI SID 1, `pm8550_gpios` pin 9, register base 0x9000), not a flash-block register. The Linux tests never touched it. Nothing in the driver pack ever writes the flash block's module-enable (0xee46) or channel-enable (0xee4e) registers.

Chain of evidence (all read from the binaries):

1. `qccamflash8380.sys` 0x140004aa0 is `IoBuildDeviceIoControlRequest` + `IofCallDriver` to a device object found by `IoGetDeviceInterfaces` (0x1400049c8) with interface GUID `{20952871-af3d-4a8a-9e47-cb346507b95b}` (file offset 0xc678). The same GUID is in `qcpmic8380.sys`. It is not PoFx/PEP.
2. IOCTLs the flash driver sends to qcpmic:
   - 0x802f0fb0 SetTargetCurrent, 6 bytes `{u16 LED1, u16 LED2, u16 0}`.
   - 0x802f0fac SetSafetyTimer, 16 bytes `{u8 en[3], u32 ms[3]}` = enable LED1 and LED2, 1270 ms each.
   - 0x802f0fcc and 0x802f0fd0 (strobe source and strobe control), sent only for trigger type 0 and type 3.
   - 0x802f0fa0 (status read).
   - **0x80028fa8 = `CameraWhiteLEDFlashPMIC_Fire`** (0x140004d38), 28 bytes `{1, 8, 0, 0, on ? 10 : 0, 1, 4}`.
3. In `qcpmic8380.sys` the 0x8002xxxx group is the PMIC GPIO module (dispatcher ~0x140008000-0x1400089f4; per-PMIC table at 0x14003ba70 with GPIO counts by PMIC model, 12 for model 0x49 = PM8550, base list 0x8800, 0x8900, ...). 0x80028fa8 calls 0x140029f10, a "configure digital output": PMIC index 1, GPIO index 8 (base 0x9000 = Linux `gpio9`), mode output, CMOS, voltage source VIN0, low drive strength, pull-down (reg 0x42 = 4), and reg 0x44 = 0x80 (output high) when the source field is 10, 0x00 (low) when 0 (0x140029518). The DSDT `PMIC.PMCF` list maps index 1 to SID 1.
4. Trigger type 2 (Dell, from `CAMP_PCFG_QRD` word 0x03008110) in `DeviceConfigure` (0x140005b14): SetTargetCurrent(LED1 = 700, LED2 = 0) and SetSafetyTimer only. The switching is in `NotifySof` (0x140005900): with shutter type 1 (Dell), `Software_Flash_On` (= Fire(1), 0x140006dd0) at SOF count % 4 == 1 and `Software_Flash_Off` (= Fire(0), 0x140006e40) at % 4 == 3. So GPIO9 is high for two frames and low for two frames. With shutter type 0 it alternates every frame.
5. `qcpmic8380.sys` flash functions (table at 0x140039470) confirm the flash block is PM8550 at 0xee00 with the same register map as Linux `leds-qcom-flash` (timer 0x3e+n, itarget 0x42+n, ires 0x49, strobe 0x4a+n, chan_en 0x4e, module_en 0x46). The only functions that write 0xee46/0xee4e are IOCTLs 0x802f0fc8 and 0x802f0fb8, and no binary in the camera or chipset packages contains those codes.
6. Channel mapping (question 2): in `qcpmic8380.sys` LED index n is flash channel n+1, and for PM8550 the driver hard-codes ganging at 0x1400210fc (`0x0203` stored at flash-context+25): **LED1 = channels 1 + 4, LED2 = channels 2 + 3**, each channel programmed with half the requested current (0x1400273d8). So "LED1 = 700 mA" is 350 mA on channel 1 plus 350 mA on channel 4. This agrees with the Zenbook using channel 4.

What GPIO9 does electrically is not known. For trigger type 0 (the MTP reference board) Windows sets the channels to hardware strobe, level-triggered, active high (0x802f0fd0 `{LED1, hw=1, edge=0, pol=1}`), and then sets GPIO9 high once and leaves it high. So GPIO9 is an enable or gate, not a per-frame strobe line.

## Ranked hypotheses

### 1. GPIO9 gates the emitter and Linux never drove it (most likely)

- For: everything above. PM8550 gpio9 is unused in the Dell DT and in every upstream X1E laptop DT I fetched (CRD, T14s, Yoga Slim 7x, Romulus, XPS 13 9345, OmniBook X14, Zenbook A14, Thena); they use only gpio10/11 (CRD also gpio6). Linux software strobe on channels 1 to 4 gave no light and no fault, which fits a switch between the PMIC output and the LED, or a separate LED driver whose enable is GPIO9.
- Against: the Zenbook series lights its LED with software strobe on channel 4 and no GPIO, so on that board GPIO9 is not required. Dell would have to differ.
- Test 0 (sudo, no reboot, read-only): see what firmware left on the pin.
  `sudo grep -n gpio9 "/sys/kernel/debug/pinctrl/c42d000.spmi:pmic@1:gpio@8800/pinconf-pins"`
  An "out ... low, pull-down" state on an otherwise unused pin would support this.
- Test 1 (sudo, no reboot; `CONFIG_GPIO_SYSFS_LEGACY=y` is set). The chip `c42d000.spmi:pmic@1:gpio@8800` currently has base 757, so gpio9 is 765; recompute after a reboot.
  ```
  n=$(( $(cat "$(dirname "$(grep -l 'pmic@1:gpio@8800' /sys/class/gpio/gpiochip*/label)")/base") + 8 ))
  echo $n | sudo tee /sys/class/gpio/export
  echo out | sudo tee /sys/class/gpio/gpio$n/direction     # low
  echo 1 | sudo tee /sys/class/gpio/gpio$n/value           # high: capture now
  echo 0 | sudo tee /sys/class/gpio/gpio$n/value
  echo $n | sudo tee /sys/class/gpio/unexport
  ```
  Run three variants while streaming: (a) GPIO9 high alone; (b) GPIO9 high plus the existing software pulses on `ir:flash-4`, then `ir:flash-1`; (c) GPIO9 high plus torch brightness on those two.
- Risk: Windows drives this pin the same way whenever the IR camera streams, so the level itself is safe. Windows keeps it at 50% duty in two-frame bursts; hold it high for a second or two at a time, and do not look into the emitter at close range. The sysfs route keeps the firmware's voltage source, pull and strength. To copy Windows exactly use a DT pin state (reboot needed): `pins = "gpio9"; function = "normal"; power-source = <0>; bias-pull-down; output-low; qcom,drive-strength = <3>;` (3 = low) plus a `gpio-leds` node on `<&pm8550_gpios 9 GPIO_ACTIVE_HIGH>`.

### 2. PMIC channels 1 + 4 in hardware-strobe mode, gated by GPIO9

- For: Windows never sets chan_en/module_en and, for type 2, never writes the strobe-control registers, so if the PMIC channels carry the LED current they must be pre-armed (firmware or reset defaults) and triggered by a pin. Type 0 explicitly selects hardware strobe.
- Against: if the LED were directly on a PMIC channel, Linux software strobe should have lit it regardless of the strobe pin.
- Test (sudo, reboot without the emitter-test overlay so Linux has not rewritten the block): dump the firmware state of the flash block and of GPIO9. Lines are 9 bytes each, so this reads only 256 registers per command.
  ```
  sudo dd if=/sys/kernel/debug/regmap/0-01/registers bs=9 skip=$((0xee00)) count=256 2>/dev/null
  sudo dd if=/sys/kernel/debug/regmap/0-01/registers bs=9 skip=$((0x9000)) count=256 2>/dev/null
  ```
  Look at 0xee3e-0xee4e and 0xee67: strobe control with bit 2 set, or chan_en/module_en already set, means hardware strobe is pre-armed. Read-only, no risk. I have not confirmed the debugfs file name `0-01` on this kernel.
- If test 1 fails in all variants, the full Windows state needs a small test module or a patched `leds-qcom-flash`: itarget 27 (350 mA) on channels 1 and 4, ires bits clear, timers 0xfe, strobe control 0x05 on both, module_en, chan_en 0x09, then GPIO9 high. `leds-qcom-flash` already has this as `external_strobe_set`, but it is reachable only through a registered V4L2 flash subdevice, not sysfs.

### 3. The sensor's strobe output takes part (weak)

- For: a global-shutter sensor normally times the illumination pulse; a frame-level GPIO suggests finer timing happens elsewhere.
- Against: `strobeStartAddr` and `strobeWidthAddr` in the sensor module are empty (`notes-hm1092-fields.txt` lines 18-19), so Windows does not program a sensor strobe at run time. The 184 init writes are already replayed by the Linux driver.
- Test: none needed unless 1 and 2 fail.

### 4. False negative in the flash-mode test (unlikely to be the whole story)

- `capture.sh` keeps only the last of 15 frames. With 50 ms pulses every 100 ms or more and a 33 ms exposure, roughly one frame in three falls entirely in a gap, so the one correct channel had about a 1 in 3 chance of reading dark. The torch test does not have this problem and was also dark.
- "Values read back correctly" are the LED class's cached settings, not PMIC registers, and `flash_fault` cannot report an open LED.
- The `leds-qcom-flash` strobe sequence itself (budget, itarget, timer, module_en, chan_en) is correct against the Windows register usage. I did not compare the 7.3 source with master.
- Fix: report the maximum frame mean over all 15 frames.

### Ruled out: CCI timer / GPIO 109 path

`CameraIRLED_Trigger` (0x140006eb0) is called only when trigger type is 1 (0x140005a2c) and from the type 1 callback thread. Dell is type 2. CCI timer index 0 is GPIO 109, which is the sensor reset on this board, so it cannot be an emitter line. GPIO 90 is already high whenever the sensor is powered.

## Corrections to the summary and README

- 0x140004aa0 was "not identified": it is an IOCTL to `qcpmic8380.sys` (see 1 above), not PoFxPowerControl.
- SetSafetyTimer is 0x802f0fac. 0x80028fa8 is Fire, and Fire is a PMIC GPIO write, not a flash-block call.
- README: "The FLSH resource file is empty, so there is no GPIO involved" is wrong; the GPIO goes through the PMIC driver, not PEP.
- README: "The channel is unknown": Windows LED1 is channels 1 + 4 at half current each.
- Config payload order is `{shutter type, trigger type, CCI timer index}` (prints at 0x140005ad8-0x140005b10), stored at context +200/+204/+208.
- Trigger type 0 (not analysed before): hardware-strobe setup, then Fire(1). Any other type: software-strobe setup, then Fire(0).

## Question 3: PCFG and PRLD

- `CAMP_PCFG_QRD`: 0x306 is a flag word (bits 1 and 2 = sensor slots 1 and 2 present; bit 9 = privacy-shutter info present). Then two sets of four per-slot words: the first set is the default, the second (0, 0x130010, 0x3008110, 0) is used when the SoC id is 0x27b (0x140003208); it differs only in the colour camera's PHY (1 instead of 3). The 0xd200b3 / 0xd2009b words are "PLD Vertical/Horizontal Offset (mm)": 210 mm vertical, 179 mm (colour) and 155 mm (IR) horizontal, again in two sets. Nothing emitter-related.
- `CAMP_PRLD_QRD` (0x506, zeros) is the privacy LED configuration, tied to `PSTATE_SET 6 'CAM_PRLD'` in `CAMP_PERF_QRD`. Not the emitter.
- MTP vs QRD: the MTP IR words (0x1000110, 0x1100110) are trigger type 0, shutter type 0; QRD is trigger type 2, shutter type 1. MTP powers the IR sensor from PM8010 LDO4/LDO7 where Dell uses GPIO 90. `CAMP_RES` differs only in CCI pins.

## Question 4: public record

- Upstream master has no IR emitter, `pm8550_flash` user or PM8550 gpio9 user in any X1E laptop DT.
- The Zenbook A14 series (channel 4, software strobe, 700 mA, no GPIO) is the only prior work I found; the thread has no emitter failure reports and no mention of Dell. One web search for X1E IR-emitter reports returned nothing relevant.
