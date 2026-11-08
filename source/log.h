#ifndef DRSETUP_LOG_H
#define DRSETUP_LOG_H

/* the install thread's FSA handle (-1 until it is opened) */
extern int drsetup_fsa;

/* open /dev/fsa through MCP (see log.c for why not stroopwafel's FSA_Open) */
int drsetup_fsa_open(void);

/* append a line to the SD log */
void drsetup_log(const char *msg);

/* SD log + serial + the recovery screen's text console */
void drsetup_say(const char *msg);

/* read up to max bytes of a file; returns the byte count or an FSA error */
int drsetup_read_file(const char *path, char *buf, int max);

#endif /* DRSETUP_LOG_H */
