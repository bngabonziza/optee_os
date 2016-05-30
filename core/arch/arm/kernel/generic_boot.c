/*
 * Copyright (c) 2015, Linaro Limited
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <arm.h>
#include <assert.h>
#include <compiler.h>
#include <inttypes.h>
#include <kernel/generic_boot.h>
#include <kernel/thread.h>
#include <kernel/panic.h>
#include <kernel/misc.h>
#include <malloc.h>
#include <mm/core_mmu.h>
#include <mm/tee_mm.h>
#include <mm/tee_mmu.h>
#include <mm/tee_pager.h>
#include <sm/tee_mon.h>
#include <trace.h>
#include <tee/tee_cryp_provider.h>
#include <utee_defines.h>
#include <util.h>

#include <platform_config.h>
#include <kernel/thread.h>


#if !defined(CFG_WITH_ARM_TRUSTED_FW)
#include <sm/sm.h>
#endif

#if defined(CFG_WITH_VFP)
#include <kernel/vfp.h>
#endif

#define PADDR_INVALID		0xffffffff

#ifdef CFG_BOOT_SYNC_CPU
/*
 * Array used when booting, to synchronize cpu.
 * When 0, the cpu has not started.
 * When 1, it has started
 */
uint32_t sem_cpu_sync[CFG_TEE_CORE_NB_CORE] __data;
#endif

/* May be overridden in plat-$(PLATFORM)/main.c */
__weak void main_init_gic(void)
{
}

#if defined(CFG_WITH_ARM_TRUSTED_FW)
void init_sec_mon(uint32_t nsec_entry __maybe_unused)
{
	assert(nsec_entry == PADDR_INVALID);
	/* Do nothing as we don't have a secure monitor */
}
#else
/* May be overridden in plat-$(PLATFORM)/main.c */
__weak void init_sec_mon(uint32_t nsec_entry)
{
	struct sm_nsec_ctx *nsec_ctx;

	assert(nsec_entry != PADDR_INVALID);

	/* Initialize secure monitor */
	nsec_ctx = sm_get_nsec_ctx();
	nsec_ctx->mon_lr = nsec_entry;
	nsec_ctx->mon_spsr = CPSR_MODE_SVC | CPSR_I;

}
#endif

#if defined(CFG_WITH_ARM_TRUSTED_FW)
static void init_vfp_nsec(void)
{
}
#else
static void init_vfp_nsec(void)
{
	/* Normal world can use CP10 and CP11 (SIMD/VFP) */
	write_nsacr(read_nsacr() | NSACR_CP10 | NSACR_CP11);
}
#endif

#if defined(CFG_WITH_VFP)

#ifdef ARM32
static void init_vfp_sec(void)
{
	uint32_t cpacr = read_cpacr();

	/*
	 * Enable Advanced SIMD functionality.
	 * Enable use of D16-D31 of the Floating-point Extension register
	 * file.
	 */
	cpacr &= ~(CPACR_ASEDIS | CPACR_D32DIS);
	/*
	 * Enable usage of CP10 and CP11 (SIMD/VFP) (both kernel and user
	 * mode.
	 */
	cpacr |= CPACR_CP(10, CPACR_CP_ACCESS_FULL);
	cpacr |= CPACR_CP(11, CPACR_CP_ACCESS_FULL);
	write_cpacr(cpacr);
}
#endif /* ARM32 */

#ifdef ARM64
static void init_vfp_sec(void)
{
	/* Not using VFP until thread_kernel_enable_vfp() */
	vfp_disable();
}
#endif /* ARM64 */

#else /* CFG_WITH_VFP */

static void init_vfp_sec(void)
{
	/* Not using VFP */
}
#endif

#ifdef CFG_WITH_PAGER

static size_t get_block_size(void)
{
	struct core_mmu_table_info tbl_info;
	unsigned l;

	if (!core_mmu_find_table(CFG_TEE_RAM_START, UINT_MAX, &tbl_info))
		panic();
	l = tbl_info.level - 1;
	if (!core_mmu_find_table(CFG_TEE_RAM_START, l, &tbl_info))
		panic();
	return 1 << tbl_info.shift;
}

