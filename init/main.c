﻿/*
 *  linux/init/main.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 *
 *  GK 2/5/95  -  Changed to support mounting root fs via NFS
 *  Added initrd & change_root: Werner Almesberger & Hans Lermen, Feb '96
 *  Moan early if gcc is old, avoiding bogus kernels - Paul Gortmaker, May '96
 *  Simplified starting of init:  Michael A. Griffith <grif@acm.org>
 */

#define DEBUG		/* Enable initcall_debug */

#include <linux/types.h>
#include <linux/extable.h>
#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/binfmts.h>
#include <linux/kernel.h>
#include <linux/syscalls.h>
#include <linux/stackprotector.h>
#include <linux/string.h>
#include <linux/ctype.h>
#include <linux/delay.h>
#include <linux/ioport.h>
#include <linux/init.h>
#include <linux/initrd.h>
#include <linux/bootmem.h>
#include <linux/acpi.h>
#include <linux/console.h>
#include <linux/nmi.h>
#include <linux/percpu.h>
#include <linux/kmod.h>
#include <linux/vmalloc.h>
#include <linux/kernel_stat.h>
#include <linux/start_kernel.h>
#include <linux/security.h>
#include <linux/smp.h>
#include <linux/profile.h>
#include <linux/rcupdate.h>
#include <linux/moduleparam.h>
#include <linux/kallsyms.h>
#include <linux/writeback.h>
#include <linux/cpu.h>
#include <linux/cpuset.h>
#include <linux/cgroup.h>
#include <linux/efi.h>
#include <linux/tick.h>
#include <linux/sched/isolation.h>
#include <linux/interrupt.h>
#include <linux/taskstats_kern.h>
#include <linux/delayacct.h>
#include <linux/unistd.h>
#include <linux/rmap.h>
#include <linux/mempolicy.h>
#include <linux/key.h>
#include <linux/buffer_head.h>
#include <linux/page_ext.h>
#include <linux/debug_locks.h>
#include <linux/debugobjects.h>
#include <linux/lockdep.h>
#include <linux/kmemleak.h>
#include <linux/pid_namespace.h>
#include <linux/device.h>
#include <linux/kthread.h>
#include <linux/sched.h>
#include <linux/sched/init.h>
#include <linux/signal.h>
#include <linux/idr.h>
#include <linux/kgdb.h>
#include <linux/ftrace.h>
#include <linux/async.h>
#include <linux/sfi.h>
#include <linux/shmem_fs.h>
#include <linux/slab.h>
#include <linux/perf_event.h>
#include <linux/ptrace.h>
#include <linux/pti.h>
#include <linux/blkdev.h>
#include <linux/elevator.h>
#include <linux/sched_clock.h>
#include <linux/sched/task.h>
#include <linux/sched/task_stack.h>
#include <linux/context_tracking.h>
#include <linux/random.h>
#include <linux/list.h>
#include <linux/integrity.h>
#include <linux/proc_ns.h>
#include <linux/io.h>
#include <linux/cache.h>
#include <linux/rodata_test.h>

#include <asm/io.h>
#include <asm/bugs.h>
#include <asm/setup.h>
#include <asm/sections.h>
#include <asm/cacheflush.h>

static int kernel_init(void *);

extern void init_IRQ(void);
extern void radix_tree_init(void);

/*
 * Debug helper: via this flag we know that we are in 'early bootup code'
 * where only the boot processor is running with IRQ disabled.  This means
 * two things - IRQ must not be enabled before the flag is cleared and some
 * operations which are not allowed with IRQ disabled are allowed while the
 * flag is set.
 */
bool early_boot_irqs_disabled __read_mostly;

enum system_states system_state __read_mostly;
EXPORT_SYMBOL(system_state);

/*
 * Boot command-line arguments
 */
#define MAX_INIT_ARGS CONFIG_INIT_ENV_ARG_LIMIT
#define MAX_INIT_ENVS CONFIG_INIT_ENV_ARG_LIMIT

extern void time_init(void);
/* Default late time init is NULL. archs can override this later. */
void (*__initdata late_time_init)(void);

/* Untouched command line saved by arch-specific code. */
char __initdata boot_command_line[COMMAND_LINE_SIZE];
/* Untouched saved command line (eg. for /proc) */
char *saved_command_line;
/* Command line for parameter parsing */
static char *static_command_line;
/* Command line for per-initcall parameter parsing */
static char *initcall_command_line;

static char *execute_command;
static char *ramdisk_execute_command;

/*
 * Used to generate warnings if static_key manipulation functions are used
 * before jump_label_init is called.
 */
bool static_key_initialized __read_mostly;
EXPORT_SYMBOL_GPL(static_key_initialized);

/*
 * If set, this is an indication to the drivers that reset the underlying
 * device before going ahead with the initialization otherwise driver might
 * rely on the BIOS and skip the reset operation.
 *
 * This is useful if kernel is booting in an unreliable environment.
 * For ex. kdump situation where previous kernel has crashed, BIOS has been
 * skipped and devices will be in unknown state.
 */
unsigned int reset_devices;
EXPORT_SYMBOL(reset_devices);

static int __init set_reset_devices(char *str)
{
	reset_devices = 1;
	return 1;
}

__setup("reset_devices", set_reset_devices);

static const char *argv_init[MAX_INIT_ARGS+2] = { "init", NULL, };
const char *envp_init[MAX_INIT_ENVS+2] = { "HOME=/", "TERM=linux", NULL, };
static const char *panic_later, *panic_param;

extern const struct obs_kernel_param __setup_start[], __setup_end[];

static bool __init obsolete_checksetup(char *line)
{
	const struct obs_kernel_param *p;
	bool had_early_param = false;

	p = __setup_start;
	do {
		int n = strlen(p->str);
		if (parameqn(line, p->str, n)) {
			if (p->early) {
				/* Already done in parse_early_param?
				 * (Needs exact match on param part).
				 * Keep iterating, as we can have early
				 * params and __setups of same names 8( */
				if (line[n] == '\0' || line[n] == '=')
					had_early_param = true;
			} else if (!p->setup_func) {
				pr_warn("Parameter %s is obsolete, ignored\n",
					p->str);
				return true;
			} else if (p->setup_func(line + n))
				return true;
		}
		p++;
	} while (p < __setup_end);

	return had_early_param;
}

