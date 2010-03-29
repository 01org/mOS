/*
 *  Copyright (c) 1991,1992,1995  Linus Torvalds
 *  Copyright (c) 1994  Alan Modra
 *  Copyright (c) 1995  Markus Kuhn
 *  Copyright (c) 1996  Ingo Molnar
 *  Copyright (c) 1998  Andrea Arcangeli
 *  Copyright (c) 2002,2006  Vojtech Pavlik
 *  Copyright (c) 2003  Andi Kleen
 *
 */

#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/time.h>
#include <linux/sysctl.h>
#include <linux/percpu.h>
#include <linux/kernel_stat.h>
#include <linux/posix-timers.h>
#include <linux/cpufreq.h>
#include <linux/clocksource.h>
#include <linux/sysdev.h>

#include <asm/vsyscall.h>
#include <asm/delay.h>
#include <asm/time.h>
#include <asm/timer.h>

#include <xen/clock.h>
#include <xen/sysctl.h>
#include <xen/interface/vcpu.h>

#include <asm/i8253.h>
DEFINE_SPINLOCK(i8253_lock);
EXPORT_SYMBOL(i8253_lock);

#ifdef CONFIG_X86_64
volatile unsigned long __jiffies __section_jiffies = INITIAL_JIFFIES;
#endif

#define XEN_SHIFT 22

unsigned int cpu_khz;	/* Detected as we calibrate the TSC */
EXPORT_SYMBOL(cpu_khz);

/* These are peridically updated in shared_info, and then copied here. */
struct shadow_time_info {
	u64 tsc_timestamp;     /* TSC at last update of time vals.  */
	u64 system_timestamp;  /* Time, in nanosecs, since boot.    */
	u32 tsc_to_nsec_mul;
	u32 tsc_to_usec_mul;
	int tsc_shift;
	u32 version;
};
static DEFINE_PER_CPU(struct shadow_time_info, shadow_time);
static struct timespec shadow_tv;
static u32 shadow_tv_version;

static u64 jiffies_bias, system_time_bias;

/* Current runstate of each CPU (updated automatically by the hypervisor). */
DEFINE_PER_CPU(struct vcpu_runstate_info, runstate);

/* Must be signed, as it's compared with s64 quantities which can be -ve. */
#define NS_PER_TICK (1000000000LL/HZ)

static void __clock_was_set(struct work_struct *unused)
{
	clock_was_set();
}
static DECLARE_WORK(clock_was_set_work, __clock_was_set);

/*
 * GCC 4.3 can turn loops over an induction variable into division. We do
 * not support arbitrary 64-bit division, and so must break the induction.
 */
#define clobber_induction_variable(v) asm ( "" : "+r" (v) )

static inline void __normalize_time(time_t *sec, s64 *nsec)
{
	while (*nsec >= NSEC_PER_SEC) {
		clobber_induction_variable(*nsec);
		(*nsec) -= NSEC_PER_SEC;
		(*sec)++;
	}
	while (*nsec < 0) {
		clobber_induction_variable(*nsec);
		(*nsec) += NSEC_PER_SEC;
		(*sec)--;
	}
}

/* Does this guest OS track Xen time, or set its wall clock independently? */
static int independent_wallclock = 0;
static int __init __independent_wallclock(char *str)
{
	independent_wallclock = 1;
	return 1;
}
__setup("independent_wallclock", __independent_wallclock);

int xen_independent_wallclock(void)
{
	return independent_wallclock;
}

/* Permitted clock jitter, in nsecs, beyond which a warning will be printed. */
static unsigned long permitted_clock_jitter = 10000000UL; /* 10ms */
static int __init __permitted_clock_jitter(char *str)
{
	permitted_clock_jitter = simple_strtoul(str, NULL, 0);
	return 1;
}
__setup("permitted_clock_jitter=", __permitted_clock_jitter);

/*
 * Scale a 64-bit delta by scaling and multiplying by a 32-bit fraction,
 * yielding a 64-bit result.
 */
