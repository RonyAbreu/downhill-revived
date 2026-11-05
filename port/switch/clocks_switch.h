/* The Switch's CPU, GPU and memory clocks (clocks.c, finding 144 NX3), for switch_main.c. */
#ifndef SWITCH_CLOCKS_H
#define SWITCH_CLOCKS_H

/* Raises the clocks and keeps them raised. `mode` is dhd.ini's DHD_SWITCH_CLOCKS: "on" (the
   default, and what NULL or an empty string mean), "max", or "off" to touch nothing. Whatever
   it does, and whatever it fails to do, is logged. */
void switch_clocks_start(const char *mode);

/* Puts the clocks back as they were. Safe when start did nothing. */
void switch_clocks_stop(void);

#endif
