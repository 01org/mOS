/*
 * IA-32 Huge TLB Page Support for Kernel.
 *
 * Copyright (C) 2002, Rohit Seth <rohit.seth@intel.com>
 */

#include <linux/config.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/hugetlb.h>
#include <linux/pagemap.h>
#include <linux/smp_lock.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/err.h>
#include <linux/sysctl.h>
#include <linux/mempolicy.h>
#include <asm/mman.h>
#include <asm/pgalloc.h>
#include <asm/tlb.h>
#include <asm/tlbflush.h>

/* AK: this should be all moved into the pgdat */

static long    htlbpagemem[MAX_NUMNODES];
int     htlbpage_max;
static long    htlbzone_pages[MAX_NUMNODES];

static struct list_head hugepage_freelists[MAX_NUMNODES];
static spinlock_t htlbpage_lock = SPIN_LOCK_UNLOCKED;

static int hugetlb_alloc_fault(struct mm_struct *mm, struct vm_area_struct *vma, 
			       unsigned long addr, int flush);

static void enqueue_huge_page(struct page *page)
{
	list_add(&page->lru,
		&hugepage_freelists[page_zone(page)->zone_pgdat->node_id]);
}

static struct page *dequeue_huge_page(struct vm_area_struct *vma, unsigned long addr)
{
	int nid = mpol_first_node(vma, addr); 
	struct page *page = NULL;

	if (list_empty(&hugepage_freelists[nid])) {
		for (nid = 0; nid < MAX_NUMNODES; ++nid)
			if (mpol_node_valid(nid, vma, addr) && 
			    !list_empty(&hugepage_freelists[nid]))
				break;
	}
	if (nid >= 0 && nid < MAX_NUMNODES && !list_empty(&hugepage_freelists[nid])) {
		page = list_entry(hugepage_freelists[nid].next, struct page, lru);
		list_del(&page->lru);
	}
	return page;
}

static struct page *alloc_fresh_huge_page(void)
{
	static int nid = 0;
	struct page *page;
	page = alloc_pages_node(nid, GFP_HIGHUSER, HUGETLB_PAGE_ORDER);
	nid = (nid + 1) % numnodes;
	return page;
}

static void free_huge_page(struct page *page);

/* you must hold the htlbpage_lock spinlock before calling this */
static struct page *__alloc_hugetlb_page(struct vm_area_struct *vma, unsigned long addr)
{
	int i;
	struct page *page;

	page = dequeue_huge_page(vma, addr);
	if (!page)
		return NULL;
	htlbpagemem[page_zone(page)->zone_pgdat->node_id]--;
	set_page_count(page, 1);
	page->lru.prev = (void *)free_huge_page;
	for (i = 0; i < (HPAGE_SIZE/PAGE_SIZE); ++i)
		clear_highpage(&page[i]);
	return page;
}

static pte_t *huge_pte_alloc(struct mm_struct *mm, unsigned long addr)
{
	pgd_t *pgd;
	pmd_t *pmd = NULL;

	pgd = pgd_offset(mm, addr);
	pmd = pmd_alloc(mm, pgd, addr);
	return (pte_t *) pmd;
}

static pte_t *huge_pte_offset(struct mm_struct *mm, unsigned long addr)
{
	pgd_t *pgd;
	pmd_t *pmd = NULL;

	pgd = pgd_offset(mm, addr);
	if (pgd_none(*pgd))
		return NULL;
	if (pgd_present(*pgd))
		pmd = pmd_offset(pgd, addr);
	return (pte_t *) pmd;
}

static void set_huge_pte(struct mm_struct *mm, struct vm_area_struct *vma, struct page *page, pte_t * page_table, int write_access)
{
	pte_t entry;

	mm->rss += (HPAGE_SIZE / PAGE_SIZE);
	if (write_access) {
		entry =
		    pte_mkwrite(pte_mkdirty(mk_pte(page, vma->vm_page_prot)));
	} else
		entry = pte_wrprotect(mk_pte(page, vma->vm_page_prot));
	entry = pte_mkyoung(entry);
	mk_pte_huge(entry);
	set_pte(page_table, entry);
}

/*
 * This function checks for proper alignment of input addr and len parameters.
 */
int is_aligned_hugepage_range(unsigned long addr, unsigned long len)
{
	if (len & ~HPAGE_MASK)
		return -EINVAL;
	if (addr & ~HPAGE_MASK)
		return -EINVAL;
	return 0;
}

