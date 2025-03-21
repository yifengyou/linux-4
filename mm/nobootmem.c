﻿// SPDX-License-Identifier: GPL-2.0
/*
 *  bootmem - A boot-time physical memory allocator and configurator
 *
 *  Copyright (C) 1999 Ingo Molnar
 *                1999 Kanoj Sarcar, SGI
 *                2008 Johannes Weiner
 *
 * Access to this subsystem has to be serialized externally (which is true
 * for the boot process anyway).
 */
#include <linux/init.h>
#include <linux/pfn.h>
#include <linux/slab.h>
#include <linux/export.h>
#include <linux/kmemleak.h>
#include <linux/range.h>
#include <linux/memblock.h>
#include <linux/bootmem.h>

#include <asm/bug.h>
#include <asm/io.h>

#include "internal.h"

#ifndef CONFIG_HAVE_MEMBLOCK
#error CONFIG_HAVE_MEMBLOCK not defined
#endif

#ifndef CONFIG_NEED_MULTIPLE_NODES
struct pglist_data __refdata contig_page_data;
EXPORT_SYMBOL(contig_page_data);
#endif

unsigned long max_low_pfn;
unsigned long min_low_pfn;
unsigned long max_pfn;
unsigned long long max_possible_pfn;

static void * __init __alloc_memory_core_early(int nid, u64 size, u64 align,
					u64 goal, u64 limit)
{
	void *ptr;
	u64 addr;
	ulong flags = choose_memblock_flags();

	if (limit > memblock.current_limit)
		limit = memblock.current_limit;

again:
	addr = memblock_find_in_range_node(size, align, goal, limit, nid,
					   flags);
	if (!addr && (flags & MEMBLOCK_MIRROR)) {
		flags &= ~MEMBLOCK_MIRROR;
		pr_warn("Could not allocate %pap bytes of mirrored memory\n",
			&size);
		goto again;
	}
	if (!addr)
		return NULL;

	if (memblock_reserve(addr, size))
		return NULL;

	ptr = phys_to_virt(addr);
	memset(ptr, 0, size);
	/*
	 * The min_count is set to 0 so that bootmem allocated blocks
	 * are never reported as leaks.
	 */
	kmemleak_alloc(ptr, size, 0, 0);
	return ptr;
}

/*
 * free_bootmem_late - free bootmem pages directly to page allocator
 * @addr: starting address of the range
 * @size: size of the range in bytes
 *
 * This is only useful when the bootmem allocator has already been torn
 * down, but we are still initializing the system.  Pages are given directly
 * to the page allocator, no bootmem metadata is updated because it is gone.
 */
void __init free_bootmem_late(unsigned long addr, unsigned long size)
{
	unsigned long cursor, end;

	kmemleak_free_part_phys(addr, size);

	cursor = PFN_UP(addr);
	end = PFN_DOWN(addr + size);

	for (; cursor < end; cursor++) {
		__free_pages_bootmem(pfn_to_page(cursor), cursor, 0);
		totalram_pages++;
	}
}

static void __init __free_pages_memory(unsigned long start, unsigned long end)
{
	int order;

	while (start < end) {
		order = min(MAX_ORDER - 1UL, __ffs(start));

		while (start + (1UL << order) > end)
			order--;

		__free_pages_bootmem(pfn_to_page(start), start, order);

		start += (1UL << order);
	}
}

static unsigned long __init __free_memory_core(phys_addr_t start,
				 phys_addr_t end)
{
	unsigned long start_pfn = PFN_UP(start);
	unsigned long end_pfn = min_t(unsigned long,
				      PFN_DOWN(end), max_low_pfn);

	if (start_pfn >= end_pfn)
		return 0;

	__free_pages_memory(start_pfn, end_pfn);

	return end_pfn - start_pfn;
}

static void __init kdev_dump_memblock_info(void)
{
	struct memblock_region *reg;
	int i;

	// 打印 memory 区域
	pr_kdev("kdev memblock.memory:\n");
	for (i = 0; i < memblock.memory.cnt; i++) {
		reg = &memblock.memory.regions[i];
		pr_kdev("[%3d] [0x%016llx-0x%016llx], flags: 0x%x (size: %llu MB / %llu KB)\n",
				i, reg->base, reg->base + reg->size,
				reg->flags, 
				reg->size >> 20,          // 转换为 MB（1 MB = 1024 KB）
				reg->size >> 10);         // 转换为 KB（1 KB = 1024 B）
	}

	// 打印 reserved 区域
	pr_kdev("kdev memblock.reserved:\n");
	for (i = 0; i < memblock.reserved.cnt; i++) {
		reg = &memblock.reserved.regions[i];
		pr_kdev("[%3d] [0x%016llx-0x%016llx], flags: 0x%x (size: %llu MB / %llu KB)\n",
				i, reg->base, reg->base + reg->size,
				reg->flags, 
				reg->size >> 20,          // 转换为 MB
				reg->size >> 10);         // 转换为 KB
	}
}

