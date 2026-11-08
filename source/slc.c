/*
 * SLC content the reinstall and the next boot need
 * (IOSU notes: docs/slc_minimum_for_recovery.md).
 */
#include <wafel/types.h>
#include <wafel/utils.h>
#include <wafel/trampoline.h>
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

/*
 * Set product_area and game_region in sys_prod.xml to region. Written in place
 * ("r+", same length, never truncated): the FSA client has no rename for a
 * temp file + swap, and a truncated sys_prod.xml would lose production data.
 * A value with a different number of digits is refused.
 */
void slc_set_region(u32 region)
{
    int fsa = drsetup_fsa;
    char *in = (char *)iosAllocAligned(0xCAFF, 0x1000, 0x40);
    char digits[10];
    int n = in ? drsetup_read_file(SYS_PROD_XML, in, 0xfc0) : -1;
    int pa = n > 0 ? xml_value(in, n, "<product_area") : -1;
    int gr = n > 0 ? xml_value(in, n, "<game_region") : -1;
    int la = 0, lg = 0;
    u32 va = pa >= 0 ? xml_u32(in, n, pa, &la) : 0;
    u32 vg = gr >= 0 ? xml_u32(in, n, gr, &lg) : 0;
    debug_printf("drsetup: sys_prod product_area %u game_region %u, wanted %u\n",
                 va, vg, region);
    if (n <= 0) {
        drsetup_say("region: FAILED to read sys_prod.xml");
    } else if (pa < 0 || gr < 0 || !la || !lg || n >= 0xfc0) {
        drsetup_say("region: FAILED to parse sys_prod.xml, region unchanged");
    } else if (va == region && vg == region) {
        drsetup_say("region: sys_prod already matches the Wii U Menu");
    } else if (put_u32(digits, region) != la || la != lg) {
        drsetup_say("region: new value has a different length, sys_prod NOT changed");
    } else {
        put_u32(in + pa, region);
        put_u32(in + gr, region);
        int w = -1, h = -1;
        if (FSA_OpenFile(fsa, SYS_PROD_XML, "r+", &h) == 0 && h >= 0) {
            if (FSA_SetPosFile(fsa, h, 0) == 0)
                w = FSA_WriteFile(fsa, in, 1, n, h, 0);
            FSA_CloseFile(fsa, h);
        }
        int f = FSA_FlushVolume(fsa, "/vol/system_slc");
        debug_printf("drsetup: wrote sys_prod.xml region %u: %d of %d bytes, flush %X\n",
                     region, w, n, f);
        drsetup_say(w == n ? "region: sys_prod product_area/game_region set to the Wii U Menu's region"
                           : "region: FAILED to write sys_prod.xml");
    }
    if (in) iosFree(0xCAFF, in);
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
