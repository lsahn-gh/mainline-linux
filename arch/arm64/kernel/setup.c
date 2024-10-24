// SPDX-License-Identifier: GPL-2.0-only
/*
 * Based on arch/arm/kernel/setup.c
 *
 * Copyright (C) 1995-2001 Russell King
 * Copyright (C) 2012 ARM Ltd.
 */

#include <linux/acpi.h>
#include <linux/export.h>
#include <linux/kernel.h>
#include <linux/stddef.h>
#include <linux/ioport.h>
#include <linux/delay.h>
#include <linux/initrd.h>
#include <linux/console.h>
#include <linux/cache.h>
#include <linux/screen_info.h>
#include <linux/init.h>
#include <linux/kexec.h>
#include <linux/root_dev.h>
#include <linux/cpu.h>
#include <linux/interrupt.h>
#include <linux/smp.h>
#include <linux/fs.h>
#include <linux/panic_notifier.h>
#include <linux/proc_fs.h>
#include <linux/memblock.h>
#include <linux/of_fdt.h>
#include <linux/efi.h>
#include <linux/psci.h>
#include <linux/sched/task.h>
#include <linux/mm.h>

#include <asm/acpi.h>
#include <asm/fixmap.h>
#include <asm/cpu.h>
#include <asm/cputype.h>
#include <asm/daifflags.h>
#include <asm/elf.h>
#include <asm/cpufeature.h>
#include <asm/cpu_ops.h>
#include <asm/kasan.h>
#include <asm/numa.h>
#include <asm/sections.h>
#include <asm/setup.h>
#include <asm/smp_plat.h>
#include <asm/cacheflush.h>
#include <asm/tlbflush.h>
#include <asm/traps.h>
#include <asm/efi.h>
#include <asm/xen/hypervisor.h>
#include <asm/mmu_context.h>

static int num_standard_resources;
static struct resource *standard_resources;

/* IAMROOT, 2021.09.04:
 * - mmu enable 이후에 __primary_switched 에서 변수 초기화가 이루어진다.
 *
 *   fdt: flattened device tree
 *   __initdata : include/linux/init.h
 */
phys_addr_t __fdt_pointer __initdata;

/*
 * Standard memory resources
 */
static struct resource mem_res[] = {
	{
		.name = "Kernel code",
		.start = 0,
		.end = 0,
		.flags = IORESOURCE_SYSTEM_RAM
	},
	{
		.name = "Kernel data",
		.start = 0,
		.end = 0,
		.flags = IORESOURCE_SYSTEM_RAM
	}
};

#define kernel_code mem_res[0]
#define kernel_data mem_res[1]

/*
 * The recorded values of x0 .. x3 upon kernel entry.
 */
u64 __cacheline_aligned boot_args[4];

void __init smp_setup_processor_id(void)
{
	/* IAMROOT, 2021.09.11:
	 * - cpu 0번이면 mpidr 값이 0일 확률이 높지만 아닐 수도 있다.
	 *   이를 대비하여 MPIDR reg에서 값을 읽어오며 이때 MPIDR_HWID_BITMASK를
	 *   이용하여 MPIDR에서 {aff3, aff2, aff1, aff0} 값만 가져온다.
	 */
	u64 mpidr = read_cpuid_mpidr() & MPIDR_HWID_BITMASK;
	set_cpu_logical_map(0, mpidr);

	pr_info("Booting Linux on physical CPU 0x%010lx [0x%08x]\n",
		(unsigned long)mpidr, read_cpuid_id());
}

bool arch_match_cpu_phys_id(int cpu, u64 phys_id)
{
	return phys_id == cpu_logical_map(cpu);
}

struct mpidr_hash mpidr_hash;
/**
 * smp_build_mpidr_hash - Pre-compute shifts required at each affinity
 *			  level in order to build a linear index from an
 *			  MPIDR value. Resulting algorithm is a collision
 *			  free hash carried out through shifting and ORing
 */
