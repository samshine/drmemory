/* **********************************************************
 * Copyright (c) 2008-2009 VMware, Inc.  All rights reserved.
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

/***************************************************************************
 * report.c: Dr. Memory error reporting
 */

#include "dr_api.h"
#include "drmemory.h"
#include "shadow.h"
#include "readwrite.h"
#include "syscall.h"
#include "alloc.h" 
#include "report.h"
#include "callstack.h"
#include "heap.h"
#include "alloc_drmem.h"
#ifdef LINUX
# include <errno.h>
#endif
#include <limits.h>

static uint error_id; /* errors + leaks */
static uint num_reported_errors;
static uint num_total_leaks;
static uint num_throttled_errors;
static uint num_throttled_leaks;
static uint num_leaks_ignored;
static size_t num_bytes_leaked;
static size_t num_bytes_possible_leaked;
static uint num_suppressions;
static uint num_suppressions_matched;
static uint num_suppressed_leaks;
static uint num_reachable_leaks;

static uint saved_throttled_leaks;
static uint saved_total_leaks;
static uint saved_leaks_ignored;
static uint saved_suppressed_leaks;
static uint saved_possible_leaks_total;
static uint saved_possible_leaks_unique;
static uint saved_reachable_leaks;
static uint saved_leaks_unique;
static uint saved_leaks_total;
static size_t saved_bytes_leaked;
static size_t saved_bytes_possible_leaked;

static uint64 timestamp_start;

/***************************************************************************/
/* Store all errors so we can eliminate duplicates (PR 484167) */

enum {
    ERROR_UNADDRESSABLE,
    ERROR_UNDEFINED,
    ERROR_INVALID_HEAP_ARG,
    ERROR_WARNING,
    ERROR_LEAK,
    ERROR_POSSIBLE_LEAK,
    ERROR_MAX_VAL,
};

static const char *const error_name[] = {
    "unaddressable access(es)",
    "uninitialized access(es)",
    "invalid heap argument(s)",
    "warning(s)",
    "leak(s)",    
    "possible leak(s)",    
};

static const char *const suppress_name[] = {
    "UNADDRESSABLE ACCESS",
    "UNINITIALIZED READ",
    "INVALID HEAP ARGUMENT",
    "WARNING",
    "LEAK",    
    "POSSIBLE LEAK",    
};

/* The error_lock protects these as well as error_table */
static uint num_unique[ERROR_MAX_VAL];
static uint num_total[ERROR_MAX_VAL];

/* Though any one instance of an address can have only one error
 * type, the same address could have multiple via different
 * executions.  Thus we must use a key combining the callstack and
 * the error type.
 */
typedef struct _stored_error_t {
    /* We can shrink some of these fields if memory is tight but we shouldn't
     * have more than a few thousand of these
     */
    uint id;
    uint errtype; /* from ERROR_ enum */
    uint count;
    bool suppressed;
    packed_callstack_t *pcs;
    /* We also keep a linked list so we can iterate in id order */
    struct _stored_error_t *next;
} stored_error_t;

#define ERROR_HASH_BITS 8
hashtable_t error_table;
/* We need an outer lock to synchronize stored_error_t data access.
 * Since we never remove from error_table we could instead have
 * a lock per stored_error_t but we save space, assuming errors
 * are rare enough to not be a bottleneck.
 */
static void *error_lock;
/* We also keep a linked list so we can iterate in id order, but composed
 * of hashtable payloads so no separate free is necessary.
 * Protected by error_lock.
 */
static stored_error_t *error_head;
static stored_error_t *error_tail;

/* Only initializes the errtype field */
stored_error_t *
stored_error_create(uint type)
{
    stored_error_t *err = global_alloc(sizeof(*err), HEAPSTAT_MISC);
    memset(err, 0, sizeof(*err));
    ASSERT(type < ERROR_MAX_VAL, "invalid error type");
    err->errtype = type;
    return err;
}

void
stored_error_free(stored_error_t *err)
{
    uint ref;
    ASSERT(err != NULL, "invalid arg");
    if (err->pcs != NULL) {
        ref = packed_callstack_free(err->pcs);
        ASSERT(ref == 0, "invalid ref count");
    }
    global_free(err, sizeof(*err), HEAPSTAT_MISC);
}

uint
stored_error_hash(stored_error_t *err)
{
    /* do NOT use id or count as they won't be filled out at lookup time */
    uint hash;
    ASSERT(err != NULL, "invalid arg");
    hash = packed_callstack_hash(err->pcs);
    hash ^= err->errtype;
    return hash;
}

bool
stored_error_cmp(stored_error_t *err1, stored_error_t *err2)
{
    /* do NOT use id or count as they won't be filled out at lookup time */
    ASSERT(err1 != NULL && err2 != NULL, "invalid arg");
    if (err1->errtype != err2->errtype)
        return false;
    return (packed_callstack_cmp(err1->pcs, err2->pcs));
}

/* A prefix for supplying additional info on a reported error beyond
 * the primary line, timestamp line, and callstack itself (from PR 535568)
 */
#define INFO_PFX IF_DRSYMS_ELSE("Note: ", "  info: ")

/***************************************************************************
 * suppression list
 */

/* For each error type, we have a list of callstacks, with each
 * callstack a variable-sized array of frames, each frame a string.
 */
typedef struct _suppress_spec_t {
    bool symbolic; /* whether mod!func, else mod+offs */
    uint num_frames;
    char **frames; /* variable-sized array of strings */
    /* During initial reading it's easier to build a linked list rather than
     * add resizable array support.  We could convert to an array after
     * reading both suppress files, but we have pointers scattered all
     * over anyway so we leave it a list.
     */
    struct _suppress_spec_t *next;
} suppress_spec_t;

/* We suppress error type separately (PR 507837) */
static suppress_spec_t *supp_list[ERROR_MAX_VAL];
static uint supp_num[ERROR_MAX_VAL];

/* We are single-threaded when we read the suppression file */
#define BUFSIZE 4096
static char buf[BUFSIZE];

#ifdef USE_DRSYMS
static void *suppress_file_lock;
#endif

static int
get_suppress_type(char *line)
{
    int i;
    ASSERT(line != NULL, "invalid param");
    if (line[0] == '\0')
        return -1;
    /* Perf: we could stick the 6 names in a hashtable */
    for (i = 0; i < ERROR_MAX_VAL; i++) {
        if (strstr(line, suppress_name[i]) == line)
            return i;
    }
    return -1;
}

static suppress_spec_t *
add_suppress_spec(int type, bool symbolic, uint num_frames, char **frames)
{
    suppress_spec_t *spec;
    uint i;
#ifndef USE_DRSYMS
    if (symbolic) /* not supported */
        return NULL;
#endif
    ASSERT(type >= 0 && type < ERROR_MAX_VAL, "internal error type error");
    spec = (suppress_spec_t *) global_alloc(sizeof(*spec), HEAPSTAT_MISC);
    spec->symbolic = symbolic;
    spec->num_frames = num_frames;
    spec->frames = (char **)
        global_alloc(num_frames*sizeof(char*), HEAPSTAT_MISC);
    LOG(2, "read suppression %d of type %s\n", num_suppressions,
        suppress_name[type]);
    for (i = 0; i < num_frames; i++) {
        LOG(2, "  frame %d: \"%s\"\n", i, frames[i]);
        spec->frames[i] = frames[i];
        frames[i] = NULL;
    }
    /* insert into list */
    spec->next = supp_list[type];
    supp_list[type] = spec;
    supp_num[type]++;
    num_suppressions++;
    return spec;
}

