/* **********************************************************
 * Copyright (c) 2010-2011 Google, Inc.  All rights reserved.
 * Copyright (c) 2007-2010 VMware, Inc.  All rights reserved.
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

#ifndef _SYSCALL_OS_H_
#define _SYSCALL_OS_H_ 1

enum {
    /*****************************************/
    /* syscall_arg_t.flags */
    SYSARG_READ                = 0x00000001,
    SYSARG_WRITE               = 0x00000002,
    /* The data structure type has pointers or uninitialized fields
     * or padding and needs special processing according to the
     * SYSARG_TYPE_* code stored in syscall_arg_t.misc.
     */
    SYSARG_COMPLEX_TYPE        = 0x00000004,
    /* the size points at the IO_STATUS_BLOCK param */
    SYSARG_POST_SIZE_IO_STATUS = 0x00000008,
    /* the size points at a poiner-to-8-byte value param */
    SYSARG_POST_SIZE_8BYTES    = 0x00000010,
    /* the param holding the size is a pointer b/c it's an IN OUT var */
    SYSARG_LENGTH_INOUT        = 0x00000020,
    /* The size is not in bytes but in elements where the size of
     * each element is in the misc field.  The misc field can
     * contain <= in which case the element size is stored in that
     * parameter number.
     * This flag trumps SYSARG_COMPLEX_TYPE, so if there is an
     * overlap then special handling must be done for the type.
     */
    SYSARG_SIZE_IN_ELEMENTS    = 0x00000040,
    /* BOOLEAN is only 1 byte so ok if only lsb is defined
     * FIXME: are we going to need the sizes of all the params, esp.
     * when we move to 64-bit?
     */
    SYSARG_INLINED_BOOLEAN     = 0x00000080,
    /* for SYSARG_POST_SIZE_RETVAL on a duplicate entry, nothing is
     * written if the count, given in the first entry, is zero,
     * regardless of the buffer pointer value.
     */
    SYSARG_NO_WRITE_IF_COUNT_0 = 0x00000100,

    /*****************************************/
    /* syscall_arg_t.size, using values that cannot be mistaken for
     * a parameter reference.
     */
    SYSARG_SIZE_CSTRING       = -100,
    /* used in repeated syscall_arg_t entry for post-syscall size */
    SYSARG_POST_SIZE_RETVAL   = -101,
    /* size is stored as a field of size 4 bytes with an offset
     * given by syscall_arg_t.misc
     */
    SYSARG_SIZE_IN_FIELD      = -102,

    /*****************************************/
    /* syscall_arg_t.misc when flags has SYSARG_COMPLEX_TYPE */
    /* The following flags are used on Windows. */
    SYSARG_TYPE_PORT_MESSAGE        =  0,
    SYSARG_TYPE_CONTEXT             =  1,
    SYSARG_TYPE_EXCEPTION_RECORD    =  2,
    SYSARG_TYPE_SECURITY_QOS        =  3,
    SYSARG_TYPE_SECURITY_DESCRIPTOR =  4,
    SYSARG_TYPE_UNICODE_STRING      =  5,
    SYSARG_TYPE_CSTRING_WIDE        =  6,
    SYSARG_TYPE_OBJECT_ATTRIBUTES   =  7,
    SYSARG_TYPE_LARGE_STRING        =  8,
    SYSARG_TYPE_DEVMODEW            =  9,
    SYSARG_TYPE_WNDCLASSEXW         = 10,
    SYSARG_TYPE_CLSMENUNAME         = 11,
    SYSARG_TYPE_MENUITEMINFOW       = 12,
    SYSARG_TYPE_UNICODE_STRING_NOLEN= 13,
};

/* We encode the actual size of a write, if it can differ from the
 * requested size, as a subsequent syscall_arg_t entry with the same
 * param#.  A negative size there refers to a parameter that should be
 * de-referenced to obtain the actual write size.  The de-reference size
 * is assumed to be 4 unless SYSARG_POST_SIZE_8BYTES is set.
 */
typedef struct _syscall_arg_t {
    int param; /* ordinal of parameter */
    int size; /* >0 = abs size; <=0 = -param that holds size */
    uint flags; /* SYSARG_ flags */
    /* Meaning depends on flags.  I'd use a union but that would make
     * the syscall tables ugly w/ a ton of braces.
     * Currently used for:
     * - SYSARG_COMPLEX_TYPE: holds SYSARG_TYPE_* enum value
     * - SYSARG_SIZE_IN_ELEMENTS: holds size of array entry
     * - SYSARG_SIZE_FIELD: holds offset of 4-byte size field
     */
    int misc;
} syscall_arg_t;