void print_node_memory_info(void)
{
	int nid;
	pg_data_t *pgdat;

	for_each_online_node(nid) {
		pgdat = NODE_DATA(nid);
		pr_kdev("Node %d: Start=0x%llx, End=0x%llx, Pages=%lu\n",
				nid,
				(u64)pgdat->node_start_pfn << PAGE_SHIFT,
				(u64)(pgdat->node_start_pfn + pgdat->node_spanned_pages) << PAGE_SHIFT,
				pgdat->node_present_pages);
	}
}

static unsigned long __init free_low_memory_core_early(void)
{
	/*
		将memblock的内存释放到node中，仅用于nobootmem情况下
		函数通过遍历memblock的可用内存区域，按伙伴系统的内存块大小（2的order次幂）划分内存，
		并添加到free_area链表中。这标志着内存管理从临时性的memblock过渡到正式的伙伴系统

		mm_init → mem_init → memblock_free_all → free_low_memory_core_early →
		__free_memory_core → __free_pages_memory → memblock_free_pages →
		__free_pages_core → __free_one_page
	*/
	unsigned long count = 0;
	phys_addr_t start, end;
	u64 i;

	memblock_clear_hotplug(0, -1);

	/*
		for (i = 0, __next_reserved_mem_region(&i, &start, &end); 
			i != (u64)ULLONG_MAX; 
			__next_reserved_mem_region(&i, &start, &end)) {
			reserve_bootmem_region(start, end);
		}
	*/
	for_each_reserved_mem_region(i, &start, &end)
		reserve_bootmem_region(start, end);

	/*
	 * We need to use NUMA_NO_NODE instead of NODE_DATA(0)->node_id
	 *  because in some case like Node0 doesn't have RAM installed
	 *  low ram will be on Node1
	 */

	/* kdev: 遍历memblock

	void __next_mem_range(u64 *idx, int nid, ulong flags,
				 struct memblock_type *type_a,
				 struct memblock_type *type_b, phys_addr_t *out_start,
				 phys_addr_t *out_end, int *out_nid);

	#define for_each_mem_range(i, type_a, type_b, nid, flags,		\
			   p_start, p_end, p_nid)			\
	for (i = 0, __next_mem_range(&i, nid, flags, type_a, type_b,	\
				     p_start, p_end, p_nid);		\
	     i != (u64)ULLONG_MAX;					\
	     __next_mem_range(&i, nid, flags, type_a, type_b,		\
			      p_start, p_end, p_nid))
			      
	#define for_each_free_mem_range(i, nid, flags, p_start, p_end, p_nid)	\
			for_each_mem_range(i, &memblock.memory, &memblock.reserved, \
					   nid, flags, p_start, p_end, p_nid)

	__next_free_mem_range遍历 memblock 的空闲区域（memblock.memory 减去 memblock.reserved），返回满足条件的区域

	{
		int __i = 0;
		phys_addr_t start, end;

		for (__i = 0, __next_mem_range(&__i, NUMA_NO_NODE, MEMBLOCK_NONE, &start, &end, NULL);
			__i != (u64)ULLONG_MAX;
			__next_mem_range(&__i, NUMA_NO_NODE, MEMBLOCK_NONE, &start, &end, NULL)
		) {
			count += __free_memory_core(start, end);
			pr_kdev("%s File:[%s],Line:[%d] try add [0x%016llx]-[0x%016llx] to node, totalram_pages:[%ld]*4K\n",
			   __func__, __FILE__, __LINE__, start, end, count);
		}
	}
	*/


	
	kdev_dump_memblock_info(); // kdev: 打印memblock信息，memblock信息不消失
	for_each_free_mem_range(i, NUMA_NO_NODE, MEMBLOCK_NONE, &start, &end,
				NULL) {
		count += __free_memory_core(start, end);
		pr_kdev("%s File:[%s],Line:[%d] try add [0x%016llx]-[0x%016llx] to node, totalram_pages:[%ld]*4K\n",
			__FUNCTION__, __FILE__, __LINE__, start, end, count);
	}

	print_node_memory_info();
	return count;
}

