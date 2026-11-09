/*
 * SLC content the reinstall and the next boot need
 * (IOSU notes: docs/slc_minimum_for_recovery.md).
 */
#include <string.h>
#include <wafel/types.h>
#include <wafel/utils.h>
#include <wafel/trampoline.h>
#include <wafel/patch.h>
#include <wafel/ios/svc.h>
#include <wafel/services/fsa.h>

#include "config.h"
#include "mcp.h"
#include "log.h"
#include "slc.h"

#define CAFE_XML      "/vol/system_slc/proc/prefs/cafe.xml"
#define SYSTEM_XML    "/vol/system_slc/config/system.xml"
#define SYS_PROD_XML  "/vol/system_slc/config/sys_prod.xml"
#define CERT_SYS      "/vol/system_slc/rights/sys/cert.sys"

#define ADDR_LOAD_SYSPROD_CALL  0x0510f04c  /* DR update: bl MCP_LoadSysProdXML */

/* ------------------------------------------------------------------ */
/* initial setup                                                       */
/* ------------------------------------------------------------------ */
/*
 * Without this the SLC still says setup was done (cafe.initial_launch = 2), the
 * Wii U Menu starts on the freshly installed MLC, its first save access fails
 * (FLUSH_QUOTA NOT_FOUND, FS_VOLSTATE_FATAL) and it hangs at the Wii U logo
 * (hardware). Stock recovery never resets it: only a full SLC wipe (devkit
 * "reflash") removes /vol/system_slc/proc/prefs. Deleting cafe.xml gets the
 * same state: the SCI library (SCIInitCafeSettings, e.g. IOS-ACP 0xe0043c34)
 * recreates the cafe group with version 5 and everything else 0, and the
 * initial setup asks for language, country and EULA again (hardware).
 * /dev/usr_cfg isn't running in recovery mode, so the file is removed via FSA.
 */
void slc_reset_initial_setup(void)
{
    int r = FSA_Remove(drsetup_fsa, CAFE_XML);
    int f = FSA_FlushVolume(drsetup_fsa, "/vol/system_slc");
    debug_printf("drsetup: remove %s: %X, flush %X\n", CAFE_XML, r, f);
    if (r == 0)
        drsetup_log("deleted cafe.xml (initial setup on next boot)");
    else if (r == FSA_STATUS_NOT_FOUND)
        drsetup_log("cafe.xml already absent (initial setup on next boot)");
    else
        drsetup_say("FAILED to delete cafe.xml, initial setup may not start");
}

/* ------------------------------------------------------------------ */
/* system.xml and cert.sys                                             */
/* ------------------------------------------------------------------ */
/*
 *  - system.xml: MCP_LoadSysconfig has no fallback; the Menu install would
 *    write a mostly-zero one. If it doesn't exist, write MCP's own defaults
 *    with MCP_WriteDefaultSystemConfig(1).
 *  - cert.sys: ES creates it empty, but TMD verification needs CP0000000b from
 *    it ("Failed to find cert, Root-CA00000003-CP0000000b", hardware). Every
 *    WUP's title.cert holds the three certs (CA00000003, CP0000000b,
 *    XS0000000c). If one is missing, merge a package title.cert with ES's own
 *    esMergeCertSys (fills an empty store or adds the missing certs through
 *    /tmp/cert.sys + rename). It needs the file to exist (ES creates it at
 *    startup); otherwise the file is written directly.
 */
static int has_str(const char *b, int n, const char *s)
{
    int l = 0;
    while (s[l]) l++;
    for (int i = 0; i + l <= n; i++) {
        int k = 0;
        while (k < l && b[i + k] == s[k]) k++;
        if (k == l)
            return 1;
    }
    return 0;
}

static int certs_complete(const char *b, int n)
{
    return n > 0 && has_str(b, n, "CA00000003") && has_str(b, n, "CP0000000b")
        && has_str(b, n, "XS0000000c");
}