/* IAMROOT, 2022.01.01:
 * - Arm에서 PE(cpu core) id는 MPIDR_EL1 reg에 저장된 aff0..3 값으로
 *   구분할 수 있는데 multi-cluster 구조에서는 이 값이 linear 하지 않아서
 *   다른 복잡한 방법을 사용하여야 하며 PE 별로 중복되지 않아야 하므로
 *   hashing 방법을 사용하는 것이다.
 *
 *   MPIDR_EL1 값을 읽어 affinity level 별로 필요한 shift 값들을 저장하고
 *   나중에 cpu_suspend(), cpu_resume() 함수 내부에서 사용한다.
 */
static void __init smp_build_mpidr_hash(void)
{
	u32 i, affinity, fs[4], bits[4], ls;
	u64 mask = 0;
	/*
	 * Pre-scan the list of MPIDRS and filter out bits that do
	 * not contribute to affinity levels, ie they never toggle.
	 */
	/* IAMROOT, 2022.01.02:
	 * - bootcpu의 PE-id와 나머지 cpu의 PE-id를 xor하고 mask와 or 한다.
	 *
	 *   예) cpu0 PE-id: 0x1
	 *       cpu1 PE-id: 0x2
	 *       ---------------
	 *             mask: 0x3
	 *
	 *   예) cpu0 PE-id: 0xf01
	 *       cpu1 PE-id: 0xf02
	 *       -----------------
	 *             mask: 0x3
	 */
	for_each_possible_cpu(i)
		mask |= (cpu_logical_map(i) ^ cpu_logical_map(0));
	pr_debug("mask of set bits %#llx\n", mask);
	/*
	 * Find and stash the last and first bit set at all affinity levels to
	 * check how many bits are required to represent them.
	 */
	for (i = 0; i < 4; i++) {
		/* IAMROOT, 2022.01.02:
		 * - MPIDR_EL1 reg의 aff(i) 값을 가져온다.
		 */
		affinity = MPIDR_AFFINITY_LEVEL(mask, i);
		/*
		 * Find the MSB bit and LSB bits position
		 * to determine how many bits are required
		 * to express the affinity level.
		 */
		/* IAMROOT, 2024.10.06:
		 * - 주석 참고 (설명 잘 되어있음)
		 */
		ls = fls(affinity);
		/* IAMROOT, 2022.01.02:
		 * - affinity != 0: nth of first find (set) bit - 1
		 *   affinity == 0: 0
		 */
		fs[i] = affinity ? ffs(affinity) - 1 : 0;
		/* IAMROOT, 2022.01.02:
		 * - ls(last set)과 fs(first set) 사이의 bit 개수 차이를 저장
		 *
		 *   예)
		 *   - affinity: 0
		 *     bits: 0 == 0 - 0
		 *   - affinity: 7 (0b111)
		 *     bits: 3 == 3 - 0
		 *   - affinity: 15 (0b1111)
		 *     bits: 4 == 4 - 0
		 *   - affinity: 255 (0b11111111)
		 *     bits: 8 == 8 - 0
		 */
		bits[i] = ls - fs[i];
	}

	/* IAMROOT, 2024.10.06:
	 * - 중간 정리
	 *
	 *   1). fs[i]  : aff(i)에서 첫번째 set된 bit 번호 (right-to-left)
	 *   2). bits[i]: aff(i)에서 마지막 set된 bit 번호 - fs[i]
	 *   3). mask   : cpu0 PE-id와 나머지 cpu PE-id를 xor하고 or한 값
	 *
	 *   저장되어 있음.
	 *
	 *   Note. fs[i] + bits[i]를 계산하면 ls[i]를 구할 수 있음.
	 */

	/*
	 * An index can be created from the MPIDR_EL1 by isolating the
	 * significant bits at each affinity level and by shifting
	 * them in order to compress the 32 bits values space to a
	 * compressed set of values. This is equivalent to hashing
	 * the MPIDR_EL1 through shifting and ORing. It is a collision free
	 * hash though not minimal since some levels might contain a number
	 * of CPUs that is not an exact power of 2 and their bit
	 * representation might contain holes, eg MPIDR_EL1[7:0] = {0x2, 0x80}.
	 */
	/* IAMROOT, 2022.01.02:
	 * - hashing 값과 decoding 계산에 필요한 operand들을 mpidr_hash 구조체에
	 *   저장한다.
	 *
	 *   예)
	 *      mask: 0x00050404
	 *
	 *      --------+------+-------+-------+-------+
	 *      level   | aff3 | aff2  | aff1  | aff0  |
	 *      --------+------+-------+-------+-------+
	 *       mask   |   0  | 0b101 | 0b100 | 0b100 |
	 *      --------+------+-------+-------+-------+
	 *
	 *      - shift_aff[0]: 2  == 0 + 2
	 *      - shift_aff[1]: 10 == 8 + 2 - 0
	 *      - shift_aff[2]: 16 == 16 + 0 - (0 + 0)
	 *      - shift_aff[3]: 26 == 24 + 0 - (2 + 0 + 0)
	 *      - mask        : 0x00050404
	 *      - bits        : 2  == 0 + 2 + 0 + 0
	 */
	mpidr_hash.shift_aff[0] = MPIDR_LEVEL_SHIFT(0) + fs[0];
	mpidr_hash.shift_aff[1] = MPIDR_LEVEL_SHIFT(1) + fs[1] - bits[0];
	mpidr_hash.shift_aff[2] = MPIDR_LEVEL_SHIFT(2) + fs[2] -
						(bits[1] + bits[0]);
	mpidr_hash.shift_aff[3] = MPIDR_LEVEL_SHIFT(3) +
				  fs[3] - (bits[2] + bits[1] + bits[0]);
	mpidr_hash.mask = mask;
	mpidr_hash.bits = bits[3] + bits[2] + bits[1] + bits[0];
	pr_debug("MPIDR hash: aff0[%u] aff1[%u] aff2[%u] aff3[%u] mask[%#llx] bits[%u]\n",
		mpidr_hash.shift_aff[0],
		mpidr_hash.shift_aff[1],
		mpidr_hash.shift_aff[2],
		mpidr_hash.shift_aff[3],
		mpidr_hash.mask,
		mpidr_hash.bits);
	/*
	 * 4x is an arbitrary value used to warn on a hash table much bigger
	 * than expected on most systems.
	 */
	if (mpidr_hash_size() > 4 * num_possible_cpus())
		pr_warn("Large number of MPIDR hash buckets detected\n");
}

