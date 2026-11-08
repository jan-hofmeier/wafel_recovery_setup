# wafel_recovery_setup (`5drsetup.ipx`)

Boots IOS-MCP straight into its factory **Boot Recovery** mode and reinstalls
titles from a **FAT32** SD card, reusing MCP's own recovery reinstall routine
`MCP_DR_SystemUpdateFromMedia` (0x0510eaa8).

Unlike [`wafel_setup_mlc`](../../wafel_setup_mlc), the MCP **recovery screen is
drawn on the TV**: it is rendered on the ARM through the BSP `DISPLAY` / `DC_INIT`
controller, so no PPC / Cafe OS is needed (see
[../docs/mcp_factory_provisioning.md](../docs/mcp_factory_provisioning.md),
"Display path"). The reinstall itself is MCP's staged update engine with
rollback, not one-title-at-a-time `MCP_Install`.

> **Tested on hardware (internal MLC, 2026-11-08): full system reinstall succeeds, recovery screen with progress bar on the TV, initial setup starts afterwards. USB MLC not supported yet. It can format the MLC.** Read the
> risks below before running. Keep a full NAND/MLC backup.

## Two versions

(Renamed 2026-11-08: directory `wafel_recovery_update/` → `wafel_recovery_setup/`,
`5recupd.ipx` → `5drsetup.ipx`, `5recins.ipx` → `5drinst.ipx`, SD log
`wafel_recupd.log` → `wafel_drsetup.log`, message prefix `wafel_recupd:` →
`drsetup:`. "dr" is MCP's own name for Boot Recovery: `MCP_DR_…`,
`/dev/mcp_recovery`. Hardware test notes below that quote older log lines still
show the old prefix.)

Like `wafel_setup_mlc` (`main` vs. its `just_install` branch) there are two
builds from the same source:

| ipx | Build | What it does |
|---|---|---|
| `5drsetup.ipx` | `make` | **Full:** formats SLCCMPT/MLC if they have no valid file system, deletes `cafe.xml` (initial setup on the next boot), installs, fixes the region in `sys_prod.xml` |
| `5drinst.ipx` | `make INSTALLONLY=1` | **Install only:** no format anywhere, `cafe.xml` and region untouched; just the reinstall. A volume without a valid file system fails at the mount like a missing MLC. Deletes itself (`5drinst.ipx`) after success |

Formats, per build (each blocked `bl MCP_FormatDevice` is replaced by a no-op
returning 0; the ramdisk format is never blocked):

| Build | SLC (`0x0510f59e` resume, `0x0510ecd6` DR update) | MLC, SLCCMPT |
|---|---|---|
| `make` | **blocked** | conditional (only without a valid file system) |
| `make INSTALLONLY=1` | **blocked** | blocked |
| add `SLCFORMAT=1` | conditional, for rebuilding a blank SLC | as above |

**Both builds repair the SLC before the install** (`slc_prepare`, see
`docs/slc_minimum_for_recovery.md` in the IOSU notes):
- `system.xml` missing → MCP's own `MCP_WriteDefaultSystemConfig(1)`
  (`0x050049c0`, Thumb), which stock MCP only calls after a full SLC wipe.
- `cert.sys` missing or without all of `CA00000003`, `CP0000000b`,
  `XS0000000c` → the first `title.cert` found in the package (every WUP's holds
  exactly these three) is merged with ES's own `esMergeCertSys` (`0x05050448`,
  ARM; fills an empty store or adds the missing certs via `/tmp/cert.sys` +
  rename). If `cert.sys` doesn't exist at all it is written directly.
- `sys_prod.xml` needs nothing: MCP rebuilds it from the SEEPROM.

**Rebuilding a blank SLC** (hardware-tested on redNAND, 2026-11-09): put the
MLC titles **and OSv10** (`000500101000400a`) into `wafel_install`, boot
without an isfshax plugin and use `make SLCFORMAT=1` where SCFM doesn't
format the SLC itself (redNAND with SCFM disabled). The core needs the redNAND
fix for the SLC spare-area ops (stroopwafel `rednand.c`,
`rednand_slc_spare_read` / `rednand_slc_mark_bad`); without it the ISFS
format jumps to 0. Result: SLC formatted, `system.xml` and `cert.sys`
restored by the plugin, `sys_prod.xml` from SEEPROM, all titles incl. OSv10
installed; the next boot loads the IOS from the new SLC and starts the initial
setup. Details in the IOSU notes, `docs/slc_minimum_for_recovery.md`.

