# wafel_recovery_update (`5recupd.ipx`)

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

## What it does

1. `kern_main` patches MCP so that on the next boot it enters Boot Recovery mode
   on a retail console, the recovery thread starts, and the recovery reinstall
   routine installs from a plain folder instead of an ATFS "UP" volume.
2. The recovery thread mounts the SLC/MLC/SLCCMPT (formatting only what is
   unformatted), initialises ES/install, and draws the recovery screen on the TV.
3. A worker thread then mounts the FAT SD at `/vol/sdcard` and, if
   `/vol/sdcard/wafel_install` exists, calls `MCP_DR_SystemUpdateFromMedia` on it.
   Before that it sets `initial_launch` to 0 in
   `/vol/system_slc/proc/prefs/cafe.xml`, so the next boot runs the initial setup.
   MCP prints "Updating from …", a progress bar ("Updating system, please
   wait...") and the success/failure line on the TV.
4. On success the plugin deletes itself from the SD so the next boot is normal.
   MCP powers the console off right after the success line.

## SD layout

A normal FAT32 SD card:

```
sd:/wiiu/ios_plugins/5recupd.ipx      <- this plugin
sd:/wafel_install/                     <- package root (mounted at /vol/sdcard/wafel_install)
   <title 1>/  title.tmd title.tik title.cert *.app *.h3
   <title 2>/  ...
```

Same per-title WUP layout as `wafel_setup_mlc`. No `app.xml` / `update.inf` is
needed: on a non-ATFS volume the engine enumerates the subdirectories directly.

## Compile-time options

Pass via `EXTRA_CFLAGS`, e.g.
`make EXTRA_CFLAGS="-DRECUPD_FORCE_FORMAT"`.

| Define | Default | Effect |
|---|---|---|
| `RECUPD_FORCE_FORMAT` | off | Keep the factory forced format of SLCCMPT and the MLC. Off = format only when the volume is unformatted / the wrong file system (like `wafel_setup_mlc`). |
| `RECUPD_REQUIRE_FULL_SYSTEM` | off | Keep the factory check that the package is a complete system (OS + boot titles). Off (default) installs any set of WUPs. |
| `RECUPD_KEEP_NORMAL_LEVEL` | off | Leave the security level alone. By default (`RECUPD_FORCE_TEST_LEVEL`) MCP's `IOS_GetSecurityLevel` is patched to return Test; without it a reinstall of titles already installed at the same or a newer version is skipped (see below). |

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
| Blue blinking | worker running / install in progress |
| Solid blue | success (plugin deleted itself) |
| Orange | no `wafel_install` folder on SD — nothing done, recovery menu stays up |
| Red / red blinking | FSA/open failure or the update failed (see `/vol/sdcard/wafel_recupd.log` and the on-screen line) |

## Build

```
STROOPWAFEL_ROOT=/home/jan/wiiu/stroopwafel DEVKITARM=/opt/devkitpro/devkitARM make
```

Produces `5recupd.ipx`. Copy it to `sd:/wiiu/ios_plugins/`.

`make NOINSTALL=1` builds a test variant: it boots into recovery, shows the
screen and resets `initial_launch`, but stops before the reinstall (orange LED).
It does not delete itself. Run `make clean` before switching between the two.

## How it works (patch map)

All addresses are IOSU 5.5.x retail (`fw.elf` 00050010-1000400A). Each patch is
guarded by an `expect` check of the original bytes.

| VA | Patch | Purpose |
|---|---|---|
| 0x08129ce0 | `bx lr` | no kernel panic |
| 0x0501fc98 | `b 0x0501fdec` | `MCP_Startup`: force the Boot Recovery branch (security level Test, PM mode 0x40000) |
| 0x0510ea1c | `movs r0,#0; nop` | `MCP_StartDRThread`: start the recovery thread on retail |
| 0x050340ee | `movs r0,#0; nop` | never start the PPC |
| 0x0510ec38 / 0x0510ed96 / 0x0510ecd4 | `movs r2,#0` | `MCP_DR_SystemUpdateFromMedia`: SLCCMPT / MLC / SLC formats become conditional (omitted with `RECUPD_FORCE_FORMAT`) |
| 0x0510ef48 | `b 0x0510f0c0` | skip ticket deletion, go to the progress thread + `MCP_SystemUpdate` |
| 0x0510eaee | `movs r0,#0; nop` | skip the full-system-package check (omitted with `RECUPD_REQUIRE_FULL_SYSTEM`) |
| 0x05059090 | `movs r0,#0xa; bx lr` | MCP `IOS_GetSecurityLevel` returns Test, so the update engine doesn't skip same-version titles (omitted with `RECUPD_KEEP_NORMAL_LEVEL`) |
| bl @0x0510f88a | `trampoline_t_blreplace` | `bl MCP_ShowMenu` in the recovery thread: run it, log the menu surface, then start the worker |
| bl @0x051003fe | `trampoline_t_blreplace` (found by scanning `MCP_ShowMenu`) | `bl MCP_InitMenu`: replace DC_CONFIG's framebuffer with the surface the TV scans out (read in `kern_main`), see "Display" |

The worker calls `MCP_DR_SystemUpdateFromMedia("/vol/sdcard/wafel_install", 0)`
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
3. **Open: with a USB MLC (`4usbmlc.ipx`) the reinstall fails at step 8**,
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
   - Future USB-MLC support needs its own wait (the HDD can take a while to
     spin up and the USB services start late): let resume continue past the
     MLC wait and catch up the MLC-dependent steps from the worker once
     `/dev/mlc01` appears.
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
     `wafel_recupd.log` was written.
   - The recovery screen wasn't visible and the next boot hung at the Wii U logo;
     both fixed, see "Display" and "Initial setup" below.
5. **Final run** (display fix + `initial_launch` reset, MLC already WFS, so no
   format): menu at once, text and progress bar during the install, `System update
   was successful` after 4 min 42 s, self-delete OK. Initial setup starts on the
   next boot.

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
  isn't running (open/read/write return `-6`), so the worker edits `cafe.xml`
  directly: the one-digit value is overwritten in place (`r+`, same length, the
  file is never truncated), then `/vol/system_slc` is flushed. This happens
  **before** the reinstall, because MCP powers off right after a successful one.
- With `initial_launch = 0` the next boot started the initial setup (confirmed).
- `cafe.xml` as found (661 bytes): `version 5`, `language 1`, `cntry_reg 78`,
  `eula_agree 0`, `eula_version 101`, `initial_launch 2`, `eco 0`, `fast_boot 0`.
  `/vol/system_slc/proc/prefs/` also holds `nn.xml`, `cos.xml`, `parent.xml`,
  `p_acct1..12.xml`, `console.xml`, `caffeine.xml`, `NETCfg0..5.xml` and others;
  `/vol/system_slc/config/` only `eco.xml`, `rtc.xml`, `sys_prod.xml`, `system.xml`.
- Region (`wafel_setup_mlc`'s `fix_region`) wasn't needed here: the Menu matched
  the console's region.

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
  `0x081299e8`); the default `RECUPD_FORCE_TEST_LEVEL` patch handles that by
  forcing MCP's `IOS_GetSecurityLevel` to Test instead.
- It force-formats nothing by default, but `MCP_DR_SystemUpdateFromMedia` still
  **unmounts and reformats the MLC's WFS when it differs**, and always deletes
  `/vol/system_slc/security/versions.bin` (only when `wipe_slc` = 0; regenerated by the update).
- An MLC that holds WFS from a *different* console may make the conditional
  format's `GetVolumeInfo` fail; use `RECUPD_FORCE_FORMAT` to rebuild it.
- `wipe_slc` is hard-coded to 0: the SLC (including `scfm.img`, `ivs.bin`,
  tickets, SLC system titles) is never touched.
- The plugin enters recovery on **every** boot while present, so it deletes
  itself from the SD after a successful run. If it fails, remove the `.ipx`
  manually to boot normally.
