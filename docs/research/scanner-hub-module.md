# Scanner hub module — design research (planned, 2026-11-19)

Target: a **Scanner tab in ExStarModManager** grouping everything that
talks to the Einstar Rockit device itself, as opposed to the PC-side app.
Planned scope: fan control (vendor is loud), home-network (STA) assistant,
live device dashboard, log collector. Status: **research complete, no code
yet**. This document is the standing entry point; per-module notes link
out as features land.

## Where control actually lives

The fan (and most device behavior) runs **on the scanner** — an RK3568
Linux box (Buildroot) whose main process `app_tx2` implements the vendor
fan curve. The PC is a client: `TX2Plugin` carries a control channel
("RW" commands) over USB/Wi-Fi; the device also exposes plain hardware
nodes over SSH (`/dev/hwmon/pwm1` duty, `/dev/hwmon/fan1_input` RPM,
`/sys/class/thermal/thermal_zone0/temp`). The PC does not fight the
device over fan control — nothing on the PC writes duty.

## Vendor command surface (verified in `TX2Plugin.2.4.1.dll`, 2.1.0-10)

Read/telemetry (safe, stateless):
- state bits via `readDeviceState`: `WIFICHANNEL`, `WIFIQUALITY`,
  `FANBLOCKED`, `MIPIERROR`, `BATTERYGAUGESTATUS`, `FrameIDErrorCount`,
  `FirmwareUpgradeProgress`;
- telemetry keys: `ZCPUTEMP` (SoC), `ZGIRDERTEMP` (projector),
  `ZBATTERYCUR` / `ZBATTERYVTG` (battery current/voltage),
  `RWFanActualSpeed`, `threshold`, `readKeyStatus` (button words
  `STATE/BUTTON2/BUTTON3`), init flags, `GetWiFiAdapterInfo`,
  max/min trigger period per USB vs Wi-Fi channel (Wi-Fi can cap frame
  rate — worth documenting);
- connectivity: `readDeviceConnectStatusByNet`, device channel/status.
- The hub UI already receives `temp`/`batteryPower`/`bCharge`/disk info
  in its device model but shows little of it — a dashboard is mostly a
  display problem, not a new-data problem.

Control (device settings via vendor commands — config level, reversible):
- fan: `RWFanSpeed` (duty), `threshold` (+ actual-speed read-back,
  `FANBLOCKED` bit) — read-only or coarse duty control without touching
  the device binary at all;
- indicator LEDs (`RWLEDControlType` incl. RGB modes,
  `writeScanIndication`, `writeBatteryIndication`), buzzer
  (`RWTrumpet` named tones, `RWtrumpetVolmne` volume);
- network: `RWAPControl(isSwitch)` (AP on/off), `APReboot` (clean
  device reboot), `SelectWIFIChannel`, `RWWiredRndisIP`/`writeWiredIP`
  (USB gadget IP), `SEGIP1..SEGIP4` (adapter IP segments);
- IMU: `RWIMUSwitch` — the plugin currently sends `isSwitch=0` at
  connect, i.e. the onboard ICM-42680 gyro stream is disabled by default.

Factory/service channels — **out of policy, never use**:
`onePageIO` (raw device-memory writes), bulk "custom message" test
channels, `RWSerial` (device identity), country-code writes, and any
on-disk patching of `app_tx2` (memory-only patches at runtime are the
accepted pattern; reboot is the rollback).

## Feature matrix (agreed shape: everything under one Scanner tab)

Tier 1 (first iteration):
1. **Fan** — status card (duty/RPM/CPU+projector temp live) plus
   in-memory pseudo-PID patch of `app_tx2` (reboot restores factory);
   version gate before applying; refuse while scanning; `FANBLOCKED`
   alert. Recipe (anchors/blob) pending from collaborator.
2. **Dashboard** — temperature / RPM / battery (V, I, %) / Wi-Fi
   channel+quality / free disk / firmware version. Sources: vendor
   telemetry already on the PC + SSH reads as fallback.
3. **Home-network assistant** — mode/IP/signal status; STA configure
   (writing only the scanner's own config files over SSH, explicit
   consent; device reverts to AP on reboot by design); documented
   router option that keeps the scanner reachable at the address the
   hub expects (vendor's own managed-network plan — the stock hub
   hardcodes the scanner AP address, plain STA + home router is NOT
   visible to it; see the Wi-Fi research artifact).
4. **Log collector** — pull the scanner-side log bundles + dmesg + a
   hwmon snapshot into one PC-side zip; strip device identity before
   anything leaves the machine.

Tier 2 (needs one more research pass): scanner-button remap to app
actions (button events already reach the PC; collaborator proved the
channel is usable); indicator-LED themes; AP utilities (channel
picker, AP-off, reboot); completion/overheat buzzer tones.

Tier 3 (research only): enabling the IMU stream and feeding the
vendor's SLAM fusion inputs (scan-jump/anti-flip project — separate
epic); battery health curves; vendor service workmodes (ROI /
LaserCalibrate) — deliberately untouched.

## Constraints carried from the project policy

- Injection-only on the PC; on the device: vendor commands + read-only
  SSH; device-disk writes only for the scanner's own configuration
  (STA), behind an explicit consent dialog; never for binaries.
- Version gate + byte snapshot before any RAM patch; reboot = rollback.
- No device identity (serial), SSIDs or credentials in logs or UI.
- Elevation/consent/logging live in the manager (where they already
  exist); the hub-side surface stays a read-mostly card if we add one.

## Live verification checklist (run when a scanner is next connected)

1. Settle which device scripts/config are stock vs modified by
   timestamps (`ls -l --time-style=full-iso /etc/init.d/`).
2. Confirm `app_tx2` build hash and the fan-control hook site state
   (maps to patch/original per the collaborator's status recipe).
3. Enumerate `/dev/hwmon` + thermal zone names on the unit.
4. Check which telemetry the stock UI already displays on TX2 (battery
   dialog exists) — decides dashboard scope.
5. Capture hub→scanner ports during a Wi-Fi session (netstat) for the
   network-bridge option.
6. Measure +/- button event latency end-to-end (tier 2).