static void
read_suppression_file(file_t f)
{
    char *line, *newline = NULL;
    int bufread = 0, bufwant;
    int len;
    /* current callstack */
    int type, curtype = -1;
    bool symbolic = false, modoffs = false; /* format of callstack */
    /* even if a suppression callstack is longer than our max, we match
     * any prefix so we can truncate at the max.
     * the entries of this array are dynamically allocated and become
     * the final strings stored in supp_list.
     */
    char **frames = (char **)
        global_alloc(options.callstack_max_frames*sizeof(char*), HEAPSTAT_MISC);
    uint i, num_frames = 0;
    memset(frames, 0, options.callstack_max_frames*sizeof(char*));

    while (true) {
        if (newline == NULL) {
            bufwant = BUFSIZE-1;
            bufread = dr_read_file(f, buf, bufwant);
            ASSERT(bufread <= bufwant, "internal error reading suppression file");
            if (bufread <= 0)
                break;
            buf[bufread] = '\0';
            newline = strchr(buf, '\n');
            line = buf;
        } else {
            line = newline + 1;
            newline = strchr(line, '\n');
            if (newline == NULL) {
                /* shift 1st part of line to start of buf, then read in rest */
                /* the memory for the processed part can be reused  */
                bufwant = line - buf;
                ASSERT(bufwant <= bufread, "internal error reading suppression file");
                len = bufread - bufwant; /* what is left from last time */
                /* since strings may overlap, should use memmove, not strncpy */
                /* FIXME corner case: if len == 0, nothing to move */
                memmove(buf, line, len);
                bufread = dr_read_file(f, buf+len, bufwant);
                ASSERT(bufread <= bufwant, "internal error reading suppression file");
                if (bufread <= 0)
                    break;
                bufread += len; /* bufread is total in buf */
                buf[bufread] = '\0';
                newline = strchr(buf, '\n');
                line = buf;
            }
        }
        /* buffer is big enough to hold at least one line */
        ASSERT(newline != NULL, "internal error: suppression file malformed?");
        *newline = '\0';
        if (newline > line && *(newline-1) == '\r')
            *(newline-1) = '\0';
        LOG(3, "suppression file line: \"%s\"\n", line);
        /* Lines look like this:
         * UNINITIALIZED READ
         * <ADVAPI32.dll+0x3c0d>
         * # comment line; blank (newline) lines are allowed too
         * LEAK
         * <libc.so.6+0x2bc80>
         * <+0x2bc80>
         * <not in a module>
         *
         * Note: no leading white spaces.
         * Note: <+0x###> is only on esxi due to PR 363063;  it will go way once
         *       the bug is fixed
         *
         * For USE_DRSYMS, this client now also supports mod!func callstacks:
         * INVALID HEAP ARGUMENT
         * suppress.exe!invalid_free_test1
         * suppress.exe!test
         * suppress.exe!main
         */
        if (line[0] == '\0' || line[0] == '#')
            continue; /* Skip blank and comment lines. */
        type = get_suppress_type(line);
        if (type > -1) {
            if (curtype > -1) {
                /* the prior callstack completed successfully */
                add_suppress_spec(curtype, symbolic, num_frames, frames);
            }
            /* starting a new callstack */
            curtype = type;
            symbolic = false;
            modoffs = false;
            num_frames = 0;
        } else if (num_frames >= options.callstack_max_frames) {
            /* we truncate suppression callstacks to match requested max */
            LOG(1, "WARNING: requested max frames truncates suppression callstacks\n");
            /* just continue to next line */
        } else {
            if (curtype == -1) {
                usage_error("malformed suppression: no error type on line ", line);
                ASSERT(false, "should not reach here");
            } else if ((symbolic && line[0] == '<') ||
                       (modoffs && line[0] != '<')) {
                usage_error("malformed suppression mixes symbols and offsets: ", line);
                ASSERT(false, "should not reach here");
            }
            if (!modoffs && line[0] == '<')
                modoffs = true;
            else if (!symbolic && line[0] != '<')
                symbolic = true;
            if (frames[num_frames] != NULL) {
                global_free(frames[num_frames], strlen(frames[num_frames])+1,
                            HEAPSTAT_MISC);
            }
            frames[num_frames] = drmem_strdup(line, HEAPSTAT_MISC);
            num_frames++;
        }
    }
    if (curtype > -1) {
        /* the last callstack completed successfully */
        add_suppress_spec(curtype, symbolic, num_frames, frames);
    }
    for (i = 0; i < options.callstack_max_frames; i++) {
        if (frames[i] != NULL)
            global_free(frames[i], strlen(frames[i])+1, HEAPSTAT_MISC);
    }
    global_free(frames, options.callstack_max_frames*sizeof(char*), HEAPSTAT_MISC);
}

static void
open_and_read_suppression_file(const char *fname, bool is_default)
{
    uint prev_suppressions = num_suppressions;
    const char *label = (is_default) ? "default" : "user";
    if (fname == NULL || fname[0] == '\0') {
        dr_fprintf(f_global, "No %s suppression file specified\n", label);
    } else {
        file_t f = dr_open_file(fname, DR_FILE_READ);
        if (f == INVALID_FILE) {
            NOTIFY_ERROR("Error opening %s suppression file %s\n", label, fname);
            dr_abort();
            return;
        }
        read_suppression_file(f);
        /* Don't print to stderr about default suppression file */
        NOTIFY_COND(!is_default, f_global, "Recorded %d suppression(s) from %s %s\n",
                    num_suppressions - prev_suppressions, label, fname);
#ifdef USE_DRSYMS
        ELOGF(0, f_results, "Recorded %d suppression(s) from %s %s"NL,
              num_suppressions - prev_suppressions, label, fname);
#endif
    }
}

#ifdef USE_DRSYMS
/* up to caller to lock f_results file */
static void
write_suppress_pattern(uint type, char *cstack, bool symbolic)
{
    char *eframe, *epos, *end, *ques;
    ASSERT(type >= 0 && type < ERROR_MAX_VAL, "invalid error type");
    ASSERT(cstack != NULL, "invalid param");
    dr_fprintf(f_suppress, "%s"NL, suppress_name[type]);
    eframe = cstack;
    while (eframe != NULL && *eframe != '\0') {
        epos = strstr(eframe, "system call");
        if (epos != NULL) {
            end = strchr(epos, '\n');
            ASSERT(end != NULL, "suppress generation error");
            dr_fprintf(f_suppress, "%.*s"NL, end - epos, epos);
        } else if (symbolic) {
            epos = strchr(eframe, '>');
            ASSERT(epos != NULL && *(epos+1) == ' ', "suppress generation error");
            epos += 2; /* skip '> ' */
            end = strchr(epos, '\n');
            ASSERT(end != NULL, "suppress generation error");
            /* i#285: replace ? with * */
            ques = strchr(epos, '?');
            if (ques != NULL && ques < end) {
                dr_fprintf(f_suppress, "%.*s*", ques - epos, epos);
                epos = ques + 1;
            }
            dr_fprintf(f_suppress, "%.*s"NL, end - epos, epos);
        } else {
            epos = strchr(eframe, '<');
            ASSERT(epos != NULL, "suppress generation error");
            end = strchr(epos, '>');
            ASSERT(end != NULL, "suppress generation error");
            dr_fprintf(f_suppress, "%.*s>"NL, end - epos, epos);
            end = strchr(epos, '\n');
            ASSERT(end != NULL, "suppress generation error");
        }
    
        /* move to next frame: skip file:line# line */
        eframe = strchr(end + 1, '\n');
        ASSERT(eframe != NULL, "malformed suppression during compare");
        eframe++;
    }
}
#endif

