/*
 * wafel_recovery_setup (5drsetup.ipx, install-only build 5drinst.ipx)
 *
 * Boots IOS-MCP into its factory "Boot Recovery" mode on a retail console and
 * reinstalls titles from a FAT32 SD folder with MCP's own recovery reinstall
 * (MCP_DR_SystemUpdateFromMedia 0x0510eaa8). The recovery screen with progress
 * is drawn on the TV by MCP itself (display.c).
 *
 * Files:
 *   main.c     kern_main: recovery entry and the patches of the update path
 *   display.c  recovery screen on the TV (surface, 1080p, screen log, install start)
 *   install.c  install thread: SD, SLC preparation, the update, result
 *   slc.c      cafe.xml, system.xml/cert.sys repair, region fix
 *   log.c      FSA handle, SD log, messages on the screen
 *
 * All addresses are IOSU 5.5.x retail (fw.elf 00050010-1000400A). Patches are
 * applied unconditionally: stroopwafel only runs on 5.5.x, where these modules
 * don't change between releases.
 */
#include <wafel/types.h>
#include <wafel/utils.h>
#include <wafel/patch.h>
#include <wafel/trampoline.h>

#include "config.h"
#include "display.h"
#include "slc.h"
#include "mcp.h"
#include "prompt.h"

#define ADDR_KERN_PANIC         0x08129ce0  /* kernel panic -> bx lr */
#define ADDR_STARTUP_DBGCHK     0x0501fc98  /* MCP_Startup: bl IOS_CheckDebugMode */
#define ADDR_DRTHREAD_DBGCHK    0x0510ea1c  /* MCP_StartDRThread: bl IOS_CheckDebugMode */
#define ADDR_NO_PPC             0x050340ee  /* loadCafeOSKernel: start the PPC */
#define ADDR_MCP_GETSECLEVEL    0x05059090  /* MCP's IOS_GetSecurityLevel thunk */
#define ADDR_BOOTTITLE_CHK      0x0510eaee  /* DR update: bl CheckPackageHasBootTitles */
#define ADDR_TICKET_SKIP        0x0510ef48  /* DR update: start of the ticket wipe */
#define ADDR_FMT_SLCCMPT_FORCE  0x0510ec38  /* DR update: movs r2,#1 (force) */
#define ADDR_FMT_SLC_FORCE      0x0510ecd4
#define ADDR_FMT_MLC_FORCE      0x0510ed96
#define ADDR_MCP_FORMATDEVICE   0x050160d8

/*
 * Formats: each bl MCP_FormatDevice listed here is replaced by a no-op that
 * returns 0, so the volume is never formatted (a volume without a valid file
 * system then fails at the mount, like a missing one). The ramdisk format of
 * recovery resume (0x0510f65e) is never blocked.
 *  - SLC: blocked unless built with SLCFORMAT=1 (formatting the SLC destroys
 *    isfshax, scfm.img, tickets and config). A blank SLC is normally formatted
 *    by SCFM at boot; SLCFORMAT is for setups without SCFM (redNAND).
 *  - MLC and SLCCMPT: blocked in the install-only build; the full build lets
 *    them format when they have no valid file system.
 * The calls that aren't blocked go through drsetup_confirm_format: if MCP is
 * really about to format, the user has to confirm with EJECT first.
 */
static const struct { uintptr_t addr; int slc; } format_calls[] = {
    { 0x0510f59e, 1 },      /* resume: /dev/slc01 */
    { 0x0510f5b8, 0 },      /* resume: /dev/mlc01 */
    { 0x0510f5ce, 0 },      /* resume: /dev/slccmpt01 */
    { 0x0510ec3a, 0 },      /* DR update: slccmpt */
    { 0x0510ecd6, 1 },      /* DR update: slc (only with wipeSlc) */
    { 0x0510ed98, 0 },      /* DR update: mlc */
};

static int drsetup_no_format(const char *dev, const char *fs, int force, int r3,
                             int (*org)(const char *, const char *, int))
{
    (void)r3; (void)org;
    debug_printf("drsetup: format blocked: %s (%s, force %d)\n", dev, fs, force);
    return 0;
}

/* MCP_FormatDevice's own decision: it formats if forced or if the attached
 * volume's file system isn't the requested one; if it can't get the volume
 * info it returns that error without formatting */
