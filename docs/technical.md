# Technical notes

How the plugin works and what was found while writing it. All addresses are
IOSU 5.5.x retail (`fw.elf`, 00050010-1000400A); stroopwafel only runs on
5.5.x, where these modules are the same in every release. Code comments cover
the details of each function; this file collects what doesn't fit there.

## Overview

1. `kern_main` patches IOS-MCP so it boots into Boot Recovery on a retail
   console, starts the recovery thread and never starts the PPC.
2. Recovery resume (PM resume of `MCP_DRThreadEntry`) mounts SLC, MLC and
   SLCCMPT, formats what has no valid file system (after the EJECT prompt),
   initialises ES/install and draws the recovery screen.
3. The hook on the recovery thread's `bl MCP_ShowMenu` starts the install
   thread. It mounts the SD at `/vol/sdcard`, deletes `cafe.xml` (full build),
   repairs `system.xml`/`cert.sys` and calls
   `MCP_DR_SystemUpdateFromMedia("/vol/sdcard/wafel_install", 0)`.
4. Just before MCP reloads `sys_prod.xml` at the end of a successful update,
   the region is fixed (full build).
5. Success = recovery step global `0x1fe01584 == 0x13` and the final
   `MCP_LoadSysProdXML` returned 0. Then the plugin deletes itself.
   `MCP_DR_SystemUpdateFromMedia` itself returns no meaningful value.

## Patch map

Patches are applied without checking the original bytes first: a silently
skipped patch is worse than a crash.

| VA | Patch | Purpose |
|---|---|---|
| 0x08129ce0 | `bx lr` | no kernel panic |
| 0x0501fc98 | `b 0x0501fdec` | `MCP_Startup`: take the Boot Recovery branch (PM mode 0x40000) |
| 0x0510ea1c | `movs r0,#0; nop` | `MCP_StartDRThread`: start the recovery thread on retail |
| 0x050340ee | `movs r0,#0; nop` | never start the PPC |
| 0x05059090 | `movs r0,#0xa; bx lr` | MCP's `IOS_GetSecurityLevel` returns Test (see below) |
| 0x0510eaee | `movs r0,#0; nop` | skip the "package is a complete system" check |
| 0x0510ef48 | `b 0x0510f0c0` | skip the ticket wipe |
| 0x0510ec38 / 0x0510ed96 / 0x0510ecd4 | `movs r2,#0` | DR update: SLCCMPT / MLC / SLC formats only when needed (not forced) |
| `bl MCP_FormatDevice` ×6 | blreplace | SLC: blocked; MLC/SLCCMPT: EJECT prompt (full) or blocked (install only) |
| bl @0x0510f88a | blreplace | `bl MCP_ShowMenu` in the recovery thread: screen-log switch, then start the install thread |
| bl @0x051003fe | blreplace | `bl MCP_InitMenu`: draw into the surface the TV scans out |
| 0x051003c4 | `movs r3,#0` | `DC_INIT` id 0 (1080p) instead of 1 (480p) |
| bl @0x0510f04c | blreplace | `bl MCP_LoadSysProdXML` after a successful update: region fix, record the result |
| 0x0503cc44 / 0x0503cd32 | `nop` | config writer: allow writing the read-only sys_prod nodes (full build) |
| bl @0x05028556 | blreplace | MCP's read of the SMC event flags: hand EJECT/POWER to an open prompt |

## Security level

On retail `IOS_SetSecurityLevel` does nothing (the kernel only writes the
level when the debug fuse is set), so recovery runs at **Normal**. At Normal
the update engine skips every title already installed at the same or a newer
version, so a reinstall would silently do nothing. The plugin patches MCP's
`IOS_GetSecurityLevel` thunk to return Test. This only affects MCP.

## Display

- `MCP_ShowMenu` (`0x051003a8`) initialises the display through BSP `DISPLAY`
  (`DC_INIT`, `DC_CONFIG`) and draws into a 1280×720 surface at `0x14001000`.
  After minute has used the display, `DC_INIT` changes the video mode but the
  scanout stays on the previous surface (`0x17500000`); the new surface
  address never takes effect. Why it doesn't latch is not known.
- Fix: `kern_main` (MMU off) reads `D1GRPH_ENABLE` (`0x6100`) and the primary
  surface address (`0x6110`) over ABIF (`0x0d800620/624`). If a 1280×720×4
  surface is being scanned out, the hook on `bl MCP_InitMenu` hands that
  buffer to MCP. The text console and progress box are cut out of it, so they
  are visible too. If nothing usable is scanned out, MCP's own surface is
  kept.
- `DC_INIT` id 1 programs 480p timing, so the 1280×720 surface was scaled down
  and blurry. Id 0 is 1080p with the same surface and PLLs, the timing minute
  uses.
- `MCP_ShowMenu` calls BSP through Thumb→ARM thunks, and the Thumb `blreplace`
  only handles `BL`, so the hook sits on `bl MCP_InitMenu`.
- With `wafel_screen_log` loaded, the plugin sets its exported
  `scrlog_bottom_only` before the menu is drawn, so the log only uses a box at
  the bottom and doesn't re-initialise the TV.

## Format prompts

- Recovery resume formats `/dev/mlc01` and `/dev/slccmpt01` (`0x0510f5b8`,
  `0x0510f5ce`) when they don't hold the expected file system. The prompt
  wrapper repeats `MCP_FormatDevice`'s own decision (`0x050160d8`): volume
  info through MCP's FSA handle (`0x05070184`), file system name at `+0x24`,
  compared over 8 characters, or `force`. Only if MCP would really format does
  it ask. If the volume info fails, MCP doesn't format either.