static bool
on_suppression_list(uint type, char *cstack)
{
    bool match = false;
    suppress_spec_t *spec;
    uint i;
    char *pframe = NULL, *pat, *nextpat, *eframe, *epos, *nexte, *tmp;
    ASSERT(type >= 0 && type < ERROR_MAX_VAL, "invalid error type");
    for (spec = supp_list[type]; spec != NULL; spec = spec->next) {
        LOG(3, "supp: comparing to suppression pattern\n");
        eframe = cstack;
        for (i = 0; i < spec->num_frames; i++) {
            ASSERT(eframe != NULL, "suppression search error");
            if (*eframe == '\0')
                goto supp_done; /* no match: pattern longer than error */
            epos = strstr(eframe, "system call");
            if (epos != NULL) {
                /* system call - nothing to do here */
            } else if (spec->symbolic) {
                epos = strchr(eframe, '>');
                ASSERT(epos != NULL && *(epos+1) == ' ', "suppress parse error");
                epos += 2; /* skip '> ' */
            } else {
                epos = strchr(eframe, '<');
                ASSERT(epos != NULL, "suppress parse error");
            }
            /* make a copy for local modification */
            if (pframe != NULL)
                global_free(pframe, strlen(pframe)+1, HEAPSTAT_MISC);
            pframe = drmem_strdup(spec->frames[i], HEAPSTAT_MISC);
            pat = pframe;
            LOG(3, "  supp: comparing to pattern frame \"%s\"\n", pframe);
            while (pat != NULL) {
                /* PR 464821: support wildcards in suppression frames.
                 * We do strstr for each segment between the *'s.
                 */
                nextpat = strchr(pat, '*');
                if (nextpat != NULL) {
                    if (nextpat == pat) {
                        pat = nextpat + 1;
                        continue;
                    }
                    *nextpat = '\0';
                }
                LOG(3, "\tnext pattern segment: \"%s\"\n", pat);
                LOG(4, "\tcmp to: \"%s\"\n", epos);
                nexte = strstr(epos, pat);
                /* if no wildcard then make sure to match start */
                if (nexte == NULL || (pat == pframe && nexte != epos))
                    goto next_supp;
                /* make sure we didn't match beyond '>' for modoffs or into next
                 * line for either
                 */
                if (!spec->symbolic) {
                    tmp = strchr(epos, '>');
                    if (tmp != NULL && tmp < nexte)
                        goto next_supp;
                }
                tmp = strchr(epos, '\n');
                if (tmp != NULL && tmp < nexte)
                    goto next_supp;
                /* pattern segment matched: move to next */
                epos = nexte + strlen(pat);
                pat = (nextpat == NULL) ? NULL : (nextpat + 1);
            }
            /* move to next frame */
            eframe = strchr(epos, '\n');
            ASSERT(eframe != NULL, "malformed suppression during compare");
#ifdef USE_DRSYMS
            /* skip file:line# line */
            eframe = strchr(eframe + 1, '\n');
            ASSERT(eframe != NULL, "malformed suppression during compare");
#endif
            eframe++;
        }
        /* PR 460923: pattern is considered a prefix */
        LOG(3, "supp: pattern ended => prefix match\n");
        match = true;
        goto supp_done;
    next_supp:
        continue;
    }
 supp_done:
    if (pframe != NULL)
        global_free(pframe, strlen(pframe)+1, HEAPSTAT_MISC);
    if (!match) {
        LOG(3, "supp: no match\n");
#ifdef USE_DRSYMS
        /* write supp patterns to f_suppress */
        dr_mutex_lock(suppress_file_lock);
        write_suppress_pattern(type, cstack, true/*mod!func*/);
        dr_fprintf(f_suppress, "\n# the mod+offs form of the above callstack:"NL);
        write_suppress_pattern(type, cstack, false/*mod+offs*/);
        dr_fprintf(f_suppress, ""NL);
        dr_mutex_unlock(suppress_file_lock);
#endif
    }
    return match;
}

/***************************************************************************/

static void
print_timestamp(file_t f, uint64 timestamp, const char *prefix)
{
    dr_time_t time;
    uint64 abssec = timestamp / 1000;
    uint msec = (uint) (timestamp % 1000);
    uint sec = (uint) (abssec % 60);
    uint min = (uint) (abssec / 60);
    uint hour = min / 60;
    min %= 60;
    ELOGF(0, f, "%s: %u:%02d:%02d.%03d", prefix, hour, min, sec, msec);
    dr_get_time(&time);
    /* US-style month/day/year */
    ELOGF(0, f, " == %02d:%02d:%02d.%03d %02d/%02d/%04d\n",
          time.hour, time.minute, time.second, time.milliseconds,
          time.month, time.day, time.year);
}

/* Returns pointer to penultimate dir separator in string or NULL if can't find */
static const char *
up_one_dir(const char *string)
{
    const char *dir1 = NULL, *dir2 = NULL;
    while (*string != '\0') {
        if (*string == DIRSEP IF_WINDOWS(|| *string == '\\')) {
            dir1 = dir2;
            dir2 = string;
        }
        string++;
    }
    return dir1;
}

void
report_init(void)
{
    timestamp_start = dr_get_milliseconds();
    print_timestamp(f_global, timestamp_start, "start time");

    error_lock = dr_mutex_create();
 
    hashtable_init_ex(&error_table, ERROR_HASH_BITS, HASH_CUSTOM,
                      false/*!str_dup*/, false/*using error_lock*/,
                      (void (*)(void*)) stored_error_free,
                      (uint (*)(void*)) stored_error_hash,
                      (bool (*)(void*, void*)) stored_error_cmp);

    /* must be BEFORE read_suppression_file (PR 474542) */
    callstack_init(options.callstack_max_frames,
                   /* I used to use options.stack_swap_threshold but that
                    * was decreased for PR 525807 and anything smaller than
                    * ~0x20000 leads to bad callstacks on gcc b/c of a huge
                    * initial frame
                    */
                   0x20000,
                   /* default flags: but if we have apps w/ DGC we may
                    * want to expose some flags as options */
                   0,
                   /* scan forward 1 page: good compromise bet perf (scanning
                    * can be the bottleneck) and good callstacks
                    */
                   PAGE_SIZE,
                   get_syscall_name);

#ifdef USE_DRSYMS
    suppress_file_lock = dr_mutex_create();
    LOGF(0, f_results, "Dr. Memory results for pid %d: \"%s\""NL,
         dr_get_process_id(), dr_get_application_name());
    LOGF(0, f_suppress, "# File for suppressing errors found in pid %d: \"%s\""NL NL,
         dr_get_process_id(), dr_get_application_name());
#endif

    if (options.use_default_suppress) {
        /* the default suppression file must be located at
         *   dr_get_client_path()/../suppress-default.txt
         */
        const char *const DEFAULT_SUPPRESS_NAME = "suppress-default.txt";
        char dname[MAXIMUM_PATH];
        const char *mypath = dr_get_client_path(client_id);
        /* Windows kernel doesn't like paths with .. (0xc0000033 =
         * Object Name invalid) so we can't do just strrchr and add ..
         */
        const char *sep = up_one_dir(mypath);
        ASSERT(sep != NULL, "client lib path not absolute?");
        ASSERT(sep - mypath < BUFFER_SIZE_ELEMENTS(dname), "buffer too small");
        if (sep != NULL && sep - mypath < BUFFER_SIZE_ELEMENTS(dname)) {
            size_t len = dr_snprintf(dname, sep - mypath, "%s", mypath);
            if (len > 0) {
                len = dr_snprintf(dname + len, BUFFER_SIZE_ELEMENTS(dname) - len,
                                  "%c%s", DIRSEP, DEFAULT_SUPPRESS_NAME);
                if (len > 0)
                    open_and_read_suppression_file(dname, true);
                else
                    ASSERT(false, "default-suppress snprintf error");
            } else
                ASSERT(false, "default-suppress snprintf error");
        }
    }

    open_and_read_suppression_file(options.suppress_file, false);
}

