/*
 * ARMv8 PMUv3 Performance Events handling code.
 *
 * Copyright (C) 2012 ARM Limited
 * Author: Will Deacon <will.deacon@arm.com>
 *
 * This code is based heavily on the ARMv7 perf event code.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <asm/irq_regs.h>
#include <asm/perf_event.h>
#include <asm/sysreg.h>
#include <asm/virt.h>

#include <linux/acpi.h>
#include <linux/clocksource.h>
#include <linux/kvm_host.h>
#include <linux/of.h>
#include <linux/perf/arm_pmu.h>
#include <linux/platform_device.h>
#include <linux/sched/clock.h>

/* ARMv8 Cortex-A53 specific event types. */
#define ARMV8_A53_PERFCTR_PREF_LINEFILL				0xC2

/* ARMv8 Cavium ThunderX specific event types. */
#define ARMV8_THUNDER_PERFCTR_L1D_CACHE_MISS_ST			0xE9
#define ARMV8_THUNDER_PERFCTR_L1D_CACHE_PREF_ACCESS		0xEA
#define ARMV8_THUNDER_PERFCTR_L1D_CACHE_PREF_MISS		0xEB
#define ARMV8_THUNDER_PERFCTR_L1I_CACHE_PREF_ACCESS		0xEC
#define ARMV8_THUNDER_PERFCTR_L1I_CACHE_PREF_MISS		0xED

/*
 * ARMv8 Architectural defined events, not all of these may
 * be supported on any given implementation. Unsupported events will
 * be disabled at run-time based on the PMCEID registers.
 */
static const unsigned armv8_pmuv3_perf_map[PERF_COUNT_HW_MAX] = {
	PERF_MAP_ALL_UNSUPPORTED,
	[PERF_COUNT_HW_CPU_CYCLES]		= ARMV8_PMUV3_PERFCTR_CPU_CYCLES,
	[PERF_COUNT_HW_INSTRUCTIONS]		= ARMV8_PMUV3_PERFCTR_INST_RETIRED,
	[PERF_COUNT_HW_CACHE_REFERENCES]	= ARMV8_PMUV3_PERFCTR_L1D_CACHE,
	[PERF_COUNT_HW_CACHE_MISSES]		= ARMV8_PMUV3_PERFCTR_L1D_CACHE_REFILL,
	[PERF_COUNT_HW_BRANCH_INSTRUCTIONS]	= ARMV8_PMUV3_PERFCTR_PC_WRITE_RETIRED,
	[PERF_COUNT_HW_BRANCH_MISSES]		= ARMV8_PMUV3_PERFCTR_BR_MIS_PRED,
	[PERF_COUNT_HW_BUS_CYCLES]		= ARMV8_PMUV3_PERFCTR_BUS_CYCLES,
	[PERF_COUNT_HW_STALLED_CYCLES_FRONTEND]	= ARMV8_PMUV3_PERFCTR_STALL_FRONTEND,
	[PERF_COUNT_HW_STALLED_CYCLES_BACKEND]	= ARMV8_PMUV3_PERFCTR_STALL_BACKEND,
};

static const unsigned armv8_pmuv3_perf_cache_map[PERF_COUNT_HW_CACHE_MAX]
						[PERF_COUNT_HW_CACHE_OP_MAX]
						[PERF_COUNT_HW_CACHE_RESULT_MAX] = {
	PERF_CACHE_MAP_ALL_UNSUPPORTED,

	[C(L1D)][C(OP_READ)][C(RESULT_ACCESS)]	= ARMV8_PMUV3_PERFCTR_L1D_CACHE,
	[C(L1D)][C(OP_READ)][C(RESULT_MISS)]	= ARMV8_PMUV3_PERFCTR_L1D_CACHE_REFILL,

	[C(L1I)][C(OP_READ)][C(RESULT_ACCESS)]	= ARMV8_PMUV3_PERFCTR_L1I_CACHE,
	[C(L1I)][C(OP_READ)][C(RESULT_MISS)]	= ARMV8_PMUV3_PERFCTR_L1I_CACHE_REFILL,

	[C(DTLB)][C(OP_READ)][C(RESULT_MISS)]	= ARMV8_PMUV3_PERFCTR_L1D_TLB_REFILL,
	[C(DTLB)][C(OP_READ)][C(RESULT_ACCESS)]	= ARMV8_PMUV3_PERFCTR_L1D_TLB,

	[C(ITLB)][C(OP_READ)][C(RESULT_MISS)]	= ARMV8_PMUV3_PERFCTR_L1I_TLB_REFILL,
	[C(ITLB)][C(OP_READ)][C(RESULT_ACCESS)]	= ARMV8_PMUV3_PERFCTR_L1I_TLB,

	[C(BPU)][C(OP_READ)][C(RESULT_ACCESS)]	= ARMV8_PMUV3_PERFCTR_BR_PRED,
	[C(BPU)][C(OP_READ)][C(RESULT_MISS)]	= ARMV8_PMUV3_PERFCTR_BR_MIS_PRED,
};

static const unsigned armv8_a53_perf_cache_map[PERF_COUNT_HW_CACHE_MAX]
					      [PERF_COUNT_HW_CACHE_OP_MAX]
					      [PERF_COUNT_HW_CACHE_RESULT_MAX] = {
	PERF_CACHE_MAP_ALL_UNSUPPORTED,

	[C(L1D)][C(OP_PREFETCH)][C(RESULT_MISS)] = ARMV8_A53_PERFCTR_PREF_LINEFILL,

	[C(NODE)][C(OP_READ)][C(RESULT_ACCESS)]	= ARMV8_IMPDEF_PERFCTR_BUS_ACCESS_RD,
	[C(NODE)][C(OP_WRITE)][C(RESULT_ACCESS)] = ARMV8_IMPDEF_PERFCTR_BUS_ACCESS_WR,
};

static const unsigned armv8_a57_perf_cache_map[PERF_COUNT_HW_CACHE_MAX]
					      [PERF_COUNT_HW_CACHE_OP_MAX]
					      [PERF_COUNT_HW_CACHE_RESULT_MAX] = {
	PERF_CACHE_MAP_ALL_UNSUPPORTED,

	[C(L1D)][C(OP_READ)][C(RESULT_ACCESS)]	= ARMV8_IMPDEF_PERFCTR_L1D_CACHE_RD,
	[C(L1D)][C(OP_READ)][C(RESULT_MISS)]	= ARMV8_IMPDEF_PERFCTR_L1D_CACHE_REFILL_RD,
	[C(L1D)][C(OP_WRITE)][C(RESULT_ACCESS)]	= ARMV8_IMPDEF_PERFCTR_L1D_CACHE_WR,
	[C(L1D)][C(OP_WRITE)][C(RESULT_MISS)]	= ARMV8_IMPDEF_PERFCTR_L1D_CACHE_REFILL_WR,

	[C(DTLB)][C(OP_READ)][C(RESULT_MISS)]	= ARMV8_IMPDEF_PERFCTR_L1D_TLB_REFILL_RD,
	[C(DTLB)][C(OP_WRITE)][C(RESULT_MISS)]	= ARMV8_IMPDEF_PERFCTR_L1D_TLB_REFILL_WR,

	[C(NODE)][C(OP_READ)][C(RESULT_ACCESS)]	= ARMV8_IMPDEF_PERFCTR_BUS_ACCESS_RD,
	[C(NODE)][C(OP_WRITE)][C(RESULT_ACCESS)] = ARMV8_IMPDEF_PERFCTR_BUS_ACCESS_WR,
};

static const unsigned armv8_a73_perf_cache_map[PERF_COUNT_HW_CACHE_MAX]
					      [PERF_COUNT_HW_CACHE_OP_MAX]
					      [PERF_COUNT_HW_CACHE_RESULT_MAX] = {
	PERF_CACHE_MAP_ALL_UNSUPPORTED,

	[C(L1D)][C(OP_READ)][C(RESULT_ACCESS)]	= ARMV8_IMPDEF_PERFCTR_L1D_CACHE_RD,
	[C(L1D)][C(OP_WRITE)][C(RESULT_ACCESS)]	= ARMV8_IMPDEF_PERFCTR_L1D_CACHE_WR,
};