static void path_join(char *out, int max, const char *a, const char *b, const char *c)
{
    const char *parts[] = { a, b, c };
    int o = 0;
    for (int p = 0; p < 3; p++)
        for (const char *q = parts[p]; *q && o < max - 1; q++)
            out[o++] = *q;
    out[o] = 0;
}

static void prepare_system_xml(char *buf)
{
    int n = drsetup_read_file(SYSTEM_XML, buf, 0x4000);
    if (n != FSA_STATUS_NOT_FOUND)
        return;
    MCP_WriteDefaultSystemConfig(1);
    FSA_FlushVolume(drsetup_fsa, "/vol/system_slc");
    n = drsetup_read_file(SYSTEM_XML, buf, 0x4000);
    debug_printf("drsetup: wrote default system.xml: now %d bytes\n", n);
    drsetup_say(n > 0 ? "system.xml was missing, wrote MCP's defaults"
                      : "system.xml missing and writing defaults FAILED");
}

static void prepare_cert_sys(char *buf, char *path)
{
    int n = drsetup_read_file(CERT_SYS, buf, 0x4000);
    if (certs_complete(buf, n))
        return;
    int exists = n >= 0;
    debug_printf("drsetup: cert.sys %s (%d), CA/CP/XS incomplete\n",
                 exists ? "present" : "missing", n);

    /* title.cert of the first package folder that has one */
    int dir = -1, c = -1;
    if (FSA_OpenDir(drsetup_fsa, DRSETUP_INSTALL_DIR, &dir) == 0) {
        FSDirectoryEntry *e = (FSDirectoryEntry *)iosAlloc(0xCAFF, sizeof(FSDirectoryEntry));
        while (e && c <= 0 && FSA_ReadDir(drsetup_fsa, dir, e) == 0) {
            path_join(path, 0x300, DRSETUP_INSTALL_DIR "/", e->name, "/title.cert");
            c = drsetup_read_file(path, buf, 0x4000);
        }
        if (e) iosFree(0xCAFF, e);
        FSA_CloseDir(drsetup_fsa, dir);
    }
    if (c <= 0 || !certs_complete(buf, c)) {
        drsetup_say("cert.sys incomplete and no usable title.cert in the package");
        return;
    }

    int r;
    if (exists) {
        r = esMergeCertSys(buf, c);
        debug_printf("drsetup: esMergeCertSys(%s, %d) = %d\n", path, c, r);
    } else {
        int h = -1;
        r = FSA_OpenFile(drsetup_fsa, CERT_SYS, "w", &h);
        if (r == 0 && h >= 0) {
            r = FSA_WriteFile(drsetup_fsa, buf, 1, c, h, 0) == c ? 0 : -1;
            FSA_CloseFile(drsetup_fsa, h);
        }
        debug_printf("drsetup: wrote cert.sys from %s: %d\n", path, r);
    }
    FSA_FlushVolume(drsetup_fsa, "/vol/system_slc");
    n = drsetup_read_file(CERT_SYS, buf, 0x4000);
    drsetup_say(certs_complete(buf, n) ? "cert.sys was incomplete, restored it from the package's title.cert"
                                       : "restoring cert.sys FAILED");
}

void slc_prepare(void)
{
    char *buf = (char *)iosAllocAligned(0xCAFF, 0x4000, 0x40);
    char *path = (char *)iosAllocAligned(0xCAFF, 0x300, 0x40);
    if (buf && path) {
        prepare_system_xml(buf);
        prepare_cert_sys(buf, path);
    } else {
        drsetup_say("prepare SLC: out of memory");
    }
    if (buf) iosFree(0xCAFF, buf);
    if (path) iosFree(0xCAFF, path);
}