/*
 * This should be approx 2 Bo*oMips to start (note initial shift), and will
 * still work even if initially too large, it will just take slightly longer
 */
unsigned long loops_per_jiffy = (1<<12);
EXPORT_SYMBOL(loops_per_jiffy);

static int __init debug_kernel(char *str)
{
	console_loglevel = CONSOLE_LOGLEVEL_DEBUG;
	return 0;
}

static int __init quiet_kernel(char *str)
{
	console_loglevel = CONSOLE_LOGLEVEL_QUIET;
	return 0;
}

early_param("debug", debug_kernel);
early_param("quiet", quiet_kernel);

static int __init loglevel(char *str)
{
	int newlevel;

	/*
	 * Only update loglevel value when a correct setting was passed,
	 * to prevent blind crashes (when loglevel being set to 0) that
	 * are quite hard to debug
	 */
	if (get_option(&str, &newlevel)) {
		console_loglevel = newlevel;
		return 0;
	}

	return -EINVAL;
}

early_param("loglevel", loglevel);

/* Change NUL term back to "=", to make "param" the whole string. */
static int __init repair_env_string(char *param, char *val,
				    const char *unused, void *arg)
{
	if (val) {
		/* param=val or param="val"? */
		if (val == param+strlen(param)+1)
			val[-1] = '=';
		else if (val == param+strlen(param)+2) {
			val[-2] = '=';
			memmove(val-1, val, strlen(val)+1);
			val--;
		} else
			BUG();
	}
	return 0;
}

/* Anything after -- gets handed straight to init. */
static int __init set_init_arg(char *param, char *val,
			       const char *unused, void *arg)
{
	unsigned int i;

	if (panic_later)
		return 0;

	repair_env_string(param, val, unused, NULL);

	for (i = 0; argv_init[i]; i++) {
		if (i == MAX_INIT_ARGS) {
			panic_later = "init";
			panic_param = param;
			return 0;
		}
	}
	argv_init[i] = param;
	return 0;
}

/*
 * Unknown boot options get handed to init, unless they look like
 * unused parameters (modprobe will find them in /proc/cmdline).
 */
static int __init unknown_bootoption(char *param, char *val,
				     const char *unused, void *arg)
{
	repair_env_string(param, val, unused, NULL);

	/* Handle obsolete-style parameters */
	if (obsolete_checksetup(param))
		return 0;

	/* Unused module parameter. */
	if (strchr(param, '.') && (!val || strchr(param, '.') < val))
		return 0;

	if (panic_later)
		return 0;

	if (val) {
		/* Environment option */
		unsigned int i;
		for (i = 0; envp_init[i]; i++) {
			if (i == MAX_INIT_ENVS) {
				panic_later = "env";
				panic_param = param;
			}
			if (!strncmp(param, envp_init[i], val - param))
				break;
		}
		envp_init[i] = param;
	} else {
		/* Command line option */
		unsigned int i;
		for (i = 0; argv_init[i]; i++) {
			if (i == MAX_INIT_ARGS) {
				panic_later = "init";
				panic_param = param;
			}
		}
		argv_init[i] = param;
	}
	return 0;
}

static int __init init_setup(char *str)
{
	unsigned int i;

	execute_command = str;
	/*
	 * In case LILO is going to boot us with default command line,
	 * it prepends "auto" before the whole cmdline which makes
	 * the shell think it should execute a script with such name.
	 * So we ignore all arguments entered _before_ init=... [MJ]
	 */
	for (i = 1; i < MAX_INIT_ARGS; i++)
		argv_init[i] = NULL;
	return 1;
}
__setup("init=", init_setup);

static int __init rdinit_setup(char *str)
{
	unsigned int i;

	ramdisk_execute_command = str;
	/* See "auto" comment in init_setup */
	for (i = 1; i < MAX_INIT_ARGS; i++)
		argv_init[i] = NULL;
	return 1;
}
__setup("rdinit=", rdinit_setup);

#ifndef CONFIG_SMP
static const unsigned int setup_max_cpus = NR_CPUS;
static inline void setup_nr_cpu_ids(void) { }
static inline void smp_prepare_cpus(unsigned int maxcpus) { }
#endif

/*
 * We need to store the untouched command line for future reference.
 * We also need to store the touched command line since the parameter
 * parsing is performed in place, and we should allow a component to
 * store reference of name/value for future reference.
 */
static void __init setup_command_line(char *command_line)
{
	saved_command_line =
		memblock_virt_alloc(strlen(boot_command_line) + 1, 0);
	initcall_command_line =
		memblock_virt_alloc(strlen(boot_command_line) + 1, 0);
	static_command_line = memblock_virt_alloc(strlen(command_line) + 1, 0);
	strcpy(saved_command_line, boot_command_line);
	strcpy(static_command_line, command_line);
}

/*
 * We need to finalize in a non-__init function or else race conditions
 * between the root thread and the init thread may cause start_kernel to
 * be reaped by free_initmem before the root thread has proceeded to
 * cpu_idle.
 *
 * gcc-3.4 accidentally inlines this function, so use noinline.
 */

static __initdata DECLARE_COMPLETION(kthreadd_done);

void yyf_count_all_threads(void)
{
	struct task_struct *p;
	int kthread_count;

	kthread_count = 0;
	for_each_process(p)
		kthread_count++;

	pr_kdev("current total thread num is [%d]\n", kthread_count);
}


