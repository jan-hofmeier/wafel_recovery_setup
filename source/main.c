/*
 * wafel_recovery_setup (5drsetup.ipx)
 *
 * Boots IOS-MCP straight into its factory "Boot Recovery" mode and reinstalls
 * titles from a FAT32 SD card, reusing MCP's own recovery reinstall routine
 * MCP_DR_SystemUpdateFromMedia (0x0510eaa8). Unlike wafel_setup_mlc, this draws
 * the MCP recovery screen on the TV (the screen is rendered on the ARM through
 * the BSP DISPLAY/DC controller, so no PPC is needed), and it reuses the
 * recovery engine's staged install with rollback instead of MCP_Install.
 *
 * Differences from the factory flow (see docs/mcp_factory_provisioning.md):
 *  - Forced formats are made conditional (format only when the volume is
 *    unformatted / wrong fs), like wafel_setup_mlc. RECUPD_FORCE_FORMAT keeps
 *    the factory forced format.
 *  - Ticket deletion is always skipped (we never delete tickets).
 *  - The ATFS "UP" media detection is bypassed; titles are read from a plain
 *    FAT folder /vol/sdcard/wafel_install (one WUP-style dir per title).
 *  - The full-system-package check is bypassed by default.
 *
 * Every patch site is checked against its expected original bytes first; a
 * mismatch skips that patch and is reported in the kernel log.
 *
 * STATIC-ANALYSIS ONLY, UNTESTED ON HARDWARE. It formats the MLC. Read the
 * README before running.
 */
#include <wafel/types.h>
#include <wafel/utils.h>
#include <wafel/dynamic.h>
#include <wafel/ios_dynamic.h>
#include <wafel/patch.h>
#include <wafel/trampoline.h>
#include <wafel/ios/svc.h>
#include <wafel/ios/thread.h>
#include <wafel/services/fsa.h>

#include "config.h"
#include "led.h"
#include "bsp.h"

/* ------------------------------------------------------------------ */
/* IOSU 5.5.x retail addresses (fw.elf 00050010-1000400A)             */
/* ------------------------------------------------------------------ */
#define ADDR_KERN_PANIC             0x08129ce0  /* -> bx lr                      */
#define ADDR_STARTUP_DBGCHK         0x0501fc98  /* bl IOS_CheckDebugMode         */
#define ADDR_STARTUP_RECOVERY       0x0501fdec  /* SetSecLevel(Test)+PM 0x40000  */
#define ADDR_DRTHREAD_DBGCHK        0x0510ea1c  /* bl IOS_CheckDebugMode         */
#define ADDR_NO_PPC                 0x050340ee  /* loadCafeOSKernel: start PPC    */
#define ADDR_FMT_SLCCMPT            0x0510ec38  /* movs r2,#1 (force)            */
#define ADDR_FMT_SLC                0x0510ecd4  /* movs r2,#1 (force, wipeSlc)   */
#define ADDR_FMT_MLC               0x0510ed96   /* movs r2,#1 (force)            */
#define ADDR_BOOTTITLE_CHK          0x0510eaee  /* bl CheckPackageHasBootTitles  */
#define ADDR_TICKET_SKIP            0x0510ef48  /* movs r1,#0x80 (start of wipe) */
#define ADDR_SHOWMENU_CALL          0x0510f88a  /* bl MCP_ShowMenu               */
#define ADDR_MCP_GETSECLEVEL        0x05059090  /* MCP IOS_GetSecurityLevel thunk*/

/* original (big-endian) instruction words used for the expect guards */
#define ORIG_STARTUP_DBGCHK         0xf039fa22
#define ORIG_DRTHREAD_DBGCHK        0xf74afb60
#define ORIG_BOOTTITLE_CHK          0xf721f8b1
#define ORIG_FMT_FORCE              0x2201      /* movs r2,#1 */
#define ORIG_TICKET_SKIP            0x2180      /* movs r1,#0x80 */
#define ORIG_MCP_GETSECLEVEL        0x477846c0  /* bx pc; nop */

/* MCP functions called at runtime from the worker thread (thumb -> +1) */
typedef int (*dr_update_fn)(const char *path, int wipe_slc);
static const dr_update_fn MCP_DR_SystemUpdateFromMedia =
    (dr_update_fn)(0x0510eaa8 | 1);
