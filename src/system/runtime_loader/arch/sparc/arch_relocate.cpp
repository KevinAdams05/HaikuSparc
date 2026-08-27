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
		case R_SPARC_TLS_DTPMOD64:
		case R_SPARC_TLS_DTPOFF64:
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
				dprintf("sparc: could not resolve \"%s\" for \"%s\": %"
					B_PRId32 "\n", SYMNAME(image, sym), image->name, status);
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

			case R_SPARC_TLS_DTPMOD64:
				/*	Which module's thread-local block the offset beside this one
					is measured in -- the first half of a general-dynamic TLS
					pair, whose two words __tls_get_addr() is handed together.

					A symbol index of zero means this module's own block, which
					is the only form libroot currently carries.
				 */
				write_word64(P, symbolImage == NULL
					? image->dso_tls_id : symbolImage->dso_tls_id);
				break;

			case R_SPARC_TLS_DTPOFF64:
				/*	And the offset within that block. resolve_symbol() returns a
					TLS symbol's value already relative to its module, so there
					is nothing to add a load address to -- which is the whole
					point of the pair: neither half needs to know where the
					block will be until the thread asks.
				 */
				write_word64(P, S + A);
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
				int64 jumpOffset = (int64)S - (int64)(P + 8);

				/*	What a `call` can actually reach.
				 *
				 *	Its displacement field is thirty bits counted in
				 *	*instructions*, so the byte range is a signed 32-bit one:
				 *	-2 GB to just under +2 GB.
				 *
				 *	Worth stating because the obvious test is wrong by a factor
				 *	of two. Requiring bits 31:30 to be 00 or 11 -- which is what
				 *	the kernel's copy of this did, and what this inherited -- is
				 *	a signed *31*-bit range, so it rejects everything between
				 *	1 GB and 2 GB away. Userspace here is a shade under 2 GB
				 *	end to end, so two images can perfectly legally sit further
				 *	apart than that check allows: libgcc_s.so.1's forty-six PLT
				 *	entries resolve into libroot.so, the two landed 1.7 GB apart,
				 *	and every one of them was refused as unreachable.
				 *
				 *	It was also not a 64-bit test. Looking only at bits 31:30
				 *	says nothing about bits 63:32, so a displacement that
				 *	genuinely does not fit could pass.
				 */
				if (jumpOffset < -(int64)0x80000000
					|| jumpOffset > (int64)0x7ffffffc) {
					/*	Genuinely out of reach. The psABI's answer is a longer
						sequence that materialises the address in a register and
						jumps through it; a 2 GB user address space cannot
						produce this, so writing it untested would be worse than
						saying so.
					 */
					dprintf("sparc: R_SPARC_JMP_SLOT in \"%s\": %#" B_PRIx64
						" is further than a call reaches\n", image->name,
						jumpOffset);
					return B_BAD_DATA;
				}

				/*	Written back to front, so the word the entry is entered at
					is the last one to change. Every intermediate state then
					still runs the *old* entry rather than half of each. That
					costs nothing here -- binding is eager, and nothing can call
					through the entry while this runs -- and it is the difference
					between working and not if it ever becomes lazy. NetBSD's
					sparc64 loader orders it the same way and says why.
				 */
				uint32* instructions = (uint32*)P;
				instructions[3] = 0x9e100001;	// mov %g1, %o7
				instructions[2] = 0x40000000
					| ((jumpOffset >> 2) & 0x3fffffff);	// call
				instructions[1] = 0x8210000f;	// mov %o7, %g1
				instructions[0] = 0x01000000;	// nop

				/*	And made visible to instruction fetches, because UltraSPARC
					does not keep its instruction cache coherent with stores.
					`flush` operates on at least the doubleword containing its
					address, so two of them cover the four words -- and the
					MEMBAR before them is what makes the stores visible system
					wide (UltraSPARC-IIi manual 14.4.4, printed p.196).

					Inline rather than through a helper because runtime_loader
					links against neither the kernel nor libroot.
				 */
				asm volatile("membar #StoreStore" ::: "memory");
				asm volatile("flush %0" :: "r"(P) : "memory");
				asm volatile("flush %0" :: "r"(P + 8) : "memory");
				break;
			}

			default:
				/*	dprintf() rather than printf(): runtime_loader's printf goes
					to standard error through _kern_write, and while it is
					relocating libroot there is no standard error yet. dprintf
					goes to _kern_debug_output, which is the serial console the
					kernel is already using.
				 */
				dprintf("sparc: unhandled relocation type %d in \"%s\" at "
					"offset %#lx\n", type, image->name,
					(unsigned long)rel[i].r_offset);
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

	/*	The instruction cache is flushed by the R_SPARC_JMP_SLOT case itself,
		per entry, rather than once over the table here -- which is what the
		architecture asks for: "it must issue a FLUSH instruction for each
		modified doubleword of instructions" (SPARC V9 manual H.1.6, printed
		p.308). Nothing else in this file writes code.

		**Two things this implementation does not do**, both discovered by
		reading NetBSD's `sys/arch/sparc64/include/elf_support.h`, which is the
		same job done properly and is BSD-licensed if either is ever needed:

		  - It writes one encoding. NetBSD picks among four by range -- a single
		    `ba,a` within 8 MB, `sethi`/`jmp` for a 32-bit address, `sethi`/`xor`/
		    `jmp` for the top 32-bit range, and the `call` form above -- and has a
		    six-instruction general case beyond them. Ours reports and fails
		    instead, which cannot happen while Haiku's user address space is under
		    2 GB and the whole of it is within a `call`'s reach.
		  - It ignores `r_addend`. A non-zero addend means a PLT index of 32768 or
		    more, where the entry is a *pointer* to a stub rather than code, and
		    the fix is arithmetic on that pointer rather than four stores. No
		    library here has anything like that many entries.

		Both are limits rather than bugs, and both would present as a jump into
		nothing, so they are worth having written down.
	 */

	return B_OK;
}