Test switch `DELTEST=1`: deletes `sys_prod.xml`, `cert.sys`, `title.list`
and `system.xml` (with flush and check) at the start of the install thread.

Both share everything else (recovery entry, 1080p screen, success check,
self-delete, messages). Run `make clean` between builds; `make clean` removes
both `.ipx`. Hardware 2026-11-08: `5drinst.ipx` on the internal MLC — six
formats skipped (only the ramdisk is formatted, as on every boot), install
successful, no region step, plugin removed.

## What it does

1. `kern_main` patches MCP so that on the next boot it enters Boot Recovery mode
   on a retail console, the recovery thread starts, and the recovery reinstall
   routine installs from a plain folder instead of an ATFS "UP" volume.
2. The recovery thread mounts the SLC/MLC/SLCCMPT (formatting only what is
   unformatted), initialises ES/install, and draws the recovery screen on the TV.
3. An install thread then mounts the FAT SD at `/vol/sdcard` and, if
   `/vol/sdcard/wafel_install` exists, calls `MCP_DR_SystemUpdateFromMedia` on it.
   Before that it deletes `/vol/system_slc/proc/prefs/cafe.xml`, so the next
   boot runs the initial setup (see "Initial setup").
   MCP prints "Updating from …", a progress bar ("Updating system, please
   wait...") and the success/failure line on the TV.
4. After a successful install, just before MCP reloads `sys_prod.xml`, the
   plugin sets `product_area`/`game_region` to the region of the Wii U Menu the
   install made the coldboot title (see "Region").
5. On success the plugin deletes itself from the SD so the next boot is normal.
   MCP powers the console off right after the success line.

## SD layout

A normal FAT32 SD card:

```
sd:/wiiu/ios_plugins/5drsetup.ipx      <- this plugin
sd:/wafel_install/                     <- package root (mounted at /vol/sdcard/wafel_install)
   <title 1>/  title.tmd title.tik title.cert *.app *.h3
   <title 2>/  ...
```

Same per-title WUP layout as `wafel_setup_mlc`. No `app.xml` / `update.inf` is
needed: on a non-ATFS volume the engine enumerates the subdirectories directly.

## Compile-time options

Set in `source/config.h`.

| Define | Default | Effect |
|---|---|---|
| `DRSETUP_FORCE_FORMAT` | off | Keep the factory forced format of SLCCMPT and the MLC. Off = format only when the volume is unformatted / the wrong file system (like `wafel_setup_mlc`). |
| `DRSETUP_REQUIRE_FULL_SYSTEM` | off | Keep the factory check that the package is a complete system (OS + boot titles). Off (default) installs any set of WUPs. |
| `DRSETUP_KEEP_NORMAL_LEVEL` | off | Leave the security level alone. By default (`DRSETUP_FORCE_TEST_LEVEL`) MCP's `IOS_GetSecurityLevel` is patched to return Test; without it a reinstall of titles already installed at the same or a newer version is skipped (see below). |

Tickets are **never** deleted, in any build.

### Why the security level is forced

On retail, `IOS_SetSecurityLevel` is a **no-op**: the kernel implementation
(`0x081299e8`) only writes the level when `KernelCheckDebugMode()==0` (the OTP
debug fuse), so the recovery thread's own `IOS_SetSecurityLevel(Test)` does
nothing and the level stays **Normal**. At Normal the update engine
(`FUN_0502ffc4`) does `if (IOS_GetSecurityLevel()==Normal && package_version <=
installed_version) skip`, i.e. it **skips any title already installed at an equal
or newer version** — which would silently no-op a reinstall. So instead of
fighting `SetSecurityLevel`, the plugin patches MCP's `IOS_GetSecurityLevel`
thunk (`0x05059090` → `movs r0,#0xa; bx lr`) to report Test. This is scoped to
MCP only (not the kernel, ES, crypto or FS).

## LED codes

| LED | Meaning |
|---|---|
| Blue blinking | install thread running / install in progress |
| Solid blue | success (plugin deleted itself) |
| Orange | no `wafel_install` folder on SD — nothing done, recovery menu stays up |
| Red / red blinking | FSA/open failure or the update failed (see `/vol/sdcard/wafel_drsetup.log` and the on-screen line) |

## Build

```
STROOPWAFEL_ROOT=/home/jan/wiiu/stroopwafel DEVKITARM=/opt/devkitpro/devkitARM make
```

Produces `5drsetup.ipx`. Copy it to `sd:/wiiu/ios_plugins/`.

Test variants (run `make clean` before switching builds):

- `make NOINSTALL=1`: boots into recovery and shows the screen, but changes
  nothing (stops before the `cafe.xml` delete and the reinstall) (orange LED) and prints
  "TEST BUILD (NOINSTALL)" on the recovery screen. It does **not** delete itself,
  so every boot goes back into recovery.
- `make REGIONTEST=1`: sets sys_prod's region to 2 (USA) before the install, so
  the region fix afterwards has to write it back. Otherwise a normal run.

## Source layout

| File | Contents |
|---|---|
| `source/main.c` | `kern_main`: recovery entry, panic, PPC, security level, boot-title check, ticket skip, format rules |
| `source/display.c` | recovery screen: TV surface, `MCP_InitMenu` redirect, 1080p, screen-log bottom box, `MCP_ShowMenu` hook that starts the install thread |
| `source/install.c` | install thread: SD, SLC preparation, the update, success check, self-delete |
| `source/slc.c` | `cafe.xml` delete, `system.xml`/`cert.sys` repair, region fix + sys_prod hook, DELTEST |
| `source/log.c` | FSA handle (`MCP_fsaInit(NULL)`), SD log, messages on the screen |
| `source/mcp.h` | MCP addresses and function pointers |
| `source/patch16.h` | halfword patch helper (2-byte Thumb patches; see `docs/stroopwafel_plugin_api.md`) |

## How it works (patch map)

All addresses are IOSU 5.5.x retail (`fw.elf` 00050010-1000400A). Patches are
applied unconditionally: stroopwafel only runs on 5.5.x, where these modules
don't change between releases. (Earlier versions compared each site with its
original bytes and skipped the patch on a mismatch; removed 2026-11-09, since
a silently skipped patch is worse than a crash.)

