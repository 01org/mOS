#ifdef __KERNEL__
#ifndef _ASM_IRQ_H
#define _ASM_IRQ_H

/*
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 */

#include <asm/atomic.h>

/*
 * This definition means virtually nothing now and could/should go away.
 */
#define NR_IRQS		512

extern void disable_irq(unsigned int);
extern void disable_irq_nosync(unsigned int);
extern void enable_irq(unsigned int);

/*
 * Valid interrupt numbers are from 0 to MAX_IRQS - 1.
 */
#define MAX_IRQS	(1<<24)

extern void *_get_irq_desc(unsigned int irq);
extern void *_get_real_irq_desc(unsigned int irq);
#define get_irq_desc(irq) ((irq_desc_t *)_get_irq_desc(irq))
#define get_real_irq_desc(irq) ((irq_desc_t *)_get_real_irq_desc(irq))

/* Define a way to iterate across irqs fairly efficiently. */
#define for_each_irq(i) \
	for ((i) = 0; (i) < MAX_IRQS; (i) = _next_irq(i))
unsigned int _next_irq(unsigned int irq);

static __inline__ int irq_canonicalize(int irq)
{
	return irq;
}

/*
 * Because many systems have two overlapping names spaces for
 * interrupts (ISA and XICS for example), and the ISA interrupts
 * have historically not been easy to renumber, we allow ISA
 * interrupts to take values 0 - 15, and shift up the remaining 
 * interrupts by 0x10.  
 *
 * This would be nice to remove at some point as it adds confusion
 * and adds a nasty end case if any platform native interrupts have 
 * values within 0x10 of the end of that namespace.
 */

#define NUM_ISA_INTERRUPTS	0x10
extern int __irq_offset_value;

extern inline int irq_offset_up(int irq)
{
	return(irq + __irq_offset_value);
}

extern inline int irq_offset_down(int irq)
{
	return(irq - __irq_offset_value);
}

extern inline int irq_offset_value(void)
{
	return __irq_offset_value;
}

#endif /* _ASM_IRQ_H */
#endif /* __KERNEL__ */
