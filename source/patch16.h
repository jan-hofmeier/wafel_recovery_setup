#ifndef DRSETUP_PATCH16_H
#define DRSETUP_PATCH16_H

#include <wafel/types.h>
#include <wafel/ios_dynamic.h>
#include <wafel/patch.h>

/*
 * Write one Thumb halfword at kernel time. Don't use ASM_T_PATCH_K for 2-byte
 * patches: it assembles the Thumb snippet inline in the calling ARM function
 * and switches back to .arm without realigning, so the ARM code after a 2-byte
 * snippet is shifted by 2 bytes (undefined instruction on hardware).
 */
static inline void patch16_k(uintptr_t addr, u16 val)
{
    memcpy16((void *)ios_elf_vaddr_to_paddr(addr), &val, sizeof(val));
}

#endif /* DRSETUP_PATCH16_H */
