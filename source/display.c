/*
 * The MCP recovery screen on the TV.
 *
 * MCP_ShowMenu (0x051003a8) does BSP DISPLAY DC_INIT, then DC_CONFIG (w, h,
 * framebuffer 0x14001000) and draws there. Two problems on hardware:
 *  - DC_INIT changes the video mode but the scanout stays on the surface that
 *    was active before (minute's 0x17500000): the new surface address is
 *    written but never takes effect. Fix: kern_main reads the surface the TV
 *    scans out, and the bl MCP_InitMenu in MCP_ShowMenu is replaced so MCP's
 *    screen uses that buffer. The text console and progress bar are cut out of
 *    the same surface, so they become visible too. If the TV wasn't scanning
 *    out a usable surface at kern_main, nothing changes.
 *  - MCP passes DC_INIT id 1, whose BSP tables (dc1) program 480p (H/V total
 *    858x525): the 1280x720 surface is scaled down and thin text is blurry.
 *    Id 0 (dc0) is 1080p (2200x1125), same PLLs and surface, minute's timing.
 */
#include <wafel/types.h>
#include <wafel/utils.h>
#include <wafel/dynamic.h>
#include <wafel/patch.h>
#include <wafel/trampoline.h>
#include <wafel/ios/svc.h>
#include <wafel/ios/thread.h>

#include "mcp.h"
#include "install.h"
#include "display.h"

#define ADDR_SHOWMENU_CALL      0x0510f88a  /* recovery thread: bl MCP_ShowMenu */
#define ADDR_INITMENU_CALL      0x051003fe  /* MCP_ShowMenu: bl MCP_InitMenu */
#define ADDR_DC_INIT_ID         0x051003c4  /* MCP_ShowMenu: movs r3,#1 (DC_INIT id) */

#define LT_ABIF_OFFSET          0x0d800620
#define LT_ABIF_DATA            0x0d800624
#define D1GRPH_ENABLE           0x6100
#define D1GRPH_PRIMARY_SURFACE  0x6110

static u32 tv_surface;      /* set in kern_main, 0 = keep DC_CONFIG's buffer */

static u32 abif_gpu_read32_k(u32 off)
{
    *(volatile u32 *)LT_ABIF_OFFSET = (off & 0xffff) | 0xc0000000;
    return *(volatile u32 *)LT_ABIF_DATA;
}

typedef void (*init_menu_fn)(s32 w, s32 h, s32 pitch, s32 format, u32 pixels);

/* replaces "bl MCP_InitMenu"; pixels = DC_CONFIG's framebuffer */
static void drsetup_initmenu(s32 w, s32 h, s32 pitch, s32 format,
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

/* replaces the recovery thread's "bl MCP_ShowMenu" */
static int drsetup_showmenu(int r0, int r1, int r2, int r3, int (*org)(void))
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
    install_start();
    return ret;
}

void display_kern_patches(void)
{
    u32 en = abif_gpu_read32_k(D1GRPH_ENABLE);
    u32 surf = abif_gpu_read32_k(D1GRPH_PRIMARY_SURFACE) & ~0xffu;
    debug_printf("drsetup: TV D1 enable %08X surface %08X\n", en, surf);
    if ((en & 1) && surf >= 0x14000000 && surf + 1280 * 720 * 4 <= 0x1d000000)
        tv_surface = surf;

    ASM_T_PATCH_K(ADDR_DC_INIT_ID, "movs r3, #0\n");        /* DC_INIT id 0: 1080p */
    trampoline_t_blreplace(ADDR_INITMENU_CALL, drsetup_initmenu);
    trampoline_t_blreplace(ADDR_SHOWMENU_CALL, drsetup_showmenu);
}