#ifdef LINUX
void
report_fork_init(void)
{
    uint i;
    /* We reset so the child's timestamps will be relative to its start.
     * The global timestamp printed in the log can be used to find
     * time relative to the grandparent.
     */
    timestamp_start = dr_get_milliseconds();
    print_timestamp(f_global, timestamp_start, "start time");

    /* PR 513984: fork child should not inherit errors from parent */
    dr_mutex_lock(error_lock);
    error_id = 0;
    for (i = 0; i < ERROR_MAX_VAL; i++) {
        num_unique[i] = 0;
        num_total[i] = 0;
    }
    num_reported_errors = 0;
    num_total_leaks = 0;
    num_throttled_errors = 0;
    num_throttled_leaks = 0;
    num_leaks_ignored = 0;
    num_bytes_leaked = 0;
    num_bytes_possible_leaked = 0;
    num_suppressions = 0;
    num_suppressions_matched = 0;
    num_suppressed_leaks = 0;
    num_reachable_leaks = 0;
    /* FIXME: provide hashtable_clear() */
    hashtable_delete(&error_table);
    hashtable_init_ex(&error_table, ERROR_HASH_BITS, HASH_CUSTOM,
                      false/*!str_dup*/, false/*using error_lock*/,
                      (void (*)(void*)) stored_error_free,
                      (uint (*)(void*)) stored_error_hash,
                      (bool (*)(void*, void*)) stored_error_cmp);
    /* Be sure to reset the error list (xref PR 519222)
     * The error list points at hashtable payloads so nothing to free 
     */
    error_head = NULL;
    error_tail = NULL;
    dr_mutex_unlock(error_lock);
}
#endif

/* N.B.: for PR 477013, postprocess.pl duplicates some of this syntax
 * exactly: try to keep the two in sync
 */
void
report_summary_to_file(file_t f, bool stderr_too)
{
    uint i;
    stored_error_t *err;
    bool notify = (options.summary && stderr_too);

    /* Too much info to put on stderr, so just in logfile */
    dr_fprintf(f, ""NL);
    dr_fprintf(f, "DUPLICATE ERROR COUNTS:"NL);
    for (err = error_head; err != NULL; err = err->next) {
        if (err->count > 1 && !err->suppressed &&
            /* possible leaks are left with id==0 and should be ignored
             * except in summary, unless -possible_leaks
             */
            (err->errtype != ERROR_POSSIBLE_LEAK || options.possible_leaks)) {
            ASSERT(err->id > 0, "error id wrong");
            dr_fprintf(f, "\tError #%d: %6d"NL, err->id, err->count);
        }
    }

    dr_fprintf(f, ""NL);
    NOTIFY_COND(notify, f, "ERRORS FOUND:"NL);
    for (i = 0; i < ERROR_MAX_VAL; i++) {
        if (i == ERROR_LEAK || i == ERROR_POSSIBLE_LEAK) {
            if (options.count_leaks) {
                size_t bytes = (i == ERROR_LEAK) ?
                    num_bytes_leaked : num_bytes_possible_leaked;
                if (options.check_leaks) {
                    NOTIFY_COND(notify, f,
                                "  %5d unique, %5d total, %6d byte(s) of %s"NL,
                                num_unique[i], num_total[i], bytes, error_name[i]);
                } else {
                    /* We don't have dup checking */
                    NOTIFY_COND(notify, f,
                                "  %5d total, %6d byte(s) of %s"NL,
                                num_unique[i], bytes, error_name[i]);
                }
                if (i == ERROR_LEAK && !options.check_leaks) {
                    NOTIFY_COND(notify, f,
                                "         (re-run with \"-check_leaks\" for details)"NL);
                }
                if (i == ERROR_POSSIBLE_LEAK && !options.possible_leaks) {
                    NOTIFY_COND(notify, f,
                                "         (re-run with \"-check_leaks -possible_leaks\""
                                " for details)"NL);
                }
            }
        } else if (i != ERROR_INVALID_HEAP_ARG || options.check_invalid_frees) {
            NOTIFY_COND(notify, f, "  %5d unique, %5d total %s"NL,
                        num_unique[i], num_total[i], error_name[i]);
        }
    }
    NOTIFY_COND(notify, f, "ERRORS IGNORED:"NL);
    NOTIFY_COND(notify, f, "  %5d suppressed error(s)"NL,
                num_suppressions_matched);
    NOTIFY_COND(notify, f, "  %5d suppressed leak(s)"NL,
                num_suppressed_leaks);
    NOTIFY_COND(notify, f, "  %5d ignored assumed-innocuous system leak(s)"NL,
                num_leaks_ignored);
    NOTIFY_COND(notify, f, "  %5d still-reachable allocation(s)"NL,
                num_reachable_leaks);
    if (!options.show_reachable) {
        NOTIFY_COND(notify, f, "         (re-run with \"-check_leaks "
                    "-show_reachable\" for details)"NL);
    }
    NOTIFY_COND(notify, f, "  %5d error(s) beyond -report_max"NL,
                num_throttled_errors);
    NOTIFY_COND(notify, f, "  %5d leak(s) beyond -report_leak_max"NL,
                num_throttled_leaks);
    NOTIFY_COND(notify, f, "Details: %s/results.txt\n", logsubdir);
}

void
report_summary(void)
{
    report_summary_to_file(f_global, true);
#ifdef USE_DRSYMS
    report_summary_to_file(f_results, false);
#endif
}

void
report_exit(void)
{
    uint i, j;
#ifdef USE_DRSYMS
    LOGF(0, f_results, NL"==========================================================================="NL"FINAL SUMMARY:"NL);
    dr_mutex_destroy(suppress_file_lock);
#endif
    report_summary();

    hashtable_delete(&error_table);
    dr_mutex_destroy(error_lock);

    callstack_exit();

    for (i = 0; i < ERROR_MAX_VAL; i++) {
        suppress_spec_t *spec;
        while (supp_list[i] != NULL) {
            spec = supp_list[i];
            for (j = 0; j < spec->num_frames; j++) {
                global_free(spec->frames[j], strlen(spec->frames[j])+1, HEAPSTAT_MISC);
            }
            global_free(spec->frames, spec->num_frames*sizeof(char*), HEAPSTAT_MISC);
            supp_list[i] = spec->next;
            global_free(spec, sizeof(*spec), HEAPSTAT_MISC);
        }
    }
}

void
report_thread_init(void *drcontext)
{
    callstack_thread_init(drcontext);
}

void
report_thread_exit(void *drcontext)
{
    callstack_thread_exit(drcontext);
}

/***************************************************************************/

static void
print_timestamp_and_thread(char *buf, size_t bufsz, size_t *sofar)
{
    /* PR 465163: include timestamp and thread id in callstacks */
    ssize_t len = 0;
    uint64 timestamp = dr_get_milliseconds() - timestamp_start;
    uint64 abssec = timestamp / 1000;
    uint msec = (uint) (timestamp % 1000);
    uint sec = (uint) (abssec % 60);
    uint min = (uint) (abssec / 60);
    uint hour = min / 60;
    min %= 60;
    BUFPRINT(buf, bufsz, *sofar, len, "@%u:%02d:%02d.%03d in thread %d"NL,
             hour, min, sec, msec, dr_get_thread_id(dr_get_current_drcontext()));
}

