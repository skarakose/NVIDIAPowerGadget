# NVIDIA Power Gadget

macOS menu bar monitor and fan-curve controller for NVIDIA GPUs supported by
the `NVRM`/NullMoth driver path.

The menu bar item shows the GPU temperature beside the fan RPM column,
plus any extra metrics you enable. Click
it to open the fan curve popup. Use “+ Add point” to create temperature/speed
points; the curve is saved as JSON in 
`/Library/Application Support/NVIDIAPowerGadget/curve.json` 
and loaded on the next launch. Each point controls 
the minimum fan duty from that temperature upward.

## Build

Build the app bundle from this directory:

```sh
sh build-app.sh
```

The result is `NVIDIAPowerGadget.app`. Copy it to
`/Applications` and launch it like a normal menu bar application:

```sh
cp -R NVIDIAPowerGadget.app /Applications/
open /Applications/NVIDIAPowerGadget.app
```

It is an agent-style menu bar app, so it does not have a Dock icon. Add it to
macOS startup from **System Settings → General → Login Items → Open at Login**.

The application requires the `NVRM` driver and the permissions needed by the
existing `nvfan.c` backend. Fan writes are restricted to the backend's safe
30–100% range.

## Fan control since driver 2.0 (root daemon)

Driver 2.0 checks the real uid for privileged RM operations, so fan writes
need root. Reads still work unprivileged. The setup is split:

- `NVIDIAPowerGadget.app` (runs as you): shows temperature/RPM and writes
  the curve to `/Library/Application Support/NVIDIAPowerGadget/curve.json`
  (created `root:admin` 775 by the installer, so writing needs an admin
  account; non-admin users get a read-only monitor).
- `fanctld` (runs as root via LaunchDaemon): every 2 s reads the
  temperature, reads the shared curve file, and applies the fan
  speed when the step changes. Empty curve = hands off, changes nothing.

Install:

```sh
sh build-app.sh
cp -R NVIDIAPowerGadget.app /Applications/
sudo sh install-daemon.sh
```

Logs: `/var/log/nvidiapowergadget-fanctl.log`. Removal:

```sh
sudo launchctl bootout system/com.nvidiapowergadget.fanctl 2>/dev/null || true
sudo rm /Library/LaunchDaemons/com.nvidiapowergadget.fanctl.plist
sudo rm -rf /usr/local/libexec/nvidiapowergadget \
  "/Library/Application Support/NVIDIAPowerGadget" \
  /var/log/nvidiapowergadget-fanctl.log
sudo rm -rf /Applications/NVIDIAPowerGadget.app
```

Also remove the app from **System Settings → General → Login Items** if
added there. Note: deleting the `Application Support` folder also deletes
your fan curve (`curve.json`) and monitor selection (`display.json`).

## Monitor

Click the ⚙ button in the fan curve popup to open the Monitor settings.
Each checkbox adds a readout to the menu bar item:

- VRAM usage (%), VRAM usage (MB)
- Graphic engine (%), Framebuffer (%), NVENC (%), NVDEC (%)
- NVENC / NVDEC (show when active): only shown while the engine is in use
- GPU clock (MHz), Memory clock (MHz)

The selection is saved as JSON in
`/Library/Application Support/NVIDIAPowerGadget/display.json`
and loaded on the next launch. All boxes are off by default.

## Caution
- The NVIDIAPowerGadget app has only been tested on a dual-fan Gigabyte RTX 3060 Ti
  with NullMoth drivers based on NVIDIA OGKM 615.78.08.
  Compatibility with other hardware is not guaranteed.

## License

GPL-3.0-or-later, see `LICENSE`.
Everyone may use this program, but anyone who modifies and distributes it
must share the modified source under the same license.