static int reset_managed_pages_done __initdata;

void reset_node_managed_pages(pg_data_t *pgdat)
{
	struct zone *z;

	for (z = pgdat->node_zones; z < pgdat->node_zones + MAX_NR_ZONES; z++)
		z->managed_pages = 0;
}

void __init reset_all_zones_managed_pages(void)
{
	struct pglist_data *pgdat;

	if (reset_managed_pages_done)
		return;

	for_each_online_pgdat(pgdat)
		reset_node_managed_pages(pgdat);

	reset_managed_pages_done = 1;
}

/**
 * free_all_bootmem - release free pages to the buddy allocator
 *
 * Returns the number of pages actually released.
 */
unsigned long __init free_all_bootmem(void) // kdev: 不启用bootmem情况下用memblock，因为bootmem性能差
{
	/*
		kdev: 通过CONFIG_NO_BOOTMEM=y配置，内核启用nobootmem兼容层。
		此时，free_all_bootmem_core()实际调用memblock的释放逻辑，
		遍历memblock.memory中的空闲区域，将其移交伙伴系统
	*/
	unsigned long pages;

	reset_all_zones_managed_pages(); // kdev: 将所有内存区域的managed_pages计数器归零，确保伙伴系统能正确统计可用页

	pages = free_low_memory_core_early();
	
	// unsigned long totalram_pages;
	totalram_pages += pages;

	return pages;
}

/**
 * free_bootmem_node - mark a page range as usable
 * @pgdat: node the range resides on
 * @physaddr: starting address of the range
 * @size: size of the range in bytes
 *
 * Partial pages will be considered reserved and left as they are.
 *
 * The range must reside completely on the specified node.
 */
void __init free_bootmem_node(pg_data_t *pgdat, unsigned long physaddr,
			      unsigned long size)
{
	memblock_free(physaddr, size);
}

/**
 * free_bootmem - mark a page range as usable
 * @addr: starting address of the range
 * @size: size of the range in bytes
 *
 * Partial pages will be considered reserved and left as they are.
 *
 * The range must be contiguous but may span node boundaries.
 */
void __init free_bootmem(unsigned long addr, unsigned long size)
{
	memblock_free(addr, size);
}

static void * __init ___alloc_bootmem_nopanic(unsigned long size,
					unsigned long align,
					unsigned long goal,
					unsigned long limit)
{
	void *ptr;

	if (WARN_ON_ONCE(slab_is_available()))
		return kzalloc(size, GFP_NOWAIT);

restart:

	ptr = __alloc_memory_core_early(NUMA_NO_NODE, size, align, goal, limit);

	if (ptr)
		return ptr;

	if (goal != 0) {
		goal = 0;
		goto restart;
	}

	return NULL;
}

/**
 * __alloc_bootmem_nopanic - allocate boot memory without panicking
 * @size: size of the request in bytes
 * @align: alignment of the region
 * @goal: preferred starting address of the region
 *
 * The goal is dropped if it can not be satisfied and the allocation will
 * fall back to memory below @goal.
 *
 * Allocation may happen on any node in the system.
 *
 * Returns NULL on failure.
 */
void * __init __alloc_bootmem_nopanic(unsigned long size, unsigned long align,
					unsigned long goal)
{
	unsigned long limit = -1UL;

	return ___alloc_bootmem_nopanic(size, align, goal, limit);
}

static void * __init ___alloc_bootmem(unsigned long size, unsigned long align,
					unsigned long goal, unsigned long limit)
{
	void *mem = ___alloc_bootmem_nopanic(size, align, goal, limit);

	if (mem)
		return mem;
	/*
	 * Whoops, we cannot satisfy the allocation request.
	 */
	pr_alert("bootmem alloc of %lu bytes failed!\n", size);
	panic("Out of memory");
	return NULL;
}

/**
 * __alloc_bootmem - allocate boot memory
 * @size: size of the request in bytes
 * @align: alignment of the region
 * @goal: preferred starting address of the region
 *
 * The goal is dropped if it can not be satisfied and the allocation will
 * fall back to memory below @goal.
 *
 * Allocation may happen on any node in the system.
 *
 * The function panics if the request can not be satisfied.
 */