static void
report_error_from_buffer(file_t f, char *buf, app_loc_t *loc)
{
    print_buffer(f, buf);

#ifdef USE_DRSYMS
    if (f != f_global)
        print_buffer(f_global, buf);
#else
    /* FIXME: for PR 456181 we need atomic reports for -no_thread_logs,
     * but we need PR 457375 to disassemble to a buffer.
     * For now we do a racy write after the callstack that may get
     * split from the error report.  Since drsyms writes directly to
     * results file excluding there until we have as part of buffer.
     */
    /* safe_read doesn't help since still have a race w/ disassemble_with_info:
     * we want a client try/except (i#51/PR 198875)
     */
    if (loc != NULL && loc->type == APP_LOC_PC) {
        app_pc cur_pc = loc_to_pc(loc);
        if (cur_pc != NULL && dr_memory_is_readable(cur_pc, MAX_INSTR_SIZE)) {
            disassemble_with_info(dr_get_current_drcontext(), cur_pc, f,
                                  true/*show pc*/, true/*show bytes*/);
        }
    }
#endif
}

/* caller should hold error_lock */
static void
acquire_error_number(stored_error_t *err)
{
    err->id = atomic_add32_return_sum((volatile int *)&error_id, 1);
    num_unique[err->errtype]++;
}

/* Records a callstack for mc (or uses the passed-in pcs) and checks
 * whether this is a new error or a duplicate.  If new, it adds a new
 * entry to the error table.  Either way, it increments the error's
 * count, and increments the num_total count if the error is not
 * marked as suppressed.  If it is marked as suppressed, it's up to
 * caller to increment any other counters.
 * Returns holding error_lock.
 */
static stored_error_t *
record_error(uint type, packed_callstack_t *pcs, app_loc_t *loc, dr_mcontext_t *mc,
             bool have_lock)
{
    stored_error_t *err = stored_error_create(type);
    if (pcs == NULL) {
        packed_callstack_record(&err->pcs, mc, loc);
    } else {
        /* lifetimes differ so we must clone */
        err->pcs = packed_callstack_clone(pcs);
    }
    if (!have_lock)
        dr_mutex_lock(error_lock);
    /* add returns false if already there */
    if (hashtable_add(&error_table, (void *)err, (void *)err)) {
        err->id = 0; /* caller must call acquire_error_number() to set */
        /* add to linked list */
        if (error_tail == NULL) {
            ASSERT(error_head == NULL, "error list inconsistent");
            error_head = err;
            error_tail = err;
        } else {
            ASSERT(error_head != NULL, "error list inconsistent");
            error_tail->next = err;
            error_tail = err;
        }
    } else {
        stored_error_t *existing = hashtable_lookup(&error_table, (void *)err);
        ASSERT(existing != NULL, "entry must exist");
        stored_error_free(err);
        err = existing;
        /* FIXME PR 423750: print out a line for the dup saying 
         * "Error #n: reading 0xaddr", perhaps option-controlled if we don't
         * want to fill up logs in common-case
         */
    }
    /* If marked as suppressed, up to caller to increment counters */
    err->count++;
    if (!err->suppressed)
        num_total[type]++;
    return err;
}

/* PR 535568: report nearest mallocs and whether freed.
 * Should this go up by the container range?  Would have to be same
 * line, else adjust postprocess.pl.
 * FIXME PR 423750: provide this info on dups not just 1st unique.
 */
static void
report_heap_info(char *buf, size_t bufsz, size_t *sofar, app_pc addr, size_t sz)
{
    per_thread_t *pt = (per_thread_t *) dr_get_tls_field(dr_get_current_drcontext());
    ssize_t len = 0;
    byte *start, *end, *next_start = NULL, *prev_end = NULL;
    ssize_t size;
    bool found = false;
    if (!is_in_heap_region(addr))
        return;
    /* I measured replacing the malloc hashtable with an interval tree
     * and the cost is noticeable on heap-intensive benchmarks, so we
     * instead use shadow values to find malloc boundaries
     */
    /* We don't walk more than PAGE_SIZE: FIXME: make larger? */
    for (end = addr+sz; end < addr+sz + PAGE_SIZE; ) {
        if (!shadow_check_range(end, PAGE_SIZE, SHADOW_UNADDRESSABLE,
                                &start, NULL, NULL)) {
            LOG(3, "report_heap_info: next addressable="PFX"\n", start);
            size = malloc_size((byte*)ALIGN_FORWARD(start, MALLOC_CHUNK_ALIGNMENT));
            if (size <= -1) {
                /* An earlier unaddr adjacent to real malloc could
                 * have marked as addr so try align-8 forward as our
                 * loop will miss that if all addr in between
                 */
                size = malloc_size((byte*)ALIGN_FORWARD(start+1, MALLOC_CHUNK_ALIGNMENT));
            }
            if (size > -1) {
                found = true;
                next_start = start;
                BUFPRINT(buf, bufsz, *sofar, len,
                         "%snext higher malloc: "PFX"-"PFX""NL,
                         INFO_PFX, start, start+size);
                break;
            } /* else probably an earlier unaddr error, for which we marked
               * the memory as addressable!
               */
            end = shadow_next_dword((byte *)ALIGN_FORWARD(start, 4),
                                    addr+sz + PAGE_SIZE, SHADOW_UNADDRESSABLE);
        } else
            break;
    }
    /* If we can't find a higher malloc better to not print anything since we're
     * using heuristics and could be wrong (if we had rbtree I'd print "no higher")
     */
    for (start = addr; start > addr - PAGE_SIZE; ) {
        if (!shadow_check_range_backward(start-1, PAGE_SIZE,
                                         SHADOW_UNADDRESSABLE, &end)) {
            LOG(3, "report_heap_info: prev addressable="PFX"\n", end);
            start = (byte *) ALIGN_BACKWARD(end, 4);
            start = shadow_prev_dword(start, start - PAGE_SIZE, SHADOW_UNADDRESSABLE);
            LOG(3, "\tfrom there, prev unaddressable="PFX"\n", start);
            if (start != NULL) {
                start += 4; /* move to start of addressable */
                size = malloc_size(start);
                if (size <= -1) {
                    /* An earlier unaddr adjacent to real malloc could
                     * have marked as addr so try align-8 back as our
                     * loop will miss that if all addr in between
                     */
                    size = malloc_size((byte*)ALIGN_BACKWARD(start-1,
                                                             MALLOC_CHUNK_ALIGNMENT));
                }
                if (size > -1) {
                    found = true;
                    prev_end = start + size;
                    BUFPRINT(buf, bufsz, *sofar, len,
                             "%sprev lower malloc:  "PFX"-"PFX""NL, INFO_PFX, start,
                             prev_end);
                    break;
                } /* else probably an earlier unaddr error, for which we marked
                   * the memory as addressable!
                   */
            }
        } else
            break;
    }
    /* Look at both delay free list and at malloc entries marked
     * invalid.  The latter will find frees beyond the limit of the
     * delay list as well as free-by-realloc (xref PR 493888).
     */
    found = overlaps_delayed_free(addr, addr+sz, &start, &end);
    if (!found && next_start != NULL) {
        /* Heuristic: try 8-byte-aligned ptrs between here and valid mallocs */
        for (start = (byte *) ALIGN_FORWARD(addr+sz, MALLOC_CHUNK_ALIGNMENT);
             start < next_start; start += MALLOC_CHUNK_ALIGNMENT) {
            size = malloc_size_include_invalid(start);
            if (size > -1) {
                found = true;
                end = start + size;
                break;
            }
        }
    }
    if (!found && prev_end != NULL) {
        /* Heuristic: try 8-byte-aligned ptrs between here and valid mallocs */
        for (start = (byte *) ALIGN_BACKWARD(addr, MALLOC_CHUNK_ALIGNMENT);
             start > prev_end; start -= MALLOC_CHUNK_ALIGNMENT) {
            size = malloc_size_include_invalid(start);
            if (size > -1) {
                found = true;
                end = start + size;
                break;
            }
        }
    }
    if (found) {
        /* Note that due to the finite size of the delayed
         * free list (and realloc not on it: PR 493888) and
         * new malloc entries replacing invalid we can't
         * guarantee to identify use-after-free
         */
        BUFPRINT(buf, bufsz, *sofar, len,
                 "%s"PFX"-"PFX" overlaps freed memory "PFX"-"PFX""NL,
                 INFO_PFX, addr, addr+sz, start, end);
    }
    if (pt->in_heap_routine > 0) {
        BUFPRINT(buf, bufsz, *sofar, len,
                 "%s<inside heap routine: may be false positive>"NL, INFO_PFX);
    }
}

