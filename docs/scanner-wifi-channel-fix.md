# Scanner Wi-Fi channel fix ("phone does not see the scanner")

## Symptom

A phone (or tablet) sees every Wi-Fi network **except** the scanner's
access point, while all other devices connect fine. Typical after buying
a new phone. Nothing is broken - it is a regulatory-table bug on the
scanner side.

## Cause (verified on a real device)

The scanner creates its own 5 GHz access point and picks the channel
from a country table stored on the device
(`/shining3d/country_code.db`, country in `/backup/country_code`).
The vendor rows for KZ/RU/CN/US all contain `149,153,157,161` - the
5.725–5.850 GHz band. But client devices with the local regulatory
domain (e.g. any phone sold for Kazakhstan) are **not allowed to scan
that band at all**, so they physically never look there and the AP is
"invisible". The scanner happily chose channel 153 (the cleanest one).
The intersection that is both AP-usable and visible to locally sold
phones is the non-DFS block **36,40,44,48** (80 MHz wide - no
throughput loss; the scanner uses 80 MHz in either band).

## One-click fix

Prerequisites: scanner on and connected to the PC (its Wi-Fi or USB,
the PC must reach `192.168.42.3`), Windows optional feature
**OpenSSH Client** installed (default on Windows 11).

Either of the two identical tools (same procedure, same safeguards):

```
ExStarModManager.exe --scanner-wifi status    # read-only check
ExStarModManager.exe --scanner-wifi fix       # one-click repair
ExStarModManager.exe --scanner-wifi rollback  # back to factory state

tools\scanner-wifi-fix.ps1 -Mode status|fix|rollback   (standalone)
```

What `fix` does, in order:

1. Reads the device's country code and its current channel row
   (read-only status).
2. Saves the factory table to
   `%LOCALAPPDATA%\ExStarHubMods\scanner\country_code.db.factory`
   (first run only; it refuses to patch without a verified backup).
3. Rewrites **one line** of the device's regulatory table: the row of
   the device's own country, keeping only channels ≤ 64 (falls back to
   `36,40,44,48` if nothing would remain). No vendor binaries are
   touched; nothing else on flash is written; `/tmp` usage is RAM only.
4. Restarts the AP through the vendor's own init script
   (`S99softap.sh`, detached so it outlives the SSH session that dies
   with the AP; ~10 s blip, PC reconnects by itself).
5. Reconnects and verifies the AP is now on a visible channel.

Login: the vendor's factory-default SSH credentials (the same ones
embedded in the vendor's own PC plugin); override with
`--scanner-pass` / `-Password`. The password is passed via a temporary
askpass helper inside the user temp folder and deleted immediately - it
never appears on a command line.

## After a scanner firmware update

A firmware update may reflash the regulatory table and bring the
optimistic `149..161` row back. If the phone stops seeing the AP again:
run `--scanner-wifi status`, then `fix` again (the tool is idempotent;
keep the factory backup file, refresh it manually if you want a backup
of the new firmware's table).

## Manual last resort (SSH)

```
sed -i 's|^KZ:.*|KZ:36,40,44,48|' /shining3d/country_code.db && sync
/etc/init.d/S99softap.sh stop; sleep 2; /etc/init.d/S99softap.sh start &
```

(replace `KZ` with the code printed by `cat /backup/country_code`)

## Note on regulation

The fix only **narrows** the scanner's channels to the block that the
local regulation allows client devices to use; it does not unlock any
band and does not change transmit power settings or other devices'
behaviour.