static noinline void __ref rest_init(void)
{
	struct task_struct *tsk;
	int pid;

	pr_kdev("%s File:[%s],Line:[%d] start\n", __FUNCTION__, __FILE__, __LINE__);


	// kdev: show current thread num
	
	pr_kdev("%s File:[%s],Line:[%d] no kthread created\n", __FUNCTION__, __FILE__, __LINE__);
	yyf_count_all_threads();

	rcu_scheduler_starting(); // kdev: 激活rcu调度器
	/*
	 * We need to spawn init first so that it obtains pid 1, however
	 * the init task will end up wanting to create kthreads, which, if
	 * we schedule it before we create kthreadd, will OOPS.
	 */
	pid = kernel_thread(kernel_init, NULL, CLONE_FS);

	// kdev: show current thread num
	pr_kdev("%s File:[%s],Line:[%d] new thread kernel_init\n", __FUNCTION__, __FILE__, __LINE__);
	yyf_count_all_threads();
	

	/*
	 * Pin init on the boot CPU. Task migration is not properly working
	 * until sched_init_smp() has been run. It will set the allowed
	 * CPUs for init to the non isolated CPUs.
	 */
	rcu_read_lock();
	tsk = find_task_by_pid_ns(pid, &init_pid_ns);
	set_cpus_allowed_ptr(tsk, cpumask_of(smp_processor_id()));
	rcu_read_unlock();

	numa_default_policy();
	pid = kernel_thread(kthreadd, NULL, CLONE_FS | CLONE_FILES);
	rcu_read_lock();
	kthreadd_task = find_task_by_pid_ns(pid, &init_pid_ns);
	rcu_read_unlock();

	// kdev: show current thread num
	pr_kdev("%s File:[%s],Line:[%d] new thread kthreadd\n", __FUNCTION__, __FILE__, __LINE__);
	yyf_count_all_threads();

	/*
	 * Enable might_sleep() and smp_processor_id() checks.
	 * They cannot be enabled earlier because with CONFIG_PRREMPT=y
	 * kernel_thread() would trigger might_sleep() splats. With
	 * CONFIG_PREEMPT_VOLUNTARY=y the init task might have scheduled
	 * already, but it's stuck on the kthreadd_done completion.
	 */
	system_state = SYSTEM_SCHEDULING;

	complete(&kthreadd_done);

	/*
	 * The boot idle thread must execute schedule()
	 * at least once to get things moving:
	 */
	schedule_preempt_disabled(); // kdev: 禁用抢占
	/* Call into cpu_idle with preempt disabled */
	cpu_startup_entry(CPUHP_ONLINE); // kdev: 0号进程进入do_idle()循环
}

/* Check for early params. */
static int __init do_early_param(char *param, char *val,
				 const char *unused, void *arg)
{
	const struct obs_kernel_param *p;

	for (p = __setup_start; p < __setup_end; p++) {
		if ((p->early && parameq(param, p->str)) ||
		    (strcmp(param, "console") == 0 &&
		     strcmp(p->str, "earlycon") == 0)
		) {
			if (p->setup_func(val) != 0)
				pr_warn("Malformed early option '%s'\n", param);
		}
	}
	/* We accept everything at this stage. */
	return 0;
}

void __init parse_early_options(char *cmdline)
{
	parse_args("early options", cmdline, NULL, 0, 0, 0, NULL,
		   do_early_param);
}

/* Arch code calls this early on, or if not, just before other parsing. */
void __init parse_early_param(void)
{
	static int done __initdata;
	static char tmp_cmdline[COMMAND_LINE_SIZE] __initdata;
	
	pr_kdev("%s File:[%s],Line:[%d] start\n", __FUNCTION__, __FILE__, __LINE__);

	if (done)
		return;

	/* All fall through to do_early_param. */
	strlcpy(tmp_cmdline, boot_command_line, COMMAND_LINE_SIZE);
	parse_early_options(tmp_cmdline);
	done = 1;
	pr_kdev("%s File:[%s],Line:[%d] finished\n", __FUNCTION__, __FILE__, __LINE__);
}

void __init __weak arch_post_acpi_subsys_init(void) { }

void __init __weak smp_setup_processor_id(void)
{
}

# if THREAD_SIZE >= PAGE_SIZE
void __init __weak thread_stack_cache_init(void)
{
}
#endif

void __init __weak mem_encrypt_init(void) { }

/*
 * Set up kernel memory allocators
 */
static void __init mm_init(void) // kdev: memory management initialization
{
	/*
	 * page_ext requires contiguous pages,
	 * bigger than MAX_ORDER unless SPARSEMEM.
	 */

	/*
		mm_init() 在 start_kernel() 阶段被调用，其核心任务包括：
		
		初始化页扩展元数据（page_ext）
		释放启动阶段保留的内存到伙伴系统
		构建 slab 分配器基础设施
		设置页表管理机制
		初始化虚拟内存和 I/O 映射空间
		处理 x86_64 架构特有的安全特性（如 PTI）
	*/
	page_ext_init_flatmem(); // kdev: 为每个物理页分配扩展元数据结构 page_ext，用于存储页状态跟踪、内存检测等高级功能数据
	mem_init(); // kdev: 完成物理内存的最终初始化，释放启动阶段保留的内存到伙伴系统。
	kmem_cache_init(); // kdev: 初始化 slab 分配器，创建用于分配小对象的缓存池。
	pgtable_init(); // kdev: 初始化页表相关基础设施
	vmalloc_init(); // kdev: 初始化 vmalloc 虚拟地址空间（位于内核地址空间的高端区域）
	ioremap_huge_init();
	/* Should be run before the first non-init thread is created */
	init_espfix_bsp();
	/* Should be run after espfix64 is set up. */
	pti_init();
}