static void
report_error(uint type, app_loc_t *loc, app_pc addr, size_t sz, bool write,
             app_pc container_start, app_pc container_end,
             const char *msg, dr_mcontext_t *mc)
{
    per_thread_t *pt = (per_thread_t *) dr_get_tls_field(dr_get_current_drcontext());
    stored_error_t *err;
    char *cstack_start, *errnum_start;
    bool reporting = false;
    ssize_t len = 0;
    size_t sofar = 0;

    /* Our report_max throttling is post-dup-checking, to make the option
     * useful (else if 1st error has 20K instances, won't see any others).
     * Also, num_reported_errors doesn't count suppressed errors.
     * Also, suppressed errors are printed to the log until report_max is
     * reached so they can fill it up.
     * FIXME Perhaps we can avoid printing suppressed errors at all by default.
     * If perf of dup check or suppression matching is an issue
     * we can add -report_all_max or something.
     */
    if (options.report_max >= 0 && num_reported_errors >= options.report_max) {
        num_throttled_errors++;
        goto report_error_done;
    }
    err = record_error(type, NULL, loc, mc, false/*no lock */);
    if (err->count > 1) {
        if (err->suppressed) {
            num_suppressions_matched++;
        } else {
            ASSERT(err->id != 0, "duplicate should have id");
            /* We want -pause_at_un* to pause at dups so we consider it "reporting" */
            reporting = true;
        }
        dr_mutex_unlock(error_lock);
        goto report_error_done;
    }
    ASSERT(err->id == 0, "non-duplicate should not have id");

    /* We need to know whether suppressed so we can prefix "Error" with "SUPPRESSED"
     * and print the right error #.  So we go ahead and do that partway into
     * the buffer, and then later we adjust the prefix to line up.
     */
    sofar = MAX_ERROR_INITIAL_LINES;
    cstack_start = pt->errbuf + sofar;
    packed_callstack_print(err->pcs, 0/*all frames*/, pt->errbuf, pt->errbufsz, &sofar);
    sofar = 0; /* now we print to start */
    /* ensure starts at beginning of line (can be in middle of another log) */
    if (!options.thread_logs)
        BUFPRINT(pt->errbuf, pt->errbufsz, sofar, len, ""NL);

    reporting = !on_suppression_list(type, cstack_start);
    if (!reporting) {
        BUFPRINT(pt->errbuf, pt->errbufsz, sofar, len, "SUPPRESSED ");
        err->suppressed = true;
        num_suppressions_matched++;
        num_total[type]--;
    } else {
        acquire_error_number(err);
        num_reported_errors++;
    }
    dr_mutex_unlock(error_lock);

    /* For Linux and ESXi, postprocess.pl will produce the official
     * error numbers (after symbol suppression might remove some errors).
     * But we still want error numbers here, so that we can refer to them
     * when we list the duplicate counts at the end of the run, and
     * also for PR 423750 which will say "Error #n: reading 0xaddr".
     * On Windows for USE_DRSYMS these are the official error numbers.
     */
    BUFPRINT(pt->errbuf, pt->errbufsz, sofar, len, "Error #%d: ", err->id);
    errnum_start = pt->errbuf + sofar - 7/*"%5d: "*/;
    
    if (type == ERROR_UNADDRESSABLE) {
        BUFPRINT(pt->errbuf, pt->errbufsz, sofar, len,
                 "UNADDRESSABLE ACCESS: %s "PFX"-"PFX" %d byte(s)",
                 write ? "writing" : "reading", addr, addr+sz, sz);
        /* only report for syscall params or large (string) ops: always if subset */
        if (container_start != NULL &&
            (container_end - container_start > 8 || addr > container_start ||
             addr+sz < container_end || loc->type == APP_LOC_SYSCALL)) {
            ASSERT(container_end > container_start, "invalid range");
            BUFPRINT(pt->errbuf, pt->errbufsz, sofar, len,
                     " within "PFX"-"PFX""NL, container_start, container_end);
        } else
            BUFPRINT(pt->errbuf, pt->errbufsz, sofar, len, ""NL);
    } else if (type == ERROR_UNDEFINED) {
        BUFPRINT(pt->errbuf, pt->errbufsz, sofar, len, "UNINITIALIZED READ: ");
        if (addr < (app_pc)(64*1024)) {
            /* We use a hack to indicate registers.  These addresses should
             * be unadressable, not undefined, if real addresses.
             * FIXME: use dr_loc_t here as well for cleaner multi-type
             */
            BUFPRINT(pt->errbuf, pt->errbufsz, sofar, len,
                     "reading register %s"NL, (addr == (app_pc)REG_EFLAGS) ?
                     "eflags" : get_register_name((reg_id_t)(ptr_uint_t)addr));
        } else {
            BUFPRINT(pt->errbuf, pt->errbufsz, sofar, len,
                     "reading "PFX"-"PFX" %d byte(s)", addr, addr+sz, sz);
            /* only report for syscall params or large (string) ops: always if subset */
            if (container_start != NULL &&
                (container_end - container_start > 8 || addr > container_start ||
                 addr+sz < container_end || loc->type == APP_LOC_SYSCALL)) {
                ASSERT(container_end > container_start, "invalid range");
                BUFPRINT(pt->errbuf, pt->errbufsz, sofar, len,
                         " within "PFX"-"PFX""NL, container_start, container_end);
            } else
                BUFPRINT(pt->errbuf, pt->errbufsz, sofar, len, ""NL);
        }
    } else if (type == ERROR_INVALID_HEAP_ARG) {
        /* Note that on Windows the call stack will likely show libc, since
         * we monitor Rtl inside ntdll
         */
        ASSERT(msg != NULL, "invalid arg");
        BUFPRINT(pt->errbuf, pt->errbufsz, sofar, len,
                 "INVALID HEAP ARGUMENT: %s "PFX""NL, msg, addr);
    } else if (type == ERROR_WARNING) {
        ASSERT(msg != NULL, "invalid arg");
        BUFPRINT(pt->errbuf, pt->errbufsz, sofar, len, "%sWARNING: %s"NL,
                 /* if in log file, distinguish from internal warnings via "REPORTED" */
                 IF_DRSYMS_ELSE("", "REPORTED "), msg);
    } else {
        ASSERT(false, "unknown error type");
        BUFPRINT(pt->errbuf, pt->errbufsz, sofar, len,
                 "UNKNOWN ERROR TYPE: REPORT THIS BUG"NL);
    }

    print_timestamp_and_thread(pt->errbuf, pt->errbufsz, &sofar);

    if (type == ERROR_UNADDRESSABLE) {
        /* print auxiliary info about the target address (PR 535568) */
        report_heap_info(pt->errbuf, pt->errbufsz, &sofar, addr, sz);
    }

    /* Now shift the prefix to abut the callstack */
    ASSERT(sofar < MAX_ERROR_INITIAL_LINES, "buffer too small");
    memmove(pt->errbuf + MAX_ERROR_INITIAL_LINES - sofar, pt->errbuf, sofar);

    report_error_from_buffer(IF_DRSYMS_ELSE(reporting ? f_results : pt->f, pt->f),
                             pt->errbuf + MAX_ERROR_INITIAL_LINES - sofar, loc);
    
 report_error_done:
    if (type == ERROR_UNADDRESSABLE && reporting && options.pause_at_unaddressable)
        wait_for_user("pausing at unaddressable access error");
    else if (type == ERROR_UNDEFINED && reporting && options.pause_at_uninitialized)
        wait_for_user("pausing at uninitialized read error");
}

