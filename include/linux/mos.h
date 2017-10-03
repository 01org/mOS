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

#if defined(CONFIG_X86_64) || defined(CONFIG_X86_PAE)
enum lwkmem_kind_t {kind_4k = 0, kind_2m, kind_1g, kind_last};
#else
enum lwkmem_kind_t {kind_4k = 0, kind_4m, kind_1g, kind_last};
#endif
extern unsigned long lwk_page_shift[];
enum lwkmem_type_t {lwkmem_dram = 0, lwkmem_hbm, lwkmem_nvram, lwkmem_type_last };
enum allocate_site_t {
	lwkmem_mmap = 0,
	lwkmem_brk = 1,
	lwkmem_static = 2,
	lwkmem_stack = 3,
	lwkmem_site_last = 4
};

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

        /* Amount of memory reseved for this process */
        int64_t lwkmem;

        /* Phys memory regions reserved for this process */
        struct list_head lwkmem_list;

        /* Lists of blocks of each kind partitioned for this process */
	struct list_head free_list[kind_last][MAX_NUMNODES];
	struct list_head busy_list[kind_last][MAX_NUMNODES];

        /* Number of blocks available of each kind */
        int64_t num_blks[kind_last];

	/* brk line for lwkmem heap management */
	unsigned long brk;

	/* end of heap region (minus one).  This may move beyond the
	 * brk value when backing heap with large pages and thus
	 * mapping a region that exceeds the brk line.
	 */
	unsigned long brk_end;

	/* Disable LWK-memory backed heap */
	bool lwkmem_brk_disable;

	int64_t max_page_size;
	int64_t heap_page_size;

	int domain_info[lwkmem_type_last][MAX_NUMNODES];
	int domain_info_len[lwkmem_type_last];
	int domain_order_index[lwkmem_type_last][kind_last];
	bool lwkmem_interleave_disable;
	int lwkmem_interleave;

	struct memory_preference_t {
		enum lwkmem_type_t lower_type_order[lwkmem_type_last];
		enum lwkmem_type_t upper_type_order[lwkmem_type_last];
		unsigned long threshold;
	} memory_preference[lwkmem_site_last];

	bool lwkmem_load_elf_segs;

	unsigned long lwkmem_mmap_aligned_threshold;
	unsigned long lwkmem_mmap_alignment;
	unsigned long lwkmem_next_addr;

	/* Total number of blocks used (allocated). */
	unsigned long blks_allocated[kind_last][MAX_NUMNODES];
	unsigned long blks_in_use[kind_last][MAX_NUMNODES];
	unsigned long max_allocated[MAX_NUMNODES];
	bool report_blks_allocated;

	long brk_clear_len;
	int lwkmem_init;
#endif

#ifdef CONFIG_MOS_SCHEDULER
	/* Scheduler attributes go here */
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
#include <linux/page-flags.h>

/*
 * Upper 6 bytes are magic number ("LWKMEM") to identify a vma containing LWK
 * memory. Use lower 8 bits to store size of page - PAGE_SHIFT. Other bits
 * unused for now.
 * Storing the page size in the vma means we need page-size homogeneous vmas.
 */
#define _LWKMEM_MASK            (0x0ffff)
#define _LWKMEM_ORDER_MASK      (0x000ff)
#define _LWKMEM         ((unsigned long)(0x4c574b4d454d << 16))
#define is_lwkmem(vma)  (((unsigned long)((vma)->vm_private_data) & \
			  ~_LWKMEM_MASK) == _LWKMEM)
#define LWK_PAGE_SHIFT(vma)     (((long)((vma)->vm_private_data) & \
                                  _LWKMEM_ORDER_MASK) + PAGE_SHIFT)
#define _LWKPG          ((unsigned long)(0x004c574b5047))
#define is_lwkpg(page)  ((((page)->private) & _LWKPG) == _LWKPG)

extern struct page *lwkmem_user_to_page(struct mm_struct *mm,
					unsigned long addr,
					unsigned int *size);
extern void lwkmem_available(unsigned long *totalraam, unsigned long *freeraam);

extern unsigned long elf_map_to_lwkmem(unsigned long addr, unsigned long size,
					int prot, int type);
extern long elf_unmap_from_lwkmem(unsigned long addr, unsigned long size);

#else
#define is_lwkmem(vma) 0
#define is_lwkpg(page) 0
#endif /* CONFIG_MOS_LWKMEM */


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

#ifdef CONFIG_MOS_LWKMEM
extern int find_vma_links(struct mm_struct *mm, unsigned long addr,
			  unsigned long end, struct vm_area_struct **pprev,
			  struct rb_node ***rb_link, struct rb_node **rb_parent);

extern void vma_link(struct mm_struct *mm, struct vm_area_struct *vma,
		     struct vm_area_struct *prev, struct rb_node **rb_link,
		     struct rb_node *rb_parent);
#endif

#else
static inline int lwk_config_lwkcpus(char *parm_value, char *p) { return -1; }
static inline int lwk_config_lwkmem(char *parm_value) { return -1; }
static inline void lwkctl_def_partition(void) {}

#define is_lwkmem(vma) 0
#define is_lwkpg(page) 0

#endif /* CONFIG_MOS_FOR_HPC */
#endif /* _LINUX_MOS_H */