/* ------------------------------------------------------------------ */
/* region (sys_prod.xml)                                               */
/* ------------------------------------------------------------------ */
/*
 * sys_prod.xml is changed through MCP's own config client (the UC library,
 * same code as /dev/usr_cfg), like /dev/mcp ioctlv 0x41 SetSysProdSettings
 * does. That ioctl isn't reachable here: in Boot Recovery /dev/mcp is the
 * recovery thread's resource manager, which only knows ioctl 0x7e.
 * MCP_Cnfg_Write reads the file, merges the given items into its tree
 * (Cnfg_MergeTree 0x0503c970) and saves the whole tree, so the other
 * sys_prod fields are kept. The sys_prod nodes are read-only for every
 * caller; the access checks are patched out in slc_kern_patches (same
 * patches as wafel_setup_mlc).
 */
typedef struct UCItem {         /* 0x54 bytes, as coreinit's UCSysConfig */
    char  name[0x40];
    u32   access;               /* 0: no access-attribute check on write */
    u32   type;
    s32   error;                /* per-item result */
    u32   size;                 /* must match the node's length attribute */
    void *data;
} UCItem;
_Static_assert(sizeof(UCItem) == 0x54, "UCItem size");

#define UC_U32      3
#define UC_COMPLEX  8
#define UC_PERMS    0x777       /* caller permissions MCP uses for sys_prod */

#define MCP_Cnfg_Read  ((int (*)(u32 perms, u32 n, UCItem *items))(0x0503b43c | 1))
#define MCP_Cnfg_Write ((int (*)(u32 perms, u32 n, UCItem *items))(0x0503b400 | 1))

#define ADDR_CNFG_LEAF_ACCESS  0x0503cc44  /* Cnfg_MergeTree: node not writable -> error */
#define ADDR_CNFG_ITEM_ACCESS  0x0503cd32  /* Cnfg_MergeTree: item access != node access */

static void uc_item(UCItem *it, const char *name, u32 type, u32 size, void *data)
{
    memset(it, 0, sizeof(*it));
    strncpy(it->name, name, sizeof(it->name) - 1);
    it->type = type;
    it->size = size;
    it->data = data;
}

/* set product_area and game_region in sys_prod.xml to region */
void slc_set_region(u32 region)
{
    u32 area = 0, game = 0;
    UCItem it[3];
    uc_item(&it[0], "slc:sys_prod", UC_COMPLEX, 0, NULL);
    uc_item(&it[1], "slc:sys_prod.product_area", UC_U32, 4, &area);
    uc_item(&it[2], "slc:sys_prod.game_region", UC_U32, 4, &game);

    int r = MCP_Cnfg_Read(UC_PERMS, 3, it);
    debug_printf("drsetup: sys_prod read %d (%d %d %d): product_area %u game_region %u, wanted %u\n",
                 r, it[0].error, it[1].error, it[2].error, area, game, region);
    if (r != 0) {
        drsetup_say("region: FAILED to read sys_prod.xml, region unchanged");
        return;
    }
    if (area == region && game == region) {
        drsetup_say("region: sys_prod already matches the Wii U Menu");
        return;
    }
    area = game = region;
    r = MCP_Cnfg_Write(UC_PERMS, 3, it);
    debug_printf("drsetup: sys_prod write region %u: %d (%d %d %d)\n",
                 region, r, it[0].error, it[1].error, it[2].error);
    drsetup_say(r == 0 ? "region: sys_prod product_area/game_region set to the Wii U Menu's region"
                       : "region: FAILED to write sys_prod.xml");
#ifdef DRSETUP_REGION_TEST
    /* the write must keep the other fields: read them back */
    char serial[12 + 1] = {0};
    u32 version = 0;
    area = game = 0;
    UCItem chk[5];
    memcpy(chk, it, sizeof(it));
    uc_item(&chk[3], "slc:sys_prod.serial_id", 6, 12, serial);
    uc_item(&chk[4], "slc:sys_prod.version", UC_U32, 4, &version);
    r = MCP_Cnfg_Read(UC_PERMS, 5, chk);
    debug_printf("drsetup: REGIONTEST re-read %d: area %u game %u version %u serial '%s' (errors %d %d %d %d %d)\n",
                 r, area, game, version, serial, chk[0].error, chk[1].error, chk[2].error,
                 chk[3].error, chk[4].error);
#endif
}

