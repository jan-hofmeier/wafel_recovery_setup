#ifndef DRSETUP_PROMPT_H
#define DRSETUP_PROMPT_H

#include <wafel/types.h>

/* kern_main: count EJECT presses seen by MCP's wake event handler */
void prompt_kern_patches(void);

/* Show question on the recovery screen (brought up early if needed) and wait,
 * without timeout, for EJECT. Cancelling = holding POWER (SMC hard power-off);
 * a short POWER press is ignored. */
void drsetup_confirm(const char *question);

#endif /* DRSETUP_PROMPT_H */
