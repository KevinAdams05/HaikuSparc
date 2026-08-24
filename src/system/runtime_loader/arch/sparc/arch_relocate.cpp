/*
 * Copyright 2026, Kevin Adams <kevinadams05@gmail.com>
 * Copyright 2012-2018, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Ithamar R. Adema <ithamar@upgrade-android.com>
 */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "runtime_loader_private.h"

#include <runtime_loader.h>

//#define TRACE_RLD
#ifdef TRACE_RLD
#	define TRACE(x) dprintf x
#else
#	define TRACE(x) ;
#endif


/*	The field writers.

	Every one of these ORs into the instruction word rather than replacing it,
	because on SPARC a relocation nearly always fills an immediate field inside an
	instruction whose opcode and registers are already there. The masks are what
	each field has room for; a value too large for the field is truncated rather
	than allowed to reach the opcode bits above it.

	The same set the kernel's arch_elf.cpp carries, deliberately: the two are
	solving the same problem against the same psABI, and where they differ it
	should be because userland needs something extra, not because they drifted.
*/
static inline void
write_word32(addr_t P, Elf64_Word value)
{
	*(Elf64_Word*)P = value;
}


static inline void
write_word64(addr_t P, Elf64_Xword value)
{
	*(Elf64_Xword*)P = value;
}


static inline void
write_hi30(addr_t P, Elf64_Word value)
{
	*(Elf64_Word*)P |= value >> 2;
}


// Branch displacements, in units of instructions and masked to the width the
// instruction encodes. Bicc carries 22 bits and BPcc 19.
static inline void
write_disp22(addr_t P, Elf64_Word value)
{
	*(Elf64_Word*)P |= (value >> 2) & 0x3fffff;
}


static inline void
write_disp19(addr_t P, Elf64_Word value)
{
	*(Elf64_Word*)P |= (value >> 2) & 0x7ffff;
}


static inline void
write_hi22(addr_t P, Elf64_Word value)
{
	*(Elf64_Word*)P |= (value >> 10) & 0x3fffff;
}


static inline void
write_lo10(addr_t P, Elf64_Word value)
{
	*(Elf64_Word*)P |= value & 0x3ff;
}


static inline void
write_hh22(addr_t P, Elf64_Xword value)
{
	*(Elf64_Word*)P |= value >> 42;
}


static inline void
write_hm10(addr_t P, Elf64_Xword value)
{
	*(Elf64_Word*)P |= (value >> 32) & 0x3ff;
}


/*!	Whether a relocation type needs its symbol looked up.

	Asked separately rather than resolving whenever the symbol index is non-zero,
	which is what most architectures here do and what SPARC cannot do. Its
	GOT-base relocations carry a symbol index that names the `.got` *section*
	rather than a definition, and resolving that either fails or produces a value
	the relocation must not use -- see the R_SPARC_HI22 case for what happens when
	it is used.
*/
static bool
needs_symbol(int type)
{
	switch (type) {
		case R_SPARC_WDISP30:
		case R_SPARC_WDISP22:
		case R_SPARC_WDISP19:
		case R_SPARC_HH22:
		case R_SPARC_LM22:
		case R_SPARC_HM10:
		case R_SPARC_GLOB_DAT:
		case R_SPARC_JMP_SLOT:
		case R_SPARC_64:
		case R_SPARC_COPY:
			return true;
		default:
			return false;
	}
}