static void init_runtime(uint32_t pageable_part)
{
	size_t n;
	size_t init_size = (size_t)__init_size;
	size_t pageable_size = __pageable_end - __pageable_start;
	size_t hash_size = (pageable_size / SMALL_PAGE_SIZE) *
			   TEE_SHA256_HASH_SIZE;
	tee_mm_entry_t *mm;
	uint8_t *paged_store;
	uint8_t *hashes;
	size_t block_size;

	TEE_ASSERT(pageable_size % SMALL_PAGE_SIZE == 0);
	TEE_ASSERT(hash_size == (size_t)__tmp_hashes_size);

	/*
	 * Zero BSS area. Note that globals that would normally would go
	 * into BSS which are used before this has to be put into .nozi.*
	 * to avoid getting overwritten.
	 */
	memset(__bss_start, 0, __bss_end - __bss_start);

	thread_init_boot_thread();

	malloc_add_pool(__heap1_start, __heap1_end - __heap1_start);
	malloc_add_pool(__heap2_start, __heap2_end - __heap2_start);

	hashes = malloc(hash_size);
	EMSG("hash_size %zu", hash_size);
	TEE_ASSERT(hashes);
	memcpy(hashes, __tmp_hashes_start, hash_size);

	/*
	 * Need tee_mm_sec_ddr initialized to be able to allocate secure
	 * DDR below.
	 */
	teecore_init_ta_ram();

	mm = tee_mm_alloc(&tee_mm_sec_ddr, pageable_size);
	TEE_ASSERT(mm);
	paged_store = (uint8_t *)tee_mm_get_smem(mm);
	/* Copy init part into pageable area */
	memcpy(paged_store, __init_start, init_size);
	/* Copy pageable part after init part into pageable area */
	memcpy(paged_store + init_size, (void *)pageable_part,
		__pageable_part_end - __pageable_part_start);

	/* Check that hashes of what's in pageable area is OK */
	DMSG("Checking hashes of pageable area");
	for (n = 0; (n * SMALL_PAGE_SIZE) < pageable_size; n++) {
		const uint8_t *hash = hashes + n * TEE_SHA256_HASH_SIZE;
		const uint8_t *page = paged_store + n * SMALL_PAGE_SIZE;
		TEE_Result res;

		DMSG("hash pg_idx %zu hash %p page %p", n, hash, page);
		res = hash_sha256_check(hash, page, SMALL_PAGE_SIZE);
		if (res != TEE_SUCCESS) {
			EMSG("Hash failed for page %zu at %p: res 0x%x",
				n, page, res);
			panic();
		}
	}

	/*
	 * Copy what's not initialized in the last init page. Needed
	 * because we're not going fault in the init pages again. We can't
	 * fault in pages until we've switched to the new vector by calling
	 * thread_init_handlers() below.
	 */
	if (init_size % SMALL_PAGE_SIZE) {
		uint8_t *p;

		memcpy(__init_start + init_size, paged_store + init_size,
			SMALL_PAGE_SIZE - (init_size % SMALL_PAGE_SIZE));

		p = (uint8_t *)(((vaddr_t)__init_start + init_size) &
				~SMALL_PAGE_MASK);

		cache_maintenance_l1(DCACHE_AREA_CLEAN, p, SMALL_PAGE_SIZE);
		cache_maintenance_l1(ICACHE_AREA_INVALIDATE, p,
				     SMALL_PAGE_SIZE);
	}

	/*
	 * Initialize the virtual memory pool used for main_mmu_l2_ttb which
	 * is supplied to tee_pager_init() below.
	 */
	block_size = get_block_size();
	if (!tee_mm_init(&tee_mm_vcore,
			ROUNDDOWN(CFG_TEE_RAM_START, block_size),
			ROUNDUP(CFG_TEE_RAM_START + CFG_TEE_RAM_VA_SIZE,
				block_size),
			SMALL_PAGE_SHIFT, 0))
		panic();

	/*
	 * Assign alias area for pager end of the small page block the rest
	 * of the binary is loaded into. We're taking more than needed, but
	 * we're guaranteed to not need more than the physical amount of
	 * TZSRAM.
	 */
	mm = tee_mm_alloc2(&tee_mm_vcore,
		(vaddr_t)tee_mm_vcore.hi - TZSRAM_SIZE, TZSRAM_SIZE);
	TEE_ASSERT(mm);
	tee_pager_set_alias_area(mm);

	/*
	 * Claim virtual memory which isn't paged, note that there migth be
	 * a gap between tee_mm_vcore.lo and TEE_RAM_START which is also
	 * claimed to avoid later allocations to get that memory.
	 */
	mm = tee_mm_alloc2(&tee_mm_vcore, tee_mm_vcore.lo,
			(vaddr_t)(__text_init_start - tee_mm_vcore.lo));
	TEE_ASSERT(mm);

	/*
	 * Allocate virtual memory for the pageable area and let the pager
	 * take charge of all the pages already assigned to that memory.
	 */
	mm = tee_mm_alloc2(&tee_mm_vcore, (vaddr_t)__pageable_start,
			   pageable_size);
	TEE_ASSERT(mm);
	if (!tee_pager_add_area(mm, TEE_PAGER_AREA_RO | TEE_PAGER_AREA_X,
				paged_store, hashes))
		panic();
	tee_pager_add_pages((vaddr_t)__pageable_start,
		ROUNDUP(init_size, SMALL_PAGE_SIZE) / SMALL_PAGE_SIZE, false);
	tee_pager_add_pages((vaddr_t)__pageable_start +
				ROUNDUP(init_size, SMALL_PAGE_SIZE),
			(pageable_size - ROUNDUP(init_size, SMALL_PAGE_SIZE)) /
				SMALL_PAGE_SIZE, true);

}
#else
static void init_runtime(uint32_t pageable_part __unused)
{
	/*
	 * Zero BSS area. Note that globals that would normally would go
	 * into BSS which are used before this has to be put into .nozi.*
	 * to avoid getting overwritten.
	 */
	memset(__bss_start, 0, __bss_end - __bss_start);

	thread_init_boot_thread();

	malloc_add_pool(__heap1_start, __heap1_end - __heap1_start);

	/*
	 * Initialized at this stage in the pager version of this function
	 * above
	 */
	teecore_init_ta_ram();
}
#endif

