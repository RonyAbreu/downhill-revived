/* PCSX2-format texture replacement packs for the native renderer (texpack.c, finding 057). */
#ifndef PORT_GS_TEXPACK_H
#define PORT_GS_TEXPACK_H
#include "gs.h"
#include "replace_texture.h"

typedef struct TexPackEntry TexPackEntry;

/* Reads DHD_TEXTURES and DHD_TEXTURE_PACK (dhd.ini "textures", "texture_pack"), indexes the
   pack's file names and starts the loaders, which make their textures with device. The default
   memory budget is half of the graphics card's dedicated memory, video_memory bytes (0, unknown:
   1 GB). 0: no pack, nothing else here may be called. */
int texpack_open(const ReplaceDevice *device, uint64_t video_memory);

/* Whether a pack folder is named, or in DHD_DATA_DIR or beside the game (whatever DHD_TEXTURES
   says). */
int texpack_available(void);

/* A texture just decoded from GS memory: its replacement in the pack, or NULL. The name is
   PCSX2's, from the texture's GS memory, its CLUT and its context's TEX0/TEX1/MIPTBP registers
   (GS worker thread). */
TexPackEntry *texpack_find(const Gs *g, unsigned ctxt);

/* The replacement (the ReplaceDevice's view), if it is loaded; else NULL and its loading starts
   (once). It stays valid for at least 8 fields after the last call that returned it. */
void *texpack_view(TexPackEntry *e, uint64_t field);

/* The loaders told to end, and joined once they have, waiting up to ms (the Switch's exit, where
   no thread may outlive the return to the Homebrew Menu: finding 146). Safe without a pack. */
void texpack_halt(unsigned ms);

/* Once a field (GS worker): replacements over the memory budget that no recent field drew are
   released. */
void texpack_field(uint64_t field);

/* Counts at exit (stderr). */
void texpack_report(void);

#endif
