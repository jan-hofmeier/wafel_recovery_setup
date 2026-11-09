/*
 * Yes/no prompts on the recovery screen, answered with the EJECT button.
 *
 * The SMC reports buttons in register 0x41 (BSP "SMC" "SystemEventFlag",
 * bits: 0x40 POWER, 0x20 EJECT), cleared on read. MCP reads it on every
 * SysEvent interrupt in MCP_HandleWakeEvents (0x05028508, called from the
 * ShutdownRebootRelaunch thread). Hardware: during PM resume (where the
 * format prompts run) that handler isn't serving the interrupt yet; a press
 * stays latched and MCP only reads it after resume. So the prompt polls the
 * register itself through MCP's own MCP_bspRead. For prompts after resume
 * MCP's handler may read first, so its read (bl MCP_bspRead at 0x05028556)
 * is wrapped: while a prompt is open the events it reads are handed to the
 * prompt and hidden from MCP (in recovery mode MCP would restart on POWER).
 *
 * EJECT = continue. There is no "no": the user holds POWER, which the SMC
 * turns into a hard power-off. A clean power-off isn't possible here (hardware:
 * MCP_RequestOff during PM resume -> "MCP: Power transitions are explicitly
 * disabled.", -31). A short POWER press is swallowed: MCP would restart into
 * recovery and ask again.
 */#include <wafel/types.h>
#include <wafel/utils.h>
#include <wafel/trampoline.h>
#include <wafel/ios/thread.h>

#include "mcp.h"
#include "display.h"
#include "prompt.h"

#define ADDR_WAKE_EVENT_READ   0x05028556  /* MCP_HandleWakeEvents: read SystemEventFlag */
#define SMC_EVENT_POWER        0x40
#define SMC_EVENT_EJECT        0x20

static volatile int prompt_open;
static volatile u8 hooked_events;       /* events MCP's handler read during a prompt */

typedef int (*bsp_read_fn)(const char *entity, u32 instance, const char *attribute,
                           u32 size, void *out);
#define MCP_bspRead ((bsp_read_fn)(0x05059568 | 1))

/* read (and clear) the SMC event flags; 0 on error */
static u8 read_events(void)
{
    u8 ev = 0;
    if (MCP_bspRead("SMC", 0, "SystemEventFlag", 1, &ev) != 0)
        return 0;
    if (ev)
        debug_printf("drsetup: prompt read SMC events %02X\n", ev);
    return ev;
}

static int drsetup_wake_event_read(const char *entity, u32 instance, const char *attribute,
                                   u32 size, bsp_read_fn org, u32 lr, u8 *out)
{
    (void)lr;
    int r = org(entity, instance, attribute, size, out);
    if (r == 0 && prompt_open && (*out & (SMC_EVENT_EJECT | SMC_EVENT_POWER))) {
        hooked_events |= *out & (SMC_EVENT_EJECT | SMC_EVENT_POWER);
        *out &= ~(SMC_EVENT_EJECT | SMC_EVENT_POWER);
    }
    return r;
}

void prompt_kern_patches(void)
{
    trampoline_t_blreplace(ADDR_WAKE_EVENT_READ, drsetup_wake_event_read);
}

void drsetup_confirm(const char *question)
{
    display_show_early();
    read_events();                      /* drop presses latched before the prompt */
    hooked_events = 0;
    prompt_open = 1;
    DrCon_Printf("\n%s\nPress EJECT to continue. Hold POWER to cancel and power off.\n", question);
    debug_printf("drsetup: prompt: %s\n", question);

    while (!((read_events() | hooked_events) & SMC_EVENT_EJECT))
        msleep(100);
    prompt_open = 0;

    DrCon_Printf("EJECT pressed: continuing.\n");
    debug_printf("drsetup: prompt confirmed\n");
}