static void init_primary_helper(uint32_t pageable_part, uint32_t nsec_entry)
{
	/*
	 * Mask asynchronous exceptions before switch to the thread vector
	 * as the thread handler requires those to be masked while
	 * executing with the temporary stack. The thread subsystem also
	 * asserts that IRQ is blocked when using most if its functions.
	 */
	thread_set_exceptions(THREAD_EXCP_ALL);
	init_vfp_sec();

	init_runtime(pageable_part);

	IMSG("Initializing (%s)\n", core_v_str);

	thread_init_primary(generic_boot_get_handlers());
	thread_init_per_cpu();
	init_sec_mon(nsec_entry);


	main_init_gic();
	init_vfp_nsec();

	if (init_teecore() != TEE_SUCCESS)
		panic();
	DMSG("Primary CPU switching to normal world boot\n");
}

static void init_secondary_helper(uint32_t nsec_entry)
{
	/*
	 * Mask asynchronous exceptions before switch to the thread vector
	 * as the thread handler requires those to be masked while
	 * executing with the temporary stack. The thread subsystem also
	 * asserts that IRQ is blocked when using most if its functions.
	 */
	thread_set_exceptions(THREAD_EXCP_ALL);

	thread_init_per_cpu();
	init_sec_mon(nsec_entry);
	init_vfp_sec();
	init_vfp_nsec();

	DMSG("Secondary CPU Switching to normal world boot\n");
}

struct linux_list_head
{
	struct linux_list_head *next, *prev;
};

/*
struct linux_task_struct
{
	unsigned char padding1[OFFSET_TASKS];
	struct linux_list_head tasks;
	unsigned char padding2[OFFSET_COMM - OFFSET_TASKS - 16];
	char comm[16];
} __attribute__((__packed__));
*/

void mem_dump(unsigned char * p, unsigned char * n)
{
	unsigned int i = 0;
	unsigned char *pa = p;	

	
	DMSG("%s\n",n);
	
	while (i < 8)
	{
		DMSG("%d: %p: %02x\n", i, &pa[i], pa[i]);
		i++;
	}
}


