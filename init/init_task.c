// SPDX-License-Identifier: GPL-2.0
#include <linux/init_task.h>
#include <linux/export.h>
#include <linux/mqueue.h>
#include <linux/sched.h>
#include <linux/sched/sysctl.h>
#include <linux/sched/rt.h>
#include <linux/sched/task.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/mm.h>

#include <asm/pgtable.h>
#include <linux/uaccess.h>

static struct signal_struct init_signals = INIT_SIGNALS(init_signals);
static struct sighand_struct init_sighand = INIT_SIGHAND(init_sighand);

// yyf: 0号进程的任务结构体（struct task_struct）在编译时静态初始化，存储在init_task变量中
/*
当系统启动完成后，0号进程转变为空闲进程，其核心行为包括：

CPU空闲管理：
当CPU没有其他可运行任务时，调度器会切换到0号进程。
执行cpu_idle_loop()函数，进入低功耗状态（如HLT指令或WFI指令），降低功耗。
统计空闲时间：内核通过0号进程统计CPU的空闲时间，用于性能分析和电源管理。
*/
/* Initial task structure */
struct task_struct init_task = INIT_TASK(init_task);
EXPORT_SYMBOL(init_task);

/*
 * Initial thread structure. Alignment of this is handled by a special
 * linker map entry.
 */
union thread_union init_thread_union __init_task_data = {
#ifndef CONFIG_THREAD_INFO_IN_TASK
	INIT_THREAD_INFO(init_task)
#endif
};
