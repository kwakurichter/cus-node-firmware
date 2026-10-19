# ArduSwarm TWR anchor mode

This firmware adds mode 5, an ArduSwarm four-message TWR responder compatible with
the `codex/ranging-anchor-twr` AP_Ranging implementation and wire protocol v1.
It does not implement an ArduSwarm TDoA mode. Changes and documentation are
AI-assisted. USB enumeration with the repaired USB driver has been confirmed
on user hardware. One-tag/one-anchor ranging and the MODE/RANGING LEDs have
also been exercised on hardware. Multi-anchor recovery, endpoint timestamp
agreement, contention and navigation acceptance remain open.

## Build

```sh
git submodule update --init
make clean
make -j4 ARDUSWARM_ONLY=1
```

The default dedicated image retains mode numbers but marks modes 0–4 unavailable.
Select mode 5 explicitly. This profile leaves SRAM for the 1600-byte main-task
stack and 128-byte USB receive queue. With GCC 15.2.1, a previous build enabling all legacy
algorithms (`ARDUSWARM_ONLY=0`) exceeded the 16 KiB SRAM limit by 592 bytes;
that profile requires further memory reduction before it can be used. Always
clean when changing ARDUSWARM_ONLY or BOOTLOAD, since Make does not track flag
changes. Neither command flashes the device.

## Configure over USB serial

At the main console menu:

1. Press a digit `1`–`8` to store the physical anchor ID (or use the existing `i`
   menu). ID 0 or IDs above 8 disable the ArduSwarm radio algorithm at boot.
2. Press `w` to select ArduSwarm anchor mode 5.
3. Send these lines, one at a time:

```text
@net 1
@channel 2
@ant 16384
@reply 3000
@timeout 30
```

4. Reboot the node, then send `@status` for active settings and the most recent
   tag/range/session/sequence plus range/failure counters. Status now prints the
   stored settings separately from active runtime values; zero runtime fields
   while mode 5 is inactive do not imply erased EEPROM. Saved command values
   are echoed immediately; they do not change the active radio until reboot.

`@net` accepts 1–32767, channel 1–5 or 7, antenna delay 0–32767 device ticks,
reply delay 1500–20000 us, timeout 5–200 ms. At boot the timeout must also cover
four reply delays plus 3 ms margin. All radios need compatible timing and the
same network/RF profile. The profile overrides legacy radio-mode/power settings
only for mode 5: short-data fast-accuracy, 64 MHz PRF preamble code 9, smart power,
and hardware TX/RX antenna delay. Antenna delay requires per-device calibration;
16384 is only a starting value. A node does not need the surveyed position:
store that on every vehicle with the matching anchor ID.

Configuration fields are appended to the existing EEPROM TLV format; old IDs
are preserved. Missing new fields use the defaults shown above. The 100-byte
configuration area has checked appends and scalar/list lengths. Writes verify
readback/checksum; storage is not power-loss atomic. A corrupt image falls back
to legacy defaults (address 0), which cannot start ArduSwarm anchor ranging until
configured again. The new mode does not process legacy over-the-air LPP changes.

## LED indicators

After the startup lamp test, MODE stays on when a valid ArduSwarm anchor
configuration has been applied and the receiver has been started. It is not a
heartbeat or proof of a working link. RANGING flashes for 50 ms whenever the
anchor computes a valid range from a received FINAL; confirm the tag also
receives REPORT using its range logs. At high update rates it may look steady.
SYNC is unused by this TWR mode. The radio's hardware RX/TX indicators remain
enabled separately. An unavailable legacy mode or invalid anchor configuration
does not light MODE in the dedicated ArduSwarm build.

Use a serial terminal for this console. Disconnect QGroundControl's link to
the node before opening the terminal; the node console does not speak MAVLink. Disable automatic reconnection to this USB device as well.

The console now discards complete MAVLink 1/2 packets, including MAVLink 2
signatures, before any menu or setting handler sees their bytes. Previously,
a GCS heartbeat with sequence 100 contained ASCII `d`, the legacy EEPROM-reset
command; other sequence/payload bytes could select an ID, change mode, or enter
production tests. This could change saved configuration during operation and
appear as lost settings after reboot. The filter does not turn the node into a
MAVLink device. After an incomplete packet, one idle second permits text input
again. Keep QGroundControl disconnected from the node for console use.

Settings are written immediately; `@...` commands report `Saved ...; reboot
required` after readback validation. A full power cycle should preserve them.
If defaults return with GCS traffic excluded, capture startup CONFIG messages
before reconfiguring: a checksum or magic failure causes default restoration,
while an I2C read failure is reported separately. The user confirmed a configure/power-cycle/readback test on the filter/status
image with QGroundControl disconnected. Repeat this check for newly configured
anchors before the multi-anchor bench test.

## Protocol and validation

`inc/twr_protocol.h` is identical to AP_Ranging's header. Header bytes are:
magic 52 A7, version 1, message type, network uint16, source role/ID, destination
role/ID, sequence uint16, initiator session uint32. All multi-byte fields are
little-endian. Tags have role 1 and IDs 1–255; anchors role 2 and IDs 1–8.
POLL/RESPONSE are 16 bytes, FINAL 31, REPORT 32, excluding the radio FCS.
FINAL carries three five-byte initiator timestamps; REPORT carries status 0 and
three five-byte responder timestamps. The responder computes after FINAL and
the tag computes after REPORT. There is one active transaction per anchor.
Deadline, duplicate, source, session, sequence, network, and timestamp checks
prevent unrelated packets from refreshing ranges. Network ID is not security
authentication. Stock Bitcraze packets are incompatible.

Run the matching AP_Ranging portable runner with `--node` pointing here to test
both copies, two-sided ranges, frame loss, wraparound, and EEPROM persistence and
bounds under AddressSanitizer/UBSan. Existing EEPROM Unity tests run with
`rake unit`. The ARM USB packet-memory regression runs with
`python3 test/host/test_usb_pma.py` (arm-none-eabi-gcc, Unicorn and pyelftools
required); it checks the optimized HAL code for 16-bit PMA accesses and buffer
bounds. No Docker is needed for those host checks.

Before deployment, verify the actual delayed FINAL timestamp, both endpoint
ranges, calibration sign and scale, EEPROM save/reboot on real hardware, stack
high-water use, NLOS/dropout behavior, and contention with one/two/four tags.
A successful firmware build alone does not establish navigation readiness.


## Optional radio diagnostic requests

This AI-assisted update supports the extended bench POLL/REPORT format documented
in ArduPilot's AP_Ranging README (Per-anchor radio diagnostics). Standard polls
retain the original report. No settings migration or manual node diagnostic
switch is needed. `@status` prints `ArduSwarm radio diagnostics supported=1`.
The tag requests captures per anchor with RNG_RF_DIAG; 128 selects anchor 8.
Only upgraded nodes can answer diagnostic requests. Captured POLL/FINAL receive
power, first-path power, timestamp correction and uncorrected timestamps return
in the matched report for logging on the tag. The normal corrected range remains
unchanged in algorithm; additional SPI work and packet airtime need bench testing.