static inline u64 scale_delta(u64 delta, u32 mul_frac, int shift)
{
	u64 product;
#ifdef __i386__
	u32 tmp1, tmp2;
#endif

	if (shift < 0)
		delta >>= -shift;
	else
		delta <<= shift;

#ifdef __i386__
	__asm__ (
		"mul  %5       ; "
		"mov  %4,%%eax ; "
		"mov  %%edx,%4 ; "
		"mul  %5       ; "
		"xor  %5,%5    ; "
		"add  %4,%%eax ; "
		"adc  %5,%%edx ; "
		: "=A" (product), "=r" (tmp1), "=r" (tmp2)
		: "a" ((u32)delta), "1" ((u32)(delta >> 32)), "2" (mul_frac) );
#else
	__asm__ (
		"mul %%rdx ; shrd $32,%%rdx,%%rax"
		: "=a" (product) : "0" (delta), "d" ((u64)mul_frac) );
#endif

	return product;
}

static inline u64 get64(volatile u64 *ptr)
{
#ifndef CONFIG_64BIT
	u64 res;
	__asm__("movl %%ebx,%%eax\n"
		"movl %%ecx,%%edx\n"
		LOCK_PREFIX "cmpxchg8b %1"
		: "=&A" (res) : "m" (*ptr));
	return res;
#else
	return *ptr;
#endif
}

static inline u64 get64_local(volatile u64 *ptr)
{
#ifndef CONFIG_64BIT
	u64 res;
	__asm__("movl %%ebx,%%eax\n"
		"movl %%ecx,%%edx\n"
		"cmpxchg8b %1"
		: "=&A" (res) : "m" (*ptr));
	return res;
#else
	return *ptr;
#endif
}

static void init_cpu_khz(void)
{
	u64 __cpu_khz = 1000000ULL << 32;
	struct vcpu_time_info *info = &vcpu_info(0)->time;
	do_div(__cpu_khz, info->tsc_to_system_mul);
	if (info->tsc_shift < 0)
		cpu_khz = __cpu_khz << -info->tsc_shift;
	else
		cpu_khz = __cpu_khz >> info->tsc_shift;
}

static u64 get_nsec_offset(struct shadow_time_info *shadow)
{
	u64 now, delta;
	rdtscll(now);
	delta = now - shadow->tsc_timestamp;
	return scale_delta(delta, shadow->tsc_to_nsec_mul, shadow->tsc_shift);
}

static inline u64 processed_system_time(void)
{
	return (jiffies_64 - jiffies_bias) * NS_PER_TICK + system_time_bias;
}

static void __update_wallclock(time_t sec, long nsec)
{
	long wtm_nsec, xtime_nsec;
	time_t wtm_sec, xtime_sec;
	u64 tmp, wc_nsec;

	/* Adjust wall-clock time base. */
	wc_nsec = processed_system_time();
	wc_nsec += sec * (u64)NSEC_PER_SEC;
	wc_nsec += nsec;

	/* Split wallclock base into seconds and nanoseconds. */
	tmp = wc_nsec;
	xtime_nsec = do_div(tmp, 1000000000);
	xtime_sec  = (time_t)tmp;

	wtm_sec  = wall_to_monotonic.tv_sec + (xtime.tv_sec - xtime_sec);
	wtm_nsec = wall_to_monotonic.tv_nsec + (xtime.tv_nsec - xtime_nsec);

	set_normalized_timespec(&xtime, xtime_sec, xtime_nsec);
	set_normalized_timespec(&wall_to_monotonic, wtm_sec, wtm_nsec);
}

static void update_wallclock(void)
{
	shared_info_t *s = HYPERVISOR_shared_info;

	do {
		shadow_tv_version = s->wc_version;
		rmb();
		shadow_tv.tv_sec  = s->wc_sec;
		shadow_tv.tv_nsec = s->wc_nsec;
		rmb();
	} while ((s->wc_version & 1) | (shadow_tv_version ^ s->wc_version));

	if (!independent_wallclock)
		__update_wallclock(shadow_tv.tv_sec, shadow_tv.tv_nsec);
}

void xen_check_wallclock_update(void)
{
	if (shadow_tv_version != HYPERVISOR_shared_info->wc_version) {
		write_seqlock(&xtime_lock);
		update_wallclock();
		write_sequnlock(&xtime_lock);
		if (keventd_up())
			schedule_work(&clock_was_set_work);
	}
}

/*
 * Reads a consistent set of time-base values from Xen, into a shadow data
 * area.
 */