asmlinkage __visible void __init start_kernel(void)
{
	char *command_line;
	char *after_dashes;

	char *log_buf_ptr = log_buf_addr_get(); // kdev: log_buf是static

	/* kdev: add multi variable type for test addr */
	static long static_var = 0x1234567890ABCDEF;
	long local_var = 0xEFDCBA0987654321;
	char *str_ptr = "this_is_kernel_addr_test_string";
	phys_addr_t static_phys, local_phys, str_phys;

	dump_stack();
	pr_kdev("%s File:[%s],Line:[%d] start\n", __FUNCTION__, __FILE__, __LINE__);
	pr_kdev("%s File:[%s],Line:[%d] enable early printk\n", __FUNCTION__, __FILE__, __LINE__);

	// kdev: logbuf 是静态变量 static char __log_buf[__LOG_BUF_LEN] __aligned(LOG_ALIGN);
	// setup_log_buf() 重新设置缓存大小、空间
	pr_kdev("early log_buf addr:[0x%px] log_buf_len=[%d](%dKB)\n", log_buf_ptr, log_buf_len_get(), log_buf_len_get()/1024);

	// kdev: 基本整数类型
	pr_kdev("type [short] sizeof(short)=[%d]\n", sizeof(short));
	pr_kdev("type [int] sizeof(int)=[%d]\n", sizeof(int));
	pr_kdev("type [long] sizeof(long)=[%d]\n", sizeof(long));
	pr_kdev("type [long long] sizeof(long long)=[%d]\n\n", sizeof(long long));
	
	// kdev: 无符号整数类型
	pr_kdev("type [unsigned short] sizeof(unsigned short)=[%d]\n", sizeof(unsigned short));
	pr_kdev("type [unsigned int] sizeof(unsigned int)=[%d]\n", sizeof(unsigned int));
	pr_kdev("type [unsigned long] sizeof(unsigned long)=[%d]\n", sizeof(unsigned long));
	pr_kdev("type [unsigned long long] sizeof(unsigned long long)=[%d]\n\n", sizeof(unsigned long long));
	
	// kdev: 显式位宽类型（需包含 <linux/types.h>）
	pr_kdev("type [u8] sizeof(u8)=[%d]\n", sizeof(u8));
	pr_kdev("type [u16] sizeof(u16)=[%d]\n", sizeof(u16));
	pr_kdev("type [u32] sizeof(u32)=[%d]\n", sizeof(u32));
	pr_kdev("type [u64] sizeof(u64)=[%d]\n", sizeof(u64));
	pr_kdev("type [s8] sizeof(s8)=[%d]\n", sizeof(s8));
	pr_kdev("type [s16] sizeof(s16)=[%d]\n", sizeof(s16));
	pr_kdev("type [s32] sizeof(s32)=[%d]\n", sizeof(s32));
	pr_kdev("type [s64] sizeof(s64)=[%d]\n\n", sizeof(s64));
	
	// kdev: 指针类型
	pr_kdev("type [void*] sizeof(void*)=[%d]\n", sizeof(void*));
	pr_kdev("type [char*] sizeof(char*)=[%d]\n", sizeof(char*));
	pr_kdev("type [int*] sizeof(int*)=[%d]\n", sizeof(int*));
	pr_kdev("type [struct task_struct*] sizeof(struct task_struct*)=[%d]\n\n", sizeof(struct task_struct*));
	
	// kdev: 内核专用类型（需包含相关头文件）
	pr_kdev("type [size_t] sizeof(size_t)=[%d]\n", sizeof(size_t)); // kdev: size_t（阳光型）无符号整型（unsigned long），表示不可能为负的计量值
	pr_kdev("type [ssize_t] sizeof(ssize_t)=[%d]\n", sizeof(ssize_t)); // kdev: ssize_t（务实型）有符号整型（signed long），反馈成功/失败的场景
	pr_kdev("type [phys_addr_t] sizeof(phys_addr_t)=[%d]\n", sizeof(phys_addr_t));
	pr_kdev("type [loff_t] sizeof(loff_t)=[%d]\n", sizeof(loff_t));
	pr_kdev("type [pid_t] sizeof(pid_t)=[%d]\n", sizeof(pid_t));
	pr_kdev("type [dev_t] sizeof(dev_t)=[%d]\n", sizeof(dev_t));
	pr_kdev("type [time_t] sizeof(time_t)=[%d]\n", sizeof(time_t));
	pr_kdev("type [time64_t] sizeof(time64_t)=[%d]\n", sizeof(time64_t));
	pr_kdev("type [bool] sizeof(bool)=[%d]\n\n", sizeof(bool));
	
	// kdev: 关键数据结构（大小可能随内核配置变化）
	pr_kdev("type [struct task_struct] sizeof(struct task_struct)=[%d]\n", sizeof(struct task_struct));
	pr_kdev("type [struct page] sizeof(struct page)=[%d]\n", sizeof(struct page));
	pr_kdev("type [struct file] sizeof(struct file)=[%d]\n\n", sizeof(struct file));


	// kdev: 打印静态变量信息
	static_phys = virt_to_phys(&static_var);
	pr_kdev("Static variable Virtual addr:[%px] Physical addr:[0x%llx] Value:[0x%llx]\n",
		&static_var, static_phys, static_var);
	// kdev: 打印局部变量信息
	local_phys = virt_to_phys(&local_var);
	pr_kdev("Local  variable Virtual addr:[%px] Physical addr:[0x%llx] Value:[0x%llx]\n",
		&local_var, local_phys, local_var);
	// kdev: 打印字符串信息
	str_phys = virt_to_phys(str_ptr);
	pr_kdev("String variable Virtual addr:[%px] Physical addr:[0x%llx] Value:[%s]\n\n",
		str_ptr, str_phys, str_ptr);
	

	set_task_stack_end_magic(&init_task); // kdev: 为 0 号进程（init_task，即 idle 进程）的堆栈末端设置魔数（Magic Number），用于检测堆栈溢出。若堆栈越界破坏该魔数，内核会触发异常提示
	smp_setup_processor_id(); // kdev: 对x86_64来说是空函数，其 CPU ID 通常由硬件或固件直接提供
	debug_objects_early_init(); // kdev: 初始化内核调试子系统中的对象跟踪机制，用于检测内核对象（如链表、定时器）的生命周期错误（如重复释放、未初始化使用等）

	cgroup_init_early();

	local_irq_disable();
	early_boot_irqs_disabled = true;

	/*
	 * Interrupts are still disabled. Do necessary setups, then
	 * enable them.
	 */
	boot_cpu_init(); // kdev: 将当前运行的 CPU（BSP）标记为在线、活跃和存在的状态
	page_address_init();
	pr_notice("%s", linux_banner);
	
	setup_arch(&command_line); // kdev: 设置体系结构相关设置、负责初始化自举分配器
	
	mm_init_cpumask(&init_mm);
	setup_command_line(command_line);
	setup_nr_cpu_ids();
	setup_per_cpu_areas(); // kdev: 定义percpu变量内存区域，初始化percpu变量
	smp_prepare_boot_cpu();	/* arch-specific boot-cpu hooks */
	boot_cpu_hotplug_init();

	build_all_zonelists(NULL); // kdev: 建立node和zone数据结构
	page_alloc_init();

	pr_notice("Kernel command line: [%s]\n", boot_command_line);
	pr_kdev("Kernel command line:[%s] addr:[%px]\n", boot_command_line, boot_command_line);
	
	/* parameters may set static keys */
	jump_label_init();
	parse_early_param();
	after_dashes = parse_args("Booting kernel",
				  static_command_line, __start___param,
				  __stop___param - __start___param,
				  -1, -1, NULL, &unknown_bootoption);
	if (!IS_ERR_OR_NULL(after_dashes))
		parse_args("Setting init args", after_dashes, NULL, 0, -1, -1,
			   NULL, set_init_arg);

	/*
	 * These use large bootmem allocations and must precede
	 * kmem_cache_init()
	 */
	setup_log_buf(0); // kdev: 创建并配置内核的环形日志缓冲区，用于存储printk等接口输出的日志信息
	// kdev: logbuf 是静态变量 static char __log_buf[__LOG_BUF_LEN] __aligned(LOG_ALIGN);
	pr_kdev("new  log_buf addr:[0x%px] log_buf_len=[%d](%dKB)\n", log_buf_ptr, log_buf_len_get(), log_buf_len_get()/1024);
	
	vfs_caches_init_early();
	
	sort_main_extable(); // kdev: 异常向量表排序
	
	trap_init(); // 初始化异常
	mm_init(); // kdev: 停用bootmem分配器，迁移到实际的内存管理函数

	ftrace_init();

	/* trace_printk can be enabled here */
	early_trace_init();

	/*
	 * Set up the scheduler prior starting any interrupts (such as the
	 * timer interrupt). Full topology setup happens at smp_init()
	 * time - but meanwhile we still have a functioning scheduler.
	 */
	sched_init();
	/*
	 * Disable preemption - early bootup scheduling is extremely
	 * fragile until we cpu_idle() for the first time.
	 */
	preempt_disable();
	if (WARN(!irqs_disabled(),
		 "Interrupts were enabled *very* early, fixing it\n"))
		local_irq_disable();
	radix_tree_init();

	/*
	 * Set up housekeeping before setting up workqueues to allow the unbound
	 * workqueue to take non-housekeeping into account.
	 */
	housekeeping_init();

	/*
	 * Allow workqueue creation and work item queueing/cancelling
	 * early.  Work item execution depends on kthreads and starts after
	 * workqueue_init().
	 */
	workqueue_init_early();

	rcu_init();

	/* Trace events are available after this */
	trace_init();

	context_tracking_init();
	/* init some links before init_ISA_irqs() */
	early_irq_init();
	init_IRQ();
	tick_init();
	rcu_init_nohz();
	init_timers();
	hrtimers_init();
	softirq_init();
	timekeeping_init();
	time_init();

	/*
	 * For best initial stack canary entropy, prepare it after:
	 * - setup_arch() for any UEFI RNG entropy and boot cmdline access
	 * - timekeeping_init() for ktime entropy used in random_init()
	 * - time_init() for making random_get_entropy() work on some platforms
	 * - random_init() to initialize the RNG from from early entropy sources
	 */
	random_init(command_line);
	boot_init_stack_canary();

	sched_clock_postinit();
	printk_safe_init();
	perf_event_init();
	profile_init();
	call_function_init();
	WARN(!irqs_disabled(), "Interrupts were enabled early\n");
	early_boot_irqs_disabled = false;
	local_irq_enable();

	kmem_cache_init_late(); // kdev: 初始化小块内存区域分配器

	/*
	 * HACK ALERT! This is early. We're enabling the console before
	 * we've done PCI setups etc, and console_init() must be aware of
	 * this. But we do want output early, in case something goes wrong.
	 */

	console_init();
	
	pr_info("\n\n*********************************************************\n\n");
	pr_kdev("%s File:[%s],Line:[%d] disable early printk\n", __FUNCTION__, __FILE__, __LINE__);

	
	if (panic_later)
		panic("Too many boot %s vars at `%s'", panic_later,
		      panic_param);

	pr_kdev("%s File:[%s],Line:[%d] printk test begin\n", __FUNCTION__, __FILE__, __LINE__);
	
	pr_kdev("%s File:[%s],Line:[%d] console_loglevel=[%d]\n", __FUNCTION__, __FILE__, __LINE__, console_loglevel);
	pr_kdev("%s File:[%s],Line:[%d] default_message_loglevel=[%d]\n", __FUNCTION__, __FILE__, __LINE__, default_message_loglevel);
	pr_kdev("%s File:[%s],Line:[%d] minimum_console_loglevel=[%d]\n", __FUNCTION__, __FILE__, __LINE__, minimum_console_loglevel);
	pr_kdev("%s File:[%s],Line:[%d] default_console_loglevel=[%d]\n", __FUNCTION__, __FILE__, __LINE__, default_console_loglevel);
	
	pr_emerg("%s File:[%s],Line:[%d] this is pr_emerg   (level: 0)\n", __FUNCTION__, __FILE__, __LINE__);
	pr_alert("%s File:[%s],Line:[%d] this is pr_alert   (level: 1)\n", __FUNCTION__, __FILE__, __LINE__);
	pr_crit("%s File:[%s],Line:[%d] this is pr_crit    (level: 2)\n", __FUNCTION__, __FILE__, __LINE__);
	pr_err("%s File:[%s],Line:[%d] this is pr_err     (level: 3)\n", __FUNCTION__, __FILE__, __LINE__);
	pr_warning("%s File:[%s],Line:[%d] this is pr_warning (level: 4)\n", __FUNCTION__, __FILE__, __LINE__);
	pr_warn("%s File:[%s],Line:[%d] this is pr_warn    (level: 5)\n", __FUNCTION__, __FILE__, __LINE__);
	pr_notice("%s File:[%s],Line:[%d] this is pr_notice  (level: 6)\n", __FUNCTION__, __FILE__, __LINE__);
	pr_info("%s File:[%s],Line:[%d] this is pr_info    (level: 7)\n", __FUNCTION__, __FILE__, __LINE__);
	
	pr_kdev("%s File:[%s],Line:[%d] printk test finished\n", __FUNCTION__, __FILE__, __LINE__);

	lockdep_info();

	/*
	 * Need to run this when irqs are enabled, because it wants
	 * to self-test [hard/soft]-irqs on/off lock inversion bugs
	 * too:
	 */
	locking_selftest();

	/*
	 * This needs to be called before any devices perform DMA
	 * operations that might use the SWIOTLB bounce buffers. It will
	 * mark the bounce buffers as decrypted so that their usage will
	 * not cause "plain-text" data to be decrypted when accessed.
	 */
	mem_encrypt_init();

#ifdef CONFIG_BLK_DEV_INITRD
	if (initrd_start && !initrd_below_start_ok &&
	    page_to_pfn(virt_to_page((void *)initrd_start)) < min_low_pfn) {
		pr_crit("initrd overwritten (0x%08lx < 0x%08lx) - disabling it.\n",
		    page_to_pfn(virt_to_page((void *)initrd_start)),
		    min_low_pfn);
		initrd_start = 0;
	}
#endif
	kmemleak_init();
	debug_objects_mem_init();
	setup_per_cpu_pageset(); // kdev: 为每个cpu的zone的pageset数组的第一个元素分配内存
	numa_policy_init();
	acpi_early_init();
	if (late_time_init)
		late_time_init();
	calibrate_delay();
	pid_idr_init();
	anon_vma_init();
#ifdef CONFIG_X86
	if (efi_enabled(EFI_RUNTIME_SERVICES))
		efi_enter_virtual_mode();
#endif
	thread_stack_cache_init();
	cred_init();
	


	fork_init();
	proc_caches_init();
	buffer_init();
	key_init();
	security_init();
	dbg_late_init();
	vfs_caches_init();
	pagecache_init();
	signals_init();
	proc_root_init();
	nsfs_init();
	cpuset_init();
	cgroup_init();
	taskstats_init_early();
	delayacct_init();

	check_bugs();

	acpi_subsystem_init();
	arch_post_acpi_subsys_init();
	sfi_init_late();

	if (efi_enabled(EFI_RUNTIME_SERVICES)) {
		efi_free_boot_services();
	}

	/* Do the rest non-__init'ed, we're now alive */
	rest_init(); // kdev: 其余init任务，创建1号进程和2号进程
	// kdev: 0号进程，也就是swapper转为空闲进程(idle进程)，进入无限循环
	// idle无限循环不会结束，函数不会返回 cpu_idle_loop()

	prevent_tail_call_optimization(); // kdev: 不会被执行

	// rest_init() 是内核从初始化阶段过渡到用户空间的最终步骤。
	// 由于它通过调度机制将执行流永久转移到空闲循环，
	// start_kernel() 中 rest_init() 之后的代码在正常启动流程中不会执行。
	// 这一设计确保了内核启动逻辑的完整性和资源的高效利用
	pr_kdev("%s File:[%s],Line:[%d] finished\n", __FUNCTION__, __FILE__, __LINE__);
}