static status_t
relocate_rela(image_t* rootImage, image_t* image, Elf64_Rela* rel,
	size_t relLength, SymbolLookupCache* cache)
{
	for (size_t i = 0; i < relLength / sizeof(Elf64_Rela); i++) {
		/*	ELF64_R_TYPE is the generic `info & 0xffffffff`, and the SPARC V9
			psABI says the type is only the low *eight* bits, with bits 31:8
			carrying a 24-bit datum that R_SPARC_OLO10 uses.

			Using the generic macro anyway, for two reasons. Everything a
			compiler emits leaves those bits zero, so the two readings agree; and
			where they do not, the generic one produces a type number nothing
			handles, which reports rather than quietly relocating with the wrong
			rule. The kernel's arch_elf.cpp reads it the same way, and the two
			agreeing is worth more here than either being clever.
		 */
		int type = ELF64_R_TYPE(rel[i].r_info);
		int symIndex = ELF64_R_SYM(rel[i].r_info);

		Elf64_Sym* sym = NULL;
		Elf64_Addr S = 0;
		image_t* symbolImage = NULL;

		if (symIndex != 0 && needs_symbol(type)) {
			sym = SYMBOL(image, symIndex);

			status_t status = resolve_symbol(rootImage, image, sym, cache, &S,
				&symbolImage);
			if (status != B_OK) {
				TRACE(("resolve symbol \"%s\" returned: %" B_PRId32 "\n",
					SYMNAME(image, sym), status));
				printf("resolve symbol \"%s\" returned: %" B_PRId32 "\n",
					SYMNAME(image, sym), status);
				return status;
			}
		}

		// The three quantities the psABI's formulae are written in: the place
		// being relocated, the addend, and the base this image was loaded at.
		addr_t P = image->regions[0].delta + rel[i].r_offset;
		addr_t A = rel[i].r_addend;
		addr_t B = image->regions[0].delta;

		switch (type) {
			case R_SPARC_NONE:
				break;

			case R_SPARC_WDISP30:
				write_hi30(P, S + A - P);
				break;

			case R_SPARC_WDISP22:
				write_disp22(P, S + A - P);
				break;

			case R_SPARC_WDISP19:
				write_disp19(P, S + A - P);
				break;

			case R_SPARC_HI22:
			case R_SPARC_LO10:
			{
				/*	The GOT-base idiom, and **not** what the psABI says these
					types mean.

					Position-independent code reaches its own globals through the
					GOT and finds the GOT with three instructions GCC emits at the
					top of every function that needs one:

						sethi	%hi(_GLOBAL_OFFSET_TABLE_-4), %l7
						call	__sparc_get_pc_thunk.l7	! %o7 = this address
						 add	%l7, %lo(_GLOBAL_OFFSET_TABLE_+4), %l7
						! thunk:  add %o7, %l7, %l7

					The thunk adds the address of the call, so what the two
					immediates encode between them is the distance from the call
					to the GOT -- which is why the addends are the GOT minus four
					and the GOT plus four rather than the GOT twice: the sethi
					sits four bytes before the call and the add four bytes after
					it, and those offsets cancel the instructions' own positions.
					Both halves then come out as the same value, (B + A) - P.

					The psABI defines these as (S + A) >> 10 and (S + A) & 0x3ff,
					and that is what they mean in an *object* file, where the
					symbol is _GLOBAL_OFFSET_TABLE_ itself. The link editor
					rewrites them against the `.got` section symbol while leaving
					the addend as the whole object-relative address, so adding the
					symbol's value counts the GOT twice.

					The kernel found this the expensive way: a kernel add-on came
					up with a %l7 of 0x102740000 for a GOT that belonged at
					0x8139e1c0, and the first string literal read through it
					faulted. What makes (B + A) - P certain rather than plausible
					is that *both halves of every pair agree under it* -- thirteen
					distinct addend pairs in the kernel image, each four bytes
					apart, matching two instructions four bytes either side of a
					call.
				 */
				if (type == R_SPARC_HI22)
					write_hi22(P, B + A - P);
				else
					write_lo10(P, B + A - P);
				break;
			}

			case R_SPARC_LM22:
				write_hi22(P, S + A);
				break;

			case R_SPARC_HH22:
				write_hh22(P, S + A);
				break;

			case R_SPARC_HM10:
				write_hm10(P, S + A);
				break;

			case R_SPARC_GLOB_DAT:
			case R_SPARC_64:
				write_word64(P, S + A);
				break;

			case R_SPARC_RELATIVE:
				/*	A **64-bit** write. The field on ELF64 is a full address, not
					a word, and writing only half of it leaves the rest of the
					slot untouched -- which on a big-endian machine puts the value
					in the *high* half, so a GOT entry that should hold 0x80217fc8
					holds 0x80217fc800000000 and the first dereference faults.

					The kernel image carries 1153 of these. Most of its GOT was
					wrong, for six phases, and it booted anyway because nothing
					had dereferenced the wrong parts yet.
				 */
				write_word64(P, B + A);
				break;

			case R_SPARC_COPY:
			{
				/*	The one relocation that moves data rather than computing an
					address: an executable that refers to a data object defined in
					a shared library gets its own copy in .bss, and every
					reference -- the library's included -- is bound to that copy
					instead. So this is the initial value being fetched from where
					the library put it.

					Only ever appears in an executable, never in a library, and
					only for data. The size comes from the *definition*, which is
					the symbol just resolved.
				 */
				if (sym == NULL)
					break;
				memcpy((void*)P, (void*)S, sym->st_size);
				break;
			}

			case R_SPARC_JMP_SLOT:
			{
				/*	A procedure linkage table entry, rewritten in place to jump
					to the resolved address.

					SPARC's PLT is unusual in being *code* rather than a pointer:
					the relocation names an instruction sequence, and binding a
					symbol means assembling a branch to it. Which is why this is
					four stores rather than one.

					`call` is the only branch with a 30-bit displacement, and it
					clobbers %o7 -- the caller's return address, which the callee
					needs. So %o7 is parked in %g1 before the call and put back in
					the delay slot, which executes *before* the branch is taken.
					%g1 is the ABI's scratch register and is dead at a call
					boundary, so borrowing it costs nothing.

					The displacement is measured from the call itself, at P + 8.
				 */
				addr_t jumpOffset = S - (P + 8);

				if ((jumpOffset & 0xc0000000) != 0
					&& (~jumpOffset & 0xc0000000) != 0) {
					/*	Further than a call can reach. The psABI's answer is a
						longer sequence that materialises the address in a
						register and jumps through it; nothing has needed it yet,
						and guessing at it untested would be worse than saying so.
					 */
					printf("R_SPARC_JMP_SLOT: displacement %#lx needs more than "
						"30 bits, which is not implemented\n",
						(unsigned long)jumpOffset);
					return B_BAD_DATA;
				}

				uint32* instructions = (uint32*)P;
				instructions[0] = 0x01000000;	// nop
				instructions[1] = 0x8210000f;	// mov %o7, %g1
				instructions[2] = 0x40000000
					| ((jumpOffset >> 2) & 0x3fffffff);	// call
				instructions[3] = 0x9e100001;	// mov %g1, %o7
				break;
			}

			default:
				TRACE(("unhandled relocation type %d\n", type));
				printf("unhandled relocation type %d\n", type);
				return B_BAD_DATA;
		}
	}

	return B_OK;
}