static void get_time_values_from_xen(unsigned int cpu)
{
	struct vcpu_time_info   *src;
	struct shadow_time_info *dst;
	unsigned long flags;
	u32 pre_version, post_version;

	src = &vcpu_info(cpu)->time;
	dst = &per_cpu(shadow_time, cpu);

	local_irq_save(flags);

	do {
		pre_version = dst->version = src->version;
		rmb();
		dst->tsc_timestamp     = src->tsc_timestamp;
		dst->system_timestamp  = src->system_time;
		dst->tsc_to_nsec_mul   = src->tsc_to_system_mul;
		dst->tsc_shift         = src->tsc_shift;
		rmb();
		post_version = src->version;
	} while ((pre_version & 1) | (pre_version ^ post_version));

	dst->tsc_to_usec_mul = dst->tsc_to_nsec_mul / 1000;

	local_irq_restore(flags);
}

static inline int time_values_up_to_date(void)
{
	rmb();
	return percpu_read(shadow_time.version) == vcpu_info_read(time.version);
}

static void sync_xen_wallclock(unsigned long dummy);
static DEFINE_TIMER(sync_xen_wallclock_timer, sync_xen_wallclock, 0, 0);
static void sync_xen_wallclock(unsigned long dummy)
{
	time_t sec;
	s64 nsec;
	struct xen_platform_op op;

	BUG_ON(!is_initial_xendomain());
	if (!ntp_synced() || independent_wallclock)
		return;

	write_seqlock_irq(&xtime_lock);

	sec  = xtime.tv_sec;
	nsec = xtime.tv_nsec;
	__normalize_time(&sec, &nsec);

	op.cmd = XENPF_settime;
	op.u.settime.secs        = sec;
	op.u.settime.nsecs       = nsec;
	op.u.settime.system_time = processed_system_time();
	WARN_ON(HYPERVISOR_platform_op(&op));

	update_wallclock();

	write_sequnlock_irq(&xtime_lock);

	/* Once per minute. */
	mod_timer(&sync_xen_wallclock_timer, jiffies + 60*HZ);
}

unsigned long long xen_local_clock(void)
{
	unsigned int cpu = get_cpu();
	struct shadow_time_info *shadow = &per_cpu(shadow_time, cpu);
	u64 time;
	u32 local_time_version;

	do {
		local_time_version = shadow->version;
		rdtsc_barrier();
		time = shadow->system_timestamp + get_nsec_offset(shadow);
		if (!time_values_up_to_date())
			get_time_values_from_xen(cpu);
		barrier();
	} while (local_time_version != shadow->version);

	put_cpu();

	return time;
}

/*
 * Runstate accounting
 */
void get_runstate_snapshot(struct vcpu_runstate_info *res)
{
	u64 state_time;
	struct vcpu_runstate_info *state;

	BUG_ON(preemptible());

	state = &__get_cpu_var(runstate);

	do {
		state_time = get64_local(&state->state_entry_time);
		*res = *state;
	} while (get64_local(&state->state_entry_time) != state_time);

	WARN_ON_ONCE(res->state != RUNSTATE_running);
}

/*
 * Xen sched_clock implementation.  Returns the number of unstolen
 * nanoseconds, which is nanoseconds the VCPU spent in RUNNING+BLOCKED
 * states.
 */
unsigned long long sched_clock(void)
{
	struct vcpu_runstate_info runstate;
	cycle_t now;
	u64 ret;
	s64 offset;

	/*
	 * Ideally sched_clock should be called on a per-cpu basis
	 * anyway, so preempt should already be disabled, but that's
	 * not current practice at the moment.
	 */
	preempt_disable();

	now = xen_local_clock();

	get_runstate_snapshot(&runstate);

	offset = now - runstate.state_entry_time;
	if (offset < 0)
		offset = 0;

	ret = offset + runstate.time[RUNSTATE_running]
	      + runstate.time[RUNSTATE_blocked];

	preempt_enable();

	return ret;
}