| VA | Patch | Purpose |
|---|---|---|
| 0x08129ce0 | `bx lr` | no kernel panic |
| 0x0501fc98 | `b 0x0501fdec` | `MCP_Startup`: force the Boot Recovery branch (security level Test, PM mode 0x40000) |
| 0x0510ea1c | `movs r0,#0; nop` | `MCP_StartDRThread`: start the recovery thread on retail |
| 0x050340ee | `movs r0,#0; nop` | never start the PPC |
| 0x0510ec38 / 0x0510ed96 / 0x0510ecd4 | `movs r2,#0` | `MCP_DR_SystemUpdateFromMedia`: SLCCMPT / MLC / SLC formats become conditional (omitted with `DRSETUP_FORCE_FORMAT`) |
| 0x0510ef48 | `b 0x0510f0c0` | skip ticket deletion, go to the progress thread + `MCP_SystemUpdate` |
| 0x0510eaee | `movs r0,#0; nop` | skip the full-system-package check (omitted with `DRSETUP_REQUIRE_FULL_SYSTEM`) |
| 0x05059090 | `movs r0,#0xa; bx lr` | MCP `IOS_GetSecurityLevel` returns Test, so the update engine doesn't skip same-version titles (omitted with `DRSETUP_KEEP_NORMAL_LEVEL`) |
| bl @0x0510f88a | `trampoline_t_blreplace` | `bl MCP_ShowMenu` in the recovery thread: run it, log the menu surface, then start the install thread |
| 0x051003c4 | `movs r3,#0` (was `#1`) | `MCP_ShowMenu`: BSP `DC_INIT` id 0 (1080p) instead of id 1 (480p), see "Display" |
| bl @0x0510f04c | `trampoline_t_blreplace` | `bl MCP_LoadSysProdXML` after a successful `MCP_SystemUpdate` (step 0x13): fix the region in `sys_prod.xml`, then run the original load (see "Region") |
| bl @0x051003fe | `trampoline_t_blreplace` | `bl MCP_InitMenu`: replace DC_CONFIG's framebuffer with the surface the TV scans out (read in `kern_main`), see "Display" |

The install thread calls `MCP_DR_SystemUpdateFromMedia("/vol/sdcard/wafel_install", 0)`
and reads the recovery progress global at `0x1fe01584` (`== 0x13` means a full
success).

## Hardware test results (2026-11-08)