typedef int (*wait_for_dev_fn)(const char *dev, u32 timeout_us);
static const wait_for_dev_fn wait_for_dev =
    (wait_for_dev_fn)(0x05016cd8 | 1);

/* MCP on-screen console printf (Thumb), mcpdr_misc.c */
typedef int (*drcon_printf_fn)(const char *fmt, ...);
#define DrCon_Printf ((drcon_printf_fn)(0x05110de4 | 1))

/* MCP menu state set by MCP_InitMenu (0x051114c8) */
#define MCP_MENU ((volatile u32 *)0x050bc520)

/* recovery progress/step global (.mcp.shared); == 0x13 after a full success */
#define MCP_DR_STEP   (*(volatile u32 *)0x1fe01584)
#define MCP_DR_STEP_DONE   0x13

/* ------------------------------------------------------------------ */
/* patch guards                                                        */
/* ------------------------------------------------------------------ */
static int expect32(uintptr_t addr, u32 expected)
{
    /*
     * Thumb patch sites are only 2-byte aligned and an unaligned ldr data
     * aborts at kern_main time, so read the word as two big-endian halfwords
     * (same as stroopwafel's memcpy16-based writes).
     */
    volatile u16 *p = (volatile u16 *)ios_elf_vaddr_to_paddr(addr);
    u32 val = ((u32)p[0] << 16) | p[1];
    if (val != expected) {
        debug_printf("drsetup: %08X is %08X, expected %08X\n",
                     addr, val, expected);
        return 0;
    }
    return 1;
}