int copy_hugetlb_page_range(struct mm_struct *dst, struct mm_struct *src,
			struct vm_area_struct *vma)
{
	pte_t *src_pte, *dst_pte, entry;
	struct page *ptepage;
	unsigned long addr = vma->vm_start;
	unsigned long end = vma->vm_end;

	for (; addr < end; addr += HPAGE_SIZE) {
		pmd_t *pmd;

		src_pte = huge_pte_offset(src, addr);

		pmd = (pmd_t *)src_pte;
		if (!src_pte || pte_none(*src_pte) || !pmd_large(*pmd))
			continue;

		dst_pte = huge_pte_alloc(dst, addr);
		if (!dst_pte)
			goto nomem;

		pmd = (pmd_t *)dst_pte;

		if (!pmd_none(*pmd) && !pmd_large(*pmd)) {
			struct page * page;
			page = pmd_page(*pmd);
			dec_page_state(nr_page_table_pages);
			page_cache_release(page);
			pmd_clear(pmd);
		}

		entry = *src_pte;
		ptepage = pte_page(entry);
		get_page(ptepage);
		set_pte(dst_pte, entry);
		dst->rss += (HPAGE_SIZE / PAGE_SIZE);
	}
	return 0;

nomem:
	return -ENOMEM;
}

/* no force parameter right now unlike get_user_pages itself.
   Could get funny effects on ptrace/core dumps. */
int
follow_hugetlb_page(struct mm_struct *mm, struct vm_area_struct *vma,
		    struct page **pages, struct vm_area_struct **vmas,
		    unsigned long *position, int *length, int i, int write)
{
	unsigned long vpfn, vaddr = *position;
	int remainder = *length;
	
	long needed_flags = write ? VM_WRITE : VM_READ; 
	if ((vma->vm_flags & needed_flags) == 0)
		return -EFAULT;

	spin_lock(&mm->page_table_lock);
	vpfn = vaddr/PAGE_SIZE;
	while (vaddr < vma->vm_end && remainder) {

		if (pages) {
			pte_t *pte;
			struct page *page;

			for (;;) { 
				pte = huge_pte_offset(mm, vaddr);
				if (pte && !pte_none(*pte)) {
					pmd_t *pmd = (pmd_t *)pte;
					if (!pmd_large(*pmd)) {
						page = pmd_page(*pmd);
						dec_page_state(nr_page_table_pages);
						page_cache_release(page);
						pmd_clear(pmd);
					}
					else
						break;
				}
				switch (hugetlb_alloc_fault(mm, vma, vaddr, 0)) { 
				case VM_FAULT_SIGBUS:
					return -EFAULT;
				case VM_FAULT_OOM:
					return -ENOMEM; /* or better kill? */
				} 
				spin_lock(&mm->page_table_lock);
			}

			page = &pte_page(*pte)[vpfn % (HPAGE_SIZE/PAGE_SIZE)];

			WARN_ON(!PageCompound(page));

			get_page(page);
			pages[i] = page;
		}

		if (vmas)
			vmas[i] = vma;

		vaddr += PAGE_SIZE;
		++vpfn;
		--remainder;
		++i;
	}

	spin_unlock(&mm->page_table_lock);

	*length = remainder;
	*position = vaddr;

	return i;
}

#if 0	/* This is just for testing */
struct page *
follow_huge_addr(struct mm_struct *mm,
	struct vm_area_struct *vma, unsigned long address, int write)
{
	unsigned long start = address;
	int length = 1;
	int nr;
	struct page *page;

	nr = follow_hugetlb_page(mm, vma, &page, NULL, &start, &length, 0);
	if (nr == 1)
		return page;
	return NULL;
}

/*
 * If virtual address `addr' lies within a huge page, return its controlling
 * VMA, else NULL.
 */
struct vm_area_struct *hugepage_vma(struct mm_struct *mm, unsigned long addr)
{
	if (mm->used_hugetlb) {
		struct vm_area_struct *vma = find_vma(mm, addr);
		if (vma && is_vm_hugetlb_page(vma))
			return vma;
	}
	return NULL;
}

int pmd_huge(pmd_t pmd)
{
	return 0;
}

struct page *
follow_huge_pmd(struct mm_struct *mm, unsigned long address,
		pmd_t *pmd, int write)
{
	return NULL;
}

#else

struct page *
follow_huge_addr(struct mm_struct *mm,
	struct vm_area_struct *vma, unsigned long address, int write)
{
	return NULL;
}

struct vm_area_struct *hugepage_vma(struct mm_struct *mm, unsigned long addr)
{
	return NULL;
}