Tested on the user's retail console with `00core`, `4usbmlc`, `5debug`, `5isfshax`.

1. v1 data-aborted in `kern_main`: `expect32` did a 32-bit load from the
   2-byte-aligned Thumb site `0x0510eaee`. Fixed: it now reads two halfwords.
2. v2 data-aborted in the worker at `0x05035364`: stroopwafel's `FSA_Open()`
   calls MCP `MCP_fsaInit(FSAIpcData **data)` at `0x05035294` with garbage `r0`,
   and that function dereferences `data` when it is non-NULL. Fixed: the plugin
   calls `MCP_fsaInit(NULL)` itself (`recupd_fsa_open`), once, and reuses the
   handle (each call takes one of MCP's 8 FSA client slots at `0x050a3c70`).
3. **USB MLC (`4usbmlc.ipx`), first try: the reinstall failed at step 8** (fixed, see 9.),
   `mcp_recovery.c:414:failed to stop ES, err -196645` (`0xfffcffdb`). Cause:
   - PM resumes processes in order and blocks on each one. Recovery resume
     (`MCP_DRThreadEntry` case 0xd, `/dev/mcp_recovery` = process 33) calls
     `wait_for_dev("/dev/mlc01", 15 s)` (literal `0x0510f774`). The USB MLC
     needs `/dev/ums` (process 46), which PM only resumes after that.
     So the wait always times out (`mcp_recovery.c:577:wait for mlc attach
     failed, err -32`), and mlc01 attaches about 0.3 s later. Raising the timeout
     to 60 s just moved the failure to 60.8 s, so the patch was dropped.
   - After the timeout, resume skips mount/sysdirs/sys_prod/`MCP_InitES`/
     `MCP_InitInstall` and goes to the menu. ES's FSA handle `g_05070098` stays
     `-1`, so `MCP_FSDelClient(-1)` in stop-ES (`0x0500fe28`) fails.
   - Normal boot uses the same 15 s value (`0x05027de0`), but it doesn't wait
     inside the PM resume.
   - **Fix, implemented in `wafel_usb_mlc` (2026-11-08, tested on hardware,
     see 9.):** a second hook next to its normal-boot
     `mcp_mlc_wait_hook`, on the recovery resume's `bl wait_for_dev("/dev/mlc01")`
     at `0x0510f56e` (`f707fbb3`). It brings UMS up before the wait instead of
     after it:
     1. mount `/vol/system_slc` (recovery hasn't yet; `MCP_IVS_stuff` may read
        `security/ivs.bin`), call `MCP_IVS_stuff` (`0x0501c238`), unmount again
        with `MCP_UnmountFS` (`0x05015b20`, flags `0x80000000`, as recovery does).
        Both with recovery's FSA owner `{0,0,1,0}` (rodata `0x05066e6c`): with a
        `NULL` owner the mount fails with `FSA_STATUS_PERMISSION_ERROR`
        (`-0x3001a`, first hardware run);
     2. resume `/dev/usbproc2` and `/dev/ums` early (`resume_process`, same PM
        entries as normal boot);
     3. wait for `/dev/mlc01` in 1.5 s steps, capped at 5 minutes, so a missing
        drive ends on the recovery screen.
     If the IVS can't be set, the USB MLC isn't brought up: in recovery the
     `/dev/mcp` ioctl 0x7e answer is a fixed default IVS, and formatting or
     installing with that key would leave a drive normal boot can't read.
     `MCP_IVS_stuff`'s failure path calls `IVS_WriteDeviceId` (SEEPROM word
     `0x58`, the IVS seed) when the security level isn't Normal, which the
     recovery plugin's level patch makes reachable; `wafel_usb_mlc`'s
     `bsp_eeprom_write_hook` drops writes to words `0x58`–`0x5F`.
     Also fixed there: `MCP_MountWithSubdir` was declared without the Thumb bit
     (only used in commented-out code before).
     The other `wait_for_dev` calls in the DR update (`0x0510ed10`, `0x0510ed82`)
     run after UMS is up. `wafel_usb_mlc`'s UMS-suspend hook (`0x050283aa`) is in
     the normal boot thread, so the DR update's MLC unmount doesn't suspend UMS.