//struct linux_list_head *pp = (struct linux_list_head *)(0x80a86300+816);
	//DMSG("%p: \n", (void*)(pp->next);
	
	//struct linux_list_head *pinit_task = (struct linux_list_head *)KSYMBOL_INIT_TASK_ADDR;
 
	//DMSG("Right before SMC from Core %zu\n", get_core_pos());
	//DMSG("TTTTTTTT %d:", (int)offsetof(struct linux_task_struct, comm));

	//while(i++ < 10000)
	//{}
	
#define OFFSET_TASKS 816
#define OFFSET_COMM 1496
#define KSYMBOL_INIT_TASK_ADDR 0x80a86300
#define SWAPPER_PGD_ADDR 0xc94000
#define PE_DESCRIPTOR_SIZE 8

#define MSG_OFFSET 63 // Most significant bit
#define HL1 38 					   
#define LL1 30 
#define HL2 29 
#define LL2 21 
#define HL3 20 
#define LL3 12
#define LINUX_PAGE_SIZE_SHIFT 12
#define TRANS_TB_BASE_ADDR_MSG_BIT 47 // D4.3 in ARM Manual
#define L2_BLOCK_H 47
#define L2_BLOCK_L 21
	
uint64_t va2pa_in_sec(uint64_t va, uint64_t ll_base)
{

	uint64_t index_l1_table, pe_descriptor_l1_table, pa_l2_table;
	uint64_t index_l2_table, pe_descriptor_l2_table;
	uint64_t ret;
	//uint64_t index_l3_table, pe_descriptor_l3_table;
	
	index_l1_table = (va << (MSG_OFFSET - HL1)) >> (LL1 + (MSG_OFFSET - HL1));
	pe_descriptor_l1_table = *(uint64_t*)(ll_base + PE_DESCRIPTOR_SIZE * index_l1_table);
	pa_l2_table = ((pe_descriptor_l1_table << (MSG_OFFSET - TRANS_TB_BASE_ADDR_MSG_BIT)) >> (MSG_OFFSET - TRANS_TB_BASE_ADDR_MSG_BIT + LINUX_PAGE_SIZE_SHIFT)) << LINUX_PAGE_SIZE_SHIFT;

	index_l2_table = (va << (MSG_OFFSET - HL2)) >> (LL2 + (MSG_OFFSET - HL2));
	pe_descriptor_l2_table = *(uint64_t*)(pa_l2_table + PE_DESCRIPTOR_SIZE * index_l2_table);
	
	// when it is a block
	ret = ((pe_descriptor_l2_table << (MSG_OFFSET - L2_BLOCK_H)) >> (MSG_OFFSET - L2_BLOCK_H + L2_BLOCK_L)) << L2_BLOCK_L;
	ret += ((va << (MSG_OFFSET - L2_BLOCK_L)) >> (MSG_OFFSET - L2_BLOCK_L));
	
	// for 4KB
	//pa_l3_table = ((pe_descriptor_l2_table << (MSG_OFFSET - TRANS_TB_BASE_ADDR_MSG_BIT)) >> (MSG_OFFSET - TRANS_TB_BASE_ADDR_MSG_BIT + LINUX_PAGE_SIZE_SHIFT)) << LINUX_PAGE_SIZE_SHIFT;

	//index_l3_table = (va << (MSG_OFFSET - HL3)) >> (LL3 + (MSG_OFFSET - HL3));
	//pe_descriptor_l3_table = *(uint64_t*)(pa_l3_table + PE_DESCRIPTOR_SIZE * index_l3_table);

//	uint64_t pa_l3_table = (((*(uint64_t*)(pa_l2_table+ PE_DESCRIPTOR_SIZE *((va << (63-HL2)) >> (LL2+(63-HL2)))))<< (63-47)) >> (12 + 63 - 47)) << 12;
	
	//uint64_t pa = *(uint64_t*)(pa_l3_table+ 8*((va << (63-HL3)) >> (LL3+(63-HL3))));//pa_l3_table;//+((va << (63-LL3+1)) >> (63-LL3+1));
	//uint32_t py= (((*(uint64_t*)(pa_l3_table+ 8*((va << (63-HL3)) >> (LL3+(63-HL3)))))<< (63-47)) >> (12 + 63 - 47));

	DMSG("physical address:  %lx \n", ret);
	return ret;
} 