int pmd_huge(pmd_t pmd)
{
	return !!(pmd_val(pmd) & _PAGE_PSE);
}

struct page *
follow_huge_pmd(struct mm_struct *mm, unsigned long address,
		pmd_t *pmd, int write)
{
	struct page *page;

	page = pte_page(*(pte_t *)pmd);
	if (page)
		page += ((address & ~HPAGE_MASK) >> PAGE_SHIFT);
	return page;
}
#endif

static void free_huge_page(struct page *page)
{
	BUG_ON(page_count(page));

	INIT_LIST_HEAD(&page->lru);
	page[1].mapping = NULL;

	spin_lock(&htlbpage_lock);
	enqueue_huge_page(page);
	htlbpagemem[page_zone(page)->zone_pgdat->node_id]++;
	spin_unlock(&htlbpage_lock);
}

static void __free_huge_page(struct page *page)
{
	BUG_ON(page_count(page));

	INIT_LIST_HEAD(&page->lru);
	page[1].mapping = NULL;

	enqueue_huge_page(page);
	htlbpagemem[page_zone(page)->zone_pgdat->node_id]++;
}

void huge_page_release(struct page *page)
{
	if (!put_page_testzero(page))
		return;

	free_huge_page(page);
}

void unmap_hugepage_range(struct vm_area_struct *vma,
		unsigned long start, unsigned long end)
{
	struct mm_struct *mm = vma->vm_mm;
	unsigned long address;
	pte_t *pte;
	struct page *page;

	BUG_ON(start & (HPAGE_SIZE - 1));
	BUG_ON(end & (HPAGE_SIZE - 1));

	for (address = start; address < end; address += HPAGE_SIZE) {
		pmd_t *pmd;
		pte = huge_pte_offset(mm, address);
		if (!pte || pte_none(*pte))
			continue;

		pmd = (pmd_t *) pte;
		if (!pmd_none(*pmd) && !pmd_large(*pmd)) {
			page = pmd_page(*pmd);
			dec_page_state(nr_page_table_pages);
			page_cache_release(page);
			pmd_clear(pmd);
			continue;
		}

		page = pte_page(*pte);
		huge_page_release(page);
		pte_clear(pte);
	}
	mm->rss -= (end - start) >> PAGE_SHIFT;
	flush_tlb_range(vma, start, end);
}

void
zap_hugepage_range(struct vm_area_struct *vma,
		unsigned long start, unsigned long length)
{
	struct mm_struct *mm = vma->vm_mm;
	spin_lock(&mm->page_table_lock);
	unmap_hugepage_range(vma, start, start + length);
	spin_unlock(&mm->page_table_lock);
}

/* page_table_lock hold on entry. */
static int 
hugetlb_alloc_fault(struct mm_struct *mm, struct vm_area_struct *vma, 
			       unsigned long addr, int flush)
{
	unsigned long idx;
	int ret;
	pte_t *pte;
	struct page *page = NULL;
	struct address_space *mapping;

	pte = huge_pte_alloc(mm, addr); 

	ret = VM_FAULT_OOM;
	if (unlikely(!pte))
		goto out;

	/* Handle race */
	ret = VM_FAULT_MINOR;
	if (unlikely(!pte_none(*pte)))
		goto flush;

	mapping = vma->vm_file->f_mapping;
	idx = ((addr - vma->vm_start) >> HPAGE_SHIFT)
		+ (vma->vm_pgoff >> (HPAGE_SHIFT - PAGE_SHIFT));
 retry:
	page = find_get_page(mapping, idx);
	if (!page) {
		spin_lock(&htlbpage_lock);

		/* Should do this at prefault time, but that gets us into
		   trouble with freeing right now. We do a quick overcommit 
		   check instead. */
		ret = hugetlb_get_quota(mapping);
		if (ret) {
			spin_unlock(&htlbpage_lock);
			ret = VM_FAULT_OOM;
			goto out;
		}
		
		page = __alloc_hugetlb_page(vma, addr);
		if (!page) {
			hugetlb_put_quota(mapping);
			spin_unlock(&htlbpage_lock);
			
			/* Instead of OOMing here could just transparently use
			   small pages. */
			if (flush) 
				printk(KERN_INFO "%s[%d] ran out of huge pages. Killed.\n",
				       current->comm, current->pid);
			
			ret = VM_FAULT_OOM;
			goto out;
		}
		ret = add_to_page_cache(page, mapping, idx, GFP_ATOMIC);
		if (likely(!ret))
			unlock_page(page);
		else {        		
			hugetlb_put_quota(mapping);
			if (put_page_testzero(page))
				__free_huge_page(page);
			spin_unlock(&htlbpage_lock);
			if (ret == -EEXIST)
				goto retry;
			ret = VM_FAULT_SIGBUS;
			goto out;
		}
		spin_unlock(&htlbpage_lock);
		ret = VM_FAULT_MAJOR; 
	} else
		ret = VM_FAULT_MINOR;
		
	set_huge_pte(mm, vma, page, pte, vma->vm_flags & VM_WRITE);

 flush:
	/* Don't need to flush other CPUs. They will just do a page
	   fault and flush it lazily. */
	if (flush) 
		__flush_tlb_one(addr);
	
 out:
	spin_unlock(&mm->page_table_lock);
	return ret;
}