4. **Internal MLC: success.** `4usbmlc.ipx` removed, internal 64 GB MLC.
   - The MLC came up `raw` (no valid WFS), so recovery resume's own
     *conditional* format (`MCP: Formatting Device mlc to wfs`, 0.87 s)
     formatted it before the worker ran. With an MLC that already holds a valid
     WFS this format doesn't happen.
   - The worker started at 11 s. MCP installed the 44-title system package from
     `/vol/sdcard/wafel_install` and committed it, then printed `MCP: System
     update was successful...` at 4 min 16 s.
   - MCP then powers off at once (`Power Transition -> PM_OFF`). The plugin's
     success log, self-delete (`FSA_Remove` of the ipx) and flush still got
     through: the ipx was gone from the SD afterwards, and
     `wafel_drsetup.log` was written.
   - The recovery screen wasn't visible and the next boot hung at the Wii U logo;
     both fixed, see "Display" and "Initial setup" below.
5. **Final run** (display fix + `initial_launch` reset, MLC already WFS, so no
   format): menu at once, text and progress bar during the install, `System update
   was successful` after 4 min 42 s, self-delete OK. Initial setup starts on the
   next boot.
6. **Region test** (`REGIONTEST=1`, after the user had erased the MLC): see
   "Region". Passed.
7. An earlier report "doesn't delete itself and only boots into recovery" was the
   NOINSTALL test build left on the SD (11088 bytes vs. the release), not a bug.
8. **Error handling** (2026-11-08): MLC disconnected → step 8 failure as in the
   table above, all plugin lines readable on the TV, nothing changed.
   REGIONTEST with the in-place write: 2 written, 4 written back, sys_prod load
   0, success. Release run: "already matches", load 0, plugin removed after the
   power-off line.
9. **USB MLC** (2026-11-08, `4usbmlc` with the recovery hook):
   - 190 GB HDD: hook resumed USBPROC2/UMS, `mlc01` after ~7 s, raw drive
     formatted, install successful (2 min 42 s). The next two runs (reinstall
     over it) failed at step 0x20: `FSA: ### DATA CORRUPTION ERROR ###,
     dev:mlc01, err:-1245211, path:/sys/title/00050030/10017209/code/app.xml`,
     already during recovery's title scan, then "Rollback system quota". The user
     suspects a faulty HDD and swapped it.
   - 7.4 GB flash drive: `mlc01` after ~6 s, formatted, install successful
     (6 min 46 s), plugin removed. Normal boot with `4usbmlc`: **initial setup**
     (so the drive was set up with the key normal boot uses).
   - Together with `01scrlog.ipx`: before `MCP_ShowMenu` the plugin sets the
     screen log's exported `scrlog_bottom_only` (`wafel_find_symbol`, then
     100 ms for a running redraw), so the log only draws its bottom box and never
     re-initialises the TV. User: menu fine, box readable, the log text slightly
     blurry: DC_INIT id 1 is 480p. Fixed by using id 0 (1080p), see "Display";
     then menu and log both sharp. Without the flag both drew full screen and the screen log
     re-initialised the TV after DC_INIT.
   - The screen log read the D1 surface register as `0x14001001` after
     `DC_INIT`, while the TV still showed `0x17500000`: DC_INIT does write the
     address, it just never takes effect (not latched).

## Display: why the menu was invisible, and the fix

Hardware, 2026-11-08 (TV, minute had shown its menu before booting IOSU):

- Without the fix the TV kept **minute's text**, slightly zoomed out and
  blurry. No recovery menu and no progress were ever visible.
- `MCP_ShowMenu` (`0x051003a8`) worked: BSP `DISPLAY` `DC_INIT(1)` rc 0,
  `DC_CONFIG` rc 0 = `{1, 0, 1280, 720, 0x14001000}`. `MCP_InitMenu` set up the
  1280×720 surface at `0x14001000`, and `MCP_DrawRecoveryScreen` drew there.
- `DC_INIT` changes the video mode (that is the blur: minute's picture shown in
  the new mode), but the scanout **stays on the surface that was active before**,
  minute's `0x17500000`. `DC_INIT`'s table write `0x6110` (D1GRPH_PRIMARY_SURFACE_ADDRESS)
  `= 0x14001000` does not take effect. Flushing the D-cache over `0x14001000`
  changed nothing. Copying MCP's drawing to `0x17500000` made the menu appear,
  sharp and full screen. Why the surface write doesn't latch is not known (not
  analysed: maybe minute's leftover DC state; minute's `gpu_cleanup` writes
  `0x60e0 = 0` and `0x898 = 0xFFFFFFFF`).