static const unsigned armv8_thunder_perf_cache_map[PERF_COUNT_HW_CACHE_MAX]
						   [PERF_COUNT_HW_CACHE_OP_MAX]
						   [PERF_COUNT_HW_CACHE_RESULT_MAX] = {
	PERF_CACHE_MAP_ALL_UNSUPPORTED,

	[C(L1D)][C(OP_READ)][C(RESULT_ACCESS)]	= ARMV8_IMPDEF_PERFCTR_L1D_CACHE_RD,
	[C(L1D)][C(OP_READ)][C(RESULT_MISS)]	= ARMV8_IMPDEF_PERFCTR_L1D_CACHE_REFILL_RD,
	[C(L1D)][C(OP_WRITE)][C(RESULT_ACCESS)]	= ARMV8_IMPDEF_PERFCTR_L1D_CACHE_WR,
	[C(L1D)][C(OP_WRITE)][C(RESULT_MISS)]	= ARMV8_THUNDER_PERFCTR_L1D_CACHE_MISS_ST,
	[C(L1D)][C(OP_PREFETCH)][C(RESULT_ACCESS)] = ARMV8_THUNDER_PERFCTR_L1D_CACHE_PREF_ACCESS,
	[C(L1D)][C(OP_PREFETCH)][C(RESULT_MISS)] = ARMV8_THUNDER_PERFCTR_L1D_CACHE_PREF_MISS,

	[C(L1I)][C(OP_PREFETCH)][C(RESULT_ACCESS)] = ARMV8_THUNDER_PERFCTR_L1I_CACHE_PREF_ACCESS,
	[C(L1I)][C(OP_PREFETCH)][C(RESULT_MISS)] = ARMV8_THUNDER_PERFCTR_L1I_CACHE_PREF_MISS,

	[C(DTLB)][C(OP_READ)][C(RESULT_ACCESS)]	= ARMV8_IMPDEF_PERFCTR_L1D_TLB_RD,
	[C(DTLB)][C(OP_READ)][C(RESULT_MISS)]	= ARMV8_IMPDEF_PERFCTR_L1D_TLB_REFILL_RD,
	[C(DTLB)][C(OP_WRITE)][C(RESULT_ACCESS)] = ARMV8_IMPDEF_PERFCTR_L1D_TLB_WR,
	[C(DTLB)][C(OP_WRITE)][C(RESULT_MISS)]	= ARMV8_IMPDEF_PERFCTR_L1D_TLB_REFILL_WR,
};

static const unsigned armv8_vulcan_perf_cache_map[PERF_COUNT_HW_CACHE_MAX]
					      [PERF_COUNT_HW_CACHE_OP_MAX]
					      [PERF_COUNT_HW_CACHE_RESULT_MAX] = {
	PERF_CACHE_MAP_ALL_UNSUPPORTED,

	[C(L1D)][C(OP_READ)][C(RESULT_ACCESS)]	= ARMV8_IMPDEF_PERFCTR_L1D_CACHE_RD,
	[C(L1D)][C(OP_READ)][C(RESULT_MISS)]	= ARMV8_IMPDEF_PERFCTR_L1D_CACHE_REFILL_RD,
	[C(L1D)][C(OP_WRITE)][C(RESULT_ACCESS)]	= ARMV8_IMPDEF_PERFCTR_L1D_CACHE_WR,
	[C(L1D)][C(OP_WRITE)][C(RESULT_MISS)]	= ARMV8_IMPDEF_PERFCTR_L1D_CACHE_REFILL_WR,

	[C(DTLB)][C(OP_READ)][C(RESULT_ACCESS)]	= ARMV8_IMPDEF_PERFCTR_L1D_TLB_RD,
	[C(DTLB)][C(OP_WRITE)][C(RESULT_ACCESS)] = ARMV8_IMPDEF_PERFCTR_L1D_TLB_WR,
	[C(DTLB)][C(OP_READ)][C(RESULT_MISS)]	= ARMV8_IMPDEF_PERFCTR_L1D_TLB_REFILL_RD,
	[C(DTLB)][C(OP_WRITE)][C(RESULT_MISS)]	= ARMV8_IMPDEF_PERFCTR_L1D_TLB_REFILL_WR,

	[C(NODE)][C(OP_READ)][C(RESULT_ACCESS)]	= ARMV8_IMPDEF_PERFCTR_BUS_ACCESS_RD,
	[C(NODE)][C(OP_WRITE)][C(RESULT_ACCESS)] = ARMV8_IMPDEF_PERFCTR_BUS_ACCESS_WR,
};

static ssize_t
armv8pmu_events_sysfs_show(struct device *dev,
			   struct device_attribute *attr, char *page)
{
	struct perf_pmu_events_attr *pmu_attr;

	pmu_attr = container_of(attr, struct perf_pmu_events_attr, attr);

	return sprintf(page, "event=0x%03llx\n", pmu_attr->id);
}

#define ARMV8_EVENT_ATTR_RESOLVE(m) #m
#define ARMV8_EVENT_ATTR(name, config) \
	PMU_EVENT_ATTR(name, armv8_event_attr_##name, \
		       config, armv8pmu_events_sysfs_show)

