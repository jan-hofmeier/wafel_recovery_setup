/*
 * wafel_recovery_update (5recupd.ipx)
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
        debug_printf("wafel_recupd: %08X is %08X, expected %08X\n",
                     addr, val, expected);
        return 0;
    }
    return 1;
}

static int expect16(uintptr_t addr, u16 expected)
{
    u16 val = *(u16 *)ios_elf_vaddr_to_paddr(addr);
    if (val != expected) {
        debug_printf("wafel_recupd: %08X is %04X, expected %04X\n",
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

/*
 * cafe.initial_launch = 0 so the next boot runs the initial setup. Without it
 * the SLC still says setup was done (value 2), the Wii U Menu starts on the
 * freshly formatted MLC, its first save access fails (FLUSH_QUOTA NOT_FOUND on
 * /vol/storage_mlc01/usr/save/00050010/1004a200/..., FS_VOLSTATE_FATAL) and it
 * hangs at the Wii U logo (hardware, 2026-11-08).
 * /dev/usr_cfg isn't running in recovery mode (open/read/write give -6), so
 * edit the backing file /vol/system_slc/proc/prefs/cafe.xml directly: the
 * one-digit value is replaced in place (same length, opened "r+"), then the
 * SLC is flushed.
 */
#define CAFE_XML "/vol/system_slc/proc/prefs/cafe.xml"

static void recupd_set_initial_launch(int fsa)
{
    int h = -1;
    int r = FSA_OpenFile(fsa, CAFE_XML, "r+", &h);
    if (r || h < 0) {
        debug_printf("wafel_recupd: open %s: %X\n", CAFE_XML, r);
        recupd_log(fsa, "FAILED to open cafe.xml, initial setup not reset");
        return;
    }
    char *buf = (char *)iosAllocAligned(0xCAFF, 0x2000, 0x40);
    const char *msg = "FAILED to set initial_launch in cafe.xml";
    if (buf) {
        int n = FSA_ReadFile(fsa, buf, 1, 0x1fff, h, 0);
        int pos = -1;
        if (n > 0) {
            buf[n] = 0;
            const char *tag = "<initial_launch";
            for (int i = 0; i < n && pos < 0; i++) {
                int k = 0;
                while (tag[k] && buf[i + k] == tag[k]) k++;
                if (!tag[k]) {
                    int j = i + k;
                    while (j < n && buf[j] != '>') j++;
                    if (j + 2 < n && buf[j + 2] == '<')
                        pos = j + 1;            /* single-digit value */
                }
            }
        }
        if (pos < 0) {
            debug_printf("wafel_recupd: initial_launch not found in cafe.xml (%d bytes)\n", n);
        } else if (buf[pos] == '0') {
            debug_printf("wafel_recupd: cafe.xml initial_launch already 0\n");
            msg = "cafe.xml initial_launch already 0";
        } else {
            debug_printf("wafel_recupd: cafe.xml initial_launch %c -> 0\n", buf[pos]);
            buf[pos] = '0';
            int w = -1;
            if (FSA_SetPosFile(fsa, h, 0) == 0)
                w = FSA_WriteFile(fsa, buf, 1, n, h, 0);
            debug_printf("wafel_recupd: rewrote cafe.xml: %d of %d bytes\n", w, n);
            if (w == n)
                msg = "set cafe.xml initial_launch = 0 (initial setup on next boot)";
        }
        iosFree(0xCAFF, buf);
    }
    FSA_CloseFile(fsa, h);
    r = FSA_FlushVolume(fsa, "/vol/system_slc");
    debug_printf("wafel_recupd: flush /vol/system_slc %X\n", r);
    recupd_log(fsa, msg);
}

static u32 recupd_main(void *arg)
{
    (void)arg;

    /* let the recovery screen and the SLC/MLC mounts settle */
    msleep(5000);
    SetNotificationLED(NOTIF_LED_BLUE | NOTIF_LED_BLUE_BLINKING);
    debug_printf("wafel_recupd: worker start\n");

    int fsa = -1;
    for (int i = 0; i < 20 && fsa < 0; i++) {
        fsa = recupd_fsa_open();
        if (fsa < 0) msleep(500);
    }
    if (fsa < 0) {
        debug_printf("wafel_recupd: FSA_Open failed %d\n", fsa);
        SetNotificationLED(NOTIF_LED_RED | NOTIF_LED_RED_BLINKING);
        return 0;
    }

    wait_for_dev("/dev/sdcard01", 10000000);
    int ret = FSA_Mount(fsa, "/dev/sdcard01", RECUPD_SD_MOUNT, 0, NULL, 0);
    debug_printf("wafel_recupd: mount sd %d\n", ret);

    /* require the install folder, otherwise leave the console untouched */
    int dir = -1;
    if (FSA_OpenDir(fsa, RECUPD_INSTALL_DIR, &dir) != 0) {
        debug_printf("wafel_recupd: no %s on SD\n", RECUPD_INSTALL_DIR);
        recupd_log(fsa, "no wafel_install folder on SD; nothing to do");
        SetNotificationLED(NOTIF_LED_ORANGE);
        FSA_Unmount(fsa, RECUPD_SD_MOUNT, 0);
        iosClose(fsa);
        return 0;   /* recovery menu stays on screen */
    }
    FSA_CloseDir(fsa, dir);
    recupd_set_initial_launch(fsa);

#ifdef RECUPD_NO_INSTALL
    /* test build (make NOINSTALL=1): show the recovery screen, install nothing */
    debug_printf("wafel_recupd: NOINSTALL build, not installing\n");
    SetNotificationLED(NOTIF_LED_ORANGE);
    iosClose(fsa);
    return 0;
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
    if (step == MCP_DR_STEP_DONE) {
        debug_printf("wafel_recupd: success\n");
        recupd_log(fsa, "system update was successful");
        /* self-delete so the next boot is a normal boot */
        FSA_Remove(fsa, RECUPD_IPX_PATH);
        FSA_FlushVolume(fsa, RECUPD_SD_MOUNT);
        SetNotificationLED(NOTIF_LED_BLUE);
    } else {
        debug_printf("wafel_recupd: failed at step 0x%X\n", step);
        char line[48];
        const char *p = "system update FAILED at step 0x";
        int n = 0;
        while (p[n]) { line[n] = p[n]; n++; }
        const char *hex = "0123456789abcdef";
        line[n++] = hex[(step >> 4) & 0xf];
        line[n++] = hex[step & 0xf];
        line[n] = 0;
        recupd_log(fsa, line);
        SetNotificationLED(NOTIF_LED_RED);
    }

    iosClose(fsa);
    return 0;
}