/* Call all constructor functions linked into the kernel. */
static void __init do_ctors(void)
{
#ifdef CONFIG_CONSTRUCTORS
	ctor_fn_t *fn = (ctor_fn_t *) __ctors_start;

	for (; fn < (ctor_fn_t *) __ctors_end; fn++)
		(*fn)();
#endif
}

bool initcall_debug;
core_param(initcall_debug, initcall_debug, bool, 0644);

#ifdef CONFIG_KALLSYMS
struct blacklist_entry {
	struct list_head next;
	char *buf;
};

static __initdata_or_module LIST_HEAD(blacklisted_initcalls);

static int __init initcall_blacklist(char *str)
{
	char *str_entry;
	struct blacklist_entry *entry;

	/* str argument is a comma-separated list of functions */
	do {
		str_entry = strsep(&str, ",");
		if (str_entry) {
			pr_debug("blacklisting initcall %s\n", str_entry);
			entry = alloc_bootmem(sizeof(*entry));
			entry->buf = alloc_bootmem(strlen(str_entry) + 1);
			strcpy(entry->buf, str_entry);
			list_add(&entry->next, &blacklisted_initcalls);
		}
	} while (str_entry);

	return 1;
}

static bool __init_or_module initcall_blacklisted(initcall_t fn)
{
	struct blacklist_entry *entry;
	char fn_name[KSYM_SYMBOL_LEN];
	unsigned long addr;

	if (list_empty(&blacklisted_initcalls))
		return false;

	addr = (unsigned long) dereference_function_descriptor(fn);
	sprint_symbol_no_offset(fn_name, addr);

	/*
	 * fn will be "function_name [module_name]" where [module_name] is not
	 * displayed for built-in init functions.  Strip off the [module_name].
	 */
	strreplace(fn_name, ' ', '\0');

	list_for_each_entry(entry, &blacklisted_initcalls, next) {
		if (!strcmp(fn_name, entry->buf)) {
			pr_debug("initcall %s blacklisted\n", fn_name);
			return true;
		}
	}

	return false;
}
#else
static int __init initcall_blacklist(char *str)
{
	pr_warn("initcall_blacklist requires CONFIG_KALLSYMS\n");
	return 0;
}

