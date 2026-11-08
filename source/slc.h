#ifndef DRSETUP_SLC_H
#define DRSETUP_SLC_H

#include <wafel/types.h>

/* kern_main: hook the final MCP_LoadSysProdXML of the update (region fix) */
void slc_kern_patches(void);

/* delete cafe.xml so the next boot runs the initial setup (full build) */
void slc_reset_initial_setup(void);

/* restore a missing system.xml and an incomplete cert.sys before the update */
void slc_prepare(void);

/* MCP_LoadSysProdXML result of the update: 1 = not reached */
int slc_sysprod_load_result(void);

/* test helpers */
void slc_set_region(u32 region);        /* REGIONTEST */
void slc_delete_test(void);             /* DELTEST */

#endif /* DRSETUP_SLC_H */