int arch_hugetlb_fault(struct mm_struct *mm, struct vm_area_struct *vma, 
		       unsigned long address, int write_access)
{ 
	pmd_t *pmd;
	pgd_t *pgd;
	struct page *page;

	if (write_access && !(vma->vm_flags & VM_WRITE))
		return VM_FAULT_SIGBUS;

	spin_lock(&mm->page_table_lock);	
	pgd = pgd_offset(mm, address); 
	if (pgd_none(*pgd)) 
		return hugetlb_alloc_fault(mm, vma, address, 1); 

	pmd = pmd_offset(pgd, address);

	if (!pmd_none(*pmd) && !pmd_large(*pmd)) {
		page = pmd_page(*pmd);
		dec_page_state(nr_page_table_pages);
		page_cache_release(page);
		pmd_clear(pmd);
	}

	if (pmd_none(*pmd))
		return hugetlb_alloc_fault(mm, vma, address, 1); 

	BUG_ON(!pmd_large(*pmd)); 

	/* must have been a race. Flush the TLB. NX not supported yet. */ 

	__flush_tlb_one(address); 
	spin_lock(&mm->page_table_lock);	
	return VM_FAULT_MINOR;
} 

int hugetlb_prefault(struct address_space *mapping, struct vm_area_struct *vma)
{
	return 0;
}

static void update_and_free_page(struct page *page)
{
	int j;
	struct page *map;

	map = page;
	htlbzone_pages[page_zone(page)->zone_pgdat->node_id]--;
	for (j = 0; j < (HPAGE_SIZE / PAGE_SIZE); j++) {
		map->flags &= ~(1 << PG_locked | 1 << PG_error | 1 << PG_referenced |
				1 << PG_dirty | 1 << PG_active | 1 << PG_reserved |
				1 << PG_private | 1<< PG_writeback);
		set_page_count(map, 0);
		map++;
	}
	set_page_count(page, 1);
	__free_pages(page, HUGETLB_PAGE_ORDER);
}

static int try_to_free_low(int count)
{
	struct list_head *p;
	struct page *page, *map;
	int i;

	page = NULL;
	map = NULL;
	spin_lock(&htlbpage_lock);
	
	for (i = 0; i < MAX_NUMNODES; i++) { 
		list_for_each(p, &hugepage_freelists[i]) {
			if (map) {
				list_del(&map->lru);
				update_and_free_page(map);
				htlbpagemem[page_zone(map)->zone_pgdat->node_id]--;
				map = NULL;
				if (++count == 0)
					break;
			}
			page = list_entry(p, struct page, lru);
			if (!PageHighMem(page))
				map = page;
		}
	}
	if (map) {
		list_del(&map->lru);
		update_and_free_page(map);
		htlbpagemem[page_zone(page)->zone_pgdat->node_id]--;
		count++;
	}
	spin_unlock(&htlbpage_lock);
	return count;
}

static long all_huge_pages(void)
{ 
	long pages = 0;
	int i;
	for (i = 0; i < numnodes; i++) 
		pages += htlbzone_pages[i];
	return pages;
} 

static int set_hugetlb_mem_size(int count)
{
	int lcount;
	struct page *page;
	if (count < 0)
		lcount = count;
	else { 
		lcount = count - all_huge_pages();
	}

	if (lcount == 0)
		return (int)all_huge_pages();
	if (lcount > 0) {	/* Increase the mem size. */
		while (lcount--) {
			int node;
			page = alloc_fresh_huge_page();
			if (page == NULL)
				break;
			spin_lock(&htlbpage_lock);
			enqueue_huge_page(page);
			node = page_zone(page)->zone_pgdat->node_id;
			htlbpagemem[node]++;
			htlbzone_pages[node]++;
			spin_unlock(&htlbpage_lock);
		}
		goto out;
	}
	/* Shrink the memory size. */
	lcount = try_to_free_low(lcount);
	spin_lock(&htlbpage_lock);
	while (lcount++) {
		page = __alloc_hugetlb_page(NULL, 0);
		if (page == NULL)
			break;
		update_and_free_page(page);
	}
	spin_unlock(&htlbpage_lock);
 out:
	return (int)all_huge_pages();
}