ARMV8_EVENT_ATTR(sw_incr, ARMV8_PMUV3_PERFCTR_SW_INCR);
ARMV8_EVENT_ATTR(l1i_cache_refill, ARMV8_PMUV3_PERFCTR_L1I_CACHE_REFILL);
ARMV8_EVENT_ATTR(l1i_tlb_refill, ARMV8_PMUV3_PERFCTR_L1I_TLB_REFILL);
ARMV8_EVENT_ATTR(l1d_cache_refill, ARMV8_PMUV3_PERFCTR_L1D_CACHE_REFILL);
ARMV8_EVENT_ATTR(l1d_cache, ARMV8_PMUV3_PERFCTR_L1D_CACHE);
ARMV8_EVENT_ATTR(l1d_tlb_refill, ARMV8_PMUV3_PERFCTR_L1D_TLB_REFILL);
ARMV8_EVENT_ATTR(ld_retired, ARMV8_PMUV3_PERFCTR_LD_RETIRED);
ARMV8_EVENT_ATTR(st_retired, ARMV8_PMUV3_PERFCTR_ST_RETIRED);
ARMV8_EVENT_ATTR(inst_retired, ARMV8_PMUV3_PERFCTR_INST_RETIRED);
ARMV8_EVENT_ATTR(exc_taken, ARMV8_PMUV3_PERFCTR_EXC_TAKEN);
ARMV8_EVENT_ATTR(exc_return, ARMV8_PMUV3_PERFCTR_EXC_RETURN);
ARMV8_EVENT_ATTR(cid_write_retired, ARMV8_PMUV3_PERFCTR_CID_WRITE_RETIRED);
ARMV8_EVENT_ATTR(pc_write_retired, ARMV8_PMUV3_PERFCTR_PC_WRITE_RETIRED);
ARMV8_EVENT_ATTR(br_immed_retired, ARMV8_PMUV3_PERFCTR_BR_IMMED_RETIRED);
ARMV8_EVENT_ATTR(br_return_retired, ARMV8_PMUV3_PERFCTR_BR_RETURN_RETIRED);
ARMV8_EVENT_ATTR(unaligned_ldst_retired, ARMV8_PMUV3_PERFCTR_UNALIGNED_LDST_RETIRED);
ARMV8_EVENT_ATTR(br_mis_pred, ARMV8_PMUV3_PERFCTR_BR_MIS_PRED);
ARMV8_EVENT_ATTR(cpu_cycles, ARMV8_PMUV3_PERFCTR_CPU_CYCLES);
ARMV8_EVENT_ATTR(br_pred, ARMV8_PMUV3_PERFCTR_BR_PRED);
ARMV8_EVENT_ATTR(mem_access, ARMV8_PMUV3_PERFCTR_MEM_ACCESS);
ARMV8_EVENT_ATTR(l1i_cache, ARMV8_PMUV3_PERFCTR_L1I_CACHE);
ARMV8_EVENT_ATTR(l1d_cache_wb, ARMV8_PMUV3_PERFCTR_L1D_CACHE_WB);
ARMV8_EVENT_ATTR(l2d_cache, ARMV8_PMUV3_PERFCTR_L2D_CACHE);
ARMV8_EVENT_ATTR(l2d_cache_refill, ARMV8_PMUV3_PERFCTR_L2D_CACHE_REFILL);
ARMV8_EVENT_ATTR(l2d_cache_wb, ARMV8_PMUV3_PERFCTR_L2D_CACHE_WB);
ARMV8_EVENT_ATTR(bus_access, ARMV8_PMUV3_PERFCTR_BUS_ACCESS);
ARMV8_EVENT_ATTR(memory_error, ARMV8_PMUV3_PERFCTR_MEMORY_ERROR);
ARMV8_EVENT_ATTR(inst_spec, ARMV8_PMUV3_PERFCTR_INST_SPEC);
ARMV8_EVENT_ATTR(ttbr_write_retired, ARMV8_PMUV3_PERFCTR_TTBR_WRITE_RETIRED);
ARMV8_EVENT_ATTR(bus_cycles, ARMV8_PMUV3_PERFCTR_BUS_CYCLES);
/* Don't expose the chain event in /sys, since it's useless in isolation */
ARMV8_EVENT_ATTR(l1d_cache_allocate, ARMV8_PMUV3_PERFCTR_L1D_CACHE_ALLOCATE);
ARMV8_EVENT_ATTR(l2d_cache_allocate, ARMV8_PMUV3_PERFCTR_L2D_CACHE_ALLOCATE);
ARMV8_EVENT_ATTR(br_retired, ARMV8_PMUV3_PERFCTR_BR_RETIRED);
ARMV8_EVENT_ATTR(br_mis_pred_retired, ARMV8_PMUV3_PERFCTR_BR_MIS_PRED_RETIRED);
ARMV8_EVENT_ATTR(stall_frontend, ARMV8_PMUV3_PERFCTR_STALL_FRONTEND);
ARMV8_EVENT_ATTR(stall_backend, ARMV8_PMUV3_PERFCTR_STALL_BACKEND);
ARMV8_EVENT_ATTR(l1d_tlb, ARMV8_PMUV3_PERFCTR_L1D_TLB);
ARMV8_EVENT_ATTR(l1i_tlb, ARMV8_PMUV3_PERFCTR_L1I_TLB);
ARMV8_EVENT_ATTR(l2i_cache, ARMV8_PMUV3_PERFCTR_L2I_CACHE);
ARMV8_EVENT_ATTR(l2i_cache_refill, ARMV8_PMUV3_PERFCTR_L2I_CACHE_REFILL);
ARMV8_EVENT_ATTR(l3d_cache_allocate, ARMV8_PMUV3_PERFCTR_L3D_CACHE_ALLOCATE);
ARMV8_EVENT_ATTR(l3d_cache_refill, ARMV8_PMUV3_PERFCTR_L3D_CACHE_REFILL);
ARMV8_EVENT_ATTR(l3d_cache, ARMV8_PMUV3_PERFCTR_L3D_CACHE);
ARMV8_EVENT_ATTR(l3d_cache_wb, ARMV8_PMUV3_PERFCTR_L3D_CACHE_WB);
ARMV8_EVENT_ATTR(l2d_tlb_refill, ARMV8_PMUV3_PERFCTR_L2D_TLB_REFILL);
ARMV8_EVENT_ATTR(l2i_tlb_refill, ARMV8_PMUV3_PERFCTR_L2I_TLB_REFILL);
ARMV8_EVENT_ATTR(l2d_tlb, ARMV8_PMUV3_PERFCTR_L2D_TLB);
ARMV8_EVENT_ATTR(l2i_tlb, ARMV8_PMUV3_PERFCTR_L2I_TLB);
ARMV8_EVENT_ATTR(remote_access, ARMV8_PMUV3_PERFCTR_REMOTE_ACCESS);
ARMV8_EVENT_ATTR(ll_cache, ARMV8_PMUV3_PERFCTR_LL_CACHE);
ARMV8_EVENT_ATTR(ll_cache_miss, ARMV8_PMUV3_PERFCTR_LL_CACHE_MISS);
ARMV8_EVENT_ATTR(dtlb_walk, ARMV8_PMUV3_PERFCTR_DTLB_WALK);
ARMV8_EVENT_ATTR(itlb_walk, ARMV8_PMUV3_PERFCTR_ITLB_WALK);
ARMV8_EVENT_ATTR(ll_cache_rd, ARMV8_PMUV3_PERFCTR_LL_CACHE_RD);
ARMV8_EVENT_ATTR(ll_cache_miss_rd, ARMV8_PMUV3_PERFCTR_LL_CACHE_MISS_RD);
ARMV8_EVENT_ATTR(remote_access_rd, ARMV8_PMUV3_PERFCTR_REMOTE_ACCESS_RD);
ARMV8_EVENT_ATTR(sample_pop, ARMV8_SPE_PERFCTR_SAMPLE_POP);
ARMV8_EVENT_ATTR(sample_feed, ARMV8_SPE_PERFCTR_SAMPLE_FEED);
ARMV8_EVENT_ATTR(sample_filtrate, ARMV8_SPE_PERFCTR_SAMPLE_FILTRATE);
ARMV8_EVENT_ATTR(sample_collision, ARMV8_SPE_PERFCTR_SAMPLE_COLLISION);

static struct attribute *armv8_pmuv3_event_attrs[] = {
	&armv8_event_attr_sw_incr.attr.attr,
	&armv8_event_attr_l1i_cache_refill.attr.attr,
	&armv8_event_attr_l1i_tlb_refill.attr.attr,
	&armv8_event_attr_l1d_cache_refill.attr.attr,
	&armv8_event_attr_l1d_cache.attr.attr,
	&armv8_event_attr_l1d_tlb_refill.attr.attr,
	&armv8_event_attr_ld_retired.attr.attr,
	&armv8_event_attr_st_retired.attr.attr,
	&armv8_event_attr_inst_retired.attr.attr,
	&armv8_event_attr_exc_taken.attr.attr,
	&armv8_event_attr_exc_return.attr.attr,
	&armv8_event_attr_cid_write_retired.attr.attr,
	&armv8_event_attr_pc_write_retired.attr.attr,
	&armv8_event_attr_br_immed_retired.attr.attr,
	&armv8_event_attr_br_return_retired.attr.attr,
	&armv8_event_attr_unaligned_ldst_retired.attr.attr,
	&armv8_event_attr_br_mis_pred.attr.attr,
	&armv8_event_attr_cpu_cycles.attr.attr,
	&armv8_event_attr_br_pred.attr.attr,
	&armv8_event_attr_mem_access.attr.attr,
	&armv8_event_attr_l1i_cache.attr.attr,
	&armv8_event_attr_l1d_cache_wb.attr.attr,
	&armv8_event_attr_l2d_cache.attr.attr,
	&armv8_event_attr_l2d_cache_refill.attr.attr,
	&armv8_event_attr_l2d_cache_wb.attr.attr,
	&armv8_event_attr_bus_access.attr.attr,
	&armv8_event_attr_memory_error.attr.attr,
	&armv8_event_attr_inst_spec.attr.attr,
	&armv8_event_attr_ttbr_write_retired.attr.attr,
	&armv8_event_attr_bus_cycles.attr.attr,
	&armv8_event_attr_l1d_cache_allocate.attr.attr,
	&armv8_event_attr_l2d_cache_allocate.attr.attr,
	&armv8_event_attr_br_retired.attr.attr,
	&armv8_event_attr_br_mis_pred_retired.attr.attr,
	&armv8_event_attr_stall_frontend.attr.attr,
	&armv8_event_attr_stall_backend.attr.attr,
	&armv8_event_attr_l1d_tlb.attr.attr,
	&armv8_event_attr_l1i_tlb.attr.attr,
	&armv8_event_attr_l2i_cache.attr.attr,
	&armv8_event_attr_l2i_cache_refill.attr.attr,
	&armv8_event_attr_l3d_cache_allocate.attr.attr,
	&armv8_event_attr_l3d_cache_refill.attr.attr,
	&armv8_event_attr_l3d_cache.attr.attr,
	&armv8_event_attr_l3d_cache_wb.attr.attr,
	&armv8_event_attr_l2d_tlb_refill.attr.attr,
	&armv8_event_attr_l2i_tlb_refill.attr.attr,
	&armv8_event_attr_l2d_tlb.attr.attr,
	&armv8_event_attr_l2i_tlb.attr.attr,
	&armv8_event_attr_remote_access.attr.attr,
	&armv8_event_attr_ll_cache.attr.attr,
	&armv8_event_attr_ll_cache_miss.attr.attr,
	&armv8_event_attr_dtlb_walk.attr.attr,
	&armv8_event_attr_itlb_walk.attr.attr,
	&armv8_event_attr_ll_cache_rd.attr.attr,
	&armv8_event_attr_ll_cache_miss_rd.attr.attr,
	&armv8_event_attr_remote_access_rd.attr.attr,
	&armv8_event_attr_sample_pop.attr.attr,
	&armv8_event_attr_sample_feed.attr.attr,
	&armv8_event_attr_sample_filtrate.attr.attr,
	&armv8_event_attr_sample_collision.attr.attr,
	NULL,
};

