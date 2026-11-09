# wafel_recovery_setup

A [stroopwafel](https://github.com/StroopwafelCFW/stroopwafel) plugin that
reinstalls Wii U titles from a FAT32 SD card using the console's own factory
**Boot Recovery** mode. It can rebuild a console with an empty or corrupted
MLC, and even a blank SLC.

The recovery screen, with a progress bar, is shown on the **TV**. No Cafe OS
or PPC is needed. The install itself is MCP's own recovery update.

> **Warning:** this plugin can format the MLC and SLCCMPT (only after you
> confirm on screen).

## Variants

| File | Build | What it does |
|---|---|---|
| `5drsetup.ipx` | `make` | **Full setup.** Formats MLC/SLCCMPT if they have no valid file system (asks first), installs, resets the console to the initial setup, and fixes the region to match the installed Wii U Menu. |
| `5drinst.ipx` | `make INSTALLONLY=1` | **Install only.** Never formats anything, doesn't touch the initial-setup state or the region. Just installs the titles. |

Both:
- restore a missing `system.xml` and a missing or incomplete `cert.sys` on the
  SLC before the install;
- never delete tickets;
- never format the SLC (unless built with `SLCFORMAT=1`, see below);
- delete themselves from the SD after a successful install, so the next boot
  is normal.

## SD card layout

```
sd:/wiiu/ios_plugins/5drsetup.ipx      (or 5drinst.ipx)
sd:/wafel_install/
    <title 1>/   title.tmd  title.tik  title.cert  *.app  *.h3
    <title 2>/   ...
```

Each title is a folder in the usual installable format (`title.tmd`,
`title.tik`, `title.cert`, contents). Any set of titles works; it doesn't have
to be a complete system. The SD card must be FAT32.

## Usage

1. Copy the titles to `sd:/wafel_install/` and the plugin to
   `sd:/wiiu/ios_plugins/`.
2. Boot stroopwafel. The console enters Boot Recovery and shows the recovery
   screen on the TV.
3. **Format prompts** (full build only): if the MLC or SLCCMPT has no valid
   file system, the screen shows, for example:

   ```
   No valid wfs file system detected on /dev/mlc01.
   Format it? Everything on it will be erased.
   Press EJECT to continue. Hold POWER to cancel and power off.
   ```

   Press **EJECT** to format, or **hold POWER** to turn the console off without
   formatting. There is no timeout. A short POWER press is ignored. An MLC
   formatted by another console (different key) also shows up as having no
   valid file system.
4. The install runs with a progress bar. On success MCP prints "System update
   was successful" and powers the console off. The plugin has removed itself
   by then.
5. Power on normally. After the full build the console starts the initial
   setup (language, country, ...).

Everything the plugin does is also logged to `sd:/wafel_drsetup.log` and
printed on the recovery screen.

### LED

| LED | Meaning |
|---|---|
| Blue blinking | install running |
| Solid blue | success, plugin removed |
| Orange | no `wafel_install` folder on the SD; nothing was done |
| Red / red blinking | something failed; see the screen and `sd:/wafel_drsetup.log` |

### If it fails

The plugin only deletes itself after a full success. Otherwise it stays on the
SD and every boot goes back into recovery and tries again. To boot normally,
remove the `.ipx` from `sd:/wiiu/ios_plugins/`.

## Special setups

- **USB MLC:** works together with the `wafel_usb_mlc` plugin (`4usbmlc.ipx`)
  in a version with recovery support (commit "mcp_recovery compatibility" or
  later). It brings the USB drive up in time for recovery. Tested: format and
  full install to a USB flash drive, then a normal boot into the initial setup.
- **Blank SLC:** put all titles, including the IOS (OSv10,
  `000500101000400a`), into `wafel_install`. The SLC itself is normally
  formatted at boot by IOS-FS (SCFM). Where that doesn't happen (redNAND with
  SCFM disabled), build with `SLCFORMAT=1`. With redNAND, the stroopwafel
  core needs the redNAND fix for the SLC spare-area operations.
- **Screen log:** works together with `wafel_screen_log` (`01scrlog.ipx`); the
  log is then drawn in a box at the bottom of the recovery screen.

## Building

Needs devkitARM and the stroopwafel source tree (for the `wafel_core`
headers):

```
export DEVKITARM=/path/to/devkitpro/devkitARM
export STROOPWAFEL_ROOT=/path/to/stroopwafel
make                    # 5drsetup.ipx
make clean
make INSTALLONLY=1      # 5drinst.ipx
```

Or with Docker, as the CI does:

```
docker build . -t builder
docker run --rm -v $PWD:/project builder bash -c \
  "export STROOPWAFEL_ROOT=/project/stroopwafel && make"
```

Run `make clean` before switching between variants.

Build switches:

| Switch | Effect |
|---|---|
| `INSTALLONLY=1` | install-only variant (`5drinst.ipx`) |
| `SLCFORMAT=1` | allow formatting the SLC when it has no valid file system |
| `NOINSTALL=1` | test: show the recovery screen, change nothing, stay on the SD |
| `REGIONTEST=1` | test: set the region to USA before the install, so the region fix has to write it back |
| `DELTEST=1` | test: delete `sys_prod.xml`, `cert.sys`, `title.list` and `system.xml` first, to check they get recreated |

More options are in `source/config.h`. How it works and what was found while
writing it: [docs/technical.md](docs/technical.md).