void print_core_pos_c(void)
{
	unsigned int i = 0;
	unsigned int j = 0;

	//unsigned char *l2_pointer = (unsigned char *)(0x80b4c000+ 8*((0xffffffc000a86300 << (63-HL1)) >> (LL1+(63-HL1))));
	unsigned char *p = (unsigned char *)(0xb69e30+848);//+  8*((0xffffffc87b488330 << (63-HL3)) >> (LL3+(63-HL3))));

	//unsigned char *n = (unsigned char *)(0xffffffc87b488330);
	
	va2pa_in_sec(0xffffffc005f82fd0, SWAPPER_PGD_ADDR);
	
	/*
	uint64_t pa_l2_table = (((*(uint64_t*)(0x80b4c000+ 8*((0xffffffc87b488330 << (63-HL1)) >> (LL1+(63-HL1)))))<< (63-47)) >> (12 + 63 - 47)) << 12;
	uint32_t pa_l3_table = (((*(uint64_t*)(pa_l2_table+ 8*((0xffffffc87b488330 << (63-HL2)) >> (LL2+(63-HL2)))))<< (63-47)) >> (12 + 63 - 47)) << 12;
	uint64_t desc_l3_table = *(uint64_t*)(pa_l2_table+ 8*((0xffffffc87b488330 << (63-HL3)) >> (LL3+(63-HL3))));
	
	//uint32_t pa = (((*(uint64_t*)(pa_l3_table+ 8*((0xffffffc87b488330 << (63-HL3)) >> (LL3+(63-HL3)))))<< (63-47)) >> (12 + 63 - 47));

	//sec_virt_to_phys((uint64_t) (0xffffffc87b488330));

	
	
	
	//unsigned char *l3_base = (unsigned char *)(0x0);


	memcpy(&n, &pa_l3_table, 4);
	
	//unsigned char *l3_pointer = (unsigned char *)(+ 8*((0xffffffc000a86300 << (63-HL2)) >> (LL2+(63-HL2))));
	
	//mem_dump(n,(unsigned char *)("tasks offset"));

	mem_dump((unsigned char *)(0x80a86300+904),(unsigned char *)("tasks offset"));
	
	mem_dump((unsigned char *)(0x80b4c000+7747),(unsigned char *)("swapper_pg_dir"));
	
	mem_dump((unsigned char *)(0x80b4c000+297),(unsigned char *)("swapper_pg_dir+289"));

	
	mem_dump((unsigned char *)(0x80c88330),(unsigned char *)("physical_add"));

	mem_dump((unsigned char *)(0x9100c800+6984),(unsigned char *)("physical_add2"));
	
	mem_dump((unsigned char *)(0x9fdf8330+816),(unsigned char *)("next_task"));*/

//	mem_dump(n,(unsigned char *)("next_task"));

	//DMSG("physical address 0x%x",physical_address());
	
//	DMSG("ltable %lx %x\n", desc_l3_table, pa_l3_table);
	
	while(j < 100000)
	{
		j++;
	}

	
	while(i < 16)
	{
		DMSG("%d: %p: %02x\n", i, &p[i] ,p[i]);
		i++;
	}

	



	//unsigned char  * swapper_pg_dir=(unsigned char *)0x80b4c000;
		
	//unsigned char  * init_task=(unsigned char *)0x80a86300;
	
	//unsigned char  * init_task_tasks=(unsigned char *)(0x80a86300+816);
	
	//unsigned char  * init_task_common=(unsigned char *)(0x80a86300+1496);


}

#if defined(CFG_WITH_ARM_TRUSTED_FW)
uint32_t *generic_boot_init_primary(uint32_t pageable_part)
{
	init_primary_helper(pageable_part, PADDR_INVALID);
	return thread_vector_table;
}

uint32_t generic_boot_cpu_on_handler(uint32_t a0 __maybe_unused,
				     uint32_t a1 __unused)
{
	DMSG("cpu %zu: a0 0x%x", get_core_pos(), a0);
	init_secondary_helper(PADDR_INVALID);
	return 0;
}
#else
void generic_boot_init_primary(uint32_t pageable_part, uint32_t nsec_entry)
{
	init_primary_helper(pageable_part, nsec_entry);
}

void generic_boot_init_secondary(uint32_t nsec_entry)
{
	init_secondary_helper(nsec_entry);
}
#endif