static int expect16(uintptr_t addr, u16 expected)
{
    u16 val = *(u16 *)ios_elf_vaddr_to_paddr(addr);
    if (val != expected) {
        debug_printf("drsetup: %08X is %04X, expected %04X\n",
                     addr, val, expected);
        return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* worker thread: runs in MCP context after the recovery screen is up  */
/* ------------------------------------------------------------------ */
/*
 * stroopwafel's FSA_Open() calls MCP_fsaInit (0x05035294) with whatever is in
 * r0, but MCP_fsaInit(FSAIpcData **data) dereferences data when it is non-NULL
 * (crashed on hardware with r0 = an odd string pointer). Call it with NULL.
 * Each call also takes one of MCP's 8 FSA client slots (0x050a3c70) that
 * iosClose doesn't free, so open once and reuse the handle.
 */
static int recupd_fsa_open(void)
{
    return ((int (*)(void *))0x05035294)(NULL);
}

static void recupd_log(int fsa, const char *msg)
{
    int h = -1;
    if (FSA_OpenFile(fsa, RECUPD_LOG_PATH, "a", &h) == 0 && h >= 0) {
        char *buf = (char *)iosAlloc(0xCAFF, 0x100);
        if (buf) {
            int n = 0;
            while (msg[n] && n < 0xfe) { buf[n] = msg[n]; n++; }
            buf[n++] = '\n';
            FSA_WriteFile(fsa, buf, 1, n, h, 0);
            iosFree(0xCAFF, buf);
        }
        FSA_CloseFile(fsa, h);
    }
}

/* log to the SD and print on the recovery screen (MCP's text console) */
static void recupd_say(int fsa, const char *msg)
{
    debug_printf("drsetup: %s\n", msg);
    DrCon_Printf("drsetup: %s\n", msg);
    recupd_log(fsa, msg);
}

/*
 * Initial setup on the next boot. Without it the SLC still says setup was done
 * (cafe.initial_launch = 2), the Wii U Menu starts on the freshly installed
 * MLC, its first save access fails (FLUSH_QUOTA NOT_FOUND on
 * /vol/storage_mlc01/usr/save/00050010/1004a200/..., FS_VOLSTATE_FATAL) and it
 * hangs at the Wii U logo (hardware, 2026-11-08).
 * Stock recovery never resets it: only a full SLC wipe (devkit "reflash")
 * removes /vol/system_slc/proc/prefs. Deleting cafe.xml gets the same state:
 * the SCI library (SCIInitCafeSettings, e.g. IOS-ACP 0xe0043c34) recreates the
 * cafe group with version 5 and everything else 0, initial_launch included, and
 * the initial setup asks for language, country and EULA again (hardware,
 * 2026-11-08). /dev/usr_cfg isn't running in recovery mode, so the file is
 * removed through FSA.
 */
#define CAFE_XML "/vol/system_slc/proc/prefs/cafe.xml"

static void recupd_set_initial_launch(int fsa)
{
    int r = FSA_Remove(fsa, CAFE_XML);
    int f = FSA_FlushVolume(fsa, "/vol/system_slc");
    debug_printf("drsetup: remove %s: %X, flush %X\n", CAFE_XML, r, f);
    if (r == 0)
        recupd_log(fsa, "deleted cafe.xml (initial setup on next boot)");
    else if (r == (int)0xFFFCFFE9)      /* FSA "not found": already gone */
        recupd_log(fsa, "cafe.xml already absent (initial setup on next boot)");
    else
        recupd_say(fsa, "FAILED to delete cafe.xml, initial setup may not start");
}

/*
 * Region fix, run inside MCP_DR_SystemUpdateFromMedia after a successful
 * install: the "bl MCP_LoadSysProdXML" at 0x0510f04c (step 0x13, before the
 * success line and before power transitions are re-enabled) is replaced.
 * Installing the system menu (app type 0x90000001) sets system.xml
 * default_title_id (MCP_SysCfg_SetDefaultTitle in the install path), so the
 * coldboot title now is the package's Wii U Menu. Same rule as
 * wafel_setup_mlc's fix_region: title 00050010100400x0 -> region bit 1 << x,
 * written to product_area and game_region in sys_prod.xml (decimal text) when
 * they differ. Then the original MCP_LoadSysProdXML loads the fixed file.
 */
#define ADDR_LOAD_SYSPROD_CALL  0x0510f04c   /* bl MCP_LoadSysProdXML (0x05001bc8) */
#define ORIG_LOAD_SYSPROD_CALL  0xf6f2fdbc
#define SYS_PROD_XML            "/vol/system_slc/config/sys_prod.xml"
typedef void (*get_default_title_fn)(u64 *title, u64 *os);
#define MCP_SysCfg_GetDefaultTitle ((get_default_title_fn)(0x05004714 | 1))

static int g_fsa = -1;      /* worker's FSA handle, used by the region hook */

/* find "<tag ...>" and return the offset of its text value, or -1 */
static int xml_value(const char *b, int n, const char *tag)
{
    for (int i = 0; i < n; i++) {
        int k = 0;
        while (tag[k] && i + k < n && b[i + k] == tag[k]) k++;
        if (tag[k] || i + k >= n || (b[i + k] != ' ' && b[i + k] != '>'))
            continue;
        int j = i + k;
        while (j < n && b[j] != '>') j++;
        return j + 1 < n ? j + 1 : -1;
    }
    return -1;
}

static u32 xml_u32(const char *b, int n, int pos, int *len)
{
    u32 v = 0;
    int l = 0;
    while (pos + l < n && b[pos + l] >= '0' && b[pos + l] <= '9')
        v = v * 10 + (b[pos + l++] - '0');
    *len = l;
    return v;
}

static int put_u32(char *d, u32 v)
{
    char t[10];
    int n = 0;
    do { t[n++] = '0' + v % 10; v /= 10; } while (v);
    for (int i = 0; i < n; i++) d[i] = t[n - 1 - i];
    return n;
}

/* set product_area and game_region in sys_prod.xml to region (in place) */
static void sysprod_set_region(int fsa, u32 region)
{
    int h = -1;
    if (FSA_OpenFile(fsa, SYS_PROD_XML, "r", &h) || h < 0) {
        recupd_say(fsa, "region: FAILED to open sys_prod.xml");
        return;
    }
    char *in = (char *)iosAllocAligned(0xCAFF, 0x1000, 0x40);
    char *out = (char *)iosAllocAligned(0xCAFF, 0x1000, 0x40);
    int n = (in && out) ? FSA_ReadFile(fsa, in, 1, 0xfc0, h, 0) : -1;
    FSA_CloseFile(fsa, h);
    int pa = n > 0 ? xml_value(in, n, "<product_area") : -1;
    int gr = n > 0 ? xml_value(in, n, "<game_region") : -1;
    int la = 0, lg = 0;
    u32 va = pa >= 0 ? xml_u32(in, n, pa, &la) : 0;
    u32 vg = gr >= 0 ? xml_u32(in, n, gr, &lg) : 0;
    debug_printf("drsetup: sys_prod product_area %u game_region %u, wanted %u\n",
                 va, vg, region);
    if (pa < 0 || gr < 0 || !la || !lg || n >= 0xfc0) {
        recupd_say(fsa, "region: FAILED to parse sys_prod.xml, region unchanged");
    } else if (va == region && vg == region) {
        recupd_say(fsa, "region: sys_prod already matches the Wii U Menu");
    } else if (put_u32(out, region) != la || put_u32(out, region) != lg) {
        /* No rename in the FSA client, so no temp file + swap; truncating the
         * file to rewrite it could leave sys_prod.xml empty on a failed write
         * or power loss. Only same-length values are changed. */
        recupd_say(fsa, "region: new value has a different length, sys_prod NOT changed");
    } else {
        /* overwrite both digits in place ("r+", same length, never truncated) */
        put_u32(in + pa, region);
        put_u32(in + gr, region);
        int w = -1;
        h = -1;
        if (FSA_OpenFile(fsa, SYS_PROD_XML, "r+", &h) == 0 && h >= 0) {
            if (FSA_SetPosFile(fsa, h, 0) == 0)
                w = FSA_WriteFile(fsa, in, 1, n, h, 0);
            FSA_CloseFile(fsa, h);
        }
        int f = FSA_FlushVolume(fsa, "/vol/system_slc");
        debug_printf("drsetup: wrote sys_prod.xml region %u: %d of %d bytes, flush %X\n",
                     region, w, n, f);
        recupd_say(fsa, w == n ? "region: sys_prod product_area/game_region set to the Wii U Menu's region"
                               : "region: FAILED to write sys_prod.xml");
    }
    if (in) iosFree(0xCAFF, in);
    if (out) iosFree(0xCAFF, out);
}

static void recupd_fix_region(int fsa)
{
    u64 title = 0, os = 0;
    MCP_SysCfg_GetDefaultTitle(&title, &os);
    debug_printf("drsetup: coldboot title %08X%08X\n", (u32)(title >> 32), (u32)title);
    /* (wafel_setup_mlc's version of this check can't fail: "&" binds weaker than "!=") */
    if ((title & 0xFFFFFFFFFFFFF0FFull) != 0x0005001010040000ull) {
        recupd_say(fsa, "region: coldboot title is not a Wii U Menu, region unchanged");
        return;
    }
    u32 idx = (title >> 8) & 0xf;
    if (idx >= 7) {
        recupd_say(fsa, "region: unknown Wii U Menu region, region unchanged");
        return;
    }
    sysprod_set_region(fsa, 1u << idx);
}

/*
 * MCP sets the step to 0x13 BEFORE this MCP_LoadSysProdXML call, and a failing
 * load makes the update fail with the step still 0x13. So the worker also needs
 * this call's result: 1 = not reached, else the MCP_LoadSysProdXML return value.
 */
static int g_sysprod_hooked;
static volatile int g_sysprod_load = 1;

static int recupd_loadsysprod(int r0, int r1, int r2, int r3, int (*org)(void))
{
    (void)r0; (void)r1; (void)r2; (void)r3;
#ifndef RECUPD_INSTALL_ONLY
    if (g_fsa >= 0)
        recupd_fix_region(g_fsa);
#endif
    int ret = org();
    g_sysprod_load = ret;
    debug_printf("drsetup: MCP_LoadSysProdXML returned %d\n", ret);
    return ret;
}

static u32 recupd_main(void *arg)
{
    (void)arg;

    /* let the recovery screen and the SLC/MLC mounts settle */
    msleep(5000);
    SetNotificationLED(NOTIF_LED_BLUE | NOTIF_LED_BLUE_BLINKING);
    debug_printf("drsetup: worker start\n");

    int fsa = -1;
    for (int i = 0; i < 20 && fsa < 0; i++) {
        fsa = recupd_fsa_open();
        if (fsa < 0) msleep(500);
    }
    g_fsa = fsa;
    if (fsa < 0) {
        debug_printf("drsetup: FSA_Open failed %d\n", fsa);
        SetNotificationLED(NOTIF_LED_RED | NOTIF_LED_RED_BLINKING);
        return 0;
    }

    wait_for_dev("/dev/sdcard01", 10000000);
    int ret = FSA_Mount(fsa, "/dev/sdcard01", RECUPD_SD_MOUNT, 0, NULL, 0);
    debug_printf("drsetup: mount sd %d\n", ret);

    /* require the install folder, otherwise leave the console untouched */
    int dir = -1;
    if (FSA_OpenDir(fsa, RECUPD_INSTALL_DIR, &dir) != 0) {
        debug_printf("drsetup: no %s on SD\n", RECUPD_INSTALL_DIR);
        recupd_say(fsa, "no wafel_install folder on SD, nothing installed");
        SetNotificationLED(NOTIF_LED_ORANGE);
        FSA_Unmount(fsa, RECUPD_SD_MOUNT, 0);
        iosClose(fsa);
        return 0;   /* recovery menu stays on screen */
    }
    FSA_CloseDir(fsa, dir);
#ifdef RECUPD_NO_INSTALL
    /* test build (make NOINSTALL=1): show the recovery screen, install nothing */
    debug_printf("drsetup: NOINSTALL build, not installing\n");
    DrCon_Printf("drsetup TEST BUILD (NOINSTALL): nothing installed\n");
    SetNotificationLED(NOTIF_LED_ORANGE);
    iosClose(fsa);
    return 0;
#endif
#ifdef RECUPD_INSTALL_ONLY
    recupd_say(fsa, "install only: no format, cafe.xml and region unchanged");
#else
    recupd_set_initial_launch(fsa);
#endif

#ifdef RECUPD_REGION_TEST
    /* test build (make REGIONTEST=1): break the region (USA) so the fix after
     * the install has to write it back */
    debug_printf("drsetup: REGIONTEST build, setting sys_prod region to 2\n");
    sysprod_set_region(fsa, 2);
#endif

    recupd_log(fsa, "starting recovery reinstall from " RECUPD_INSTALL_DIR);

    /*
     * Hand the package directory to MCP's recovery reinstall. It prints
     * "Updating from ..." and the success/failure line on the TV itself,
     * force-formats (now conditional) SLCCMPT/MLC, remakes the system
     * directories, (re)loads sys_prod, and runs the staged MCP_SystemUpdate.
     * wipe_slc is hard-coded to 0: we never touch the SLC.
     */
    MCP_DR_SystemUpdateFromMedia(RECUPD_INSTALL_DIR, RECUPD_WIPE_SLC);

    u32 step = MCP_DR_STEP;
    /* step 0x13 is set before the final sys_prod load; that load must have
     * worked too (if the hook couldn't be installed, only the step is known) */
    int ok = step == MCP_DR_STEP_DONE && (!g_sysprod_hooked || g_sysprod_load == 0);
    if (step == MCP_DR_STEP_DONE && !ok)
        recupd_say(fsa, "sys_prod.xml load FAILED after the install");
    if (ok) {
        /* self-delete FIRST so the next boot is a normal boot: MCP powers off
         * right after the success line, and on hardware the power-off already
         * cut into the message that came before the delete */
        int rm = FSA_Remove(fsa, RECUPD_IPX_PATH);
        FSA_FlushVolume(fsa, RECUPD_SD_MOUNT);
        debug_printf("drsetup: success, plugin removed %X\n", rm);
        recupd_say(fsa, rm == 0 ? "system update was successful, plugin removed"
                                : "system update was successful, FAILED to remove the plugin");
        SetNotificationLED(NOTIF_LED_BLUE);
    } else {
        debug_printf("drsetup: failed at step 0x%X\n", step);
        char line[48];
        const char *p = "system update FAILED at step 0x";
        int n = 0;
        while (p[n]) { line[n] = p[n]; n++; }
        const char *hex = "0123456789abcdef";
        line[n++] = hex[(step >> 4) & 0xf];
        line[n++] = hex[step & 0xf];
        line[n] = 0;
        recupd_say(fsa, line);
        recupd_say(fsa, "plugin stays on the SD: the next boot retries");
        SetNotificationLED(NOTIF_LED_RED);
    }

    iosClose(fsa);
    return 0;
}

static void start_worker(void)
{
    u8 *stack = (u8 *)iosAllocAligned(0x0001, 0x8000, 0x20);
    if (!stack) {
        debug_printf("drsetup: worker stack alloc failed\n");
        return;
    }
    int th = iosCreateThread(recupd_main, NULL, (u32 *)(stack + 0x8000),
                             0x8000, 0x78, 1);
    if (th < 0) {
        debug_printf("drsetup: create worker failed %d\n", th);
        return;
    }
    iosStartThread(th);
}


/*
 * Recovery screen on a TV that was already set up (by minute).
 *
 * MCP_ShowMenu (0x051003a8) does BSP DISPLAY DC_INIT(1), then DC_CONFIG
 * (w, h, framebuffer 0x14001000) and draws there. On hardware DC_INIT changes
 * the video mode but the scanout stays on the surface that was active before
 * (minute's 0x17500000): DC_INIT's D1GRPH_PRIMARY_SURFACE_ADDRESS write doesn't
 * take effect, so the TV showed minute's old text in the new mode (blurred) and
 * never the menu. Copying MCP's drawing to 0x17500000 showed the menu sharp.
 *
 * Fix: kern_main reads the surface the TV scans out; the bl MCP_InitMenu in
 * MCP_ShowMenu is replaced so that MCP's screen surface uses that buffer
 * instead of DC_CONFIG's (MCP_bspRead is ARM, called with BLX, which
 * stroopwafel's Thumb blreplace can't hook).
 * MCP then draws the menu, the text console and the progress bar straight into
 * the visible buffer. If the TV wasn't scanning out a usable surface at
 * kern_main, nothing changes.
 */
#define ADDR_SHOWMENU           0x051003a8
#define ADDR_SHOWMENU_END       0x05100416
#define ADDR_MCP_INITMENU       0x051114c8
#define LT_ABIF_OFFSET          0x0d800620
#define LT_ABIF_DATA            0x0d800624
#define D1GRPH_ENABLE           0x6100
#define D1GRPH_PRIMARY_SURFACE  0x6110

static u32 tv_surface;      /* set in kern_main, 0 = leave DC_CONFIG alone */

static u32 abif_gpu_read32_k(u32 off)
{
    *(volatile u32 *)LT_ABIF_OFFSET = (off & 0xffff) | 0xc0000000;
    return *(volatile u32 *)LT_ABIF_DATA;
}

typedef void (*init_menu_fn)(s32 w, s32 h, s32 pitch, s32 format, u32 pixels);

/* replaces "bl MCP_InitMenu" in MCP_ShowMenu; pixels = DC_CONFIG framebuffer */
static void recupd_initmenu(s32 w, s32 h, s32 pitch, s32 format,
                            init_menu_fn org, u32 lr, u32 pixels)
{
    (void)lr;
    if (tv_surface && pixels != tv_surface) {
        debug_printf("drsetup: DC_CONFIG fb %08X, TV scans out %08X: drawing there\n",
                     pixels, tv_surface);
        pixels = tv_surface;
    }
    org(w, h, pitch, format, pixels);
}

/* target of a Thumb BL (bit 0 clear) or BLX (ARM target) at a, else 0 */
static u32 thumb_bl_target(uintptr_t a, int *is_blx)
{
    u16 h1 = *(volatile u16 *)ios_elf_vaddr_to_paddr(a);
    u16 h2 = *(volatile u16 *)ios_elf_vaddr_to_paddr(a + 2);
    if ((h1 & 0xf800) != 0xf000)
        return 0;
    s32 off = ((h1 & 0x7ff) << 12) | ((h2 & 0x7ff) << 1);
    if (off & 0x400000)
        off -= 0x800000;
    if ((h2 & 0xf800) == 0xf800) { *is_blx = 0; return a + 4 + off; }
    if ((h2 & 0xf800) == 0xe800) { *is_blx = 1; return ((a + 4) & ~3u) + off; }
    return 0;
}

static void patch_menu_surface(void)
{
    u32 en = abif_gpu_read32_k(D1GRPH_ENABLE);
    u32 surf = abif_gpu_read32_k(D1GRPH_PRIMARY_SURFACE) & ~0xffu;
    debug_printf("drsetup: TV D1 enable %08X surface %08X\n", en, surf);
    if ((en & 1) && surf >= 0x14000000 && surf + 1280 * 720 * 4 <= 0x1d000000)
        tv_surface = surf;

    int done = 0;
    for (uintptr_t a = ADDR_SHOWMENU; a < ADDR_SHOWMENU_END; a += 2) {
        int blx = 0;
        u32 t = thumb_bl_target(a, &blx);
        if (!t)
            continue;
        debug_printf("drsetup: MCP_ShowMenu %08X: %s %08X\n", a, blx ? "blx" : "bl", t);
        if (!blx && t == ADDR_MCP_INITMENU && !done) {
            trampoline_t_blreplace(a, recupd_initmenu);
            done = 1;
        }
        a += 2;
    }
    if (!done)
        debug_printf("drsetup: bl MCP_InitMenu not found, menu may stay invisible\n");
}

/* replaces the recovery thread's "bl MCP_ShowMenu" (0x0510f88a) */
static int recupd_showmenu(int r0, int r1, int r2, int r3, int (*org)(void))
{
    (void)r0; (void)r1; (void)r2; (void)r3;
    /* wafel_screen_log (if loaded) draws into the same buffer: from now on
     * only its bottom box, so it doesn't paint over the recovery menu or
     * re-initialise the TV after DC_INIT. Wait two of its 50 ms frames so a
     * full redraw already running has finished. */
    volatile int *scrlog_bottom = (volatile int *)wafel_find_symbol("scrlog_bottom_only");
    if (scrlog_bottom) {
        *scrlog_bottom = 1;
        msleep(100);
        debug_printf("drsetup: screen log switched to the bottom box\n");
    }
    int ret = org();
    debug_printf("drsetup: menu %ux%u fb %08X\n",
                 MCP_MENU[0x28 / 4], MCP_MENU[0x2c / 4], MCP_MENU[0x50 / 4]);
    start_worker();
    return ret;
}

/*
 * Write one Thumb halfword at kernel time. Don't use ASM_T_PATCH_K for 2-byte
 * patches: it assembles the Thumb snippet inline in this ARM function and
 * switches back to .arm without realigning, so the ARM code after a 2-byte
 * snippet is shifted by 2 bytes (undefined instruction on hardware). The old
 * four 2-byte patches only worked because they added up to 8 bytes.
 */
static void patch16_k(uintptr_t addr, u16 val)
{
    memcpy16((void *)ios_elf_vaddr_to_paddr(addr), &val, sizeof(val));
}

#ifdef RECUPD_INSTALL_ONLY
/*
 * Install only: no format anywhere. Replaces every bl MCP_FormatDevice
 * (0x050160d8) of the recovery thread's resume (slc01, mlc01, slccmpt01; the
 * ramdisk one at 0x0510f65e stays) and of MCP_DR_SystemUpdateFromMedia
 * (slccmpt, slc, mlc). A volume without a valid file system then fails at the
 * mount, the same way as a missing MLC, and nothing is formatted.
 */
static const uintptr_t format_calls[] = {
    0x0510f59e, 0x0510f5b8, 0x0510f5ce,     /* resume: slc01, mlc01, slccmpt01 */
    0x0510ec3a, 0x0510ecd6, 0x0510ed98,     /* DR update: slccmpt, slc, mlc */
};

static int recupd_no_format(const char *dev, const char *fs, int force, int r3,
                            int (*org)(const char *, const char *, int))
{
    (void)r3; (void)org;
    debug_printf("drsetup: install only, not formatting %s (%s, force %d)\n",
                 dev, fs, force);
    return 0;
}
#endif

/* ------------------------------------------------------------------ */
/* kernel-time patches                                                 */
/* ------------------------------------------------------------------ */
__attribute__((target("arm")))
void kern_main(void)
{
    debug_printf("wafel_recovery_setup kern %p\n", kern_main);

    /* don't halt the system on a kernel panic */
    ASM_PATCH_K(ADDR_KERN_PANIC, "bx lr\n");

    /*
     * MCP_Startup: force the Boot Recovery branch. Replace the call to
     * IOS_CheckDebugMode with "b 0x0501fdec" (which sets security level Test
     * and PM mode 0x40000). b.n = 0xE0A8, nop = 0x46C0.
     */
    if (expect32(ADDR_STARTUP_DBGCHK, ORIG_STARTUP_DBGCHK))
        U32_PATCH_K(ADDR_STARTUP_DBGCHK, 0xe0a846c0);

    /* MCP_StartDRThread: create the recovery thread on retail (movs r0,#0;nop) */
    if (expect32(ADDR_DRTHREAD_DBGCHK, ORIG_DRTHREAD_DBGCHK))
        U32_PATCH_K(ADDR_DRTHREAD_DBGCHK, 0x200046c0);

    /* never start the PPC, so Cafe OS cannot take over the display */
    ASM_T_PATCH_K(ADDR_NO_PPC, "movs r0, #0\nnop\n");

#ifdef RECUPD_FORCE_TEST_LEVEL
    /*
     * MCP's IOS_GetSecurityLevel thunk -> return Test (10) directly. On retail
     * IOS_SetSecurityLevel is a no-op (kernel 0x081299e8 only writes the level
     * when KernelCheckDebugMode()==0), so the level stays Normal and the update
     * engine (FUN_0502ffc4) skips titles already installed at an equal/newer
     * version. Returning Test here makes MCP always reinstall. movs r0,#0xa; bx lr.
     */
    if (expect32(ADDR_MCP_GETSECLEVEL, ORIG_MCP_GETSECLEVEL))
        U32_PATCH_K(ADDR_MCP_GETSECLEVEL, 0x200a4770);
#endif

#ifdef RECUPD_INSTALL_ONLY
    for (unsigned i = 0; i < sizeof(format_calls) / sizeof(format_calls[0]); i++) {
        int blx = 0;
        if (thumb_bl_target(format_calls[i], &blx) == 0x050160d8 && !blx)
            trampoline_t_blreplace(format_calls[i], recupd_no_format);
        else
            debug_printf("drsetup: %08X is not bl MCP_FormatDevice\n", format_calls[i]);
    }
#elif !defined(RECUPD_FORCE_FORMAT)
    /* make the recovery reinstall's formats conditional (force=false) */
    if (expect16(ADDR_FMT_SLCCMPT, ORIG_FMT_FORCE))
        patch16_k(ADDR_FMT_SLCCMPT, 0x2200);   /* movs r2,#0 */
    if (expect16(ADDR_FMT_MLC, ORIG_FMT_FORCE))
        patch16_k(ADDR_FMT_MLC, 0x2200);
    if (expect16(ADDR_FMT_SLC, ORIG_FMT_FORCE))
        patch16_k(ADDR_FMT_SLC, 0x2200);
#endif

    /* always skip ticket deletion: b 0x0510f0c0 (0xE0BA) */
    if (expect16(ADDR_TICKET_SKIP, ORIG_TICKET_SKIP))
        patch16_k(ADDR_TICKET_SKIP, 0xe0ba);

#ifdef RECUPD_SKIP_BOOTTITLE_CHECK
    /* allow any set of WUPs (skip the full-system-package check) */
    if (expect32(ADDR_BOOTTITLE_CHK, ORIG_BOOTTITLE_CHK))
        U32_PATCH_K(ADDR_BOOTTITLE_CHK, 0x200046c0);          /* movs r0,#0;nop */
#endif

    /* fix the region in sys_prod after the install, before MCP reloads it */
    if (expect32(ADDR_LOAD_SYSPROD_CALL, ORIG_LOAD_SYSPROD_CALL)) {
        trampoline_t_blreplace(ADDR_LOAD_SYSPROD_CALL, recupd_loadsysprod);
        g_sysprod_hooked = 1;
    }

    /*
     * Recovery screen in 1080p like minute: MCP_ShowMenu passes DC_INIT id 1
     * (movs r3,#1 at 0x051003c4), whose BSP tables (dc1) program 480p timing
     * (H/V total 858x525), so the 1280x720 surface is scaled down and thin text
     * gets blurry. Id 0 (dc0) is 1080p (2200x1125), same PLLs, same 1280x720
     * surface, the same timing minute uses.
     */
    if (expect16(0x051003c4, 0x2301))
        patch16_k(0x051003c4, 0x2300);      /* movs r3,#0 */

    /* draw the recovery screen into the surface the TV really scans out */
    patch_menu_surface();

    /* spawn the worker just before the recovery screen is drawn */
    trampoline_t_blreplace(ADDR_SHOWMENU_CALL, recupd_showmenu);
}

void mcp_main(void) { }