- **Fix:** `kern_main` (MMU off) reads D1GRPH_ENABLE (`0x6100`) and the primary
  surface (`0x6110`) over ABIF (`0x0d800620/624`). If D1 is on and the surface
  is a 1280×720×4 buffer inside `0x14000000–0x1D000000`, the `bl MCP_InitMenu`
  in `MCP_ShowMenu` is replaced so that MCP's screen surface uses that buffer.
  All sub-surfaces (text console `g_drCon` `0x050a2b70`, progress box
  `g_progress` `0x050a3be0`) are cut out of that surface, so text and progress
  bar land in the visible buffer too. Confirmed on hardware: menu at once, sharp;
  during the install "Updating from …" and the progress bar filling up.
- `MCP_ShowMenu` calls BSP through Thumb→ARM thunks (`bl 0x05059570` /
  `0x05059568`), and stroopwafel's Thumb `blreplace` only handles `BL`, so the hook
  sits on `bl MCP_InitMenu` (`0x051003fe`), not on the `DC_CONFIG` read.
- **Video mode (1080p):** `MCP_ShowMenu` passes `DC_INIT` id 1. BSP's id-1
  tables (`dc1`) program **480p** timing (H/V total 858×525, `0x6000`/`0x6020`
  = `0x359`/`0x20c`), so the 1280×720 surface is scaled down and thin text gets
  blurry (minute's leftover text, the screen log's 8×8 font). Id 0 (`dc0`) is
  **1080p** (2200×1125, `0x897`/`0x464`), same PLLs and the same 1280×720
  surface, and the timing minute uses. The plugin patches `movs r3,#1` at
  `0x051003c4` to `#0`. Hardware: menu and screen-log box both sharp.
- If the TV wasn't scanning out a usable surface at `kern_main` (e.g. minute
  autoboot without showing anything), the plugin changes nothing and MCP uses
  `0x14001000` as on a factory console. Not tested.

## Initial setup after the reinstall

Hardware, 2026-11-08:

- After the first successful reinstall the console hung at the **Wii U logo**.
  The SLC kept `initial_launch = 2` ("setup done") in
  `/vol/system_slc/proc/prefs/cafe.xml`, so MCP launched the Wii U Menu
  (`0005001010040200`) on the freshly formatted MLC. Its first save access failed:
  `FLUSH_QUOTA ... NOT_FOUND path:[/vol/storage_mlc01/usr/save/00050010/1004a200/user/common/db/FFL_ODB.dat]`,
  then `FS_VOLSTATE_FATAL`.