static void *early_fdt_ptr __initdata;

void __init *get_early_fdt_ptr(void)
{
	return early_fdt_ptr;
}

asmlinkage void __init early_fdt_map(u64 dt_phys)
{
	int fdt_size;

/* IAMROOT, 2023.11.18:
 * - fdt 매핑 이전에 fixmap을 초기화하여 임시 page table을 생성한다.
 *   현재는 dynamic mapping subsystem이 활성화되기 전이라 fixmap을 이용한다.
 */
	early_fixmap_init();
	early_fdt_ptr = fixmap_remap_fdt(dt_phys, &fdt_size, PAGE_KERNEL);
}

/* IAMROOT, 2021.10.09:
 * - FDT를 fixmap에 매핑한 후 스캔하여 몇개의 주요 정보를 알아온다.
 */
static void __init setup_machine_fdt(phys_addr_t dt_phys)
{
	int size;
	void *dt_virt = fixmap_remap_fdt(dt_phys, &size, PAGE_KERNEL);
	const char *name;

	/* IAMROOT, 2024.01.20:
	 * - fdt를 fixmap mapping에 성공하면 memblock reserved region에도
	 *   추가한다.
	 */
	if (dt_virt)
		memblock_reserve(dt_phys, size);

	/* IAMROOT, 2021.10.14:
	 * - dt_virt가 null이거나 early_init_dt_scan(..)이 실패하면
	 *   cpu_relax(..) 호출하며 inf-loop를 수행한다.
	 */
	if (!dt_virt || !early_init_dt_scan(dt_virt)) {
		pr_crit("\n"
			"Error: invalid device tree blob at physical address %pa (virtual address 0x%p)\n"
			"The dtb must be 8-byte aligned and must not exceed 2 MB in size\n"
			"\nPlease check your bootloader.",
			&dt_phys, dt_virt);

		while (true)
			cpu_relax();
	}
	/* IAMROOT, 2024.01.20:
	 * - dt에서 물리 메모리 정보를 읽어 memblock에 추가한 상태.
	 */

	/* IAMROOT, 2021.10.16:
	 * - fdt fixmap의 page table attr만 RO로 변경한다.
	 */
	/* Early fixups are done, map the FDT as read-only now */
	fixmap_remap_fdt(dt_phys, &size, PAGE_KERNEL_RO);

	name = of_flat_dt_get_machine_name();
	if (!name)
		return;

	pr_info("Machine model: %s\n", name);
	dump_stack_set_arch_desc("%s (DT)", name);
}