/*
 * Installing the system menu (app type 0x90000001) sets system.xml
 * default_title_id, so after the install the coldboot title is the package's
 * Wii U Menu. Same rule as wafel_setup_mlc's fix_region: 00050010100400x0 ->
 * region bit 1 << x.
 */
static void fix_region(void)
{
    u64 title = 0, os = 0;
    MCP_SysCfg_GetDefaultTitle(&title, &os);
    debug_printf("drsetup: coldboot title %08X%08X\n", (u32)(title >> 32), (u32)title);
    /* (wafel_setup_mlc's version of this check can't fail: "&" binds weaker than "!=") */
    if ((title & 0xFFFFFFFFFFFFF0FFull) != 0x0005001010040000ull) {
        drsetup_say("region: coldboot title is not a Wii U Menu, region unchanged");
        return;
    }
    u32 idx = (title >> 8) & 0xf;
    if (idx >= 7) {
        drsetup_say("region: unknown Wii U Menu region, region unchanged");
        return;
    }
    slc_set_region(1u << idx);
}

/*
 * Replaces the DR update's "bl MCP_LoadSysProdXML" after a successful install
 * (step 0x13, before the success line and before power transitions are
 * re-enabled, so it doesn't race MCP's power-off). Full build: fix the region
 * first, then the original call loads the fixed file. MCP sets step 0x13
 * before this call and a failing load fails the update with the step still
 * 0x13, so the install thread also checks this call's result.
 */
static volatile int sysprod_load = 1;

static int drsetup_loadsysprod(int r0, int r1, int r2, int r3, int (*org)(void))
{
    (void)r0; (void)r1; (void)r2; (void)r3;
#ifndef DRSETUP_INSTALL_ONLY
    if (drsetup_fsa >= 0)
        fix_region();
#endif
    int ret = org();
    sysprod_load = ret;
    debug_printf("drsetup: MCP_LoadSysProdXML returned %d\n", ret);
    return ret;
}

int slc_sysprod_load_result(void)
{
    return sysprod_load;
}

void slc_kern_patches(void)
{
#if !defined(DRSETUP_INSTALL_ONLY) || defined(DRSETUP_REGION_TEST)
    /* let MCP's config client write the read-only sys_prod nodes */
    ASM_T_PATCH_K(ADDR_CNFG_LEAF_ACCESS, "nop\nnop\n");
    ASM_T_PATCH_K(ADDR_CNFG_ITEM_ACCESS, "nop\n");
#endif
    trampoline_t_blreplace(ADDR_LOAD_SYSPROD_CALL, drsetup_loadsysprod);
}

/* ------------------------------------------------------------------ */
/* DELTEST: delete what MCP/ES/the plugin should recreate              */
/* ------------------------------------------------------------------ */
void slc_delete_test(void)
{
    static const char *const del[] = {
        SYS_PROD_XML,
        CERT_SYS,
        "/vol/system_slc/rights/sys/title.list",
        SYSTEM_XML,
    };
    for (unsigned i = 0; i < sizeof(del) / sizeof(del[0]); i++) {
        int r = FSA_Remove(drsetup_fsa, del[i]);
        debug_printf("drsetup: DELTEST remove %s: %X\n", del[i], r);
    }
    int f = FSA_FlushVolume(drsetup_fsa, "/vol/system_slc");
    for (unsigned i = 0; i < sizeof(del) / sizeof(del[0]); i++) {
        int h = -1;
        int r = FSA_OpenFile(drsetup_fsa, del[i], "r", &h);
        if (r == 0 && h >= 0) FSA_CloseFile(drsetup_fsa, h);
        debug_printf("drsetup: DELTEST %s now %s (open %X), flush %X\n",
                     del[i], r == 0 ? "STILL THERE" : "gone", r, f);
    }
    drsetup_say("DELTEST: deleted sys_prod.xml, cert.sys, title.list, system.xml");
}
