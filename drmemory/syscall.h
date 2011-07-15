/* **********************************************************
 * Copyright (c) 2010-2011 Google, Inc.  All rights reserved.
 * Copyright (c) 2007-2009 VMware, Inc.  All rights reserved.
 * **********************************************************/

/* Dr. Memory: the memory debugger
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; 
 * version 2.1 of the License, and no later version.

 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Library General Public License for more details.

 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#ifndef _SYSCALL_H_
#define _SYSCALL_H_ 1

#ifdef WINDOWS
# define KUSER_SHARED_DATA_START 0x7ffe0000
#endif

/* for diagnostics: eventually provide some runtime option,
 * -logmask or something: for now have to modify this constant
 */
#define SYSCALL_VERBOSE 2

void
syscall_init(void *drcontext _IF_WINDOWS(app_pc ntdll_base));

void
syscall_exit(void);

void
syscall_thread_init(void *drcontext);

void
syscall_thread_exit(void *drcontext, per_thread_t *pt);

void
syscall_module_load(void *drcontext, const module_data_t *info, bool loaded);

void
syscall_handle_callback(void *drcontext, per_thread_t *pt_parent, per_thread_t *pt_child,
                        bool new_depth);

void
syscall_handle_cbret(void *drcontext, per_thread_t *pt_parent, per_thread_t *pt_child);

void
syscall_reset_per_thread(void *drcontext, per_thread_t *pt);

void
syscall_module_load(void *drcontext, const module_data_t *info, bool loaded);

bool
is_using_sysenter(void);

bool
is_using_sysint(void);

void
check_syscall_gateway(instr_t *inst);

#ifdef WINDOWS
/* The size, from the vsyscall start, that should be considered
 * "defined".  We size it big enough to cover both stored pc and
 * an actual code sequence.  FIXME: best to size to the actual
 * setup in use.  FIXME: is entire KUSER_SHARED_DATA page defined?
 */
# define VSYSCALL_SIZE 5
#endif

byte *
vsyscall_pc(void *drcontext, byte *entry);

const char *
get_syscall_name(uint num);

#ifdef WINDOWS
/* uses tables and other sources not available to sysnum_from_name() */
int
get_syscall_num(void *drcontext, const module_data_t *info, const char *name);
#endif

bool
syscall_is_known(uint num);

#ifdef STATISTICS
# ifdef WINDOWS
/* cover win32k.sys (0x1xxx, 0x2xxx), wow64 (0x3xxx), and ntoskrnl calls */
#  define MAX_SYSNUM 0x3100
# else
/* vmkernel has extra syscalls beyond linux */
#  define MAX_SYSNUM 1400
# endif
extern int syscall_invoked[MAX_SYSNUM];
#endif

void
check_sysmem(uint flags, int sysnum, app_pc ptr, size_t sz, dr_mcontext_t *mc,
             const char *id);

bool
handle_cstring(bool pre, int sysnum, dr_mcontext_t *mc, const char *id,
               byte *start, size_t size/*in bytes*/, uint arg_flags, char *safe,
               bool check_addr);

byte *
syscall_auxlib_start(void);

byte *
syscall_auxlib_end(void);

#endif /* _SYSCALL_H_ */