static umode_t
armv8pmu_event_attr_is_visible(struct kobject *kobj,
			       struct attribute *attr, int unused)
{
	struct device *dev = kobj_to_dev(kobj);
	struct pmu *pmu = dev_get_drvdata(dev);
	struct arm_pmu *cpu_pmu = container_of(pmu, struct arm_pmu, pmu);
	struct perf_pmu_events_attr *pmu_attr;

	pmu_attr = container_of(attr, struct perf_pmu_events_attr, attr.attr);

	if (pmu_attr->id < ARMV8_PMUV3_MAX_COMMON_EVENTS &&
	    test_bit(pmu_attr->id, cpu_pmu->pmceid_bitmap))
		return attr->mode;

	pmu_attr->id -= ARMV8_PMUV3_EXT_COMMON_EVENT_BASE;
	if (pmu_attr->id < ARMV8_PMUV3_MAX_COMMON_EVENTS &&
	    test_bit(pmu_attr->id, cpu_pmu->pmceid_ext_bitmap))
		return attr->mode;

	return 0;
}

static struct attribute_group armv8_pmuv3_events_attr_group = {
	.name = "events",
	.attrs = armv8_pmuv3_event_attrs,
	.is_visible = armv8pmu_event_attr_is_visible,
};

PMU_FORMAT_ATTR(event, "config:0-15");
PMU_FORMAT_ATTR(long, "config1:0");

static inline bool armv8pmu_event_is_64bit(struct perf_event *event)
{
	return event->attr.config1 & 0x1;
}

static struct attribute *armv8_pmuv3_format_attrs[] = {
	&format_attr_event.attr,
	&format_attr_long.attr,
	NULL,
};

static struct attribute_group armv8_pmuv3_format_attr_group = {
	.name = "format",
	.attrs = armv8_pmuv3_format_attrs,
};

/*
 * Perf Events' indices
 */
#define	ARMV8_IDX_CYCLE_COUNTER	0
#define	ARMV8_IDX_COUNTER0	1
#define	ARMV8_IDX_COUNTER_LAST(cpu_pmu) \
	(ARMV8_IDX_CYCLE_COUNTER + cpu_pmu->num_events - 1)


/*
 * We unconditionally enable ARMv8.5-PMU long event counter support
 * (64-bit events) where supported. Indicate if this arm_pmu has long
 * event counter support.
 */
static bool armv8pmu_has_long_event(struct arm_pmu *cpu_pmu)
{
	return (cpu_pmu->pmuver > ID_AA64DFR0_PMUVER_8_5);
}

/*
 * We must chain two programmable counters for 64 bit events,
 * except when we have allocated the 64bit cycle counter (for CPU
 * cycles event). This must be called only when the event has
 * a counter allocated.
 */
static inline bool armv8pmu_event_is_chained(struct perf_event *event)
{
	int idx = event->hw.idx;
	struct arm_pmu *cpu_pmu = to_arm_pmu(event->pmu);

	return !WARN_ON(idx < 0) &&
	       armv8pmu_event_is_64bit(event) &&
	       !armv8pmu_has_long_event(cpu_pmu) &&
	       (idx != ARMV8_IDX_CYCLE_COUNTER);
}

/*
 * ARMv8 low level PMU access
 */

/*
 * Perf Event to low level counters mapping
 */
#define	ARMV8_IDX_TO_COUNTER(x)	\
	(((x) - ARMV8_IDX_COUNTER0) & ARMV8_PMU_COUNTER_MASK)

/*
 * This code is really good
 */

#define PMEVN_CASE(__n, case_macro) \
	case __n: case_macro(__n); break;

#define PMEVN_SWITCH(__x, case_macro)				\
	do {							\
		switch (__x) {					\
		PMEVN_CASE(0,  case_macro);			\
		PMEVN_CASE(1,  case_macro);			\
		PMEVN_CASE(2,  case_macro);			\
		PMEVN_CASE(3,  case_macro);			\
		PMEVN_CASE(4,  case_macro);			\
		PMEVN_CASE(5,  case_macro);			\
		PMEVN_CASE(6,  case_macro);			\
		PMEVN_CASE(7,  case_macro);			\
		PMEVN_CASE(8,  case_macro);			\
		PMEVN_CASE(9,  case_macro);			\
		PMEVN_CASE(10, case_macro);			\
		PMEVN_CASE(11, case_macro);			\
		PMEVN_CASE(12, case_macro);			\
		PMEVN_CASE(13, case_macro);			\
		PMEVN_CASE(14, case_macro);			\
		PMEVN_CASE(15, case_macro);			\
		PMEVN_CASE(16, case_macro);			\
		PMEVN_CASE(17, case_macro);			\
		PMEVN_CASE(18, case_macro);			\
		PMEVN_CASE(19, case_macro);			\
		PMEVN_CASE(20, case_macro);			\
		PMEVN_CASE(21, case_macro);			\
		PMEVN_CASE(22, case_macro);			\
		PMEVN_CASE(23, case_macro);			\
		PMEVN_CASE(24, case_macro);			\
		PMEVN_CASE(25, case_macro);			\
		PMEVN_CASE(26, case_macro);			\
		PMEVN_CASE(27, case_macro);			\
		PMEVN_CASE(28, case_macro);			\
		PMEVN_CASE(29, case_macro);			\
		PMEVN_CASE(30, case_macro);			\
		default: WARN(1, "Invalid PMEV* index %#x", __x);	\
		}						\
	} while (0)