int hugetlb_sysctl_handler(ctl_table *table, int write,
		struct file *file, void *buffer, size_t *length)
{
	if (!cpu_has_pse)
		return -ENODEV;
	proc_dointvec(table, write, file, buffer, length);
	htlbpage_max = set_hugetlb_mem_size(htlbpage_max);
	return 0;
}

static int __init hugetlb_setup(char *s)
{
	if (sscanf(s, "%d", &htlbpage_max) <= 0)
		htlbpage_max = 0;
	return 1;
}
__setup("hugepages=", hugetlb_setup);

static int __init hugetlb_init(void)
{
	int i;
	struct page *page;

	if (!cpu_has_pse)
		return -ENODEV;

	for (i = 0; i < MAX_NUMNODES; ++i)
		INIT_LIST_HEAD(&hugepage_freelists[i]);

	for (i = 0; i < htlbpage_max; ++i) {
		int nid; 
		page = alloc_fresh_huge_page();
		if (!page)
			break;
		spin_lock(&htlbpage_lock);
		enqueue_huge_page(page);
		nid = page_zone(page)->zone_pgdat->node_id;
		htlbpagemem[nid]++;
		htlbzone_pages[nid]++;
		spin_unlock(&htlbpage_lock);
	}
	htlbpage_max = i;
	printk("Initial HugeTLB pages allocated: %d\n", i);
	return 0;
}
module_init(hugetlb_init);

int hugetlb_report_meminfo(char *buf)
{
	int i;
	long pages = 0, mem = 0;
	for (i = 0; i < numnodes; i++) {
		pages += htlbzone_pages[i];
		mem += htlbpagemem[i];
	}

	return sprintf(buf,
			"HugePages_Total: %5lu\n"
			"HugePages_Free:  %5lu\n"
			"Hugepagesize:    %5lu kB\n",
			pages,
			mem,
			HPAGE_SIZE/1024);
}

int hugetlb_report_node_meminfo(int node, char *buf)
{
	return sprintf(buf,
			"HugePages_Total: %5lu\n"
			"HugePages_Free:  %5lu\n"
			"Hugepagesize:    %5lu kB\n",
			htlbzone_pages[node],
			htlbpagemem[node],
			HPAGE_SIZE/1024);
}

int __is_hugepage_mem_enough(struct mempolicy *pol, size_t size)
{
	struct vm_area_struct vma = { 
#ifdef CONFIG_NUMA	
		.vm_policy = pol 
#endif	
	}; 
	long pm = 0; 
	int i;
	for (i = 0; i < numnodes; i++) { 
		/* Dumb algorithm, but hopefully does not matter here */
		if (!mpol_node_valid(i, &vma, 0))		
			continue;
		pm += htlbpagemem[i];
	}
	return size/HPAGE_SIZE <= pm;
}

/* Check process policy here. VMA policy is checked in mbind. 
   We do not catch changing of process policy later, but before
   the actual fault. */
int is_hugepage_mem_enough(size_t size)
{
	return __is_hugepage_mem_enough(NULL, size);
}

/* Return the number pages of memory we physically have, in PAGE_SIZE units. */
unsigned long hugetlb_total_pages(void)
{
	return all_huge_pages() * (HPAGE_SIZE / PAGE_SIZE);
}
EXPORT_SYMBOL(hugetlb_total_pages);

/*
 * We cannot handle pagefaults against hugetlb pages at all.  They cause
 * handle_mm_fault() to try to instantiate regular-sized pages in the
 * hugegpage VMA.  do_page_fault() is supposed to trap this, so BUG is we get
 * this far.
 */
static struct page *hugetlb_nopage(struct vm_area_struct *vma,
				unsigned long address, int *unused)
{
	BUG();
	return NULL;
}

static int hugetlb_set_policy(struct vm_area_struct *vma, struct mempolicy *new)
{
	struct inode *inode = vma->vm_file->f_dentry->d_inode;
	return mpol_set_shared_policy(&HUGETLBFS_I(inode)->policy, vma, new);
}

struct vm_operations_struct hugetlb_vm_ops = {
	.nopage = hugetlb_nopage,
	.set_policy = hugetlb_set_policy,	
};
