/*
  Copyright (C) 2010-2012 Intel Corporation.  All Rights Reserved.

  This file is part of SEP Development Kit

  SEP Development Kit is free software; you can redistribute it
  and/or modify it under the terms of the GNU General Public License
  version 2 as published by the Free Software Foundation.

  SEP Development Kit is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with SEP Development Kit; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA

  As a special exception, you may use this file as part of a free software
  library without restriction.  Specifically, if other files instantiate
  templates or use macros or inline functions from this file, or you compile
  this file and link it with other files to produce an executable, this
  file does not by itself cause the resulting executable to be covered by
  the GNU General Public License.  This exception does not however
  invalidate any other reasons why the executable file might be covered by
  the GNU General Public License.
*/
#include "vtss_config.h"
#include "module.h"
#include "regs.h"
#include "collector.h"

#include <linux/string.h>
#include <linux/timer.h>
#include <linux/delay.h>
#include <linux/kallsyms.h>
#include <linux/sched.h>
#include <linux/pid.h>
#include <linux/slab.h>
#include <linux/kprobes.h>
#include <linux/module.h>
#include <linux/moduleparam.h>

#define VTSS_MODULE_AUTHOR "Dmitry.Eremin@intel.com"
#define VTSS_MODULE_NAME   "vtss++ kernel module (" VTSS_TO_STR(VTSS_VERSION_STRING) ")"