- `wafel_setup_mlc` sets `cafe.initial_launch = 0` through `/dev/usr_cfg`
  (MCP's `UC_WriteSysConfig` `0x05044a8c`). In recovery mode `/dev/usr_cfg`
  isn't running (open/read/write return `-6`).
- First fix: edit `initial_launch` in `cafe.xml` in place (2 → 0); the next boot
  started the initial setup.
- **Current behaviour:** the install thread **deletes** `cafe.xml` (then flushes the
  SLC) before the reinstall; MCP powers off right after a successful one, so it
  can't come after. See below for why; "not found" counts as fine. Release run
  2026-11-08: the file (recreated by the previous setup boot) was deleted,
  update successful, plugin removed.
- `cafe.xml` as found (661 bytes): `version 5`, `language 1`, `cntry_reg 78`,
  `eula_agree 0`, `eula_version 101`, `initial_launch 2`, `eco 0`, `fast_boot 0`.
  `/vol/system_slc/proc/prefs/` also holds `nn.xml`, `cos.xml`, `parent.xml`,
  `p_acct1..12.xml`, `console.xml`, `caffeine.xml`, `NETCfg0..5.xml` and others;
  `/vol/system_slc/config/` only `eco.xml`, `rtc.xml`, `sys_prod.xml`, `system.xml`.
- Region (`wafel_setup_mlc`'s `fix_region`) wasn't needed here: the Menu matched
  the console's region.

### How stock does it, and deleting `cafe.xml` instead

- Stock recovery never resets `initial_launch`. `MCP_DR_SystemUpdateFromMedia`
  writes a default `system.xml` (`MCP_WriteDefaultSystemConfig(1)`
  `0x050049c0`) only with `wipeSlc != 0` (devkit `reflash`), which force-formats
  the SLC first, so `/vol/system_slc/proc/prefs/` is gone too. The factory SD
  update uses `wipeSlc = 0`, like this plugin; a factory console's prefs were
  simply never set.
- A missing `cafe.xml` is recreated by the SCI library's `SCIInitCafeSettings`
  (copy in IOS-ACP at `0xe0043c34`, also in NET, NIM-BOSS, FPD): when reading the
  9 `cafe.*` items fails it deletes the group and writes it with `version 5`
  and **everything else 0** (defaults at `0xe010c840`): language, cntry_reg,
  eula_agree, eula_version, initial_launch, eco, fast_boot.
- Hardware, 2026-11-08 (`make NOINSTALL=1 CAFEDEL=1`: back up `cafe.xml` to
  `/vol/sdcard/cafe.xml.bak`, then delete it): the next normal boot went into
  the initial setup, asking for language etc. No SCI/FS errors in the log.


## Region

Boot Recovery has no region step: `MCP_DR_SystemUpdateFromMedia` only reads
`sys_prod.xml` (and on ATFS media picks `update.inf.<REG>` from `product_area`).
The native writer, `/dev/mcp` ioctlv 0x41 `SetSysProdSettings`, is behind
`IOS_CheckDebugMode()` (`0x05024648`, OTP), closed on retail even in recovery;
`wafel_setup_mlc` NOPs it plus two XML access checks.

The plugin uses the same rule as `wafel_setup_mlc`'s `fix_region`, at the point
the user suggested: the end of the recovery update.

- Installing a title with app type `0x90000001` (system menu) calls
  `MCP_SysCfg_SetDefaultTitle` in the install path (`mcpdr_install.c`), and
  `MCP_SystemUpdate` sets one if none is set. So after the install the coldboot
  title is the package's Wii U Menu.
- The hook replaces `bl MCP_LoadSysProdXML` at `0x0510f04c` (original
  `f6f2fdbc`). It only runs after a successful install, before the success line
  and before power transitions are re-enabled, so it doesn't race the power-off.
  It runs in the install thread (that thread calls the DR function).
- It reads the coldboot title with `MCP_SysCfg_GetDefaultTitle`
  (`0x05004714`, Thumb). If it is `00050010100400x0`, the region is `1 << x`
  (1 JPN, 2 USA, 4 EUR, ...). If `product_area` or `game_region` differ, it
  rewrites `/vol/system_slc/config/sys_prod.xml` with both values replaced
  (decimal text, other fields untouched), flushes the SLC, then calls the
  original `MCP_LoadSysProdXML`, which loads the fixed file.
- The write is **in place** (`r+`, same length, the file is never truncated),
  so a failed write or power loss can't leave `sys_prod.xml` empty. The FSA
  client has no rename, so there is no temp-file swap. A region with a different
  digit count than the old one (16/32/64 for CHN/KOR/TWN vs. 1/2/4/8) is
  refused with a message, sys_prod stays unchanged.
- `wafel_setup_mlc`'s title check
  `coldbootTitle & 0xFFFFFFFFFFFFF0FF != 0005001010040000UL` never fails
  (`!=` binds tighter than `&`); the plugin's version uses parentheses.
- sys_prod.xml on the test console: 860 bytes, `product_area 4`,
  `game_region 4`, `ntsc_pal PAL`, `5ghz_country_code EU`, model `WUP-101(03)`.
- Hardware, 2026-11-08 (`REGIONTEST=1`): region set to 2 before the install, hook
  read coldboot title `0005001010040200`, rewrote sys_prod to 4 (860 bytes),
  install reported success, plugin deleted itself.

## What happens on errors

Every plugin message goes to `/vol/sdcard/wafel_drsetup.log` **and** the
recovery screen's text area (`DrCon_Printf` `0x05110de4`), prefixed
`drsetup:`. The plugin deletes itself only after a full success; in every
other case it stays on the SD, so each boot goes back into recovery and retries
until the update succeeds or the `.ipx` is removed.

| Case | What happens | Console changed? |
|---|---|---|
| No `wafel_install` folder | "no wafel_install folder on SD, nothing installed", orange LED | no |
| MLC missing / doesn't attach (tested: MLC disconnected) | recovery's 15 s wait fails, resume skips mounts and ES; "FAILED to open cafe.xml"; MCP fails at step 8 "failed to stop ES"; "system update FAILED at step 0x08", "plugin stays on the SD: the next boot retries", red LED | no (step 8 is before any format) |
| Install fails (`MCP_SystemUpdate` error) | MCP "System update failed, err N", plugin "FAILED at step 0x12", red LED; console stays on the screen (no power-off) | yes: conditional MLC format, `security/version` deleted, `cafe.xml` deleted, titles installed so far (rollback not checked) |
| Region fix fails (not a Menu, can't parse, length differs, write fails) | message on screen; the update still finishes with the old region | only on a successful write |
| Final `MCP_LoadSysProdXML` fails | MCP "System update failed" with the step **still 0x13** (MCP sets it before the load). The plugin's hook records the load result, so the install thread reports "sys_prod.xml load FAILED after the install" instead of success and stays on the SD | install done |
| Success | the plugin deletes itself **first**, then "system update was successful, plugin removed". MCP powers off right after its success line; on hardware the power-off log line came before the plugin's messages, but the delete still went through | yes |

Cosmetic, left as is: "Updating from /vol/sdcard/wafel_install//..." The
second slash is stock: `MCP_DR_SystemUpdateFromMedia` builds the path with
`snprintf(pkg, "%s/%s", root, "/")` (`0x0510eae8`), so every root ends in
`//`, the factory one (`/vol/storage_sdcard01/update`) too. A hook on that call
worked on hardware but was dropped to keep MCP's code stock.

## Static analysis of the recovery code (MCP decomp, 2026-11-08)

Line-exact reconstruction in [../mcp_decomp/mcp_recovery.c](../mcp_decomp/mcp_recovery.c).
It agrees with the USB-MLC hardware result above and adds:

- Recovery resume (`MCP_DRThreadEntry` case 0xd) stops at the first failing
  step and **returns that error to PM** as the resume reply (`-32` for the MLC
  timeout). Everything after the MLC wait is skipped: the conditional formats, the
  mounts, ramdisk, config, title manager, sys_prod, `MCP_InitES`/`Init2ndES`,
  `MCP_InitInstall`, hashtable, version list and the 0x1a "check media" message.
  Only `MCP_ShowMenu` still runs. That is why `MCP_StopES` (`0x0500fe28`) ends in
  `MCP_FSDelClient(-1)` (= -0x30025).
- Resume order is the PM RM table (`0x050711a8`, 0x58-byte entries; the devkit
  `time` command / `MCP_PrintBootTimes` prints it). A USB-MLC fix has to let resume
  return before the MLC wait, or move the MLC-dependent steps after `/dev/ums`.
  Either way, the plugin's worker would have to redo every step listed above in the
  same order (FSA/volflush are already done before the SLC wait).
- The `IOS_SetSecurityLevel(Test)` call in resume is inside
  `IOS_CheckDebugMode()==0`, so on retail it is not called at all. The README
  section above says it is a no-op on retail; both lead to the same result.
- The progress code `0x1fe01584` is posted to the LED thread on failure, but the
  LED thread shows **the same blink pattern for every step code** (except 0x1a).
  So the LED does not tell which step failed; read the log or the global.
- `MCP_DR_SystemUpdateFromMedia` returns no value (r0 is whatever the last call
  returned). The plugin correctly uses the global (`0x13` = success).

## Risks

- Tested once on retail (see above); USB MLC not supported yet.
  `IOS_SetSecurityLevel(Test)` **is** refused on retail (confirmed in the kernel,
  `0x081299e8`); the default `DRSETUP_FORCE_TEST_LEVEL` patch handles that by
  forcing MCP's `IOS_GetSecurityLevel` to Test instead.
- It force-formats nothing by default, but `MCP_DR_SystemUpdateFromMedia` still
  **unmounts and reformats the MLC's WFS when it differs**, and always deletes
  `/vol/system_slc/security/versions.bin` (only when `wipe_slc` = 0; regenerated by the update).
- An MLC that holds WFS from a *different* console may make the conditional
  format's `GetVolumeInfo` fail; use `DRSETUP_FORCE_FORMAT` to rebuild it.
- `wipe_slc` is hard-coded to 0: the SLC (including `scfm.img`, `ivs.bin`,
  tickets, SLC system titles) is never touched.
- The plugin enters recovery on **every** boot while present, so it deletes
  itself from the SD after a successful run. If it fails, remove the `.ipx`
  manually to boot normally.
