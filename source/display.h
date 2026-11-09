#ifndef DRSETUP_DISPLAY_H
#define DRSETUP_DISPLAY_H

/* kern_main: read the TV state and install the recovery screen patches */
void display_kern_patches(void);

/* bring the recovery screen up early (before the recovery thread draws it) */
void display_show_early(void);

#endif /* DRSETUP_DISPLAY_H */