int uid = 0;
module_param(uid, int, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
MODULE_PARM_DESC(uid, "An user id for profiling");

int gid = 0;
module_param(gid, int, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
MODULE_PARM_DESC(gid, "A group id for profiling");

int mode = 0;
module_param(mode, int, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
MODULE_PARM_DESC(mode, "A mode for files in procfs");

#ifdef VTSS_DEBUG_TRACE
static char debug_trace_name[64] = "";
static int  debug_trace_size     = 0;
module_param_string(trace, debug_trace_name, sizeof(debug_trace_name), S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
MODULE_PARM_DESC(trace, "Turn on trace output from functions starting with this name");

int vtss_check_trace(const char* func_name, int* flag)
{
    return (debug_trace_size && !strncmp(func_name, debug_trace_name, debug_trace_size)) ? 1 : -1;
}
#endif

/* ---- probe symbols ---- */

#if defined(CONFIG_X86_32)
#define VTSS_SYMBOL_SCHED_SWITCH  "__switch_to"
#elif defined(CONFIG_X86_64)
#define VTSS_SYMBOL_SCHED_SWITCH  "context_switch"
#endif
#define VTSS_SYMBOL_PROC_FORK     "do_fork"
#define VTSS_SYMBOL_PROC_EXEC     "do_execve"
#define VTSS_SYMBOL_PROC_EXIT     "do_exit"
#define VTSS_SYMBOL_MMAP_REGION   "mmap_region"
#ifdef VTSS_SYSCALL_TRACE
#define VTSS_SYMBOL_SYSCALL_ENTER "syscall_trace_enter"
#define VTSS_SYMBOL_SYSCALL_LEAVE "syscall_trace_leave"
#endif
#if defined(CONFIG_TRACEPOINTS) && defined(VTSS_AUTOCONF_TRACE_EVENTS_SCHED)
#include <trace/events/sched.h>
#ifdef DECLARE_TRACE_NOARGS
#define VTSS_TP_DATA   , NULL
#define VTSS_TP_PROTO  void *cb_data __attribute__ ((unused)),
#else  /* DECLARE_TRACE_NOARGS */
#define VTSS_TP_DATA
#define VTSS_TP_PROTO
#endif /* DECLARE_TRACE_NOARGS */
#endif /* CONFIG_TRACEPOINTS && VTSS_AUTOCONF_TRACE_EVENTS_SCHED */
#ifdef VTSS_AUTOCONF_TRACE_SCHED_RQ
#define VTSS_TP_RQ struct rq* rq,
#else  /* VTSS_AUTOCONF_TRACE_SCHED_RQ */
#define VTSS_TP_RQ
#endif /* VTSS_AUTOCONF_TRACE_SCHED_RQ */

#if defined(CONFIG_TRACEPOINTS) && defined(VTSS_AUTOCONF_TRACE_EVENTS_SCHED)
static void tp_sched_switch(VTSS_TP_PROTO VTSS_TP_RQ struct task_struct *prev, struct task_struct *next)
{
    vtss_sched_switch(prev, next);
}
#endif

static void jp_sched_switch(VTSS_TP_RQ struct task_struct *prev, struct task_struct *next)
{
    vtss_sched_switch(prev, next);
    jprobe_return();
}

#if defined(CONFIG_TRACEPOINTS) && defined(VTSS_AUTOCONF_TRACE_EVENTS_SCHED)
static void tp_sched_process_fork(VTSS_TP_PROTO struct task_struct *task, struct task_struct *child)
{
    vtss_target_fork(task, child);
}
#endif

static int rp_sched_process_fork_enter(struct kretprobe_instance *ri, struct pt_regs *regs)
{
    /* Skip kernel threads or if no memory */
    return (current->mm == NULL) ? 1 : 0;
}

static int rp_sched_process_fork_leave(struct kretprobe_instance *ri, struct pt_regs *regs)
{
    pid_t pid = (pid_t)regs_return_value(regs);

    if (pid) {
        struct task_struct *task = NULL;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,31)
        struct pid *p_pid = find_get_pid(pid);
        task = pid_task(p_pid, PIDTYPE_PID);
        put_pid(p_pid);
#else /* < 2.6.31 */
        rcu_read_lock();
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,24)
        task = find_task_by_vpid(pid);
#else /* < 2.6.24 */
        task = find_task_by_pid(pid);
#endif /* 2.6.24 */
        rcu_read_unlock();
#endif /* 2.6.31 */
        vtss_target_fork(current, task);
    }
    return 0;
}

/* per-instance private data */
struct rp_sched_process_exec_data
{
    char filename[VTSS_FILENAME_SIZE];
    char config[VTSS_FILENAME_SIZE];
};

static int rp_sched_process_exec_enter(struct kretprobe_instance *ri, struct pt_regs *regs)
{
    int i;
    size_t size = 0;
    char *filename, **envp;
    struct rp_sched_process_exec_data *data = (struct rp_sched_process_exec_data*)ri->data;

    if (current->mm == NULL)
        return 1; /* Skip kernel threads or if no memory */
#if defined(CONFIG_X86_32)
    filename =  (char*)REG(ax, regs);
    envp     = (char**)REG(cx, regs);
#elif defined(CONFIG_X86_64)
    filename =  (char*)REG(di, regs);
    envp     = (char**)REG(dx, regs);
#endif
    if (filename != NULL) {
        char *p = strrchr(filename, '/');
        p = p ? p+1 : filename;
        TRACE("filename: '%s' => '%s'", filename, p);
        size = min((size_t)VTSS_FILENAME_SIZE-1, (size_t)strlen(p));
        memcpy(data->filename, p, size);
    }
    data->filename[size] = '\0';
    size = 0;
    for (i = 0; envp[i] != NULL; i++) {
        TRACE("env[%d]: '%s'\n", i, envp[i]);
        if (!strncmp(envp[i], "INTEL_VTSS_PROFILE_ME=", 22 /*==strlen("INTEL_VTSS_PROFILE_ME=")*/)) {
            char *config = envp[i]+22; /*==strlen("INTEL_VTSS_PROFILE_ME=")*/
            size = min((size_t)VTSS_FILENAME_SIZE-1, (size_t)strlen(config));
            memcpy(data->config, config, size);
            break;
        }
    }
    data->config[size] = '\0';
    TRACE("ri=0x%p, data=0x%p, filename='%s', config='%s'", ri, data, data->filename, data->config);
    vtss_target_exec_enter(ri->task, data->filename, data->config);
    return 0;
}

static int rp_sched_process_exec_leave(struct kretprobe_instance *ri, struct pt_regs *regs)
{
    struct rp_sched_process_exec_data *data = (struct rp_sched_process_exec_data*)ri->data;
    int rc = regs_return_value(regs);

    TRACE("ri=0x%p, data=0x%p, filename='%s', config='%s', rc=%d", ri, data, data->filename, data->config, rc);
    vtss_target_exec_leave(ri->task, data->filename, data->config, rc);
    return 0;
}

#if defined(CONFIG_TRACEPOINTS) && defined(VTSS_AUTOCONF_TRACE_EVENTS_SCHED)
static void tp_sched_process_exit(VTSS_TP_PROTO struct task_struct *task)
{
    vtss_target_exit(task);
}
#endif

static int kp_sched_process_exit(struct kprobe *p, struct pt_regs *regs)
{
    vtss_target_exit(current);
    return 0;
}

/*
unsigned long mmap_region(
    struct file*  file,
    unsigned long addr,
    unsigned long len,
    unsigned long flags,
    unsigned int  vm_flags,
    unsigned long pgoff
);
*/

/* per-instance private data */
struct rp_mmap_region_data
{
    struct file*  file;
    unsigned long addr;
    unsigned long size;
    unsigned long pgoff;
    unsigned int  flags;
};

static int rp_mmap_region_enter(struct kretprobe_instance *ri, struct pt_regs *regs)
{
    struct rp_mmap_region_data *data = (struct rp_mmap_region_data*)ri->data;

    if (current->mm == NULL)
        return 1; /* Skip kernel threads or if no memory */
#if defined(CONFIG_X86_32)
    data->file  = (struct file*)REG(ax, regs);
    data->addr  = REG(dx, regs);
    data->size  = REG(cx, regs);
    /* get the rest from stack */
    data->flags = ((int32_t*)&REG(sp, regs))[2]; /* vm_flags */
    data->pgoff = data->file ? ((int32_t*)&REG(sp, regs))[3] : 0;
#elif defined(CONFIG_X86_64)
    data->file  = (struct file*)REG(di, regs);
    data->addr  = REG(si, regs);
    data->size  = REG(dx, regs);
    data->flags = REG(r8, regs); /* vm_flags */
    data->pgoff = data->file ? REG(r9, regs) : 0;
#endif
    TRACE("ri=0x%p, data=0x%p: (0x%p, 0x%lx, %lu, %lu, 0x%x)", ri, data, data->file, data->addr, data->size, data->pgoff, data->flags);
    return 0;
}

static int rp_mmap_region_leave(struct kretprobe_instance *ri, struct pt_regs *regs)
{
    struct rp_mmap_region_data *data = (struct rp_mmap_region_data*)ri->data;
    unsigned long rc = regs_return_value(regs);

    TRACE("ri=0x%p, data=0x%p: rc=0x%lx", ri, data, rc);
    if ((rc == data->addr) &&
        (data->flags & VM_EXEC) && !(data->flags & VM_WRITE) &&
        data->file && data->file->f_dentry)
    {
        TRACE("file=0x%p, addr=0x%lx, pgoff=%lu, size=%lu", data->file, data->addr, data->pgoff, data->size);
        vtss_mmap(data->file, data->addr, data->pgoff, data->size);
    }
    return 0;
}

#ifdef VTSS_SYSCALL_TRACE

static int kp_syscall_enter(struct kprobe *p, struct pt_regs *regs)
{
    struct pt_regs* sregs;

    if (current->mm == NULL)
        return 1; /* Skip kernel threads or if no memory */
#if defined(CONFIG_X86_32)
    sregs = (struct pt_regs*)REG(ax, regs);
#elif defined(CONFIG_X86_64)
    sregs = (struct pt_regs*)REG(di, regs);
#endif
    vtss_syscall_enter(sregs);
    return 0;
}

static int kp_syscall_leave(struct kprobe *p, struct pt_regs *regs)
{
    struct pt_regs* sregs;

    if (current->mm == NULL)
        return 1; /* Skip kernel threads or if no memory */
#if defined(CONFIG_X86_32)
    sregs = (struct pt_regs*)REG(ax, regs);
#elif defined(CONFIG_X86_64)
    sregs = (struct pt_regs*)REG(di, regs);
#endif
    vtss_syscall_leave(sregs);
    return 0;
}

#endif /* VTSS_SYSCALL_TRACE */

/* ------------------------------------------------------------------------- */
/* Helpers macros */
#ifdef VTSS_AUTOCONF_KPROBE_FLAGS
#define _SET_KPROBE_FLAGS(name) name.flags = 0;
#else
#define _SET_KPROBE_FLAGS(name)
#endif

#ifdef VTSS_AUTOCONF_KPROBE_SYMBOL_NAME
#define _SET_SYMBOL_NAME(symbol) .symbol_name = symbol,
#define _SET_KP_SYMBOL_NAME(symbol) .kp.symbol_name = symbol,
#define _LOOKUP_SYMBOL_NAME(name,symbol) /* empty */
#else
#define _SET_SYMBOL_NAME(symbol) /* empty */
#define _SET_KP_SYMBOL_NAME(symbol) /* empty */
#define _LOOKUP_SYMBOL_NAME(name,symbol) \
    name.addr = (kprobe_opcode_t*)kallsyms_lookup_name(symbol); \
    if (!name.addr) { \
        ERROR("Unable to find symbol '%s'", symbol); \
        rc = -1; \
    } else
#endif

/* ------------------------------------------------------------------------- */
/* Define kprobe stub */
#define DEFINE_KP_STUB(name,symbol) \
static struct kprobe _kp_##name = { \
    .pre_handler   = kp_##name, \
    .post_handler  = NULL, \
    .fault_handler = NULL, \
    _SET_SYMBOL_NAME(symbol) \
    .addr = (kprobe_opcode_t*)NULL \
}; \
static int probe_##name(void) \
{ \
    int rc = 0; \
    _REGISTER_TRACE(name) \
    { \
        _LOOKUP_SYMBOL_NAME(_kp_##name,symbol) \
        { \
            _SET_KPROBE_FLAGS(_kp_##name) \
            rc = register_kprobe(&_kp_##name); \
            if (rc) ERROR("register_kprobe('%s') failed: %d", symbol, rc); \
        } \
    } \
    return rc; \
} \
static int unprobe_##name(void) \
{ \
    int rc = 0; \
    _UNREGISTER_TRACE(name) \
    if (_kp_##name.addr) unregister_kprobe(&_kp_##name); \
    _kp_##name.addr = NULL; \
    return rc; \
}

/* ------------------------------------------------------------------------- */
/* Define jprobe stub */
#define DEFINE_JP_STUB(name,symbol) \
static struct jprobe _jp_##name = { \
    _SET_KP_SYMBOL_NAME(symbol) \
    .kp.addr = (kprobe_opcode_t*)NULL, \
    .entry = (kprobe_opcode_t*)jp_##name \
}; \
static int probe_##name(void) \
{ \
    int rc = 0; \
    _REGISTER_TRACE(name) \
    { \
        _LOOKUP_SYMBOL_NAME(_jp_##name.kp,symbol) \
        { \
            _SET_KPROBE_FLAGS(_jp_##name.kp) \
            rc = register_jprobe(&_jp_##name); \
            if (rc) ERROR("register_jprobe('%s') failed: %d", symbol, rc); \
        } \
    } \
    return rc; \
} \
static int unprobe_##name(void) \
{ \
    int rc = 0; \
    _UNREGISTER_TRACE(name) \
    if (_jp_##name.kp.addr) unregister_jprobe(&_jp_##name); \
    _jp_##name.kp.addr = NULL; \
    return rc; \
}

/* ------------------------------------------------------------------------- */
/* Define kretprobe stub */
#define DEFINE_RP_STUB(name,symbol,size) \
static struct kretprobe _rp_##name = { \
    _SET_KP_SYMBOL_NAME(symbol) \
    .kp.addr       = (kprobe_opcode_t*)NULL, \
    .entry_handler = rp_##name##_enter, \
    .handler       = rp_##name##_leave, \
    .data_size     = size, \
    .maxactive     = 16 /* probe up to 16 instances concurrently */ \
}; \
static int probe_##name(void) \
{ \
    int rc = 0; \
    _REGISTER_TRACE(name) \
    { \
        _LOOKUP_SYMBOL_NAME(_rp_##name.kp,symbol) \
        { \
            _SET_KPROBE_FLAGS(_rp_##name.kp) \
            rc = register_kretprobe(&_rp_##name); \
            if (rc) ERROR("register_kretprobe('%s') failed: %d", symbol, rc); \
        } \
    } \
    return rc; \
} \
static int unprobe_##name(void) \
{ \
    int rc = 0; \
    _UNREGISTER_TRACE(name) \
    if (_rp_##name.kp.addr) unregister_kretprobe(&_rp_##name); \
    _rp_##name.kp.addr = NULL; \
    if (_rp_##name.nmissed) INFO("Missed probing %d instances of '%s'", _rp_##name.nmissed, symbol); \
    return rc; \
}

/* ------------------------------------------------------------------------- */
/* stubs without tracepoints */
#define _REGISTER_TRACE(name)   /* empty */
#define _UNREGISTER_TRACE(name) /* empty */

DEFINE_RP_STUB(sched_process_exec, VTSS_SYMBOL_PROC_EXEC,   sizeof(struct rp_sched_process_exec_data))
DEFINE_RP_STUB(mmap_region,        VTSS_SYMBOL_MMAP_REGION, sizeof(struct rp_mmap_region_data))
#ifdef VTSS_SYSCALL_TRACE
DEFINE_KP_STUB(syscall_enter,      VTSS_SYMBOL_SYSCALL_ENTER)
DEFINE_KP_STUB(syscall_leave,      VTSS_SYMBOL_SYSCALL_LEAVE)
#endif

/* ------------------------------------------------------------------------- */
/* stubs with tracepoints */
#if defined(CONFIG_TRACEPOINTS) && defined(VTSS_AUTOCONF_TRACE_EVENTS_SCHED)
#undef _REGISTER_TRACE
#undef _UNREGISTER_TRACE
#define _REGISTER_TRACE(name) \
    rc = register_trace_##name(tp_##name VTSS_TP_DATA); \
    if (rc) INFO("Unable register tracepoint: %d", rc); \
    if (rc)
#define _UNREGISTER_TRACE(name) \
    rc = unregister_trace_##name(tp_##name VTSS_TP_DATA);
#endif

DEFINE_JP_STUB(sched_switch,       VTSS_SYMBOL_SCHED_SWITCH)
DEFINE_RP_STUB(sched_process_fork, VTSS_SYMBOL_PROC_FORK, 0)
DEFINE_KP_STUB(sched_process_exit, VTSS_SYMBOL_PROC_EXIT)

/* ------------------------------------------------------------------------- */
/* kernel module notifier */
static int vtss_kmodule_notifier(struct notifier_block *block, unsigned long val, void *data)
{
    struct module *mod = (struct module*)data;
    const char *name = mod->name;
    unsigned long module_core = (unsigned long)mod->module_core;
    unsigned long core_size = mod->core_size;

    if (val == MODULE_STATE_COMING) {
        TRACE("MODULE_STATE_COMING: name='%s', module_core=0x%lx, core_size=%lu", name, module_core, core_size);
        vtss_kmap(current, name, module_core, 0, core_size);
    } else if (val == MODULE_STATE_GOING) {
        TRACE("MODULE_STATE_GOING:  name='%s'", name);
    }
    return NOTIFY_DONE;
}

static struct notifier_block vtss_kmodules_nb = {
    .notifier_call = &vtss_kmodule_notifier
};

static int probe_kmodules(void)
{
    return register_module_notifier(&vtss_kmodules_nb);
}

static int unprobe_kmodules(void)
{
    return unregister_module_notifier(&vtss_kmodules_nb);
}

int vtss_probe_init(void)
{
    int rc = 0;

#ifdef VTSS_SYSCALL_TRACE
    rc |= probe_syscall_leave();
    rc |= probe_syscall_enter();
#endif
    rc |= probe_sched_process_exit();
    rc |= probe_sched_process_fork();
    rc |= probe_sched_process_exec();
    rc |= probe_mmap_region();
    rc |= probe_kmodules();
#if !defined(CONFIG_PREEMPT_NOTIFIERS) || !defined(VTSS_USE_PREEMPT_NOTIFIERS)
    rc |= probe_sched_switch();
#endif
    return rc;
}

void vtss_probe_fini(void)
{
#if !defined(CONFIG_PREEMPT_NOTIFIERS) || !defined(VTSS_USE_PREEMPT_NOTIFIERS)
    unprobe_sched_switch();
#endif
    unprobe_kmodules();
    unprobe_mmap_region();
    unprobe_sched_process_exec();
    unprobe_sched_process_fork();
    unprobe_sched_process_exit();
#ifdef VTSS_SYSCALL_TRACE
    unprobe_syscall_enter();
    unprobe_syscall_leave();
#endif
#if defined(CONFIG_TRACEPOINTS) && defined(VTSS_AUTOCONF_TRACE_EVENTS_SCHED)
    tracepoint_synchronize_unregister();
#endif
}

/* ----- module init/fini ----- */

void cleanup_module(void)
{
    vtss_fini();
    printk(VTSS_MODULE_NAME " unregistered\n");
}

int init_module(void)
{
    int rc = 0;

#ifdef VTSS_DEBUG_TRACE
    if (*debug_trace_name != '\0')
        debug_trace_size = strlen(debug_trace_name);
#endif
    rc = vtss_init();
    if (!rc) {
        printk(VTSS_MODULE_NAME " registered\n");
    } else {
        printk(VTSS_MODULE_NAME " initialization falied\n");
        vtss_fini();
    }
    return rc;
}

MODULE_LICENSE("GPL");
MODULE_AUTHOR(VTSS_MODULE_AUTHOR);
MODULE_DESCRIPTION(VTSS_MODULE_NAME);