#define RETURN_READ_PMEVCNTRN(n) \
	return read_sysreg(pmevcntr##n##_el0);
static unsigned long read_pmevcntrn(int n)
{
	PMEVN_SWITCH(n, RETURN_READ_PMEVCNTRN);
	return 0;
}
#undef RETURN_READ_PMEVCNTRN

#define WRITE_PMEVCNTRN(n) \
	write_sysreg(val, pmevcntr##n##_el0);
static void write_pmevcntrn(int n, unsigned long val)
{
	PMEVN_SWITCH(n, WRITE_PMEVCNTRN);
}
#undef WRITE_PMEVCNTRN

#define WRITE_PMEVTYPERN(n) \
	write_sysreg(val, pmevtyper##n##_el0);
static void write_pmevtypern(int n, unsigned long val)
{
	PMEVN_SWITCH(n, WRITE_PMEVTYPERN);
}
#undef WRITE_PMEVTYPERN

#undef GENERATE_PMEVN_SWITCH

static inline u32 armv8pmu_pmcr_read(void)
{
	return read_sysreg(pmcr_el0);
}

static inline void armv8pmu_pmcr_write(u32 val)
{
	val &= ARMV8_PMU_PMCR_MASK;
	isb();
	write_sysreg(val, pmcr_el0);
}

static inline int armv8pmu_has_overflowed(u32 pmovsr)
{
	return pmovsr & ARMV8_PMU_OVERFLOWED_MASK;
}

static inline int armv8pmu_counter_valid(struct arm_pmu *cpu_pmu, int idx)
{
	return idx >= ARMV8_IDX_CYCLE_COUNTER &&
		idx <= ARMV8_IDX_COUNTER_LAST(cpu_pmu);
}

static inline int armv8pmu_counter_has_overflowed(u32 pmnc, int idx)
{
	return pmnc & BIT(ARMV8_IDX_TO_COUNTER(idx));
}

static inline void armv8pmu_select_counter(int idx)
{
	u32 counter = ARMV8_IDX_TO_COUNTER(idx);
	write_sysreg(counter, pmselr_el0);
	isb();
}

static inline u64 armv8pmu_read_evcntr(int idx)
{
	u32 counter = ARMV8_IDX_TO_COUNTER(idx);

	if (pmu_nmi_enable)
		return read_pmevcntrn(counter);

	armv8pmu_select_counter(idx);
	return read_sysreg(pmxevcntr_el0);
}

static inline u64 armv8pmu_read_hw_counter(struct perf_event *event)
{
	int idx = event->hw.idx;
	u64 val = 0;

	val = armv8pmu_read_evcntr(idx);
	if (armv8pmu_event_is_chained(event))
		val = (val << 32) | armv8pmu_read_evcntr(idx - 1);
	return val;
}

/*
 * The cycle counter is always a 64-bit counter. When ARMV8_PMU_PMCR_LP
 * is set the event counters also become 64-bit counters. Unless the
 * user has requested a long counter (attr.config1) then we want to
 * interrupt upon 32-bit overflow - we achieve this by applying a bias.
 */
static bool armv8pmu_event_needs_bias(struct perf_event *event)
{
	struct arm_pmu *cpu_pmu = to_arm_pmu(event->pmu);
	struct hw_perf_event *hwc = &event->hw;
	int idx = hwc->idx;

	if (armv8pmu_event_is_64bit(event))
		return false;

	if (armv8pmu_has_long_event(cpu_pmu) ||
	    idx == ARMV8_IDX_CYCLE_COUNTER)
		return true;

	return false;
}

static u64 armv8pmu_bias_long_counter(struct perf_event *event, u64 value)
{
	if (armv8pmu_event_needs_bias(event))
		value |= GENMASK(63, 32);

	return value;
}

static u64 armv8pmu_unbias_long_counter(struct perf_event *event, u64 value)
{
	if (armv8pmu_event_needs_bias(event))
		value &= ~GENMASK(63, 32);

	return value;
}

static inline u64 armv8pmu_read_counter(struct perf_event *event)
{
	struct arm_pmu *cpu_pmu = to_arm_pmu(event->pmu);
	struct hw_perf_event *hwc = &event->hw;
	int idx = hwc->idx;
	u64 value = 0;

	if (!armv8pmu_counter_valid(cpu_pmu, idx))
		pr_err("CPU%u reading wrong counter %d\n",
			smp_processor_id(), idx);
	else if (idx == ARMV8_IDX_CYCLE_COUNTER)
		value = read_sysreg(pmccntr_el0);
	else
		value = armv8pmu_read_hw_counter(event);

	return armv8pmu_unbias_long_counter(event, value);
}

static inline void armv8pmu_write_evcntr(int idx, u64 value)
{
	u32 counter = ARMV8_IDX_TO_COUNTER(idx);

	if (pmu_nmi_enable) {
		write_pmevcntrn(counter, value);
	} else {
		armv8pmu_select_counter(idx);
		write_sysreg(value, pmxevcntr_el0);
	}
}

static inline void armv8pmu_write_hw_counter(struct perf_event *event,
					     u64 value)
{
	int idx = event->hw.idx;

	if (armv8pmu_event_is_chained(event)) {
		armv8pmu_write_evcntr(idx, upper_32_bits(value));
		armv8pmu_write_evcntr(idx - 1, lower_32_bits(value));
	} else {
		armv8pmu_write_evcntr(idx, value);
	}
}

static inline void armv8pmu_write_counter(struct perf_event *event, u64 value)
{
	struct arm_pmu *cpu_pmu = to_arm_pmu(event->pmu);
	struct hw_perf_event *hwc = &event->hw;
	int idx = hwc->idx;

	value = armv8pmu_bias_long_counter(event, value);

	if (!armv8pmu_counter_valid(cpu_pmu, idx))
		pr_err("CPU%u writing wrong counter %d\n",
			smp_processor_id(), idx);
	else if (idx == ARMV8_IDX_CYCLE_COUNTER)
		write_sysreg(value, pmccntr_el0);
	else
		armv8pmu_write_hw_counter(event, value);
}

static inline void armv8pmu_write_evtype(int idx, u32 val)
{
	u32 counter = ARMV8_IDX_TO_COUNTER(idx);

	if (pmu_nmi_enable) {
		val &= ARMV8_PMU_EVTYPE_MASK;
		write_pmevtypern(counter, val);
	} else {
		armv8pmu_select_counter(idx);
		val &= ARMV8_PMU_EVTYPE_MASK;
		write_sysreg(val, pmxevtyper_el0);
	}
}

static inline void armv8pmu_write_hw_type(struct perf_event *event)
{
	struct hw_perf_event *hwc = &event->hw;
	int idx = hwc->idx;

	/*
	 * For chained events, the low counter is programmed to count
	 * the event of interest and the high counter is programmed
	 * with CHAIN event code with filters set to count at all ELs.
	 */
	if (armv8pmu_event_is_chained(event)) {
		u32 chain_evt = ARMV8_PMUV3_PERFCTR_CHAIN |
				ARMV8_PMU_INCLUDE_EL2;

		armv8pmu_write_evtype(idx - 1, hwc->config_base);
		armv8pmu_write_evtype(idx, chain_evt);
	} else {
		armv8pmu_write_evtype(idx, hwc->config_base);
	}
}

static inline void armv8pmu_write_event_type(struct perf_event *event)
{
	if (!pmu_nmi_enable) {
		armv8pmu_write_hw_type(event);
	} else {
		struct arm_pmu *cpu_pmu = to_arm_pmu(event->pmu);
		struct hw_perf_event *hwc = &event->hw;
		int idx = hwc->idx;

		if (!armv8pmu_counter_valid(cpu_pmu, idx))
			pr_err("CPU%u writing wrong event %d\n",
				smp_processor_id(), idx);
		else if (idx == ARMV8_IDX_CYCLE_COUNTER)
			write_sysreg(hwc->config_base & ARMV8_PMU_EVTYPE_MASK,
						pmccfiltr_el0);
		else
			armv8pmu_write_hw_type(event);
	}
}

static u32 armv8pmu_event_cnten_mask(struct perf_event *event)
{
	int counter = ARMV8_IDX_TO_COUNTER(event->hw.idx);
	u32 mask = BIT(counter);

	if (armv8pmu_event_is_chained(event))
		mask |= BIT(counter - 1);
	return mask;
}

static inline void armv8pmu_enable_counter(u32 mask)
{
	write_sysreg(mask, pmcntenset_el0);
}

static inline void armv8pmu_enable_event_counter(struct perf_event *event)
{
	struct perf_event_attr *attr = &event->attr;
	u32 mask = armv8pmu_event_cnten_mask(event);

	kvm_set_pmu_events(mask, attr);

	/* We rely on the hypervisor switch code to enable guest counters */
	if (!kvm_pmu_counter_deferred(attr))
		armv8pmu_enable_counter(mask);
}

static inline void armv8pmu_disable_counter(u32 mask)
{
	write_sysreg(mask, pmcntenclr_el0);
}

static inline void armv8pmu_disable_event_counter(struct perf_event *event)
{
	struct perf_event_attr *attr = &event->attr;
	u32 mask = armv8pmu_event_cnten_mask(event);

	kvm_clr_pmu_events(mask);

	/* We rely on the hypervisor switch code to disable guest counters */
	if (!kvm_pmu_counter_deferred(attr))
		armv8pmu_disable_counter(mask);
}

static inline void armv8pmu_enable_intens(u32 mask)
{
	write_sysreg(mask, pmintenset_el1);
}

static inline void armv8pmu_enable_event_irq(struct perf_event *event)
{
	u32 counter = ARMV8_IDX_TO_COUNTER(event->hw.idx);
	armv8pmu_enable_intens(BIT(counter));
}

static inline void armv8pmu_disable_intens(u32 mask)
{
	write_sysreg(mask, pmintenclr_el1);
	isb();
	/* Clear the overflow flag in case an interrupt is pending. */
	write_sysreg(mask, pmovsclr_el0);
	isb();
}

static inline void armv8pmu_disable_event_irq(struct perf_event *event)
{
	u32 counter = ARMV8_IDX_TO_COUNTER(event->hw.idx);
	armv8pmu_disable_intens(BIT(counter));
}

static inline u32 armv8pmu_getreset_flags(void)
{
	u32 value;

	/* Read */
	value = read_sysreg(pmovsclr_el0);

	/* Write to clear flags */
	value &= ARMV8_PMU_OVSR_MASK;
	write_sysreg(value, pmovsclr_el0);

	return value;
}

static void armv8pmu_enable_event(struct perf_event *event)
{
	unsigned long flags = 0;
	struct arm_pmu *cpu_pmu = to_arm_pmu(event->pmu);
	struct pmu_hw_events *events = this_cpu_ptr(cpu_pmu->hw_events);

	/*
	 * Enable counter and interrupt, and set the counter to count
	 * the event that we're interested in.
	 */
	if (!pmu_nmi_enable)
		raw_spin_lock_irqsave(&events->pmu_lock, flags);

	/*
	 * Disable counter
	 */
	armv8pmu_disable_event_counter(event);

	/*
	 * Set event (if destined for PMNx counters).
	 */
	armv8pmu_write_event_type(event);

	/*
	 * Enable interrupt for this counter
	 */
	armv8pmu_enable_event_irq(event);

	/*
	 * Enable counter
	 */
	armv8pmu_enable_event_counter(event);

	if (!pmu_nmi_enable)
		raw_spin_unlock_irqrestore(&events->pmu_lock, flags);
}

static void armv8pmu_disable_event(struct perf_event *event)
{
	unsigned long flags = 0;
	struct arm_pmu *cpu_pmu = to_arm_pmu(event->pmu);
	struct pmu_hw_events *events = this_cpu_ptr(cpu_pmu->hw_events);

	/*
	 * Disable counter and interrupt
	 */
	if (!pmu_nmi_enable)
		raw_spin_lock_irqsave(&events->pmu_lock, flags);

	/*
	 * Disable counter
	 */
	armv8pmu_disable_event_counter(event);

	/*
	 * Disable interrupt for this counter
	 */
	armv8pmu_disable_event_irq(event);

	if (!pmu_nmi_enable)
		raw_spin_unlock_irqrestore(&events->pmu_lock, flags);
}

static void armv8pmu_start(struct arm_pmu *cpu_pmu)
{
	if (pmu_nmi_enable) {
		preempt_disable();
		/* Enable all counters */
		armv8pmu_pmcr_write(armv8pmu_pmcr_read() | ARMV8_PMU_PMCR_E);
		preempt_enable();
	} else {
		unsigned long flags;
		struct pmu_hw_events *events = this_cpu_ptr(cpu_pmu->hw_events);

		raw_spin_lock_irqsave(&events->pmu_lock, flags);
		/* Enable all counters */
		armv8pmu_pmcr_write(armv8pmu_pmcr_read() | ARMV8_PMU_PMCR_E);
		raw_spin_unlock_irqrestore(&events->pmu_lock, flags);
	}
}

static void armv8pmu_stop(struct arm_pmu *cpu_pmu)
{
	if (pmu_nmi_enable) {
		preempt_disable();
		/* Disable all counters */
		armv8pmu_pmcr_write(armv8pmu_pmcr_read() & ~ARMV8_PMU_PMCR_E);
		preempt_enable();
	} else {
		unsigned long flags;
		struct pmu_hw_events *events = this_cpu_ptr(cpu_pmu->hw_events);

		raw_spin_lock_irqsave(&events->pmu_lock, flags);
		/* Disable all counters */
		armv8pmu_pmcr_write(armv8pmu_pmcr_read() & ~ARMV8_PMU_PMCR_E);
		raw_spin_unlock_irqrestore(&events->pmu_lock, flags);
	}
}

static irqreturn_t armv8pmu_handle_irq(struct arm_pmu *cpu_pmu)
{
	u32 pmovsr;
	struct perf_sample_data data;
	struct pmu_hw_events *cpuc = this_cpu_ptr(cpu_pmu->hw_events);
	struct pt_regs *regs;
	int idx;

	/*
	 * Get and reset the IRQ flags
	 */
	pmovsr = armv8pmu_getreset_flags();

	/*
	 * Did an overflow occur?
	 */
	if (!armv8pmu_has_overflowed(pmovsr))
		return IRQ_NONE;

	/*
	 * Handle the counter(s) overflow(s)
	 */
	regs = get_irq_regs();

	/*
	 * Stop the PMU while processing the counter overflows
	 * to prevent skews in group events.
	 */
	armv8pmu_stop(cpu_pmu);
	for (idx = 0; idx < cpu_pmu->num_events; ++idx) {
		struct perf_event *event = cpuc->events[idx];
		struct hw_perf_event *hwc;

		/* Ignore if we don't have an event. */
		if (!event)
			continue;

		/*
		 * We have a single interrupt for all counters. Check that
		 * each counter has overflowed before we process it.
		 */
		if (!armv8pmu_counter_has_overflowed(pmovsr, idx))
			continue;

		hwc = &event->hw;
		armpmu_event_update(event);
		perf_sample_data_init(&data, 0, hwc->last_period);
		if (!armpmu_event_set_period(event))
			continue;

		if (perf_event_overflow(event, &data, regs))
			cpu_pmu->disable(event);
	}
	armv8pmu_start(cpu_pmu);

	/*
	 * Handle the pending perf events.
	 *
	 * Note: this call *must* be run with interrupts disabled. For
	 * platforms that can have the PMU interrupts raised as an NMI, this
	 * will not work.
	 */
	if (!pmu_nmi_enable || !in_nmi())
		irq_work_run();

	return IRQ_HANDLED;
}

static int armv8pmu_get_single_idx(struct pmu_hw_events *cpuc,
				    struct arm_pmu *cpu_pmu)
{
	int idx;

	for (idx = ARMV8_IDX_COUNTER0; idx < cpu_pmu->num_events; idx ++) {
		if (!test_and_set_bit(idx, cpuc->used_mask))
			return idx;
	}
	return -EAGAIN;
}

static int armv8pmu_get_chain_idx(struct pmu_hw_events *cpuc,
				   struct arm_pmu *cpu_pmu)
{
	int idx;

	/*
	 * Chaining requires two consecutive event counters, where
	 * the lower idx must be even.
	 */
	for (idx = ARMV8_IDX_COUNTER0 + 1; idx < cpu_pmu->num_events; idx += 2) {
		if (!test_and_set_bit(idx, cpuc->used_mask)) {
			/* Check if the preceding even counter is available */
			if (!test_and_set_bit(idx - 1, cpuc->used_mask))
				return idx;
			/* Release the Odd counter */
			clear_bit(idx, cpuc->used_mask);
		}
	}
	return -EAGAIN;
}

static int armv8pmu_get_event_idx(struct pmu_hw_events *cpuc,
				  struct perf_event *event)
{
	struct arm_pmu *cpu_pmu = to_arm_pmu(event->pmu);
	struct hw_perf_event *hwc = &event->hw;
	unsigned long evtype = hwc->config_base & ARMV8_PMU_EVTYPE_EVENT;

	/* Always prefer to place a cycle counter into the cycle counter. */
	if (evtype == ARMV8_PMUV3_PERFCTR_CPU_CYCLES) {
		if (!test_and_set_bit(ARMV8_IDX_CYCLE_COUNTER, cpuc->used_mask))
			return ARMV8_IDX_CYCLE_COUNTER;
	}

	/*
	 * Otherwise use events counters
	 */
	if (armv8pmu_event_is_64bit(event) &&
	    !armv8pmu_has_long_event(cpu_pmu))
		return	armv8pmu_get_chain_idx(cpuc, cpu_pmu);
	else
		return armv8pmu_get_single_idx(cpuc, cpu_pmu);
}

static void armv8pmu_clear_event_idx(struct pmu_hw_events *cpuc,
				     struct perf_event *event)
{
	int idx = event->hw.idx;

	clear_bit(idx, cpuc->used_mask);
	if (armv8pmu_event_is_chained(event))
		clear_bit(idx - 1, cpuc->used_mask);
}

/*
 * Add an event filter to a given event. This will only work for PMUv2 PMUs.
 */
static int armv8pmu_set_event_filter(struct hw_perf_event *event,
				     struct perf_event_attr *attr)
{
	unsigned long config_base = 0;

	if (attr->exclude_idle)
		return -EPERM;

	/*
	 * If we're running in hyp mode, then we *are* the hypervisor.
	 * Therefore we ignore exclude_hv in this configuration, since
	 * there's no hypervisor to sample anyway. This is consistent
	 * with other architectures (x86 and Power).
	 */
	if (is_kernel_in_hyp_mode()) {
		if (!attr->exclude_kernel && !attr->exclude_host)
			config_base |= ARMV8_PMU_INCLUDE_EL2;
		if (attr->exclude_guest)
			config_base |= ARMV8_PMU_EXCLUDE_EL1;
		if (attr->exclude_host)
			config_base |= ARMV8_PMU_EXCLUDE_EL0;
	} else {
		if (!attr->exclude_hv && !attr->exclude_host)
			config_base |= ARMV8_PMU_INCLUDE_EL2;
	}

	/*
	 * Filter out !VHE kernels and guest kernels
	 */
	if (attr->exclude_kernel)
		config_base |= ARMV8_PMU_EXCLUDE_EL1;

	if (attr->exclude_user)
		config_base |= ARMV8_PMU_EXCLUDE_EL0;

	/*
	 * Install the filter into config_base as this is used to
	 * construct the event type.
	 */
	event->config_base = config_base;

	return 0;
}

static int armv8pmu_filter_match(struct perf_event *event)
{
	unsigned long evtype = event->hw.config_base & ARMV8_PMU_EVTYPE_EVENT;
	return evtype != ARMV8_PMUV3_PERFCTR_CHAIN;
}

static void armv8pmu_reset(void *info)
{
	struct arm_pmu *cpu_pmu = (struct arm_pmu *)info;
	u32 pmcr;

	/* The counter and interrupt enable registers are unknown at reset. */
	armv8pmu_disable_counter(U32_MAX);
	armv8pmu_disable_intens(U32_MAX);

	/* Clear the counters we flip at guest entry/exit */
	kvm_clr_pmu_events(U32_MAX);

	/*
	 * Initialize & Reset PMNC. Request overflow interrupt for
	 * 64 bit cycle counter but cheat in armv8pmu_write_counter().
	 */
	pmcr = ARMV8_PMU_PMCR_P | ARMV8_PMU_PMCR_C | ARMV8_PMU_PMCR_LC;

	/* Enable long event counter support where available */
	if (armv8pmu_has_long_event(cpu_pmu))
		pmcr |= ARMV8_PMU_PMCR_LP;

	armv8pmu_pmcr_write(pmcr);
}

static int __armv8_pmuv3_map_event(struct perf_event *event,
				   const unsigned (*extra_event_map)
						  [PERF_COUNT_HW_MAX],
				   const unsigned (*extra_cache_map)
						  [PERF_COUNT_HW_CACHE_MAX]
						  [PERF_COUNT_HW_CACHE_OP_MAX]
						  [PERF_COUNT_HW_CACHE_RESULT_MAX])
{
	int hw_event_id;
	struct arm_pmu *armpmu = to_arm_pmu(event->pmu);

	hw_event_id = armpmu_map_event(event, &armv8_pmuv3_perf_map,
				       &armv8_pmuv3_perf_cache_map,
				       ARMV8_PMU_EVTYPE_EVENT);

	if (armv8pmu_event_is_64bit(event))
		event->hw.flags |= ARMPMU_EVT_64BIT;

	/* Onl expose micro/arch events supported by this PMU */
	if ((hw_event_id > 0) && (hw_event_id < ARMV8_PMUV3_MAX_COMMON_EVENTS)
	    && test_bit(hw_event_id, armpmu->pmceid_bitmap)) {
		return hw_event_id;
	}

	return armpmu_map_event(event, extra_event_map, extra_cache_map,
				ARMV8_PMU_EVTYPE_EVENT);
}

static int armv8_pmuv3_map_event(struct perf_event *event)
{
	return __armv8_pmuv3_map_event(event, NULL, NULL);
}

static int armv8_a53_map_event(struct perf_event *event)
{
	return __armv8_pmuv3_map_event(event, NULL, &armv8_a53_perf_cache_map);
}

static int armv8_a57_map_event(struct perf_event *event)
{
	return __armv8_pmuv3_map_event(event, NULL, &armv8_a57_perf_cache_map);
}

static int armv8_a73_map_event(struct perf_event *event)
{
	return __armv8_pmuv3_map_event(event, NULL, &armv8_a73_perf_cache_map);
}

static int armv8_thunder_map_event(struct perf_event *event)
{
	return __armv8_pmuv3_map_event(event, NULL,
				       &armv8_thunder_perf_cache_map);
}

static int armv8_vulcan_map_event(struct perf_event *event)
{
	return __armv8_pmuv3_map_event(event, NULL,
				       &armv8_vulcan_perf_cache_map);
}

struct armv8pmu_probe_info {
	struct arm_pmu *pmu;
	bool present;
};

static void __armv8pmu_probe_pmu(void *info)
{
	struct armv8pmu_probe_info *probe = info;
	struct arm_pmu *cpu_pmu = probe->pmu;
	u64 dfr0;
	u64 pmceid_raw[2];
	u32 pmceid[2];
	int pmuver;

	dfr0 = read_sysreg(id_aa64dfr0_el1);
	pmuver = cpuid_feature_extract_unsigned_field(dfr0,
			ID_AA64DFR0_PMUVER_SHIFT);
	if (pmuver == 0xf || pmuver == 0)
		return;

	cpu_pmu->pmuver = pmuver;
	probe->present = true;

	/* Read the nb of CNTx counters supported from PMNC */
	cpu_pmu->num_events = (armv8pmu_pmcr_read() >> ARMV8_PMU_PMCR_N_SHIFT)
		& ARMV8_PMU_PMCR_N_MASK;

	/* Add the CPU cycles counter */
	cpu_pmu->num_events += 1;

	pmceid[0] = pmceid_raw[0] = read_sysreg(pmceid0_el0);
	pmceid[1] = pmceid_raw[1] = read_sysreg(pmceid1_el0);

	bitmap_from_arr32(cpu_pmu->pmceid_bitmap,
			     pmceid, ARMV8_PMUV3_MAX_COMMON_EVENTS);

	pmceid[0] = pmceid_raw[0] >> 32;
	pmceid[1] = pmceid_raw[1] >> 32;

	bitmap_from_arr32(cpu_pmu->pmceid_ext_bitmap,
			     pmceid, ARMV8_PMUV3_MAX_COMMON_EVENTS);
}

static int armv8pmu_probe_pmu(struct arm_pmu *cpu_pmu)
{
	struct armv8pmu_probe_info probe = {
		.pmu = cpu_pmu,
		.present = false,
	};
	int ret;

	ret = smp_call_function_any(&cpu_pmu->supported_cpus,
				    __armv8pmu_probe_pmu,
				    &probe, 1);
	if (ret)
		return ret;

	return probe.present ? 0 : -ENODEV;
}

static int armv8_pmu_init(struct arm_pmu *cpu_pmu)
{
	int ret = armv8pmu_probe_pmu(cpu_pmu);
	if (ret)
		return ret;

	cpu_pmu->handle_irq		= armv8pmu_handle_irq,
	cpu_pmu->enable			= armv8pmu_enable_event,
	cpu_pmu->disable		= armv8pmu_disable_event,
	cpu_pmu->read_counter		= armv8pmu_read_counter,
	cpu_pmu->write_counter		= armv8pmu_write_counter,
	cpu_pmu->get_event_idx		= armv8pmu_get_event_idx,
	cpu_pmu->clear_event_idx	= armv8pmu_clear_event_idx,
	cpu_pmu->start			= armv8pmu_start,
	cpu_pmu->stop			= armv8pmu_stop,
	cpu_pmu->reset			= armv8pmu_reset,
	cpu_pmu->set_event_filter	= armv8pmu_set_event_filter;
	cpu_pmu->filter_match		= armv8pmu_filter_match;

	return 0;
}

static int armv8_pmuv3_init(struct arm_pmu *cpu_pmu)
{
	int ret = armv8_pmu_init(cpu_pmu);
	if (ret)
		return ret;

	cpu_pmu->name			= "armv8_pmuv3";
	cpu_pmu->map_event		= armv8_pmuv3_map_event;
	cpu_pmu->attr_groups[ARMPMU_ATTR_GROUP_EVENTS] =
		&armv8_pmuv3_events_attr_group;
	cpu_pmu->attr_groups[ARMPMU_ATTR_GROUP_FORMATS] =
		&armv8_pmuv3_format_attr_group;

	return 0;
}

static int armv8_a35_pmu_init(struct arm_pmu *cpu_pmu)
{
	int ret = armv8_pmu_init(cpu_pmu);
	if (ret)
		return ret;

	cpu_pmu->name			= "armv8_cortex_a35";
	cpu_pmu->map_event		= armv8_a53_map_event;
	cpu_pmu->attr_groups[ARMPMU_ATTR_GROUP_EVENTS] =
		&armv8_pmuv3_events_attr_group;
	cpu_pmu->attr_groups[ARMPMU_ATTR_GROUP_FORMATS] =
		&armv8_pmuv3_format_attr_group;

	return 0;
}

static int armv8_a53_pmu_init(struct arm_pmu *cpu_pmu)
{
	int ret = armv8_pmu_init(cpu_pmu);
	if (ret)
		return ret;

	cpu_pmu->name			= "armv8_cortex_a53";
	cpu_pmu->map_event		= armv8_a53_map_event;
	cpu_pmu->attr_groups[ARMPMU_ATTR_GROUP_EVENTS] =
		&armv8_pmuv3_events_attr_group;
	cpu_pmu->attr_groups[ARMPMU_ATTR_GROUP_FORMATS] =
		&armv8_pmuv3_format_attr_group;

	return 0;
}

static int armv8_a57_pmu_init(struct arm_pmu *cpu_pmu)
{
	int ret = armv8_pmu_init(cpu_pmu);
	if (ret)
		return ret;

	cpu_pmu->name			= "armv8_cortex_a57";
	cpu_pmu->map_event		= armv8_a57_map_event;
	cpu_pmu->attr_groups[ARMPMU_ATTR_GROUP_EVENTS] =
		&armv8_pmuv3_events_attr_group;
	cpu_pmu->attr_groups[ARMPMU_ATTR_GROUP_FORMATS] =
		&armv8_pmuv3_format_attr_group;

	return 0;
}

static int armv8_a72_pmu_init(struct arm_pmu *cpu_pmu)
{
	int ret = armv8_pmu_init(cpu_pmu);
	if (ret)
		return ret;

	cpu_pmu->name			= "armv8_cortex_a72";
	cpu_pmu->map_event		= armv8_a57_map_event;
	cpu_pmu->attr_groups[ARMPMU_ATTR_GROUP_EVENTS] =
		&armv8_pmuv3_events_attr_group;
	cpu_pmu->attr_groups[ARMPMU_ATTR_GROUP_FORMATS] =
		&armv8_pmuv3_format_attr_group;

	return 0;
}

static int armv8_a73_pmu_init(struct arm_pmu *cpu_pmu)
{
	int ret = armv8_pmu_init(cpu_pmu);
	if (ret)
		return ret;

	cpu_pmu->name			= "armv8_cortex_a73";
	cpu_pmu->map_event		= armv8_a73_map_event;
	cpu_pmu->attr_groups[ARMPMU_ATTR_GROUP_EVENTS] =
		&armv8_pmuv3_events_attr_group;
	cpu_pmu->attr_groups[ARMPMU_ATTR_GROUP_FORMATS] =
		&armv8_pmuv3_format_attr_group;

	return 0;
}

static int armv8_thunder_pmu_init(struct arm_pmu *cpu_pmu)
{
	int ret = armv8_pmu_init(cpu_pmu);
	if (ret)
		return ret;

	cpu_pmu->name			= "armv8_cavium_thunder";
	cpu_pmu->map_event		= armv8_thunder_map_event;
	cpu_pmu->attr_groups[ARMPMU_ATTR_GROUP_EVENTS] =
		&armv8_pmuv3_events_attr_group;
	cpu_pmu->attr_groups[ARMPMU_ATTR_GROUP_FORMATS] =
		&armv8_pmuv3_format_attr_group;

	return 0;
}

static int armv8_vulcan_pmu_init(struct arm_pmu *cpu_pmu)
{
	int ret = armv8_pmu_init(cpu_pmu);
	if (ret)
		return ret;

	cpu_pmu->name			= "armv8_brcm_vulcan";
	cpu_pmu->map_event		= armv8_vulcan_map_event;
	cpu_pmu->attr_groups[ARMPMU_ATTR_GROUP_EVENTS] =
		&armv8_pmuv3_events_attr_group;
	cpu_pmu->attr_groups[ARMPMU_ATTR_GROUP_FORMATS] =
		&armv8_pmuv3_format_attr_group;

	return 0;
}

static const struct of_device_id armv8_pmu_of_device_ids[] = {
	{.compatible = "arm,armv8-pmuv3",	.data = armv8_pmuv3_init},
	{.compatible = "arm,cortex-a35-pmu",	.data = armv8_a35_pmu_init},
	{.compatible = "arm,cortex-a53-pmu",	.data = armv8_a53_pmu_init},
	{.compatible = "arm,cortex-a57-pmu",	.data = armv8_a57_pmu_init},
	{.compatible = "arm,cortex-a72-pmu",	.data = armv8_a72_pmu_init},
	{.compatible = "arm,cortex-a73-pmu",	.data = armv8_a73_pmu_init},
	{.compatible = "cavium,thunder-pmu",	.data = armv8_thunder_pmu_init},
	{.compatible = "brcm,vulcan-pmu",	.data = armv8_vulcan_pmu_init},
	{},
};

static int armv8_pmu_device_probe(struct platform_device *pdev)
{
	return arm_pmu_device_probe(pdev, armv8_pmu_of_device_ids, NULL);
}

static struct platform_driver armv8_pmu_driver = {
	.driver		= {
		.name	= ARMV8_PMU_PDEV_NAME,
		.of_match_table = armv8_pmu_of_device_ids,
		.suppress_bind_attrs = true,
	},
	.probe		= armv8_pmu_device_probe,
};

static int __init armv8_pmu_driver_init(void)
{
	if (acpi_disabled)
		return platform_driver_register(&armv8_pmu_driver);
	else
		return arm_pmu_acpi_probe(armv8_pmuv3_init);
}
device_initcall(armv8_pmu_driver_init)

static u64 cyc_to_ns(u64 cyc, u16 time_shift, u32 time_mult)
{
	u64 quot, rem;

	quot = cyc >> time_shift;
	rem  = cyc & (((u64)1 << time_shift) - 1);
	return quot * time_mult +
	       ((rem * time_mult) >> time_shift);
}

void arch_perf_update_userpage(struct perf_event *event,
			       struct perf_event_mmap_page *userpg, u64 now)
{
	u32 freq;
	u32 shift;
	u64 offset;

	/*
	 * Internal timekeeping for enabled/running/stopped times
	 * is always computed with the sched_clock.
	 */
	freq = arch_timer_get_rate();
	userpg->cap_user_time = 1;

	clocks_calc_mult_shift(&userpg->time_mult, &shift, freq,
			NSEC_PER_SEC, 0);
	/*
	 * time_shift is not expected to be greater than 31 due to
	 * the original published conversion algorithm shifting a
	 * 32-bit value (now specifies a 64-bit value) - refer
	 * perf_event_mmap_page documentation in perf_event.h.
	 */
	if (shift == 32) {
		shift = 31;
		userpg->time_mult >>= 1;
	}
	userpg->time_shift = (u16)shift;
	userpg->time_offset = -now;

	offset = local_clock() - cyc_to_ns(arch_timer_read_counter(),
			userpg->time_shift, userpg->time_mult);

	/*
	 * cap_user_time_zero doesn't make sense when we're using a different
	 * time base for the records.
	 */
	if (!event->attr.use_clockid) {
		userpg->cap_user_time_zero = 1;
		userpg->time_zero = offset;
	}
}