enum {
    /* If not set, automated param comparison is used to find writes */
    SYSINFO_ALL_PARAMS_KNOWN    = 0x00000001,
    /* When checking the sysnum vs a wrapper function, do not consider
     * removing the prefix
     */
    SYSINFO_REQUIRES_PREFIX     = 0x00000002,
    /* NtUser syscall wrappers are spread across user32.dll and imm32.dll */
    SYSINFO_IMM32_DLL           = 0x00000004,
    /* Return value indicates failure only when zero */
    SYSINFO_RET_ZERO_FAIL       = 0x00000008,
    /* Return value of STATUS_BUFFER_TOO_SMALL (i#486) or
     * STATUS_BUFFER_OVERFLOW (i#531) writes final arg but no others.
     * If it turns out some syscalls distinguish between the two ret values
     * we can split the flag up but seems safer to combine.
     */
    SYSINFO_RET_SMALL_WRITE_LAST= 0x00000010,
};

#ifdef WINDOWS
/* unverified but we don't expect pointers beyond 1st 11 args
 * (even w/ dup entries for diff in vs out size to writes)
 */
# define MAX_NONINLINED_ARGS 11
#else
# define MAX_NONINLINED_ARGS 6
#endif

#define SYSCALL_ARG_TRACK_MAX_SZ 2048

typedef struct _syscall_info_t {
    int num; /* system call number: filled in dynamically */
    const char *name;
    uint flags; /* SYSINFO_ flags */
    int args_size; /* for Windows: total size of args; for Linux: arg count */
    /* list of args that are not inlined */
    syscall_arg_t arg[MAX_NONINLINED_ARGS];
    /* for custom handling w/o separate number lookup */
    int *num_out;
} syscall_info_t;

extern syscall_info_t syscall_info[];

#define SYSARG_CHECK_TYPE(flags, pre) \
    ((pre) ? (TEST(SYSARG_READ, (flags)) ? \
              MEMREF_CHECK_DEFINEDNESS : MEMREF_CHECK_ADDRESSABLE) : \
     (TEST(SYSARG_WRITE, (flags)) ? MEMREF_WRITE : 0))

void
syscall_os_init(void *drcontext _IF_WINDOWS(app_pc ntdll_base));

void
syscall_os_exit(void);

syscall_info_t *
syscall_lookup(int num);

void
syscall_os_module_load(void *drcontext, const module_data_t *info, bool loaded);

uint
get_sysparam_shadow_val(uint sysnum, uint argnum, dr_mcontext_t *mc);

void
check_sysparam_defined(uint sysnum, uint argnum, dr_mcontext_t *mc, size_t argsz);

/* for tasks unrelated to shadowing that are common to all tools */
bool
os_shared_pre_syscall(void *drcontext, int sysnum);

void
os_shared_post_syscall(void *drcontext, int sysnum);

/* for memory shadowing checks */
bool
os_shadow_pre_syscall(void *drcontext, int sysnum);

void
os_shadow_post_syscall(void *drcontext, int sysnum);

/* returns true if the given argument was processed in a non-standard way
 * (e.g. OS-specific structures) and we should skip the standard check
 */
bool
os_handle_pre_syscall_arg_access(int sysnum, dr_mcontext_t *mc, uint arg_num,
                                 const syscall_arg_t *arg_info,
                                 app_pc start, uint size);

/* returns true if the given argument was processed in a non-standard way
 * (e.g. OS-specific structures) and we should skip the standard check
 */
bool
os_handle_post_syscall_arg_access(int sysnum, dr_mcontext_t *mc, uint arg_num,
                                  const syscall_arg_t *arg_info,
                                  app_pc start, uint size);

bool
os_syscall_succeeded(int sysnum, syscall_info_t *info, ptr_int_t res);

/* provides name if known when not in syscall_lookup(num) */
const char *
os_syscall_get_name(uint num);

#ifdef WINDOWS
/* uses tables and other sources not available to sysnum_from_name() */
int
os_syscall_get_num(void *drcontext, const module_data_t *info, const char *name);
#endif

#endif /* _SYSCALL_OS_H_ */
