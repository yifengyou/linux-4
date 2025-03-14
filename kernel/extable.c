﻿/* Rewritten by Rusty Russell, on the backs of many others...
   Copyright (C) 2001 Rusty Russell, 2002 Rusty Russell IBM.

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/
#include <linux/ftrace.h>
#include <linux/memory.h>
#include <linux/extable.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/init.h>
#include <linux/kprobes.h>
#include <linux/filter.h>

#include <asm/sections.h>
#include <linux/uaccess.h>

/*
 * mutex protecting text section modification (dynamic code patching).
 * some users need to sleep (allocating memory...) while they hold this lock.
 *
 * Note: Also protects SMP-alternatives modification on x86.
 *
 * NOT exported to modules - patching kernel text is a really delicate matter.
 */
DEFINE_MUTEX(text_mutex);

extern struct exception_table_entry __start___ex_table[];
extern struct exception_table_entry __stop___ex_table[];

/* Cleared by build time tools if the table is already sorted. */
u32 __initdata __visible main_extable_sort_needed = 1;

/* Sort the kernel's built-in exception table */
void __init sort_main_extable(void)
{
	/*
		__ex_table 的检索机制本质是 通过触发异常的指令地址匹配预定义的修复代码地址

		在编译阶段，异常处理函数的地址可能按随机顺序存入表中。未经排序的异常表会导致查找时需线性遍历所有条目，效率低下。
		通过 sort_main_extable() 按异常向量号（或地址范围）排序后，可支持二分查找（O(log n) 时间复杂度），显著提升异常响应速度。

		在压力测试中，排序后的异常表可使异常处理延迟降低 30%-50%（尤其在多核高并发场景）
		有序的异常表便于内核调试工具（如 kdump）快速定位异常上下文，减少故障排查时间
	*/
	
	const struct exception_table_entry *entry;
	unsigned long num_entries;
	
	if (main_extable_sort_needed && __stop___ex_table > __start___ex_table) {
		pr_notice("Sorting __ex_table...\n");
		sort_extable(__start___ex_table, __stop___ex_table);

		/* 遍历所有异常表条目 */
		pr_kdev("Exception Handler Entry:\n");
		for (entry = __start___ex_table; entry < __stop___ex_table; entry++) {
			pr_kdev("  Fault Address:[0x%lx] Fixup Address:[0x%lx] Offset:[%td] bytes\n",
				entry->insn, entry->fixup, entry->fixup - entry->insn);
		}
	} else {
		pr_kdev("__ex_table is empty or sorting not required\n");
	}

	// 打印ext_table
}

/* Given an address, look for it in the exception tables. */
const struct exception_table_entry *search_exception_tables(unsigned long addr)
{
	const struct exception_table_entry *e;

	e = search_extable(__start___ex_table,
			   __stop___ex_table - __start___ex_table, addr);
	if (!e)
		e = search_module_extables(addr);
	return e;
}

static inline int init_kernel_text(unsigned long addr)
{
	if (addr >= (unsigned long)_sinittext &&
	    addr < (unsigned long)_einittext)
		return 1;
	return 0;
}

int notrace core_kernel_text(unsigned long addr)
{
	if (addr >= (unsigned long)_stext &&
	    addr < (unsigned long)_etext)
		return 1;

	if (system_state < SYSTEM_RUNNING &&
	    init_kernel_text(addr))
		return 1;
	return 0;
}

/**
 * core_kernel_data - tell if addr points to kernel data
 * @addr: address to test
 *
 * Returns true if @addr passed in is from the core kernel data
 * section.
 *
 * Note: On some archs it may return true for core RODATA, and false
 *  for others. But will always be true for core RW data.
 */
int core_kernel_data(unsigned long addr)
{
	if (addr >= (unsigned long)_sdata &&
	    addr < (unsigned long)_edata)
		return 1;
	return 0;
}

int __kernel_text_address(unsigned long addr)
{
	if (kernel_text_address(addr))
		return 1;
	/*
	 * There might be init symbols in saved stacktraces.
	 * Give those symbols a chance to be printed in
	 * backtraces (such as lockdep traces).
	 *
	 * Since we are after the module-symbols check, there's
	 * no danger of address overlap:
	 */
	if (init_kernel_text(addr))
		return 1;
	return 0;
}

int kernel_text_address(unsigned long addr)
{
	bool no_rcu;
	int ret = 1;

	if (core_kernel_text(addr))
		return 1;

	/*
	 * If a stack dump happens while RCU is not watching, then
	 * RCU needs to be notified that it requires to start
	 * watching again. This can happen either by tracing that
	 * triggers a stack trace, or a WARN() that happens during
	 * coming back from idle, or cpu on or offlining.
	 *
	 * is_module_text_address() as well as the kprobe slots
	 * and is_bpf_text_address() require RCU to be watching.
	 */
	no_rcu = !rcu_is_watching();

	/* Treat this like an NMI as it can happen anywhere */
	if (no_rcu)
		rcu_nmi_enter();

	if (is_module_text_address(addr))
		goto out;
	if (is_ftrace_trampoline(addr))
		goto out;
	if (is_kprobe_optinsn_slot(addr) || is_kprobe_insn_slot(addr))
		goto out;
	if (is_bpf_text_address(addr))
		goto out;
	ret = 0;
out:
	if (no_rcu)
		rcu_nmi_exit();

	return ret;
}

/*
 * On some architectures (PPC64, IA64) function pointers
 * are actually only tokens to some data that then holds the
 * real function address. As a result, to find if a function
 * pointer is part of the kernel text, we need to do some
 * special dereferencing first.
 */
int func_ptr_is_kernel_text(void *ptr)
{
	unsigned long addr;
	addr = (unsigned long) dereference_function_descriptor(ptr);
	if (core_kernel_text(addr))
		return 1;
	return is_module_text_address(addr);
}
