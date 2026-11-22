# Scanner fan control (`--scanner-fan`)

The scanner's fan is driven by a curve hardcoded inside the vendor app
(`app_tx2`): it audibly steps up under load and idles around 40 % duty.
This tool replaces that behaviour with a smooth control loop,
**memory-only**: nothing on the scanner's flash is modified outside
`/tmp` (RAM), and a scanner reboot always brings the factory curve
back.

```
ExStarModManager.exe --scanner-fan status                # read-only
ExStarModManager.exe --scanner-fan apply                 # start control loop
ExStarModManager.exe --scanner-fan stop                  # stop the loop
ExStarModManager.exe --scanner-fan restore               # write back the RAM snapshot
ExStarModManager.exe --scanner-fan probe                 # diagnostics report

    --fan-mode shell|memhook   (default: shell)
    --fan-target 45.0          target temperature, Celsius
    --fan-min 80  --fan-max 255  duty clamp
    --fan-gain 8               loop aggressiveness (per-second scale)
    --fan-poll 2000            shell loop interval, ms
    --scanner-ip 192.168.42.3  (Wi-Fi) or 192.168.40.3 (USB)
```

Requires: scanner on and reachable (its own Wi-Fi network or USB),
Windows optional feature **OpenSSH Client**. The login is the vendor's
factory-default account used by the vendor's own PC software.

## shell mode (default)

A small PID loop runs as a shell script in `/tmp`: every 2 s it reads
`/sys/class/thermal/thermal_zone0/temp`, adjusts the duty toward
`--fan-target` and writes `/dev/hwmon/pwm1`. Works on every firmware
build; the duty file stays authoritative between two ticks, which is
exactly how the community "hold" tools work. `stop` terminates the
loop; reboot also stops it. The loop's current duty is mirrored to
`/tmp/exstar_fan.state`.

Note: `app_tx2`'s own curve may re-write the PWM node between ticks
(last writer wins). The live test determines the effective behaviour;
if the vendor writes turn out to dominate, the memhook mode below
replaces the curve itself instead of racing it.

## memhook mode (advanced, strictly gated)

The in-process control loop designed for the factory EXStar 2.1.0-10
`app_tx2`: one vendor instruction is replaced by a branch into a
25-word A64 loop placed in a code cave; the loop reads the app's own
temperature variable and writes the app's own PWM variable, then
returns so the app re-executes the untouched instruction behind the
hook. Applied by writing to `/proc/<pid>/mem` of the running process
only - the binary on disk is never touched (the community tool patches
the file itself; we don't).

Before anything is written the tool verifies the whole profile: file
size and seven factory anchor words, the cave lies inside a mapped
PT_LOAD, that mapping is writable, the hook site still holds the
factory instruction. Every step that fails prints the reason and
aborts with zero writes; after writing it reads everything back and
auto-restores on any mismatch. The pre-patch RAM (hook + cave + state
word) is saved to
`%LOCALAPPDATA%\ExStarHubMods\scanner\fan-snap-<ip>.txt` -
`--scanner-fan restore` replays it; a reboot also reverts.

If the profile does not match your firmware, the tool says so and
points back to shell mode. `--scanner-fan probe` produces the report
(`fan-snap-<ip>-probe.txt`) from which a profile for a new build is
derived: ELF program headers, the anchor window, candidate hook words,
segment permissions.

## Safety

- apply while idle is recommended (the patch survives scanning, but a
  device reboot during a scan is how you test rollback, not how you
  want to finish a scan)
- `FANBLOCKED` / stalled RPM with a raised duty = physical obstruction
  - stop the loop and check the fan
- the tool never changes transmit power, regulatory tables or any
  vendor binary