void * __init __alloc_bootmem(unsigned long size, unsigned long align,
			      unsigned long goal)
{
	unsigned long limit = -1UL;

	return ___alloc_bootmem(size, align, goal, limit);
}

void * __init ___alloc_bootmem_node_nopanic(pg_data_t *pgdat,
						   unsigned long size,
						   unsigned long align,
						   unsigned long goal,
						   unsigned long limit)
{
	void *ptr;

again:
	ptr = __alloc_memory_core_early(pgdat->node_id, size, align,
					goal, limit);
	if (ptr)
		return ptr;

	ptr = __alloc_memory_core_early(NUMA_NO_NODE, size, align,
					goal, limit);
	if (ptr)
		return ptr;

	if (goal) {
		goal = 0;
		goto again;
	}

	return NULL;
}

void * __init __alloc_bootmem_node_nopanic(pg_data_t *pgdat, unsigned long size,
				   unsigned long align, unsigned long goal)
{
	if (WARN_ON_ONCE(slab_is_available()))
		return kzalloc_node(size, GFP_NOWAIT, pgdat->node_id);

	return ___alloc_bootmem_node_nopanic(pgdat, size, align, goal, 0);
}

static void * __init ___alloc_bootmem_node(pg_data_t *pgdat, unsigned long size,
				    unsigned long align, unsigned long goal,
				    unsigned long limit)
{
	void *ptr;

	ptr = ___alloc_bootmem_node_nopanic(pgdat, size, align, goal, limit);
	if (ptr)
		return ptr;

	pr_alert("bootmem alloc of %lu bytes failed!\n", size);
	panic("Out of memory");
	return NULL;
}

/**
 * __alloc_bootmem_node - allocate boot memory from a specific node
 * @pgdat: node to allocate from
 * @size: size of the request in bytes
 * @align: alignment of the region
 * @goal: preferred starting address of the region
 *
 * The goal is dropped if it can not be satisfied and the allocation will
 * fall back to memory below @goal.
 *
 * Allocation may fall back to any node in the system if the specified node
 * can not hold the requested memory.
 *
 * The function panics if the request can not be satisfied.
 */
void * __init __alloc_bootmem_node(pg_data_t *pgdat, unsigned long size,
				   unsigned long align, unsigned long goal)
{
	if (WARN_ON_ONCE(slab_is_available()))
		return kzalloc_node(size, GFP_NOWAIT, pgdat->node_id);

	return ___alloc_bootmem_node(pgdat, size, align, goal, 0);
}

void * __init __alloc_bootmem_node_high(pg_data_t *pgdat, unsigned long size,
				   unsigned long align, unsigned long goal)
{
	return __alloc_bootmem_node(pgdat, size, align, goal);
}


/**
 * __alloc_bootmem_low - allocate low boot memory
 * @size: size of the request in bytes
 * @align: alignment of the region
 * @goal: preferred starting address of the region
 *
 * The goal is dropped if it can not be satisfied and the allocation will
 * fall back to memory below @goal.
 *
 * Allocation may happen on any node in the system.
 *
 * The function panics if the request can not be satisfied.
 */
void * __init __alloc_bootmem_low(unsigned long size, unsigned long align,
				  unsigned long goal)
{
	return ___alloc_bootmem(size, align, goal, ARCH_LOW_ADDRESS_LIMIT);
}

void * __init __alloc_bootmem_low_nopanic(unsigned long size,
					  unsigned long align,
					  unsigned long goal)
{
	return ___alloc_bootmem_nopanic(size, align, goal,
					ARCH_LOW_ADDRESS_LIMIT);
}

/**
 * __alloc_bootmem_low_node - allocate low boot memory from a specific node
 * @pgdat: node to allocate from
 * @size: size of the request in bytes
 * @align: alignment of the region
 * @goal: preferred starting address of the region
 *
 * The goal is dropped if it can not be satisfied and the allocation will
 * fall back to memory below @goal.
 *
 * Allocation may fall back to any node in the system if the specified node
 * can not hold the requested memory.
 *
 * The function panics if the request can not be satisfied.
 */
void * __init __alloc_bootmem_low_node(pg_data_t *pgdat, unsigned long size,
				       unsigned long align, unsigned long goal)
{
	if (WARN_ON_ONCE(slab_is_available()))
		return kzalloc_node(size, GFP_NOWAIT, pgdat->node_id);

	return ___alloc_bootmem_node(pgdat, size, align, goal,
				     ARCH_LOW_ADDRESS_LIMIT);
}
