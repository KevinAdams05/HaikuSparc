/*
 * Copyright 2026, Kevin Adams <kevinadams05@gmail.com>
 * Distributed under the terms of the MIT License.
 */
#ifndef _KERNEL_ARCH_SPARC_VM_TYPES_H
#define _KERNEL_ARCH_SPARC_VM_TYPES_H

// Nothing architecture-specific is needed in the VM's own types on sun4u: the
// per-area and per-cache state the VM keeps is all portable. The file exists
// because <vm/VMArea.h> includes it unconditionally for every architecture.
//
// Where sun4u does need private state is the translation map, and that lives in
// arch_vm_translation_map.h and SPARCVMTranslationMap.h instead.

#endif	/* _KERNEL_ARCH_SPARC_VM_TYPES_H */
