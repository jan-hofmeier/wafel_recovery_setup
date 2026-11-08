#ifndef RECUPD_CONFIG_H
#define RECUPD_CONFIG_H

/*
 * Compile-time options for wafel_recovery_update.
 * Override from the Makefile with EXTRA_CFLAGS, e.g.
 *   make EXTRA_CFLAGS="-DRECUPD_FORCE_FORMAT"
 */

/*
 * RECUPD_FORCE_FORMAT: when defined, the three format calls inside
 * MCP_DR_SystemUpdateFromMedia keep their factory force=true argument, so
 * SLCCMPT and the MLC (and SLC, if ever wiped) are reformatted every run.
 * When NOT defined (the default), those calls are patched to force=false, so
 * MCP_FormatDevice only formats a volume whose file system does not already
 * match - the same "format only when unformatted" behaviour as
 * wafel_setup_mlc.
 */
#ifndef RECUPD_FORCE_FORMAT
/* #define RECUPD_FORCE_FORMAT */
#endif

/*
 * RECUPD_SKIP_BOOTTITLE_CHECK (default on): patch out
 * MCP_UCBFF_CheckPackageHasBootTitles so the SD package need not be a complete
 * system (OS + boot titles). Any set of WUP title folders installs. Define
 * RECUPD_REQUIRE_FULL_SYSTEM to keep the factory check instead.
 */
#ifndef RECUPD_REQUIRE_FULL_SYSTEM
#define RECUPD_SKIP_BOOTTITLE_CHECK
#endif

/*
 * RECUPD_FORCE_TEST_LEVEL (default on): patch MCP's IOS_GetSecurityLevel thunk
 * to return Test. On retail IOS_SetSecurityLevel is a no-op (gated by the OTP
 * debug fuse), so the recovery thread's own SetSecurityLevel(Test) does nothing
 * and the level stays Normal. At Normal the update engine skips any title whose
 * installed version is >= the package version, which would silently no-op a
 * reinstall over existing titles. Forcing Test makes it always install.
 * Define RECUPD_KEEP_NORMAL_LEVEL to leave the level alone (reinstall only
 * upgrades / installs onto a blank MLC).
 */
#ifndef RECUPD_KEEP_NORMAL_LEVEL
#define RECUPD_FORCE_TEST_LEVEL
#endif

/* SD directory that holds one WUP-style folder per title. */
#define RECUPD_SD_MOUNT     "/vol/sdcard"
#define RECUPD_INSTALL_DIR  "/vol/sdcard/wafel_install"
#define RECUPD_LOG_PATH     "/vol/sdcard/wafel_recupd.log"
/* self-delete after a successful run so the next boot is normal */
#define RECUPD_IPX_PATH     "/vol/sdcard/wiiu/ios_plugins/5recupd.ipx"

/* We never wipe the SLC. */
#define RECUPD_WIPE_SLC     0

#endif /* RECUPD_CONFIG_H */