- An unreadable volume, including a WFS encrypted with another console's key,
  is attached as `raw`, so the prompt doesn't print the detected type.
- These calls run during PM resume, before the recovery thread draws its
  screen, so the prompt calls `MCP_ShowMenu` early. The recovery thread's own
  call later just redraws it.
- Buttons are bits in SMC register 0x41 (BSP `SMC` / `SystemEventFlag`,
  cleared on read): 0x40 POWER, 0x20 EJECT, 0x10 disc insert. MCP reads it in
  `MCP_HandleWakeEvents` (`0x05028508`), but that handler doesn't run during PM
  resume: a press made then is only read after resume. So the prompt polls the
  register itself every 100 ms through MCP's `MCP_bspRead` thunk
  (`0x05059568`), after one read that drops presses latched before the prompt.
  The hook on MCP's own read covers prompts after resume.
- A clean power-off from the prompt isn't possible: `MCP_RequestOff` returns
  -31 ("Power transitions are explicitly disabled") during resume. Holding
  POWER is the SMC's hard power-off. A short press is swallowed, because MCP
  would restart into recovery on it.

## SLC repair

- `system.xml` missing → `MCP_WriteDefaultSystemConfig(1)` (`0x050049c0`),
  which stock MCP only calls after a full SLC wipe.
- `cert.sys` missing or without `CA00000003`, `CP0000000b` and `XS0000000c` →
  merged from the first `title.cert` in the package with ES's
  `esMergeCertSys` (`0x05050448`, ARM). ES can't verify any TMD without these.
- `sys_prod.xml` missing → nothing to do: `MCP_LoadSysProdXML` rebuilds it
  from the SEEPROM.

## Initial setup

- Recovery never resets the initial-setup flag. With a fresh MLC but the old
  `cafe.xml` (`initial_launch = 2`), the next boot starts the Wii U Menu,
  whose save data doesn't exist, and hangs at the Wii U logo.
- `/dev/usr_cfg` doesn't run in Boot Recovery, so the setting can't be changed
  through it. The full build deletes `/vol/system_slc/proc/prefs/cafe.xml`
  before the install. The SCI library recreates a missing `cafe.xml` with all
  values 0 (`initial_launch = 0`), so the next boot runs the initial setup.
  The delete happens before the install because MCP powers off right after a
  successful one.

## Region

- Installing the Wii U Menu (app type `0x90000001`) makes it the coldboot
  title. If it is `00050010100400x0`, the region is `1 << x` (1 JPN, 2 USA,
  4 EUR, ...), the same rule as `wafel_setup_mlc`.
- The usual writer, `/dev/mcp` ioctlv 0x41 `SetSysProdSettings`, doesn't exist
  in recovery: there `/dev/mcp` belongs to the recovery thread, which only
  handles ioctl 0x7e. So the plugin calls what 0x41 calls, MCP's config client:
  `MCP_Cnfg_Read` (`0x0503b43c`) and `MCP_Cnfg_Write` (`0x0503b400`) on
  `slc:sys_prod.product_area` and `.game_region` (u32, size 4, item access 0,
  caller permissions 0x777).
- `MCP_Cnfg_Write` reads the file, merges the given items into it
  (`Cnfg_MergeTree` `0x0503c970`) and writes the whole file back, so the other
  fields are kept. The sys_prod nodes are read-only for every caller; the
  patches at `0x0503cc44` (node not writable) and `0x0503cd32` (item access ≠
  node access) allow the write.
- The hook replaces `bl MCP_LoadSysProdXML` at `0x0510f04c`, which only runs
  after a successful update and before power transitions are enabled again.
  The original call then loads the fixed file. MCP sets step 0x13 *before* this
  call, so the plugin also checks the call's result.

## Errors

| Case | Result |
|---|---|
| No `wafel_install` folder | message, orange LED, nothing changed |
| MLC missing | recovery's 15 s wait fails, resume skips mounts and ES; the update fails at step 8 ("failed to stop ES") before any change |
| Install fails | MCP "System update failed", red LED, plugin stays; formats and titles installed so far stay |
| Region fix fails | message, the update finishes with the old region |
| Final `MCP_LoadSysProdXML` fails | reported as a failure although the step is 0x13 |
| Success | the plugin deletes itself first, then reports; MCP powers off |

## Recovery internals

- Recovery resume (`MCP_DRThreadEntry`, command 0xd) stops at the first
  failing step and returns its error to PM. Everything after it is skipped
  (formats, mounts, sys_prod, ES and install init); only the menu is drawn.
- PM resumes processes one at a time and waits for each. Recovery waits for
  `/dev/mlc01` inside its resume, but a USB MLC needs `/dev/ums`, which PM only
  resumes later. `wafel_usb_mlc` brings UMS up early from a hook on that wait.
- The LED blink pattern on failure is the same for every step; the step is in
  the log and in `0x1fe01584`.
- "Updating from /vol/sdcard/wafel_install//": the double slash is stock
  (`snprintf("%s/%s", root, "/")` at `0x0510eae8`).
- MCP's FSA client (`MCP_fsaInit`, `0x05035294`) dereferences its argument
  when it is not NULL; the plugin calls it with NULL and keeps one handle
  (MCP has 8 client slots).
- The DR update always deletes `/vol/system_slc/security/versions.bin`
  (the update recreates it). `wipe_slc` is always 0.
