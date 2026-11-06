/* The Switch's performance knobs (clocks.c, finding 167 NX3): the CPU, GPU and memory clocks
   for switch_main.c, and the core each of the port's threads runs on. */
#ifndef SWITCH_CLOCKS_H
#define SWITCH_CLOCKS_H

/* Raises the clocks and keeps them raised. `mode` is dhd.ini's DHD_SWITCH_CLOCKS: "on" (the
   default, and what NULL or an empty string mean), "max", or "off" to touch nothing. Whatever
   it does, and whatever it fails to do, is logged. */
void switch_clocks_start(const char *mode);

/* Puts the clocks back as they were. Safe when start did nothing. */
void switch_clocks_stop(void);

/* Pins the CALLING thread to a core, by the name it just gave itself (host_thread_name:
   "game", "vu", "gs", "render", ...). Horizon leaves every thread on the process's default
   core, so the heavy ones queue behind each other; this spreads them. A name it does not
   know, fewer than three cores to spread over, or DHD_SWITCH_CORES = off, and it does
   nothing. */
void switch_thread_core(const char *name);

/* Raises SDL's audio thread, called once from the audio callback (that thread itself is the
   only place it can be reached). DHD_SWITCH_AUDIO_PRIORITY = off leaves it alone, or a number
   sets it; 0x2B by default, one step above Horizon's ordinary 0x2C. */
void switch_audio_thread(void);

#endif