status_t
arch_relocate_image(image_t *rootImage, image_t *image,
	SymbolLookupCache* cache)
{
	status_t status;

	// SPARC uses RELA throughout: every relocation carries its addend in the
	// entry rather than in the field being relocated, so there is no REL half
	// to handle.

	if (image->rela != NULL) {
		status = relocate_rela(rootImage, image, image->rela, image->rela_len,
			cache);
		if (status != B_OK)
			return status;
	}

	if (image->pltrel != NULL) {
		status = relocate_rela(rootImage, image, (Elf64_Rela*)image->pltrel,
			image->pltrel_len, cache);
		if (status != B_OK)
			return status;
	}

	/*	The instruction cache is deliberately not flushed here, and on real
		hardware it will have to be.

		R_SPARC_JMP_SLOT writes *instructions*, and UltraSPARC does not keep its
		instruction cache coherent with stores -- the psABI requires a `flush`
		after modifying code. QEMU does not punish the omission, so leaving it out
		would be invisible until the port meets a Blade 150.

		It is not done here because it is not this file's to do: the same
		obligation applies to the kernel's own R_SPARC_JMP_SLOT handling, and
		arch_cpu_sync_icache() is an empty body on this port. One fix, in one
		place, when there is hardware to prove it against.
	 */

	return B_OK;
}
