#include <wafel/types.h>
#include <wafel/utils.h>
#include <wafel/ios/svc.h>
#include <wafel/services/fsa.h>

#include "config.h"
#include "mcp.h"
#include "log.h"

int drsetup_fsa = -1;

/*
 * stroopwafel's FSA_Open() calls MCP_fsaInit (0x05035294) with whatever is in
 * r0, but MCP_fsaInit(FSAIpcData **data) dereferences data when it is non-NULL
 * (crashed on hardware with r0 = an odd string pointer). Call it with NULL.
 * Each call also takes one of MCP's 8 FSA client slots (0x050a3c70) that
 * iosClose doesn't free, so open once and reuse the handle.
 */
int drsetup_fsa_open(void)
{
    return MCP_fsaInit(NULL);
}

void drsetup_log(const char *msg)
{
    int h = -1;
    if (drsetup_fsa < 0)
        return;
    if (FSA_OpenFile(drsetup_fsa, DRSETUP_LOG_PATH, "a", &h) == 0 && h >= 0) {
        char *buf = (char *)iosAlloc(0xCAFF, 0x100);
        if (buf) {
            int n = 0;
            while (msg[n] && n < 0xfe) { buf[n] = msg[n]; n++; }
            buf[n++] = '\n';
            FSA_WriteFile(drsetup_fsa, buf, 1, n, h, 0);
            iosFree(0xCAFF, buf);
        }
        FSA_CloseFile(drsetup_fsa, h);
    }
}

void drsetup_say(const char *msg)
{
    debug_printf("drsetup: %s\n", msg);
    DrCon_Printf("drsetup: %s\n", msg);
    drsetup_log(msg);
}

int drsetup_read_file(const char *path, char *buf, int max)
{
    int h = -1;
    int r = FSA_OpenFile(drsetup_fsa, path, "r", &h);
    if (r || h < 0)
        return r ? r : -1;
    int n = FSA_ReadFile(drsetup_fsa, buf, 1, max, h, 0);
    FSA_CloseFile(drsetup_fsa, h);
    return n;
}