static int format_needed(const char *dev, const char *fs, int force, char *found)
{
    u32 info[(FSA_VOLINFO_SIZE + 3) / 4];
    int r = MCP_fsaGetVolumeInfo(MCP_FSA_HANDLE, dev, info);
    if (r != 0) {
        debug_printf("drsetup: %s volume info error %d, no format\n", dev, r);
        return 0;
    }
    const char *vfs = (const char *)info + FSA_VOLINFO_FS;
    int same = 1;                       /* strncmp(vfs, fs, 8) == 0 */
    for (int i = 0; i < 8; i++) {
        if (vfs[i] != fs[i]) { same = 0; break; }
        if (!fs[i]) break;
    }
    for (int i = 0; i < 8; i++) {       /* printable copy for the message */
        char c = vfs[i];
        if (!c) break;
        found[i] = (c < 0x20 || c > 0x7e) ? '?' : c;
    }
    found[8] = 0;
    return force || !same;
}

static int drsetup_confirm_format(const char *dev, const char *fs, int force, int r3,
                                  int (*org)(const char *, const char *, int))
{
    (void)r3;
    char found[9] = {0};
    if (format_needed(dev, fs, force, found)) {
        debug_printf("drsetup: %s would be formatted to %s (now '%s', force %d)\n",
                     dev, fs, found, force);
        display_show_early();
        /* no "found" on screen: an unreadable volume (e.g. WFS with another
         * console's key) is always attached as "raw" */
        if (force)
            DrCon_Printf("\n%s is set to be reformatted to %s.\n", dev, fs);
        else
            DrCon_Printf("\nNo valid %s file system detected on %s.\n", fs, dev);
        drsetup_confirm("Format it? Everything on it will be erased.");
    }
    return org(dev, fs, force);
}

static void patch_formats(void)
{
    for (unsigned i = 0; i < sizeof(format_calls) / sizeof(format_calls[0]); i++) {
        int block = format_calls[i].slc;
#ifdef DRSETUP_INSTALL_ONLY
        block = 1;
#endif
#ifdef DRSETUP_ALLOW_SLC_FORMAT
        if (format_calls[i].slc)
            block = 0;
#endif
        trampoline_t_blreplace(format_calls[i].addr,
                               block ? drsetup_no_format : drsetup_confirm_format);
    }

#if !defined(DRSETUP_INSTALL_ONLY) && !defined(DRSETUP_FORCE_FORMAT)
    /* the DR update's formats only when the volume has no valid file system */
    ASM_T_PATCH_K(ADDR_FMT_SLCCMPT_FORCE, "movs r2, #0\n");
    ASM_T_PATCH_K(ADDR_FMT_MLC_FORCE, "movs r2, #0\n");
    ASM_T_PATCH_K(ADDR_FMT_SLC_FORCE, "movs r2, #0\n");
#endif
}

__attribute__((target("arm")))
void kern_main(void)
{
    debug_printf("wafel_recovery_setup kern %p\n", kern_main);

    /* don't halt the system on a kernel panic */
    ASM_PATCH_K(ADDR_KERN_PANIC, "bx lr\n");

    /* MCP_Startup: take the Boot Recovery branch (b 0x0501fdec; nop), which
     * sets security level Test and PM mode 0x40000 */
    U32_PATCH_K(ADDR_STARTUP_DBGCHK, 0xe0a846c0);

    /* MCP_StartDRThread: create the recovery thread on retail (movs r0,#0; nop) */
    U32_PATCH_K(ADDR_DRTHREAD_DBGCHK, 0x200046c0);

    /* never start the PPC, so Cafe OS can't take over the display */
    ASM_T_PATCH_K(ADDR_NO_PPC, "movs r0, #0\nnop\n");

#ifdef DRSETUP_FORCE_TEST_LEVEL
    /* MCP's IOS_GetSecurityLevel -> Test (movs r0,#0xa; bx lr), so the update
     * engine (FUN_0502ffc4) doesn't skip titles already installed at an
     * equal or newer version (see config.h) */
    U32_PATCH_K(ADDR_MCP_GETSECLEVEL, 0x200a4770);
#endif

#ifdef DRSETUP_SKIP_BOOTTITLE_CHECK
    /* any set of WUPs: skip the full-system-package check (movs r0,#0; nop) */
    U32_PATCH_K(ADDR_BOOTTITLE_CHK, 0x200046c0);
#endif

    /* never delete tickets: b 0x0510f0c0, past the ticket wipe (as .short:
     * an assembled "b" would be relative to the plugin, not to IOSU) */
    ASM_T_PATCH_K(ADDR_TICKET_SKIP, ".short 0xe0ba\n");

    patch_formats();
    prompt_kern_patches();
    slc_kern_patches();
    display_kern_patches();
}

void mcp_main(void) { }
