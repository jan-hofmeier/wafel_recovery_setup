#ifndef DRSETUP_CONFIG_H
#define DRSETUP_CONFIG_H

/*
 * Build options. The variants are selected with make switches (see the
 * Makefile and README): INSTALLONLY=1, SLCFORMAT=1, and the test switches
 * NOINSTALL=1, DELTEST=1, REGIONTEST=1. The options below are changed here.
 */

/*
 * DRSETUP_FORCE_FORMAT (default off): keep the factory force=true argument of
 * the SLCCMPT and MLC formats in MCP_DR_SystemUpdateFromMedia, so they are
 * reformatted on every run. Off: force=false, MCP_FormatDevice only formats a
 * volume whose file system doesn't match (like wafel_setup_mlc).
 */
/* #define DRSETUP_FORCE_FORMAT */

/*
 * DRSETUP_SKIP_BOOTTITLE_CHECK (default on): patch out
 * MCP_UCBFF_CheckPackageHasBootTitles so the package need not be a complete
 * system (OS + boot titles); any set of WUP folders installs. Define
 * DRSETUP_REQUIRE_FULL_SYSTEM to keep the factory check.
 */
#ifndef DRSETUP_REQUIRE_FULL_SYSTEM
#define DRSETUP_SKIP_BOOTTITLE_CHECK
#endif

/*
 * DRSETUP_FORCE_TEST_LEVEL (default on): make MCP's IOS_GetSecurityLevel
 * return Test. On retail IOS_SetSecurityLevel is a no-op (OTP debug fuse), so
 * the level stays Normal, and at Normal the update engine skips every title
 * whose installed version is >= the package version: a reinstall over
 * existing titles would do nothing. Define DRSETUP_KEEP_NORMAL_LEVEL to leave
 * the level alone (only upgrades / installs onto a blank MLC).
 */
#ifndef DRSETUP_KEEP_NORMAL_LEVEL
#define DRSETUP_FORCE_TEST_LEVEL
#endif

/* SD paths */
#define DRSETUP_SD_MOUNT     "/vol/sdcard"
#define DRSETUP_INSTALL_DIR  "/vol/sdcard/wafel_install"   /* one WUP folder per title */
#define DRSETUP_LOG_PATH     "/vol/sdcard/wafel_drsetup.log"
/* self-delete after a successful run so the next boot is a normal boot */
#ifdef DRSETUP_INSTALL_ONLY
#define DRSETUP_IPX_PATH     "/vol/sdcard/wiiu/ios_plugins/5drinst.ipx"
#else
#define DRSETUP_IPX_PATH     "/vol/sdcard/wiiu/ios_plugins/5drsetup.ipx"
#endif

/* MCP_DR_SystemUpdateFromMedia's wipeSlc argument: never wipe the SLC */
#define DRSETUP_WIPE_SLC     0

#endif /* DRSETUP_CONFIG_H */