static bool __init_or_module initcall_blacklisted(initcall_t fn)
{
	return false;
}
#endif
__setup("initcall_blacklist=", initcall_blacklist);

static int __init_or_module do_one_initcall_debug(initcall_t fn)
{
	unsigned long long calltime, delta, rettime;
	unsigned long long duration;
	int ret;

	pr_kdev("calling  %pF @ %i [0x%px]\n", fn, task_pid_nr(current), (void *)fn);
	calltime = local_clock();
	ret = fn();
	rettime = local_clock();
	delta = rettime - calltime;
	duration = delta >> 10;
	pr_kdev("initcall %pF returned %d after %lld usecs\n",
		 fn, ret, duration);

	return ret;
}

int __init_or_module do_one_initcall(initcall_t fn)
{
	int count = preempt_count();
	int ret;
	char msgbuf[64];

	if (initcall_blacklisted(fn))
		return -EPERM;

	if (initcall_debug)
		ret = do_one_initcall_debug(fn);
	else
		ret = fn();

	msgbuf[0] = 0;

	if (preempt_count() != count) {
		sprintf(msgbuf, "preemption imbalance ");
		preempt_count_set(count);
	}
	if (irqs_disabled()) {
		strlcat(msgbuf, "disabled interrupts ", sizeof(msgbuf));
		local_irq_enable();
	}
	WARN(msgbuf[0], "initcall %pF returned with %s\n", fn, msgbuf);

	add_latent_entropy();
	return ret;
}