unsigned long profile_pc(struct pt_regs *regs)
{
	unsigned long pc = instruction_pointer(regs);

	if (!user_mode_vm(regs) && in_lock_functions(pc)) {
#ifdef CONFIG_FRAME_POINTER
		return *(unsigned long *)(regs->bp + sizeof(long));
#else
		unsigned long *sp =
			(unsigned long *)kernel_stack_pointer(regs);

		/*
		 * Return address is either directly at stack pointer
		 * or above a saved flags. Eflags has bits 22-31 zero,
		 * kernel addresses don't.
		 */
		if (sp[0] >> 22)
			return sp[0];
		if (sp[1] >> 22)
			return sp[1];
#endif
	}

	return pc;
}
EXPORT_SYMBOL(profile_pc);

void mark_tsc_unstable(char *reason)
{
#ifndef CONFIG_XEN /* XXX Should tell the hypervisor about this fact. */
	tsc_unstable = 1;
#endif
}
EXPORT_SYMBOL_GPL(mark_tsc_unstable);

static cycle_t cs_last;

static cycle_t xen_clocksource_read(struct clocksource *cs)
{
#ifdef CONFIG_SMP
	cycle_t last = get64(&cs_last);
	cycle_t ret = xen_local_clock();

	if (unlikely((s64)(ret - last) < 0)) {
		if (last - ret > permitted_clock_jitter
		    && printk_ratelimit()) {
			unsigned int cpu = get_cpu();
			struct shadow_time_info *shadow = &per_cpu(shadow_time, cpu);

			printk(KERN_WARNING "clocksource/%u: "
			       "Time went backwards: "
			       "ret=%Lx delta=%Ld shadow=%Lx offset=%Lx\n",
			       cpu, ret, ret - last, shadow->system_timestamp,
			       get_nsec_offset(shadow));
			put_cpu();
		}
		return last;
	}

	for (;;) {
		cycle_t cur = cmpxchg64(&cs_last, last, ret);

		if (cur == last || (s64)(ret - cur) < 0)
			return ret;
		last = cur;
	}
#else
	return xen_local_clock();
#endif
}

/* No locking required. Interrupts are disabled on all CPUs. */
static void xen_clocksource_resume(struct clocksource *cs)
{
	unsigned long seq;
	unsigned int cpu;

	init_cpu_khz();

	for_each_online_cpu(cpu)
		get_time_values_from_xen(cpu);

	do {
		seq = read_seqbegin(&xtime_lock);
		jiffies_bias = jiffies_64;
	} while (read_seqretry(&xtime_lock, seq));
	system_time_bias = per_cpu(shadow_time, 0).system_timestamp;

	cs_last = xen_local_clock();
}

static struct clocksource clocksource_xen = {
	.name			= "xen",
	.rating			= 400,
	.read			= xen_clocksource_read,
	.mask			= CLOCKSOURCE_MASK(64),
	.mult			= 1 << XEN_SHIFT,		/* time directly in nanoseconds */
	.shift			= XEN_SHIFT,
	.flags			= CLOCK_SOURCE_IS_CONTINUOUS,
	.resume			= xen_clocksource_resume,
};

struct vcpu_runstate_info *setup_runstate_area(unsigned int cpu)
{
	struct vcpu_register_runstate_memory_area area;
	struct vcpu_runstate_info *rs = &per_cpu(runstate, cpu);
	int rc;

	set_xen_guest_handle(area.addr.h, rs);
	rc = HYPERVISOR_vcpu_op(VCPUOP_register_runstate_memory_area, cpu, &area);
	if (rc) {
		BUILD_BUG_ON(RUNSTATE_running);
		memset(rs, 0, sizeof(*rs));
		WARN_ON(rc != -ENOSYS);
	}

	return rs;
}

void xen_read_persistent_clock(struct timespec *ts)
{
	const shared_info_t *s = HYPERVISOR_shared_info;
	u32 version, sec, nsec;
	u64 delta;

	do {
		version = s->wc_version;
		rmb();
		sec     = s->wc_sec;
		nsec    = s->wc_nsec;
		rmb();
	} while ((s->wc_version & 1) | (version ^ s->wc_version));

	delta = xen_local_clock() + (u64)sec * NSEC_PER_SEC + nsec;
	do_div(delta, NSEC_PER_SEC);

	ts->tv_sec = delta;
	ts->tv_nsec = 0;
}

