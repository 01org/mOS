/*
 * Multi Operating System (mOS)
 * Copyright (c) 2016, Intel Corporation.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 */

#ifndef _LINUX_MOS_H
#define _LINUX_MOS_H


#include <linux/cpumask.h>
#include <linux/sched.h>
#include <linux/cpuhotplug.h>

/* Is current and MOS task? */
#if defined(CONFIG_MOS_FOR_HPC) && defined(MOS_IS_LWK_PROCESS)
#define is_mostask() (current->mos_flags & MOS_IS_LWK_PROCESS)
#else
#define is_mostask() (0)
#endif

#ifdef CONFIG_MOS_FOR_HPC
#define cpu_lwkcpus_mask this_cpu_ptr(&lwkcpus_mask)
#define cpu_islwkcpu(cpu) cpumask_test_cpu((cpu), cpu_lwkcpus_mask)
#define mos_lwkcpus_arg (&__mos_lwkcpus_arg)
#define mos_sccpus_arg (&__mos_sccpus_arg)
#else
#define cpu_lwkcpus_mask NULL
#define cpu_islwkcpu(cpu) false
#define mos_lwkcpus_arg NULL
#define mos_sccpus_arg NULL
#endif

#ifdef CONFIG_MOS_FOR_HPC

/* mOS definitions */
extern cpumask_t lwkcpus_mask;
extern cpumask_t mos_syscall_mask;
extern cpumask_t __mos_lwkcpus_arg;
extern cpumask_t __mos_sccpus_arg;

extern void mos_linux_enter(void);
extern void mos_linux_leave(void);
extern void mos_sysfs_update(void);

extern void mos_exit_thread(pid_t pid, pid_t tgid);

enum lwkmem_kind_t {kind_4k = 0, kind_2m, kind_4m, kind_1g, kind_last};
extern unsigned long lwk_page_shift[];
enum lwkmem_type_t {lwkmem_dram = 0, lwkmem_hbm, lwkmem_nvram, lwkmem_type_last };

struct mos_process_t {
	struct list_head list;
	pid_t tgid;
	atomic_t alive;
	cpumask_var_t lwkcpus;
	cpumask_var_t utilcpus;

	/* Array of CPUs ordered by allocation sequence */
	int *lwkcpus_sequence;

	/* Are we still yod? */
	struct mm_struct *yod_mm;

	/* Number of LWK CPUs for this process */
	int num_lwkcpus;

	/* Number of utility threads */
	int num_util_threads;

#ifdef CONFIG_MOS_LWKMEM
	/* Memory attributes go here */
#endif

#ifdef CONFIG_MOS_SCHEDULER
	/* Scheduler attributes go here */

	/* Count of threads created */
	atomic_t threads_created;
	/* Original cpus_allowed mask at launch */
	cpumask_var_t original_cpus_allowed;
	/* Control migration of system calls */
	bool move_syscalls_disable;
	/* Enabled round-robin threads. Value=timeslice in ms */
	int enable_rr;
	/* Disable sched_setaffinity. Value = errno+1 */
	int disable_setaffinity;
	/* Logging verbosity for scheduler statistics */
	int sched_stats;
	/* Maximum number of lwkcpus for utility thread use */
	int max_cpus_for_util;
	/* Maximum number of util threads per lwkcpu */
	int max_util_threads_per_cpu;
	/* List of utility threads on LWK CPUs */
	struct list_head util_list;
	/* Mutex for controlling the util_list */
	struct mutex util_list_lock;

#endif
};

extern int mos_register_option_callback(const char *name,
		int (*callback)(const char *, struct mos_process_t *));
extern int mos_unregister_option_callback(const char *name,
		  int (*callback)(const char *, struct mos_process_t *));

struct mos_process_callbacks_t {
	int (*mos_process_init)(struct mos_process_t *);
	int (*mos_process_start)(struct mos_process_t *);
	void (*mos_thread_exit)(struct mos_process_t *);
	void (*mos_process_exit)(struct mos_process_t *);
};

extern int mos_register_process_callbacks(struct mos_process_callbacks_t *);
extern int mos_unregister_process_callbacks(struct mos_process_callbacks_t *);

extern int lwkmem_get(unsigned long *mem, size_t *n) __attribute__((weak));
extern int lwkmem_reserved_get(unsigned long  *mem, size_t *n)
	__attribute__((weak));
extern int lwkmem_request(struct mos_process_t *mos_p, unsigned long *mem,
	size_t n) __attribute__((weak));
extern int lwkmem_get_debug_level(void) __attribute__((weak));
extern void lwkmem_set_debug_level(int level) __attribute__((weak));
extern void lwkmem_release(struct mos_process_t *mos_p) __attribute__((weak));
extern int lwkmem_set_domain_info(struct mos_process_t *mos_p,
	       enum lwkmem_type_t typ, unsigned long *nids,
	       size_t n) __attribute__((weak));

/* Needed by LWKCTL module to trigger the creation of default LWK partition */
extern int lwk_config_lwkcpus(char *param_value, char *profile);
extern int lwk_config_lwkmem(char *param_value);

/*
 * Function exposed by LWK control to trigger the creation of default LWK part-
 * -ition as specified in Linux command line. This function is called from
 * init/main.c during kernel bootup.
 */
extern void lwkctl_def_partition(void);
#ifdef CONFIG_MOS_LWKMEM
/* Memory additions go here */
#endif

#ifdef CONFIG_MOS_SCHEDULER
/* Scheduler additions go here */
#endif

/* The definitions below are Linux definitions. They are collected here for
 * several reasons:
 * 1). They get used in several mOS files; defining them once makes sense
 * 2). This list gives a sense of how intertwined or dependent on Linux mOS is
 * 3). An early warning system for when these definitions change in future
 *         Linux kernels and the mOS code has to adjust
 * 4). Indicate when we change a static Linux definition because we need
 *         access to it from within mOS
 * Make use to include mos.h in Linux files that needed changes in 4.)
 */
extern int do_cpu_up(unsigned int cpu, enum cpuhp_state target);
extern int do_cpu_down(unsigned int cpu, enum cpuhp_state target);

#else
static inline int lwk_config_lwkcpus(char *parm_value, char *p) { return -1; }
static inline int lwk_config_lwkmem(char *parm_value) { return -1; }
static inline void lwkctl_def_partition(void) {}
#endif /* CONFIG_MOS_FOR_HPC */
#endif /* _LINUX_MOS_H */
