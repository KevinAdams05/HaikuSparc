/*
 * Copyright 2026, Kevin Adams <kevinadams05@gmail.com>
 * Distributed under the terms of the MIT License.
 */
#ifndef _SYSTEM_ARCH_SPARC_COMMPAGE_DEFS_H
#define _SYSTEM_ARCH_SPARC_COMMPAGE_DEFS_H

#ifndef _SYSTEM_COMMPAGE_DEFS_H
#	error Must not be included directly. Include <commpage_defs.h> instead!
#endif

#define COMMPAGE_ENTRY_SPARC_SIGNAL_HANDLER \
	(COMMPAGE_ENTRY_FIRST_ARCH_SPECIFIC + 0)

#endif	/* _SYSTEM_ARCH_SPARC_COMMPAGE_DEFS_H */