extern initcall_t __initcall_start[];
extern initcall_t __initcall0_start[];
extern initcall_t __initcall1_start[];
extern initcall_t __initcall2_start[];
extern initcall_t __initcall3_start[];
extern initcall_t __initcall4_start[];
extern initcall_t __initcall5_start[];
extern initcall_t __initcall6_start[];
extern initcall_t __initcall7_start[];
extern initcall_t __initcall_end[];

static initcall_t *initcall_levels[] __initdata = {
	__initcall0_start,
	__initcall1_start,
	__initcall2_start,
	__initcall3_start,
	__initcall4_start,
	__initcall5_start,
	__initcall6_start,
	__initcall7_start,
	__initcall_end,
};

/* Keep these in sync with initcalls in include/linux/init.h */
static char *initcall_level_names[] __initdata = {
	"early",
	"core",
	"postcore",
	"arch",
	"subsys",
	"fs",
	"device",
	"late",
};

static void __init do_initcall_level(int level)
{
	initcall_t *fn;

	strcpy(initcall_command_line, saved_command_line);
	
	parse_args(initcall_level_names[level],
		   initcall_command_line, __start___param,
		   __stop___param - __start___param,
		   level, level,
		   NULL, &repair_env_string); // kdev: 每个level都调用参数解析

	for (fn = initcall_levels[level]; fn < initcall_levels[level+1]; fn++) {
		pr_kdev("do_initcalls_%d Call Func:%pS\n", level, *fn);
		do_one_initcall(*fn);
	}
}

static void __init do_initcalls(void)
{
	// 调用initcall机制0-7段区间的函数
	int level;

	pr_kdev("%s File:[%s],Line:[%d] start\n", __FUNCTION__, __FILE__, __LINE__);

	for (level = 0; level < ARRAY_SIZE(initcall_levels) - 1; level++) {
		pr_kdev("do_initcalls_%d begin !! Func:%s,File:[%s],Line:[%d]\n",
				level, __FUNCTION__, __FILE__, __LINE__);
		do_initcall_level(level);
		pr_kdev("do_initcalls_%d finished !! Func:%s,File:[%s],Line:[%d]\n",
				level, __FUNCTION__, __FILE__, __LINE__);
	}
	
	pr_kdev("%s File:[%s],Line:[%d] finished\n", __FUNCTION__, __FILE__, __LINE__);
}

/*
 * Ok, the machine is now initialized. None of the devices
 * have been touched yet, but the CPU subsystem is up and
 * running, and memory and process management works.
 *
 * Now we can finally start doing some real work..
 */
static void __init do_basic_setup(void)
{

	cpuset_init_smp();
	shmem_init();
	driver_init();
	init_irq_proc();
	do_ctors();
	usermodehelper_enable();
	do_initcalls();
}

static void __init do_pre_smp_initcalls(void)
{
	// 调用initcall机制的early段区间的函数
	initcall_t *fn;
	
	pr_kdev("%s File:[%s],Line:[%d] start\n", __FUNCTION__, __FILE__, __LINE__);
	for (fn = __initcall_start; fn < __initcall0_start; fn++) {
		pr_kdev("%s File:[%s],Line:[%d] do_initcalls_early call %pS\n", __FUNCTION__, __FILE__, __LINE__, *fn);
		do_one_initcall(*fn);
	}
	pr_kdev("%s File:[%s],Line:[%d] finished\n", __FUNCTION__, __FILE__, __LINE__);
}

/*
 * This function requests modules which should be loaded by default and is
 * called twice right after initrd is mounted and right before init is
 * exec'd.  If such modules are on either initrd or rootfs, they will be
 * loaded before control is passed to userland.
 */
void __init load_default_modules(void)
{
	load_default_elevator_module();
}

static int run_init_process(const char *init_filename)
{
	int yyf_i;
	argv_init[0] = init_filename;

	// kdev: 打印完整 argv 和 envp（必须NULL结尾）
	for (yyf_i = 0; argv_init[yyf_i]; yyf_i++)
	    pr_kdev("%s File:[%s],Line:[%d] argv[%d]=%s\n",
	    __FUNCTION__, __FILE__, __LINE__, yyf_i, argv_init[yyf_i]);
	for (yyf_i = 0; envp_init[yyf_i]; yyf_i++)
	    pr_kdev("%s File:[%s],Line:[%d] envp[%d]=%s\n",
	    __FUNCTION__, __FILE__, __LINE__, yyf_i, envp_init[yyf_i]);

	pr_info("********************************************************\n");

	return do_execve(getname_kernel(init_filename),
		(const char __user *const __user *)argv_init,
		(const char __user *const __user *)envp_init);
}

