/*
 * Install thread (MCP): mounts the SD, prepares the SLC, runs MCP's recovery
 * reinstall from DRSETUP_INSTALL_DIR and reports the result.
 */
#include <wafel/types.h>
#include <wafel/utils.h>
#include <wafel/ios/svc.h>
#include <wafel/ios/thread.h>
#include <wafel/services/fsa.h>

#include "config.h"
#include "mcp.h"
#include "led.h"
#include "log.h"
#include "slc.h"
#include "install.h"

static void report_failure(u32 step)
{
    char line[48];
    const char *p = "system update FAILED at step 0x";
    const char *hex = "0123456789abcdef";
    int n = 0;
    while (p[n]) { line[n] = p[n]; n++; }
    line[n++] = hex[(step >> 4) & 0xf];
    line[n++] = hex[step & 0xf];
    line[n] = 0;
    drsetup_say(line);
    drsetup_say("plugin stays on the SD: the next boot retries");
    SetNotificationLED(NOTIF_LED_RED);
}

static u32 install_main(void *arg)
{
    (void)arg;

    /* let the recovery screen and the SLC/MLC mounts settle */
    msleep(5000);
    SetNotificationLED(NOTIF_LED_BLUE | NOTIF_LED_BLUE_BLINKING);
    debug_printf("drsetup: install thread start\n");

    int fsa = -1;
    for (int i = 0; i < 20 && fsa < 0; i++) {
        fsa = drsetup_fsa_open();
        if (fsa < 0) msleep(500);
    }
    if (fsa < 0) {
        debug_printf("drsetup: FSA open failed %d\n", fsa);
        SetNotificationLED(NOTIF_LED_RED | NOTIF_LED_RED_BLINKING);
        return 0;
    }
    drsetup_fsa = fsa;

    MCP_WaitForDev("/dev/sdcard01", 10000000);
    int ret = FSA_Mount(fsa, "/dev/sdcard01", DRSETUP_SD_MOUNT, 0, NULL, 0);
    debug_printf("drsetup: mount sd %d\n", ret);

    /* require the install folder, otherwise leave the console untouched */
    int dir = -1;
    if (FSA_OpenDir(fsa, DRSETUP_INSTALL_DIR, &dir) != 0) {
        drsetup_say("no wafel_install folder on SD, nothing installed");
        SetNotificationLED(NOTIF_LED_ORANGE);
        FSA_Unmount(fsa, DRSETUP_SD_MOUNT, 0);
        iosClose(fsa);
        return 0;   /* recovery menu stays on screen */
    }
    FSA_CloseDir(fsa, dir);

#ifdef DRSETUP_NO_INSTALL
    /* test build: show the recovery screen, change nothing */
    debug_printf("drsetup: NOINSTALL build, not installing\n");
    DrCon_Printf("drsetup TEST BUILD (NOINSTALL): nothing installed\n");
    SetNotificationLED(NOTIF_LED_ORANGE);
    iosClose(fsa);
    return 0;
#endif
#ifdef DRSETUP_DELETE_TEST
    slc_delete_test();
#endif
#ifdef DRSETUP_INSTALL_ONLY
    drsetup_say("install only: no format, cafe.xml and region unchanged");
#else
    slc_reset_initial_setup();
#endif
#ifdef DRSETUP_REGION_TEST
    /* test build: break the region (USA) so the fix after the install has to
     * write it back */
    debug_printf("drsetup: REGIONTEST build, setting sys_prod region to 2\n");
    slc_set_region(2);
#endif

    slc_prepare();
    drsetup_log("starting recovery reinstall from " DRSETUP_INSTALL_DIR);

    /*
     * MCP's recovery reinstall: prints "Updating from ..." and the result on
     * the TV, formats SLCCMPT/MLC if allowed (see main.c), remakes the system
     * directories, (re)loads sys_prod and runs the staged MCP_SystemUpdate.
     */
    MCP_DR_SystemUpdateFromMedia(DRSETUP_INSTALL_DIR, DRSETUP_WIPE_SLC);

    /* step 0x13 is set before the final sys_prod load, which must work too */
    u32 step = MCP_DR_STEP;
    int ok = step == MCP_DR_STEP_DONE && slc_sysprod_load_result() == 0;
    if (step == MCP_DR_STEP_DONE && !ok)
        drsetup_say("sys_prod.xml load FAILED after the install");
    if (ok) {
        /* self-delete FIRST: MCP powers off right after its success line */
        int rm = FSA_Remove(fsa, DRSETUP_IPX_PATH);
        FSA_FlushVolume(fsa, DRSETUP_SD_MOUNT);
        debug_printf("drsetup: success, plugin removed %X\n", rm);
        drsetup_say(rm == 0 ? "system update was successful, plugin removed"
                            : "system update was successful, FAILED to remove the plugin");
        SetNotificationLED(NOTIF_LED_BLUE);
    } else {
        debug_printf("drsetup: failed at step 0x%X\n", step);
        report_failure(step);
    }

    iosClose(fsa);
    return 0;
}

void install_start(void)
{
    u8 *stack = (u8 *)iosAllocAligned(0x0001, 0x8000, 0x20);
    if (!stack) {
        debug_printf("drsetup: install thread stack alloc failed\n");
        return;
    }
    int th = iosCreateThread(install_main, NULL, (u32 *)(stack + 0x8000), 0x8000, 0x78, 1);
    if (th < 0) {
        debug_printf("drsetup: create install thread failed %d\n", th);
        return;
    }
    iosStartThread(th);
}