void
report_unaddressable_access(app_loc_t *loc, app_pc addr, size_t sz, bool write,
                            app_pc container_start, app_pc container_end,
                            dr_mcontext_t *mc)
{
    report_error(ERROR_UNADDRESSABLE, loc, addr, sz, write,
                 container_start, container_end, NULL, mc);
}

void
report_undefined_read(app_loc_t *loc, app_pc addr, size_t sz,
                      app_pc container_start, app_pc container_end,
                      dr_mcontext_t *mc)
{
    report_error(ERROR_UNDEFINED, loc, addr, sz, false,
                 container_start, container_end, NULL, mc);
}

void
report_invalid_heap_arg(app_loc_t *loc, app_pc addr, dr_mcontext_t *mc,
                        const char *routine)
{
    if (addr == NULL && strcmp(routine, IF_WINDOWS_ELSE("HeapFree", "free")) == 0) {
        /* free(NULL) is documented as always being properly handled (nop)
         * so we separate as not really "invalid" but just a warning
         */
        if (options.warn_null_ptr)
            report_warning(loc, mc, "free() called with NULL pointer");
    } else {
        report_error(ERROR_INVALID_HEAP_ARG, loc, addr, 0, false, NULL, NULL, routine, mc);
    }
}

void
report_warning(app_loc_t *loc, dr_mcontext_t *mc, const char *msg)
{
    report_error(ERROR_WARNING, loc, NULL, 0, false, NULL, NULL, msg, mc);
}

/* saves the values of all counts that are modified in report_leak() */
void
report_leak_stats_checkpoint(void)
{
    dr_mutex_lock(error_lock);
    saved_throttled_leaks = num_throttled_leaks;
    saved_total_leaks = num_total_leaks;
    saved_leaks_ignored = num_leaks_ignored;
    saved_suppressed_leaks = num_suppressed_leaks;
    saved_possible_leaks_unique = num_unique[ERROR_POSSIBLE_LEAK];
    saved_possible_leaks_total = num_total[ERROR_POSSIBLE_LEAK];
    saved_reachable_leaks = num_reachable_leaks;
    saved_leaks_unique = num_unique[ERROR_LEAK];
    saved_leaks_total = num_total[ERROR_LEAK];
    saved_bytes_leaked = num_bytes_leaked;
    saved_bytes_possible_leaked = num_bytes_possible_leaked;
    dr_mutex_unlock(error_lock);
}

/* restores the values of all counts that are modified in report_leak() to their
 * values as recorded in the last report_leak_stats_checkpoint() call.
 */
void
report_leak_stats_revert(void)
{
    int i;
    dr_mutex_lock(error_lock);
    num_throttled_leaks = saved_throttled_leaks;
    num_total_leaks = saved_total_leaks;
    num_leaks_ignored = saved_leaks_ignored;
    num_suppressed_leaks = saved_suppressed_leaks;
    num_unique[ERROR_POSSIBLE_LEAK] = saved_possible_leaks_unique;
    num_total[ERROR_POSSIBLE_LEAK] = saved_possible_leaks_total;
    num_reachable_leaks = saved_reachable_leaks;
    num_total[ERROR_LEAK] = saved_leaks_total;
    num_unique[ERROR_LEAK] = saved_leaks_unique;
    num_bytes_leaked = saved_bytes_leaked;
    num_bytes_possible_leaked = saved_bytes_possible_leaked;
    /* Clear leak error counts */
    for (i = 0; i < HASHTABLE_SIZE(error_table.table_bits); i++) {
        hash_entry_t *he;
        for (he = error_table.table[i]; he != NULL; he = he->next) {
            stored_error_t *err = (stored_error_t *) he->payload;
            if (err->errtype == ERROR_LEAK ||
                err->errtype == ERROR_POSSIBLE_LEAK) {
                err->count = 0;
            }
        }
    }
    dr_mutex_unlock(error_lock);
}

