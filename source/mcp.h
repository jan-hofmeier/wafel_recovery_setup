#ifndef DRSETUP_MCP_H
#define DRSETUP_MCP_H

/*
 * IOS-MCP functions and data used at runtime (IOSU 5.5.x, fw.elf
 * 00050010-1000400A). Thumb functions are called with bit 0 set.
 */
#include <wafel/types.h>

/* MCP_fsaInit(FSAIpcData **data): opens /dev/fsa and registers the client */
#define MCP_fsaInit \
    ((int (*)(void *))0x05035294)
/* MCP_DR_SystemUpdateFromMedia(root, wipeSlc): the recovery reinstall */
#define MCP_DR_SystemUpdateFromMedia \
    ((int (*)(const char *root, int wipe_slc))(0x0510eaa8 | 1))
#define MCP_WaitForDev \
    ((int (*)(const char *dev, u32 timeout_us))(0x05016cd8 | 1))
/* recovery screen text console printf */
#define DrCon_Printf \
    ((int (*)(const char *fmt, ...))(0x05110de4 | 1))
#define MCP_SysCfg_GetDefaultTitle \
    ((void (*)(u64 *title, u64 *os))(0x05004714 | 1))
/* writes MCP's default system.xml (stock: only after a full SLC wipe) */
#define MCP_WriteDefaultSystemConfig \
    ((void (*)(u32 overwrite))(0x050049c0 | 1))
/* MCP_fsaGetVolumeInfo(handle, device, info): FSA volume info of a device
 * (thunk); MCP_FSA_HANDLE is MCP's own /dev/fsa handle. FSAVolumeInfo is
 * 0x1bc bytes, file system name (8 chars) at +0x24 (see MCP_FormatDevice
 * 0x050160d8, which compares it with the requested one unless force). */
#define MCP_fsaGetVolumeInfo \
    ((int (*)(int handle, const char *device, void *info))(0x05059170 | 1))
#define MCP_FSA_HANDLE      (*(volatile int *)0x05070184)
#define FSA_VOLINFO_SIZE    0x1bc
#define FSA_VOLINFO_FS      0x24
/* ES: merge a cert chain into /sys/cert.sys (ARM) */
#define esMergeCertSys \
    ((int (*)(const void *certs, u32 size))0x05050448)

/* recovery screen surface set by MCP_InitMenu (0x051114c8):
 * +0x28 w, +0x2c h, +0x48 pitch, +0x50 pixels */
#define MCP_MENU ((volatile u32 *)0x050bc520)

/* recovery progress step (.mcp.shared); 0x13 is set before the final
 * MCP_LoadSysProdXML of a successful update */
#define MCP_DR_STEP      (*(volatile u32 *)0x1fe01584)
#define MCP_DR_STEP_DONE 0x13

/* FSA status "not found" */
#define FSA_STATUS_NOT_FOUND ((int)0xFFFCFFE9)

#endif /* DRSETUP_MCP_H */