/* IAMROOT, 2021.12.18: TODO
 * - PASS
 */
static void __init request_standard_resources(void)
{
	struct memblock_region *region;
	struct resource *res;
	unsigned long i = 0;
	size_t res_size;

	kernel_code.start   = __pa_symbol(_stext);
	kernel_code.end     = __pa_symbol(__init_begin - 1);
	kernel_data.start   = __pa_symbol(_sdata);
	kernel_data.end     = __pa_symbol(_end - 1);

	num_standard_resources = memblock.memory.cnt;
	res_size = num_standard_resources * sizeof(*standard_resources);
	standard_resources = memblock_alloc(res_size, SMP_CACHE_BYTES);
	if (!standard_resources)
		panic("%s: Failed to allocate %zu bytes\n", __func__, res_size);

	for_each_mem_region(region) {
		res = &standard_resources[i++];
		if (memblock_is_nomap(region)) {
			res->name  = "reserved";
			res->flags = IORESOURCE_MEM;
		} else {
			res->name  = "System RAM";
			res->flags = IORESOURCE_SYSTEM_RAM | IORESOURCE_BUSY;
		}
		res->start = __pfn_to_phys(memblock_region_memory_base_pfn(region));
		res->end = __pfn_to_phys(memblock_region_memory_end_pfn(region)) - 1;

		request_resource(&iomem_resource, res);

		if (kernel_code.start >= res->start &&
		    kernel_code.end <= res->end)
			request_resource(res, &kernel_code);
		if (kernel_data.start >= res->start &&
		    kernel_data.end <= res->end)
			request_resource(res, &kernel_data);
#ifdef CONFIG_KEXEC_CORE
		/* Userspace will find "Crash kernel" region in /proc/iomem. */
		if (crashk_res.end && crashk_res.start >= res->start &&
		    crashk_res.end <= res->end)
			request_resource(res, &crashk_res);
#endif
	}
}

static int __init reserve_memblock_reserved_regions(void)
{
	u64 i, j;

	for (i = 0; i < num_standard_resources; ++i) {
		struct resource *mem = &standard_resources[i];
		phys_addr_t r_start, r_end, mem_size = resource_size(mem);

		if (!memblock_is_region_reserved(mem->start, mem_size))
			continue;

		for_each_reserved_mem_range(j, &r_start, &r_end) {
			resource_size_t start, end;

			start = max(PFN_PHYS(PFN_DOWN(r_start)), mem->start);
			end = min(PFN_PHYS(PFN_UP(r_end)) - 1, mem->end);

			if (start > mem->end || end < mem->start)
				continue;

			reserve_region_with_split(mem, start, end, "reserved");
		}
	}

	return 0;
}
arch_initcall(reserve_memblock_reserved_regions);

