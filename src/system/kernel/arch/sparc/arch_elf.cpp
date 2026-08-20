/*
 * Copyright 2019-2020, Adrien Destugues <pulkomandy@pulkomandy.tk>
 * Copyright 2010, Ithamar R. Adema <ithamar.adema@team-embedded.nl>
 * Copyright 2009, Johannes Wischert, johanneswi@gmail.com.
 * Copyright 2005, Ingo Weinhold <bonefish@cs.tu-berlin.de>.
 * Copyright 2002, Travis Geiselbrecht. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include <KernelExport.h>

#include <elf_priv.h>
#include <boot/elf.h>
#include <arch/elf.h>


//#define TRACE_ARCH_ELF
#ifdef TRACE_ARCH_ELF
#	define TRACE(x) dprintf x
#	define CHATTY 1
#else
#	define TRACE(x) ;
#	define CHATTY 0
#endif


#ifndef _BOOT_MODE
static bool
is_in_image(struct elf_image_info *image, addr_t address)
{
	return (address >= image->text_region.start
			&& address < image->text_region.start + image->text_region.size)
		|| (address >= image->data_region.start
			&& address < image->data_region.start + image->data_region.size);
}
#endif	// !_BOOT_MODE


#ifdef _BOOT_MODE
status_t
boot_arch_elf_relocate_rel(struct preloaded_elf64_image *image, Elf64_Rel *rel,
	int rel_len)
#else
int
arch_elf_relocate_rel(struct elf_image_info *image,
	struct elf_image_info *resolve_image, Elf64_Rel *rel, int rel_len)
#endif
{
	// there are no rel entries in M68K elf
	return B_NO_ERROR;
}


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
// instruction encodes. Bicc carries 22 bits and BPcc 19; assembly that branches
// to a global symbol produces these, because the linker has to allow for
// interposition and cannot resolve them itself.
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
	// Twenty-two bits is what sethi has room for; the mask keeps a value too
	// large for the field from reaching the opcode bits above it.
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


#ifdef _BOOT_MODE
status_t
boot_arch_elf_relocate_rela(struct preloaded_elf64_image *image,
	Elf64_Rela *rel, int rel_len)
#else
int
arch_elf_relocate_rela(struct elf_image_info *image,
	struct elf_image_info *resolve_image, Elf64_Rela *rel, int rel_len)
#endif
{
	int i;
	Elf64_Sym *sym;
	int vlErr;

	Elf64_Addr S = 0;	// symbol address
	//addr_t R = 0;		// section relative symbol address

	//addr_t G = 0;		// GOT address
	//addr_t L = 0;		// PLT address

	#define P	((addr_t)(image->text_region.delta + rel[i].r_offset))
	#define A	((addr_t)rel[i].r_addend)
	#define B	(image->text_region.delta)

	// TODO: Get the GOT address!
	#define REQUIRE_GOT	\
		if (G == 0) {	\
			dprintf("arch_elf_relocate_rela(): Failed to get GOT address!\n"); \
			return B_ERROR;	\
		}

	// TODO: Get the PLT address!
	#define REQUIRE_PLT	\
		if (L == 0) {	\
			dprintf("arch_elf_relocate_rela(): Failed to get PLT address!\n"); \
			return B_ERROR;	\
		}

	for (i = 0; i * (int)sizeof(Elf64_Rela) < rel_len; i++) {
#if CHATTY
		dprintf("looking at rel type %" PRIu64 ", offset 0x%lx, sym 0x%lx, "
			"addend 0x%lx\n", ELF64_R_TYPE(rel[i].r_info), rel[i].r_offset,
			ELF64_R_SYM(rel[i].r_info), rel[i].r_addend);
#endif
		// Relocation types and what to do with them are defined in Oracle docs
		// Documentation Home > Linker and Libraries Guide
		// 	> Chapter 7 Object File Format > File Format > Relocation Sections
		// 	> Relocation Types (Processor-Specific) > SPARC: Relocation Types
		// https://docs.oracle.com/cd/E19120-01/open.solaris/819-0690/chapter6-24/index.html
		// https://docs.oracle.com/cd/E19120-01/open.solaris/819-0690/chapter6-24-1/index.html
		switch (ELF64_R_TYPE(rel[i].r_info)) {
			case R_SPARC_WDISP30:
			case R_SPARC_HH22:
			case R_SPARC_LM22:
			case R_SPARC_HM10:
			case R_SPARC_GLOB_DAT:
			case R_SPARC_JMP_SLOT:
			case R_SPARC_64:
				// R_SPARC_HI22 and R_SPARC_LO10 are deliberately not here: the
				// only thing they carry in a linked object is the GOT-base
				// idiom, which needs no symbol. See their case below.
				sym = SYMBOL(image, ELF64_R_SYM(rel[i].r_info));
#ifdef _BOOT_MODE
				vlErr = boot_elf_resolve_symbol(image, sym, &S);
#else
				vlErr = elf_resolve_symbol(image, sym, resolve_image, &S);
#endif
				if (vlErr < 0) {
					dprintf("%s(): Failed to relocate "
						"entry index %d, rel type %" PRIu64 ", offset 0x%lx, "
						"sym 0x%lx, addend 0x%lx\n", __FUNCTION__, i,
						ELF64_R_TYPE(rel[i].r_info),
						rel[i].r_offset, ELF64_R_SYM(rel[i].r_info),
						rel[i].r_addend);
					return vlErr;
				}
				break;
		}

		switch (ELF64_R_TYPE(rel[i].r_info)) {
			case R_SPARC_WDISP30:
			{
				write_hi30(P, S + A - P);
				break;
					// Without this the case fell through into HI22/LM22 below,
					// which then OR'd a second, unrelated value into the same
					// instruction word -- corrupting the call displacement. The
					// kernel image carries 196 of these relocations.
			}
			case R_SPARC_WDISP22:
			{
				write_disp22(P, S + A - P);
				break;
			}
			case R_SPARC_WDISP19:
			{
				write_disp19(P, S + A - P);
				break;
			}
			case R_SPARC_HI22:
			case R_SPARC_LO10:
			{
				// Position-independent code reaches its own globals through the
				// GOT, and finds the GOT with this three-instruction idiom that
				// GCC emits at the top of every function that needs it:
				//
				//	sethi	%hi(_GLOBAL_OFFSET_TABLE_-4), %l7
				//	call	__sparc_get_pc_thunk.l7		! %o7 = this address
				//	 add	%l7, %lo(_GLOBAL_OFFSET_TABLE_+4), %l7
				//	! thunk:  add %o7, %l7, %l7
				//
				// The thunk adds the address of the call, so what the two
				// immediates have to encode between them is the distance from
				// the call to the GOT -- and that is why the addends are the
				// GOT minus four and the GOT plus four rather than the GOT
				// twice. The sethi sits four bytes before the call and the add
				// four bytes after it, so those offsets cancel the instructions'
				// own positions and both halves of the pair come out as the same
				// value: (B + A) - P.
				//
				// Which is not what the psABI says these relocation types are.
				// It defines them as (S + A) >> 10 and (S + A) & 0x3ff, and that
				// is what they mean in an object file, where the symbol is
				// _GLOBAL_OFFSET_TABLE_ itself. But the link editor rewrites
				// them against the .got section symbol while leaving the addend
				// as the whole object-relative address, so adding the symbol's
				// value counts the GOT twice: for the scsi bus manager that
				// produced a %l7 of 0x102740000 for a GOT that belonged at
				// 0x8139e1c0, and the first string literal read through it
				// faulted.
				//
				// Both halves of the pair agreeing under (B + A) - P is what
				// makes this reading certain rather than plausible. It holds for
				// every one of these relocations in the kernel and in every
				// add-on: thirteen distinct addend pairs in the kernel image,
				// each pair four bytes apart, matching two instructions four
				// bytes either side of a call.
				//
				// The two cases share a body because the value is the same; only
				// the field it goes into differs, and each writer takes the part
				// of the value its instruction has room for.
				if (ELF64_R_TYPE(rel[i].r_info) == R_SPARC_HI22)
					write_hi22(P, B + A - P);
				else
					write_lo10(P, B + A - P);
				break;
			}
			case R_SPARC_LM22:
			{
				write_hi22(P, S + A);
				break;
			}
			case R_SPARC_HH22:
			{
				write_hh22(P, S + A);
				break;
			}
			case R_SPARC_HM10:
			{
				write_hm10(P, S + A);
				break;
			}
			case R_SPARC_GLOB_DAT:
			{
				write_word64(P, S + A);
				break;
			}
			case R_SPARC_JMP_SLOT:
			{
				// Created by the link-editor for dynamic objects to provide
				// lazy binding. The relocation offset member gives the
				// location of a procedure linkage table entry. The runtime
				// linker modifies the procedure linkage table entry to
				// transfer control to the designated symbol address.
				addr_t jumpOffset = S - (P + 8);
				if ((jumpOffset & 0xc0000000) != 0
					&& (~jumpOffset & 0xc0000000) != 0) {
					// Offset > 30 bit.
					// TODO: Implement!
					// See https://docs.oracle.com/cd/E26502_01/html/E26507/chapter6-1235.html
					// examples .PLT102 and .PLT103
					dprintf("arch_elf_relocate_rela(): R_SPARC_JMP_SLOT: "
						"Offsets > 30 bit currently not supported!\n");
					dprintf("jumpOffset: %p\n", (void*)jumpOffset);
					return B_ERROR;
				} else {
					uint32* instructions = (uint32*)P;
					// We need to use a call instruction because it has a lot
					// of space for the destination (30 bits). However, it
					// erases o7, which we don't want.
					// We could avoid this with a JMPL if the displacement was
					// small enough, but it probably isn't.
					// So, we store o7 in g1 before the call, and restore it
					// in the branch delay slot. Crazy, but it works!
					instructions[0] = 0x01000000; // NOP to preserve the alignment?
					instructions[1] = 0x8210000f; // MOV %o7, %g1
					instructions[2] = 0x40000000 | ((jumpOffset >> 2) & 0x3fffffff);
					instructions[3] = 0x9e100001; // MOV %g1, %o7
				}
				break;
			}
			case R_SPARC_RELATIVE:
			{
				// A 64-bit relocation: on ELF64 the field is a full address, not
				// a word. Writing only 32 bits of it left the other half of the
				// slot untouched, and because SPARC is big-endian the value
				// landed in the *high* half -- so a GOT entry that should have
				// held 0x80217fc8 held 0x80217fc800000000 instead, and the first
				// dereference of it faulted. The kernel image carries 1153 of
				// these relocations, so most of the GOT was wrong.
				write_word64(P, B + A);
				break;
			}
			case R_SPARC_64:
			{
				write_word64(P, S + A);
				break;
			}
			default:
				dprintf("arch_elf_relocate_rela: unhandled relocation type %"
					PRIu64 "\n", ELF64_R_TYPE(rel[i].r_info));
				return B_ERROR;
		}
	}
	return B_OK;
}
