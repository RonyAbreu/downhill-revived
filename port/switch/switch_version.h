/* The Switch homebrew's own identity (finding 167, NX4): the name, author and version the
   Homebrew Menu shows (the NACP, made by build_host64.py from these lines), the folder it lives in
   on the SD card, and the release it ships as (port/switch/package.py). The game inside is still
   Downhill Revived, whose own version (DHD_VERSION, port/include/version.h) the log and the
   release notes give beside this one. build_host64.py and package.py read the #defines below as
   text: keep each on one line, as a plain string. */
#ifndef DHD_SWITCH_VERSION_H
#define DHD_SWITCH_VERSION_H

#define DHD_SWITCH_NAME "Downhill Switch"
#define DHD_SWITCH_AUTHOR "RonyAbreu"
#define DHD_SWITCH_VERSION "0.1.0-beta.1"
#define DHD_SWITCH_DIR "sdmc:/switch/downhill-switch/"

#endif