static int try_to_run_init_process(const char *init_filename)
{
	int ret;

	ret = run_init_process(init_filename);

	if (ret && ret != -ENOENT) {
		pr_err("Starting init: %s exists but couldn't execute it (error %d)\n",
		       init_filename, ret);
	}

	return ret;
}

static noinline void __init kernel_init_freeable(void);

#if defined(CONFIG_STRICT_KERNEL_RWX) || defined(CONFIG_STRICT_MODULE_RWX)
bool rodata_enabled __ro_after_init = true;
static int __init set_debug_rodata(char *str)
{
	if (strtobool(str, &rodata_enabled))
		pr_warn("Invalid option string for rodata: '%s'\n", str);
	return 1;
}
__setup("rodata=", set_debug_rodata);
#endif

#ifdef CONFIG_STRICT_KERNEL_RWX
static void mark_readonly(void)
{
	if (rodata_enabled) {
		/*
		 * load_module() results in W+X mappings, which are cleaned up
		 * with call_rcu_sched().  Let's make sure that queued work is
		 * flushed so that we don't hit false positives looking for
		 * insecure pages which are W+X.
		 */
		rcu_barrier_sched();
		mark_rodata_ro();
		rodata_test();
	} else
		pr_info("Kernel memory protection disabled.\n");
}
#else
static inline void mark_readonly(void)
{
	pr_warn("This architecture does not have kernel memory protection.\n");
}
#endif

static int __ref kernel_init(void *unused)
{
	int ret;

	pr_kdev("%s File:[%s],Line:[%d] kthread 1 kernel_init running!!\n",
		__FUNCTION__, __FILE__, __LINE__);

	kernel_init_freeable(); // kdev: 内核启动阶段，自由化=解除约束
	// kdev: 解除SMP约束、解除动态加载模块约束、解除静态限制

	
	/* need to finish all async __init code before freeing the memory */
	async_synchronize_full();
	ftrace_free_init_mem();
	free_initmem();
	mark_readonly();

	/*
	 * Kernel mappings are now finalized - update the userspace page-table
	 * to finalize PTI.
	 */
	pti_finalize();

	system_state = SYSTEM_RUNNING;
	numa_default_policy();

	rcu_end_inkernel_boot();

	
	pr_info("********************************************************\n");
	pr_kdev("%s File:[%s],Line:[%d] kthread 1 kernel_init enter userspace ramdisk_execute_command=[%s]\n",
		__FUNCTION__, __FILE__, __LINE__, ramdisk_execute_command);
	pr_info("********************************************************\n");


	if (ramdisk_execute_command) {
		ret = run_init_process(ramdisk_execute_command);
		if (!ret)
			return 0;
		pr_err("Failed to execute %s (error %d)\n",
		       ramdisk_execute_command, ret);
	}

	pr_info("\n********************************************************\n");
	pr_kdev("%s File:[%s],Line:[%d] kthread 1 kernel_init enter userspace execute_command=[%s]\n",
		__FUNCTION__, __FILE__, __LINE__, execute_command);
	pr_info("\n********************************************************\n");

	/*
	 * We try each of these until one succeeds.
	 *
	 * The Bourne shell can be used instead of init if we are
	 * trying to recover a really broken machine.
	 */
	if (execute_command) {
		ret = run_init_process(execute_command);
		if (!ret)
			return 0;
		panic("Requested init %s failed (error %d).",
		      execute_command, ret);
	}

	
	pr_kdev("%s File:[%s],Line:[%d] kthread 1 kernel_init enter userspace!!\n",
		__FUNCTION__, __FILE__, __LINE__);
	
	if (!try_to_run_init_process("/sbin/init") ||
	    !try_to_run_init_process("/etc/init") ||
	    !try_to_run_init_process("/bin/init") ||
	    !try_to_run_init_process("/bin/sh"))
		return 0;

	panic("No working init found.  Try passing init= option to kernel. "
	      "See Linux Documentation/admin-guide/init.rst for guidance.");
}

static noinline void __init kernel_init_freeable(void)
{
	/*
	 * Wait until kthreadd is all set-up.
	 */
	pr_kdev("%s File:[%s],Line:[%d] start\n", __FUNCTION__, __FILE__, __LINE__);
	wait_for_completion(&kthreadd_done);

	/* Now the scheduler is fully set up and can do blocking allocations */
	gfp_allowed_mask = __GFP_BITS_MASK;

	/*
	 * init can allocate pages on any node
	 */
	set_mems_allowed(node_states[N_MEMORY]);

	cad_pid = get_pid(task_pid(current));

	smp_prepare_cpus(setup_max_cpus);

	workqueue_init();

	init_mm_internals();

	do_pre_smp_initcalls(); // kdev: 调用initcall机制的early段的函数
	lockup_detector_init();

	smp_init(); // kdev: 激活其他 CPU（APs）
	sched_init_smp();

	page_alloc_init_late();
	/* Initialize page ext after all struct pages are initialized. */
	page_ext_init();

	do_basic_setup(); // kdev: 调用initcall机制的0-7段的函数

	/* Open the /dev/console on the rootfs, this should never fail */
	if (sys_open((const char __user *) "/dev/console", O_RDWR, 0) < 0)
		pr_err("Warning: unable to open an initial console.\n");

	(void) sys_dup(0);
	(void) sys_dup(0);
	/*
	 * check if there is an early userspace init.  If yes, let it do all
	 * the work
	 */

	if (!ramdisk_execute_command)
		ramdisk_execute_command = "/init";

	if (sys_access((const char __user *) ramdisk_execute_command, 0) != 0) {
		ramdisk_execute_command = NULL;
		prepare_namespace();
	}

	/*
	 * Ok, we have completed the initial bootup, and
	 * we're essentially up and running. Get rid of the
	 * initmem segments and start the user-mode stuff..
	 *
	 * rootfs is available now, try loading the public keys
	 * and default modules
	 */


	integrity_load_keys();
	load_default_modules();
	pr_kdev("%s File:[%s],Line:[%d] finished\n", __FUNCTION__, __FILE__, __LINE__);
}