void
report_leak(bool known_malloc, app_pc addr, size_t size, size_t indirect_size,
            bool early, bool reachable, bool maybe_reachable, uint shadow_state,
            packed_callstack_t *pcs)
{
    /* If not in a known malloc region it could be an unaddressable byte
     * that was erroneously written to (and we reported already) but
     * we then marked as defined to avoid further errors: so only complain
     * if in known malloc regions.
     */
    ssize_t len = 0;
    size_t sofar = 0;
    char *buf, *buf_print;
    size_t bufsz;
    void *drcontext = dr_get_current_drcontext();
    bool suppressed = false;
    const char *label = NULL;
    bool locked_malloc = false;
    bool printed_leading_newline = false;
    stored_error_t *err = NULL;
    uint type;
    char *cstack_start;
#ifdef USE_DRSYMS
    /* only real and possible leaks go to results.txt */
    file_t tofile = f_global;
#endif

    /* Only consider report_leak_max for check_leaks, and don't count
     * reachable toward the max
     */
    if (reachable) {
        /* if options.show_reachable and past report_leak_max, we'll inc
         * this counter and num_throttled_leaks: oh well.
         */
        num_reachable_leaks++;
        if (!options.show_reachable)
            return;
        label = "REACHABLE";
    } else if (!known_malloc) {
        /* This is really a curiosity for developers: not an error for
         * addressable memory to remain within a heap region.
         */
        if (options.verbose < 2)
            return;
        label = "STILL-ADDRESSABLE ";
    }

    if (options.report_leak_max >= 0 && num_total_leaks >= options.report_leak_max) {
        num_throttled_leaks++;
        return;
    }
    if (drcontext == NULL || dr_get_tls_field(drcontext) == NULL) {
        /* at exit time thread already cleaned up */
        bufsz = MAX_ERROR_INITIAL_LINES + max_callstack_size();
        buf = (char *) global_alloc(bufsz, HEAPSTAT_CALLSTACK);
    } else {
        per_thread_t *pt = (per_thread_t *) dr_get_tls_field(drcontext);
        buf = pt->errbuf;
        bufsz = pt->errbufsz;
    }
    buf[0] = '\0';
    num_total_leaks++;

    /* we need to know the type prior to dup checking */
    if (label != NULL) {
        type = ERROR_MAX_VAL;
    } else if (early && !reachable && options.ignore_early_leaks) {
        /* early reachable are listed as reachable, not ignored */
        label = "IGNORED ";
        num_leaks_ignored++;
        type = ERROR_MAX_VAL;
    } else if (maybe_reachable) {
        type = ERROR_POSSIBLE_LEAK;
        IF_DRSYMS(tofile = f_results;)
    } else {
        type = ERROR_LEAK;
        IF_DRSYMS(tofile = f_results;)
    }

    /* protect counter updates below */
    dr_mutex_lock(error_lock);
    if (options.check_leaks) {
        /* Though the top frame makes less sense for leaks we do the same
         * top-frame check as for other error suppression.
         * FIXME PR 460923: support matching any prefix
         */
        if (pcs == NULL) {
            locked_malloc = true;
            malloc_lock(); /* unlocked below */
            pcs = malloc_get_client_data(addr);
        }
        ASSERT(pcs != NULL, "malloc must have callstack");

        /* We check dups only for real and possible leaks.
         * We have no way to eliminate dups for !check_leaks.
         */
        if (type < ERROR_MAX_VAL) {
            err = record_error(type, pcs, NULL, NULL, true/*hold lock*/);
            if (err->count > 1) {
                /* Duplicate */
                if (err->suppressed)
                    num_suppressed_leaks++;
                else {
                    /* We only count bytes for non-suppressed leaks */
                    /* Total size does not distinguish direct from indirect (PR 576032) */
                    if (maybe_reachable)
                        num_bytes_possible_leaked += size + indirect_size;
                    else
                        num_bytes_leaked += size + indirect_size;
                }
                DOLOG(3, {
                    LOG(3, "Duplicate leak of %d (%d indirect) bytes:\n",
                        size, indirect_size);
                    packed_callstack_log(err->pcs, f_global);
                });
                dr_mutex_unlock(error_lock);
                goto report_leak_done;
            }
        }

        /* We need to know whether suppressed so we can prefix "Error" with "SUPPRESSED"
         * and print the right error #.  So we go ahead and do that partway into
         * the buffer, and then later we adjust the prefix to line up.
         */
        sofar = MAX_ERROR_INITIAL_LINES;
        cstack_start = buf + sofar;
        packed_callstack_print(pcs, 0/*all frames*/, buf, bufsz, &sofar);
        if (locked_malloc)
            malloc_unlock();

        /* only real and possible leaks can be suppressed */
        if (type < ERROR_MAX_VAL)
            suppressed = on_suppression_list(type, cstack_start);

        sofar = 0; /* now we print to start */
        if (!suppressed && type < ERROR_MAX_VAL) {
            /* We can have identical leaks across nudges: keep same error #.
             * Multiple nudges are kind of messy wrt leaks: we try to not
             * increment counts or add new leaks that were there in the
             * last nudge, but we do re-print the callstacks so it's
             * easy to see all the nudges at that point.
             */
            if (err->id == 0 && (!maybe_reachable || options.possible_leaks))
                acquire_error_number(err);
            else {
                /* num_unique was set to 0 after nudge */
#ifdef STATISTICS /* for num_nudges */
                ASSERT(err->id == 0 || num_nudges > 0 ||
                       (maybe_reachable && !options.possible_leaks),
                       "invalid dup error report!");
#endif
                num_unique[err->errtype]++;
            }
            printed_leading_newline = true;
            BUFPRINT(buf, bufsz, sofar, len, NL"Error #%d: ", err->id);
            /* We only count bytes for non-suppressed leaks */
            /* Total size does not distinguish direct from indirect (PR 576032) */
            if (maybe_reachable)
                num_bytes_possible_leaked += size + indirect_size;
            else
                num_bytes_leaked += size + indirect_size;
        }
    } else if (type < ERROR_MAX_VAL) {
        /* no dup checking */
        num_unique[type]++;
        if (maybe_reachable)
            num_bytes_possible_leaked += size + indirect_size;
        else
            num_bytes_leaked += size + indirect_size;
    }

    /* ensure starts at beginning of line (can be in middle of another log) */
    if (!options.thread_logs && !printed_leading_newline)
        BUFPRINT(buf, bufsz, sofar, len, ""NL);
    if (label != NULL)
        BUFPRINT(buf, bufsz, sofar, len, label);

    if (suppressed) {
        num_suppressed_leaks++;
        if (err != NULL) {
            err->suppressed = true;
            num_total[type]--;
        }
        BUFPRINT(buf, bufsz, sofar, len, "SUPPRESSED ");
    } else if (maybe_reachable) {
        if (!options.possible_leaks) {
            dr_mutex_unlock(error_lock);
            goto report_leak_done;
        }
        BUFPRINT(buf, bufsz, sofar, len, "POSSIBLE ");
    }
    /* No longer printing out shadow info since it's not relevant for
     * reachability-based leak scanning
     */
    BUFPRINT(buf, bufsz, sofar, len,
             "LEAK %d direct bytes "PFX"-"PFX" + %d indirect bytes"NL,
             size, addr, addr+size, indirect_size);
    buf_print = buf;
    if ((type == ERROR_LEAK && options.check_leaks) ||
        (type == ERROR_POSSIBLE_LEAK && options.possible_leaks)) {
        ASSERT(pcs != NULL, "malloc must have callstack");
        /* Now shift the prefix to abut the callstack */
        ASSERT(sofar < MAX_ERROR_INITIAL_LINES, "buffer too small");
        buf_print = buf + MAX_ERROR_INITIAL_LINES - sofar;
        memmove(buf_print, buf, sofar);
    } else if (type == ERROR_LEAK || type == ERROR_POSSIBLE_LEAK) {
        BUFPRINT(buf, bufsz, sofar, len,
                 "   (run with -check_%sleaks to obtain a callstack)"NL,
                 (type == ERROR_LEAK) ? "" : "possible_");
    }
    dr_mutex_unlock(error_lock);
    report_error_from_buffer(IF_DRSYMS_ELSE(suppressed ? f_global : tofile,
                                            f_global), buf_print, NULL);

 report_leak_done:
    if (drcontext == NULL || dr_get_tls_field(drcontext) == NULL)
        global_free(buf, bufsz, HEAPSTAT_CALLSTACK);
}

/* FIXME: have some report detail threshold or max log file size */
void
report_malloc(app_pc start, app_pc end, const char *routine, dr_mcontext_t *mc)
{
    DOLOG(2, {
        per_thread_t *pt = (per_thread_t *)
            dr_get_tls_field(dr_get_current_drcontext());
        ssize_t len = 0;
        size_t sofar = 0;
        BUFPRINT(pt->errbuf, pt->errbufsz, sofar, len,
                 "%s "PFX"-"PFX"\n", routine, start, end);
        print_callstack(pt->errbuf, pt->errbufsz, &sofar, mc, false /*print addr*/,
                        false/*no fps*/, NULL, 0);
        report_error_from_buffer(pt->f, pt->errbuf, NULL);
    });
}

void
report_heap_region(bool add, app_pc start, app_pc end, dr_mcontext_t *mc)
{
    DOLOG(2, {
        ssize_t len = 0;
        size_t sofar = 0;
        char *buf;
        size_t bufsz;
        void *drcontext = dr_get_current_drcontext();
        per_thread_t *pt = (per_thread_t *)
            ((drcontext == NULL) ? NULL : dr_get_tls_field(drcontext));
        if (pt == NULL) {
            /* at init time no pt yet */
            bufsz = MAX_ERROR_INITIAL_LINES + max_callstack_size();
            buf = (char *) global_alloc(bufsz, HEAPSTAT_CALLSTACK);
        } else {
            buf = pt->errbuf;
            bufsz = pt->errbufsz;
        }
        BUFPRINT(buf, bufsz, sofar, len,
                 "%s heap region "PFX"-"PFX"\n",
                 add ? "adding" : "removing", start, end);
        print_callstack(buf, bufsz, &sofar, mc, false /*print addr*/,
                        false/*no fps*/, NULL, 0);
        report_error_from_buffer(f_global, buf, NULL);
        if (pt == NULL)
            global_free(buf, bufsz, HEAPSTAT_CALLSTACK);
    });
}

#if DEBUG
/* To print call stacks at suspected error sites when actual errors aren't
 * reported.  Helps with debugging.  Unknown ioctl() system calls are an
 * example.  We just skip them and have no idea of who made the call, making it
 * harder to identify data structures to track.
 */
void
report_callstack(void *drcontext, dr_mcontext_t *mc)
{
    print_callstack_to_file(drcontext, mc, mc->xip, INVALID_FILE/*use pt->f*/);
}
#endif /* DEBUG */
