/* SPDX-License-Identifier: GPL-2.0 */
/*
 * kaslr.h — per-boot randomization of the kernel's virtual base.
 *
 * Limine maps the kernel at its fixed default base and gives us no way to
 * ask for a different one, so the kernel moves itself: kaslr_maybe_relocate()
 * aliases the image at a random 2 MB slot of the same 1 GiB window, re-applies
 * the RELATIVE relocations for the new base, and jumps into the new mapping.
 * On any doubt it returns the base it was given and nothing changes.
 */
#ifndef INCLUDE_INIT_KASLR_H_
#define INCLUDE_INIT_KASLR_H_

#include <stdint.h>

uint64_t kaslr_maybe_relocate(uint64_t old_base, uint64_t phys_base, uint64_t hhdm);

#endif /* INCLUDE_INIT_KASLR_H_ */
