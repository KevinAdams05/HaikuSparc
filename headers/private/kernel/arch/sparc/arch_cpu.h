/*
 * Copyright 2003-2004, Axel Dörfler, axeld@pinc-software.de.
 * Copyright 2019, Adrien Destugues, pulkomandy@pulkomandy.tk.
 * Distributed under the terms of the MIT License.
 */
#ifndef _KERNEL_ARCH_SPARC_CPU_H
#define _KERNEL_ARCH_SPARC_CPU_H


#include <arch/sparc/arch_thread_types.h>
#include <kernel.h>


#define CPU_MAX_CACHE_LEVEL	8
#define CACHE_LINE_SIZE		128
	// 128 Byte lines on PPC970


#define arch_cpu_enable_user_access()
#define arch_cpu_disable_user_access()


typedef struct arch_cpu_info {
	int null;
} arch_cpu_info;


#ifdef __cplusplus
extern "C" {
#endif


static inline void
arch_cpu_pause(void)
{
	// TODO: CPU pause
}


static inline void
arch_cpu_idle(void)
{
	// TODO: CPU idle call
}


#ifdef __cplusplus
}
#endif

/*	Hands libroot's system_time() the factor the kernel worked out from the CPU's
	clock frequency. Declared here for the same reason ppc declares its own in
	the matching header: the definition is in libroot and the caller is in
	libroot, but the prototype has to be somewhere both can see.
*/
void __sparc_setup_system_time(vuint32 *cvFactor);

#endif	/* _KERNEL_ARCH_SPARC_CPU_H */