/* IAMROOT, 2021.09.11:
 * - 모든 cpu(PE)는 고유한 PE id 값을 가지고 있지만 kernel code는
 *   logical cpu id를 사용한다.
 *   (0, 1, 2, ...)
 *
 *   따라서 cpu 마다 매핑된 고유 PE id 값을 알기 위해서 @cpu 값을 통해
 *   hwid를 가져와야 한다.
 *
 *   아래가 cpu : hwid 매핑을 위한 array 이다.
 */
u64 __cpu_logical_map[NR_CPUS] = { [0 ... NR_CPUS-1] = INVALID_HWID };

/* IAMROOT, 2024.10.04:
 * - @cpu 번호에 해당하는 hwid를 가져온다.
 */
u64 cpu_logical_map(unsigned int cpu)
{
	return __cpu_logical_map[cpu];
}

/* IAMROOT, 2024.01.09:
 * - Arm64 arch에 의존적인 기능을 early setup 한다.
 */
void __init __no_sanitize_address setup_arch(char **cmdline_p)
{
	setup_initial_init_mm(_stext, _etext, _edata, _end);

	*cmdline_p = boot_command_line;

	/*
	 * If know now we are going to need KPTI then use non-global
	 * mappings from the start, avoiding the cost of rewriting
	 * everything later.
	 */
	arm64_use_ng_mappings = kaslr_requires_kpti();

	/* IAMROOT, 2021.10.16:
	 * - 정규 매핑 전에 I/O 장치들이 memory를 사용할 수 있도록 fixmap을 이용한
	 *   early memory mapping을 준비한다.
	 *
	 *   early: 정규 매핑, memblock 초기화도 안된 상황.
	 *   late : 나중에 해도 되는 작업들
	 */
	early_fixmap_init();
	early_ioremap_init();

	setup_machine_fdt(__fdt_pointer);

	/* IAMROOT, 2021.10.16: TODO
	 * - static key
	 *   if의 조건에 사용되는 변수가 변경될일이 거의 없으면서 read를 많이 해야되는경우
	 *   매번 읽는것이 아니라 해당 조건문 자체를 nop나 branch로 교체해서 if문 자체를
	 *   없애는 방법. 또한 likely와 unlikely를 사용해 branch되는 code의 주변 배치여부도
	 *   정해 tlb cache나 data cache hit도 잘되도록 하여 효율을 높인다.
	 *
	 *   enable / disable의 비용이 매우크다. 해당 변수가 존재하는 모든 code를
	 *   교체하는식으로 진행하며 tlb cache등도 비워줘야 되기 때문이다.
	 *
	 *   관련 api : static_branch_likely, static_branch_unlikely
	 */
	/*
	 * Initialise the static keys early as they may be enabled by the
	 * cpufeature code and early parameters.
	 */
	jump_label_init();
	parse_early_param();

	/* IAMROOT, 2021.10.16:
	 * - daif에서 irq, fiq를 제외한 나머지 exception을 enable.
	 */
	/*
	 * Unmask asynchronous aborts and fiq after bringing up possible
	 * earlycon. (Report possible System Errors once we can report this
	 * occurred).
	 */
	local_daif_restore(DAIF_PROCCTX_NOIRQ);

	/* IAMROOT, 2021.10.16:
	 * - head.S에서 ttbr0에 mapping 했던 idmap을 해제한다.
	 */
	/*
	 * TTBR0 is only used for the identity mapping at this stage. Make it
	 * point to zero page to avoid speculatively fetching new entries.
	 */
	cpu_uninstall_idmap();

	xen_early_init();
	efi_init();

	if (!efi_enabled(EFI_BOOT) && ((u64)_text % MIN_KIMG_ALIGN) != 0)
	     pr_warn(FW_BUG "Kernel image misaligned at boot, please fix your bootloader!");

	/* IAMROOT, 2024.09.22:
	 * - arm64 arch 용 memblock 초기화.
	 */
	arm64_memblock_init();

	/* IAMROOT, 2021.10.31:
	 * - 현재 TTBRx 값은 아래와 같음.
	 *   ttbr1_el1: init_pg_dir
	 *   ttbr0_el1: reserved_pg_dir
	 */

	paging_init();

	/* IAMROOT, 2021.10.31:
	 * - 현재 TTBRx 값은 아래와 같음.
	 *   ttbr1_el1: swapper_pg_dir
	 *   ttbr0_el1: reserved_pg_dir
	 */

	acpi_table_upgrade();

	/* Parse the ACPI tables for possible boot-time configuration */
	acpi_boot_table_init();

	/* IAMROOT, 2024.03.15:
	 * - @acpi_disabled == true 라면 dtb를 unflatten 한다.
	 */
	if (acpi_disabled)
		unflatten_device_tree();

	/* IAMROOT, 2024.10.03: TODO
	 */
	bootmem_init();

	kasan_init();

	request_standard_resources();

	early_ioremap_reset();

	if (acpi_disabled)
		psci_dt_init();
	else
		psci_acpi_init();

	/* IAMROOT, 2024.10.04:
	 * - boot cpu의 enable-method를 통해 cpu_ops를 설정한다.
	 */
	init_bootcpu_ops();
	smp_init_cpus();
	smp_build_mpidr_hash();

	/* Init percpu seeds for random tags after cpus are set up. */
	kasan_init_sw_tags();

#ifdef CONFIG_ARM64_SW_TTBR0_PAN
	/*
	 * Make sure init_thread_info.ttbr0 always generates translation
	 * faults in case uaccess_enable() is inadvertently called by the init
	 * thread.
	 */
	init_task.thread_info.ttbr0 = phys_to_ttbr(__pa_symbol(reserved_pg_dir));
#endif

	if (boot_args[1] || boot_args[2] || boot_args[3]) {
		pr_err("WARNING: x1-x3 nonzero in violation of boot protocol:\n"
			"\tx1: %016llx\n\tx2: %016llx\n\tx3: %016llx\n"
			"This indicates a broken bootloader or old kernel\n",
			boot_args[1], boot_args[2], boot_args[3]);
	}
}