static void start_worker(void)
{
    u8 *stack = (u8 *)iosAllocAligned(0x0001, 0x8000, 0x20);
    if (!stack) {
        debug_printf("wafel_recupd: worker stack alloc failed\n");
        return;
    }
    int th = iosCreateThread(recupd_main, NULL, (u32 *)(stack + 0x8000),
                             0x8000, 0x78, 1);
    if (th < 0) {
        debug_printf("wafel_recupd: create worker failed %d\n", th);
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
        debug_printf("wafel_recupd: DC_CONFIG fb %08X, TV scans out %08X: drawing there\n",
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
    debug_printf("wafel_recupd: TV D1 enable %08X surface %08X\n", en, surf);
    if ((en & 1) && surf >= 0x14000000 && surf + 1280 * 720 * 4 <= 0x1d000000)
        tv_surface = surf;

    int done = 0;
    for (uintptr_t a = ADDR_SHOWMENU; a < ADDR_SHOWMENU_END; a += 2) {
        int blx = 0;
        u32 t = thumb_bl_target(a, &blx);
        if (!t)
            continue;
        debug_printf("wafel_recupd: MCP_ShowMenu %08X: %s %08X\n", a, blx ? "blx" : "bl", t);
        if (!blx && t == ADDR_MCP_INITMENU && !done) {
            trampoline_t_blreplace(a, recupd_initmenu);
            done = 1;
        }
        a += 2;
    }
    if (!done)
        debug_printf("wafel_recupd: bl MCP_InitMenu not found, menu may stay invisible\n");
}

/* replaces the recovery thread's "bl MCP_ShowMenu" (0x0510f88a) */
static int recupd_showmenu(int r0, int r1, int r2, int r3, int (*org)(void))
{
    (void)r0; (void)r1; (void)r2; (void)r3;
    int ret = org();
    debug_printf("wafel_recupd: menu %ux%u fb %08X\n",
                 MCP_MENU[0x28 / 4], MCP_MENU[0x2c / 4], MCP_MENU[0x50 / 4]);
    start_worker();
    return ret;
}

/* ------------------------------------------------------------------ */
/* kernel-time patches                                                 */
/* ------------------------------------------------------------------ */
__attribute__((target("arm")))
void kern_main(void)
{
    debug_printf("wafel_recovery_update kern %p\n", kern_main);

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

#ifndef RECUPD_FORCE_FORMAT
    /* make the recovery reinstall's formats conditional (force=false) */
    if (expect16(ADDR_FMT_SLCCMPT, ORIG_FMT_FORCE))
        ASM_T_PATCH_K(ADDR_FMT_SLCCMPT, ".short 0x2200\n");   /* movs r2,#0 */
    if (expect16(ADDR_FMT_MLC, ORIG_FMT_FORCE))
        ASM_T_PATCH_K(ADDR_FMT_MLC, ".short 0x2200\n");
    if (expect16(ADDR_FMT_SLC, ORIG_FMT_FORCE))
        ASM_T_PATCH_K(ADDR_FMT_SLC, ".short 0x2200\n");
#endif

    /* always skip ticket deletion: b 0x0510f0c0 (0xE0BA) */
    if (expect16(ADDR_TICKET_SKIP, ORIG_TICKET_SKIP))
        ASM_T_PATCH_K(ADDR_TICKET_SKIP, ".short 0xe0ba\n");

#ifdef RECUPD_SKIP_BOOTTITLE_CHECK
    /* allow any set of WUPs (skip the full-system-package check) */
    if (expect32(ADDR_BOOTTITLE_CHK, ORIG_BOOTTITLE_CHK))
        U32_PATCH_K(ADDR_BOOTTITLE_CHK, 0x200046c0);          /* movs r0,#0;nop */
#endif

    /* draw the recovery screen into the surface the TV really scans out */
    patch_menu_surface();

    /* spawn the worker just before the recovery screen is drawn */
    trampoline_t_blreplace(ADDR_SHOWMENU_CALL, recupd_showmenu);
}

void mcp_main(void) { }