int xen_update_persistent_clock(void)
{
	if (!is_initial_xendomain())
		return -1;
	mod_timer(&sync_xen_wallclock_timer, jiffies + 1);
	return 0;
}

void __init time_init(void)
{
	init_cpu_khz();
	printk(KERN_INFO "Xen reported: %u.%03u MHz processor.\n",
	       cpu_khz / 1000, cpu_khz % 1000);

	setup_runstate_area(0);
	get_time_values_from_xen(0);

	jiffies_bias     = jiffies_64;
	system_time_bias = per_cpu(shadow_time, 0).system_timestamp;

	clocksource_register(&clocksource_xen);

	update_wallclock();

	use_tsc_delay();

	/* Cannot request_irq() until kmem is initialised. */
	late_time_init = xen_clockevents_init;
}

/* Convert jiffies to system time. */
u64 jiffies_to_st(unsigned long j)
{
	unsigned long seq;
	long delta;
	u64 st;

	do {
		seq = read_seqbegin(&xtime_lock);
		delta = j - jiffies;
		if (delta < 1) {
			/* Triggers in some wrap-around cases, but that's okay:
			 * we just end up with a shorter timeout. */
			st = processed_system_time() + NS_PER_TICK;
		} else if (((unsigned long)delta >> (BITS_PER_LONG-3)) != 0) {
			/* Very long timeout means there is no pending timer.
			 * We indicate this to Xen by passing zero timeout. */
			st = 0;
		} else {
			st = processed_system_time() + delta * (u64)NS_PER_TICK;
		}
	} while (read_seqretry(&xtime_lock, seq));

	return st;
}
EXPORT_SYMBOL(jiffies_to_st);

void xen_safe_halt(void)
{
	/* Blocking includes an implicit local_irq_enable(). */
	HYPERVISOR_block();
}
EXPORT_SYMBOL(xen_safe_halt);

void xen_halt(void)
{
	if (irqs_disabled())
		VOID(HYPERVISOR_vcpu_op(VCPUOP_down, smp_processor_id(), NULL));
}
EXPORT_SYMBOL(xen_halt);

#ifdef CONFIG_CPU_FREQ
static int time_cpufreq_notifier(struct notifier_block *nb, unsigned long val, 
				void *data)
{
	struct cpufreq_freqs *freq = data;
	struct xen_platform_op op;

	if (cpu_has(&cpu_data(freq->cpu), X86_FEATURE_CONSTANT_TSC))
		return 0;

	if (val == CPUFREQ_PRECHANGE)
		return 0;

	op.cmd = XENPF_change_freq;
	op.u.change_freq.flags = 0;
	op.u.change_freq.cpu = freq->cpu;
	op.u.change_freq.freq = (u64)freq->new * 1000;
	WARN_ON(HYPERVISOR_platform_op(&op));

	return 0;
}

static struct notifier_block time_cpufreq_notifier_block = {
	.notifier_call = time_cpufreq_notifier
};

static int __init cpufreq_time_setup(void)
{
	if (!cpufreq_register_notifier(&time_cpufreq_notifier_block,
			CPUFREQ_TRANSITION_NOTIFIER)) {
		printk(KERN_ERR "failed to set up cpufreq notifier\n");
		return -ENODEV;
	}
	return 0;
}

core_initcall(cpufreq_time_setup);
#endif

/*
 * /proc/sys/xen: This really belongs in another file. It can stay here for
 * now however.
 */
static ctl_table xen_subtable[] = {
	{
		.procname	= "independent_wallclock",
		.data		= &independent_wallclock,
		.maxlen		= sizeof(independent_wallclock),
		.mode		= 0644,
		.proc_handler	= proc_dointvec
	},
	{
		.procname	= "permitted_clock_jitter",
		.data		= &permitted_clock_jitter,
		.maxlen		= sizeof(permitted_clock_jitter),
		.mode		= 0644,
		.proc_handler	= proc_doulongvec_minmax
	},
	{ }
};
static ctl_table xen_table[] = {
	{
		.procname	= "xen",
		.mode		= 0555,
		.child		= xen_subtable
	},
	{ }
};
static int __init xen_sysctl_init(void)
{
	(void)register_sysctl_table(xen_table);
	return 0;
}
__initcall(xen_sysctl_init);