static inline bool cpu_can_disable(unsigned int cpu)
{
#ifdef CONFIG_HOTPLUG_CPU
	const struct cpu_operations *ops = get_cpu_ops(cpu);

	if (ops && ops->cpu_can_disable)
		return ops->cpu_can_disable(cpu);
#endif
	return false;
}

static int __init topology_init(void)
{
	int i;

	for_each_online_node(i)
		register_one_node(i);

	for_each_possible_cpu(i) {
		struct cpu *cpu = &per_cpu(cpu_data.cpu, i);
		cpu->hotpluggable = cpu_can_disable(i);
		register_cpu(cpu, i);
	}

	return 0;
}
subsys_initcall(topology_init);

static void dump_kernel_offset(void)
{
	const unsigned long offset = kaslr_offset();

	if (IS_ENABLED(CONFIG_RANDOMIZE_BASE) && offset > 0) {
		pr_emerg("Kernel Offset: 0x%lx from 0x%lx\n",
			 offset, KIMAGE_VADDR);
		pr_emerg("PHYS_OFFSET: 0x%llx\n", PHYS_OFFSET);
	} else {
		pr_emerg("Kernel Offset: disabled\n");
	}
}

static int arm64_panic_block_dump(struct notifier_block *self,
				  unsigned long v, void *p)
{
	dump_kernel_offset();
	dump_cpu_features();
	dump_mem_limit();
	return 0;
}

static struct notifier_block arm64_panic_block = {
	.notifier_call = arm64_panic_block_dump
};

static int __init register_arm64_panic_block(void)
{
	atomic_notifier_chain_register(&panic_notifier_list,
				       &arm64_panic_block);
	return 0;
}
device_initcall(register_arm64_panic_block);
