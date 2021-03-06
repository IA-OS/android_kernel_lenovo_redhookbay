/*COPYRIGHT**
    Copyright (C) 2005-2013 Intel Corporation.  All Rights Reserved.

    This file is part of SEP Development Kit

    SEP Development Kit is free software; you can redistribute it
    and/or modify it under the terms of the GNU General Public License
    version 2 as published by the Free Software Foundation.

    SEP Development Kit is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with SEP Development Kit; if not, write to the Free Software
    Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA

    As a special exception, you may use this file as part of a free software
    library without restriction.  Specifically, if other files instantiate
    templates or use macros or inline functions from this file, or you compile
    this file and link it with other files to produce an executable, this
    file does not by itself cause the resulting executable to be covered by
    the GNU General Public License.  This exception does not however
    invalidate any other reasons why the executable file might be covered by
    the GNU General Public License.
**COPYRIGHT*/

#include "lwpmudrv_defines.h"
#include <linux/version.h>
#include <linux/kernel.h>
#include <linux/ptrace.h>
#if defined(DRV_EM64T)
#include <asm/desc.h>
#endif

#include "lwpmudrv_types.h"
#include "rise_errors.h"
#include "lwpmudrv_ecb.h"
#include "lwpmudrv_struct.h"
#if defined(DRV_IA32) || defined(DRV_EM64T)
#include "apic.h"
#endif
#include "lwpmudrv.h"
#include "output.h"
#include "control.h"
#include "pmi.h"
#include "utility.h"
#if defined(DRV_IA32) || defined(DRV_EM64T)
#include "pebs.h"
#endif

#if defined(BUILD_CHIPSET)
#include "lwpmudrv_chipset.h"
#endif

// Desc id #0 is used for module records
#define COMPUTE_DESC_ID(index)     ((index))

extern DRV_CONFIG     pcfg;
extern uid_t          uid;

#define EFLAGS_V86_MASK       0x00020000L

/*********************************************************************
 * Global Variables / State
 *********************************************************************/

/*********************************************************************
 * Interrupt Handler
 *********************************************************************/

/*
 *  PMI_Interrupt_Handler
 *      Arguments
 *          IntFrame - Pointer to the Interrupt Frame
 *
 *      Returns
 *          None
 *
 *      Description
 *  Grab the data that is needed to populate the sample records
 */
#if defined(DRV_IA32)

asmlinkage VOID
PMI_Interrupt_Handler (
     struct pt_regs *regs
)
{
    SampleRecordPC  *psamp;
    CPU_STATE        pcpu;
    BUFFER_DESC      bd;
    U32              csdlo;        // low  half code seg descriptor
    U32              csdhi;        // high half code seg descriptor
    U32              seg_cs;       // code seg selector
    DRV_MASKS_NODE   event_mask;
    U32              this_cpu;
    U32              i;
    U32              pid;
    U32              tid;
    U64              tsc;
    U32              desc_id;
    EVENT_DESC       evt_desc;
#if defined(SECURE_SEP)
    uid_t            l_uid;
#endif
    U32              accept_interrupt = 1;
    U32              dev_idx;
    U32              event_idx;
    DRV_CONFIG       pcfg_unc;
    DISPATCH         dispatch_unc;
    U64             *result_buffer;
    U64              diff;

    this_cpu = CONTROL_THIS_CPU();
    pcpu     = &pcb[this_cpu];
    bd       = &cpu_buf[this_cpu];
    SYS_Locked_Inc(&CPU_STATE_in_interrupt(pcpu));

    // Disable the counter control
    dispatch->freeze(NULL);

#if defined(SECURE_SEP)
    l_uid            = DRV_GET_UID(current);
    accept_interrupt = (l_uid == uid);
#endif
    dispatch->check_overflow(&event_mask);
    if (GLOBAL_STATE_current_phase(driver_state) == DRV_STATE_RUNNING &&
        CPU_STATE_accept_interrupt(&pcb[this_cpu])) {

        pid = GET_CURRENT_TGID();
        tid = current->pid;

        if (DRV_CONFIG_target_pid(pcfg) > 0 && pid != DRV_CONFIG_target_pid(pcfg)) {
            accept_interrupt = 0;
        }

        if (accept_interrupt) {
            UTILITY_Read_TSC(&tsc);

            for (i = 0; i < event_mask.masks_num; i++) {
                desc_id  = COMPUTE_DESC_ID(DRV_EVENT_MASK_event_idx(&event_mask.eventmasks[i]));
                evt_desc = desc_data[desc_id];
                psamp = (SampleRecordPC *)OUTPUT_Reserve_Buffer_Space(bd,
                                                 EVENT_DESC_sample_size(evt_desc));

                if (!psamp) {
                    continue;
                }
                CPU_STATE_num_samples(pcpu)           += 1;
                SAMPLE_RECORD_descriptor_id(psamp)     = desc_id;
                SAMPLE_RECORD_tsc(psamp)               = tsc;
                SAMPLE_RECORD_pid_rec_index_raw(psamp) = 1;
                SAMPLE_RECORD_pid_rec_index(psamp)     = pid;
                SAMPLE_RECORD_tid(psamp)               = tid;
                SAMPLE_RECORD_eip(psamp)               = REGS_eip(regs);
                SAMPLE_RECORD_eflags(psamp)            = REGS_eflags(regs);
                SAMPLE_RECORD_cpu_num(psamp)           = (U16) this_cpu;
                SAMPLE_RECORD_cs(psamp)                = (U16) REGS_xcs(regs);

                if (SAMPLE_RECORD_eflags(psamp) & EFLAGS_V86_MASK) {
                    csdlo = 0;
                    csdhi = 0;
                }
                else {
                    seg_cs = SAMPLE_RECORD_cs(psamp);
                    SYS_Get_CSD(seg_cs, &csdlo, &csdhi);
                }
                SAMPLE_RECORD_csd(psamp).u1.lowWord  = csdlo;
                SAMPLE_RECORD_csd(psamp).u2.highWord = csdhi;

                SEP_PRINT_DEBUG("SAMPLE_RECORD_pid_rec_index(psamp)  %x\n", SAMPLE_RECORD_pid_rec_index(psamp));
                SEP_PRINT_DEBUG("SAMPLE_RECORD_tid(psamp) %x\n", SAMPLE_RECORD_tid(psamp));
                SEP_PRINT_DEBUG("SAMPLE_RECORD_eip(psamp) %x\n", SAMPLE_RECORD_eip(psamp));
                SEP_PRINT_DEBUG("SAMPLE_RECORD_eflags(psamp) %x\n", SAMPLE_RECORD_eflags(psamp));
                SEP_PRINT_DEBUG("SAMPLE_RECORD_cpu_num(psamp) %x\n", SAMPLE_RECORD_cpu_num(psamp));
                SEP_PRINT_DEBUG("SAMPLE_RECORD_cs(psamp) %x\n", SAMPLE_RECORD_cs(psamp));
                SEP_PRINT_DEBUG("SAMPLE_RECORD_csd(psamp).lowWord %x\n", SAMPLE_RECORD_csd(psamp).u1.lowWord);
                SEP_PRINT_DEBUG("SAMPLE_RECORD_csd(psamp).highWord %x\n", SAMPLE_RECORD_csd(psamp).u2.highWord);

                SAMPLE_RECORD_event_index(psamp) = DRV_EVENT_MASK_event_idx(&event_mask.eventmasks[i]);
                if (DRV_EVENT_MASK_precise(&event_mask.eventmasks[i]) == 1) {
                    if (EVENT_DESC_pebs_offset(evt_desc) ||
                        EVENT_DESC_latency_offset_in_sample(evt_desc)) {
                        PEBS_Fill_Buffer((S8 *)psamp,
                                     evt_desc,
                                     DRV_CONFIG_virt_phys_translation(pcfg));
                    }
                    PEBS_Modify_IP((S8 *)psamp, FALSE);
                }
                if (DRV_CONFIG_collect_lbrs(pcfg) && (DRV_EVENT_MASK_lbr_capture(&event_mask.eventmasks[i]))) {
                    dispatch->read_lbrs(((S8 *)(psamp)+EVENT_DESC_lbr_offset(evt_desc)));
                }
                if (DRV_CONFIG_power_capture(pcfg)) {
                    dispatch->read_power(((S8 *)(psamp)+EVENT_DESC_power_offset_in_sample(evt_desc)));
                }
#if defined(BUILD_CHIPSET)
                if (DRV_CONFIG_enable_chipset(pcfg)) {
                    cs_dispatch->read_counters(((S8 *)(psamp)+DRV_CONFIG_chipset_offset(pcfg)));
                }
#endif
                if (DRV_CONFIG_event_based_counts(pcfg)) {
                    dispatch->read_counts(((S8 *)(psamp)+EVENT_DESC_ebc_offset(evt_desc)), DRV_EVENT_MASK_event_idx(&event_mask.eventmasks[i]));
                }
                // need to do this per device
                for (dev_idx = 0; dev_idx < num_devices; dev_idx++) {
                    pcfg_unc = LWPMU_DEVICE_pcfg(&devices[dev_idx]);
                    dispatch_unc = LWPMU_DEVICE_dispatch(&devices[dev_idx]);
                    if (pcfg_unc && DRV_CONFIG_event_based_counts(pcfg_unc)) {
                        dispatch_unc->read_counts(((S8 *)(psamp)+DRV_CONFIG_results_offset(pcfg_unc)), dev_idx);
                        SAMPLE_RECORD_uncore_valid(psamp) = 1;

                        // skip first element because it's the group number
                        result_buffer = (U64*) ((S8*)(psamp) + DRV_CONFIG_results_offset(pcfg_unc));
                        for (event_idx = 1;
                             event_idx < LWPMU_DEVICE_num_events(&devices[dev_idx]) + 1; // need to account for 1st element being group id
                             event_idx++) {

                            // check for overflow
                            if (result_buffer[event_idx] < LWPMU_DEVICE_prev_val_per_thread(&devices[dev_idx])[this_cpu][event_idx]) {
                                // overflow occurred!  need to compensate
                                diff  = LWPMU_DEVICE_counter_mask(&devices[dev_idx])
                                      - LWPMU_DEVICE_prev_val_per_thread(&devices[dev_idx])[this_cpu][event_idx];
                                diff += result_buffer[event_idx];
                            }
                            else {
                                diff = result_buffer[event_idx] - LWPMU_DEVICE_prev_val_per_thread(&devices[dev_idx])[this_cpu][event_idx];
                            }
                            // accumulate current results into accumulator
                            LWPMU_DEVICE_acc_per_thread(&devices[dev_idx])[this_cpu][event_idx] += diff;                            
                            // update previous results array
                            LWPMU_DEVICE_prev_val_per_thread(&devices[dev_idx])[this_cpu][event_idx] = result_buffer[event_idx];
                            // update results buffer for this thread
                            result_buffer[event_idx] = LWPMU_DEVICE_acc_per_thread(&devices[dev_idx])[this_cpu][event_idx];
                        }
                    }
                }
            } // for
        }
    } 
    if (DRV_CONFIG_pebs_mode(pcfg)) {
        PEBS_Reset_Index(this_cpu);
    }

    APIC_Ack_Eoi();

    // Reset the data counters
    if (CPU_STATE_trigger_count(&pcb[this_cpu]) == 0) {
        dispatch->swap_group(FALSE);
    }
    // Re-enable the counter control
    dispatch->restart(NULL);
    atomic_set(&CPU_STATE_in_interrupt(&pcb[this_cpu]), 0);

    return;
}

#endif  // DRV_IA32


#if defined(DRV_EM64T)

#define IS_LDT_BIT       0x4
#define SEGMENT_SHIFT    3
IDTGDT_DESC              gdt_desc;

static U32
pmi_Get_CSD (
    U32     seg,
    U32    *low,
    U32    *high
)
{
    PVOID               gdt_max_addr;
    struct desc_struct *gdt;
    CodeDescriptor     *csd;

    //
    // These could be pre-init'ed values
    //
    gdt_max_addr = (PVOID) (((U64) gdt_desc.idtgdt_base) + gdt_desc.idtgdt_limit);
    gdt          = gdt_desc.idtgdt_base;

    //
    // end pre-init potential...
    //

    //
    // We don't do ldt's
    //
    if (seg & IS_LDT_BIT) {
        *low  = 0;
        *high = 0;
        return (FALSE);
    }

    //
    // segment offset is based on dropping the bottom 3 bits...
    //
    csd = (CodeDescriptor *) &(gdt[seg >> SEGMENT_SHIFT]);

    if (((PVOID) csd) >= gdt_max_addr) {
        SEP_PRINT_WARNING("segment too big in get_CSD(0x%x)\n", seg);
        return FALSE;
    }

    *low  = csd->u1.lowWord;
    *high = csd->u2.highWord;

    SEP_PRINT_DEBUG("get_CSD - seg 0x%x, low %08x, high %08x, reserved_0: %d\n",
                     seg, *low, *high, csd->u2.s2.reserved_0);

    return TRUE;
}

asmlinkage VOID
PMI_Interrupt_Handler (
     struct pt_regs *regs
)
{
    SampleRecordPC  *psamp;
    CPU_STATE        pcpu;
    BUFFER_DESC      bd;
    DRV_MASKS_NODE   event_mask;
    U32              this_cpu;
    U32              i;
    U32              is_64bit_addr;
    U32              pid;
    U32              tid;
    U64              tsc;
    U32              desc_id;
    EVENT_DESC       evt_desc;
    U32              accept_interrupt = 1;
#if defined(SECURE_SEP)
    uid_t            l_uid;
#endif
    DRV_CONFIG       pcfg_unc;
    DISPATCH         dispatch_unc;
    U32              dev_idx;
    U32              event_idx;
    U64             *result_buffer;
    U64              diff;

    // Disable the counter control
    dispatch->freeze(NULL);
    this_cpu = CONTROL_THIS_CPU();
    pcpu     = &pcb[this_cpu];
    bd       = &cpu_buf[this_cpu];
    SYS_Locked_Inc(&CPU_STATE_in_interrupt(pcpu));

#if defined(SECURE_SEP)
    l_uid            = DRV_GET_UID(current);
    accept_interrupt = (l_uid == uid);
#endif
    dispatch->check_overflow(&event_mask);
    if (GLOBAL_STATE_current_phase(driver_state) == DRV_STATE_RUNNING &&
        CPU_STATE_accept_interrupt(&pcb[this_cpu])) {

        pid  = GET_CURRENT_TGID();
        tid  = current->pid;

        if (DRV_CONFIG_target_pid(pcfg) > 0 && pid != DRV_CONFIG_target_pid(pcfg)) {
            accept_interrupt = 0;
        }
        if (accept_interrupt) {
            UTILITY_Read_TSC(&tsc);

            for (i = 0; i < event_mask.masks_num; i++) {
                desc_id  = COMPUTE_DESC_ID(DRV_EVENT_MASK_event_idx(&event_mask.eventmasks[i]));
                evt_desc = desc_data[desc_id];
                psamp = (SampleRecordPC *)OUTPUT_Reserve_Buffer_Space(bd,
                                            EVENT_DESC_sample_size(evt_desc));
                if (!psamp) {
                    continue;
                }
                CPU_STATE_num_samples(pcpu)           += 1;
                SAMPLE_RECORD_descriptor_id(psamp)     = desc_id;
                SAMPLE_RECORD_tsc(psamp)               = tsc;
                SAMPLE_RECORD_pid_rec_index_raw(psamp) = 1;
                SAMPLE_RECORD_pid_rec_index(psamp)     = pid;
                SAMPLE_RECORD_tid(psamp)               = tid;
                SAMPLE_RECORD_cpu_num(psamp)           = (U16) this_cpu;
                SAMPLE_RECORD_cs(psamp)                = (U16) REGS_cs(regs);

                pmi_Get_CSD(SAMPLE_RECORD_cs(psamp),
                        &SAMPLE_RECORD_csd(psamp).u1.lowWord,
                        &SAMPLE_RECORD_csd(psamp).u2.highWord);

                SEP_PRINT_DEBUG("SAMPLE_RECORD_pid_rec_index(psamp)  %x\n", SAMPLE_RECORD_pid_rec_index(psamp));
                SEP_PRINT_DEBUG("SAMPLE_RECORD_tid(psamp) %x\n", SAMPLE_RECORD_tid(psamp));
                SEP_PRINT_DEBUG("SAMPLE_RECORD_cpu_num(psamp) %x\n", SAMPLE_RECORD_cpu_num(psamp));
                SEP_PRINT_DEBUG("SAMPLE_RECORD_cs(psamp) %x\n", SAMPLE_RECORD_cs(psamp));
                SEP_PRINT_DEBUG("SAMPLE_RECORD_csd(psamp).lowWord %x\n", SAMPLE_RECORD_csd(psamp).u1.lowWord);
                SEP_PRINT_DEBUG("SAMPLE_RECORD_csd(psamp).highWord %x\n", SAMPLE_RECORD_csd(psamp).u2.highWord);

                is_64bit_addr = (SAMPLE_RECORD_csd(psamp).u2.s2.reserved_0 == 1);
                if (is_64bit_addr) {
                    SAMPLE_RECORD_iip(psamp)           = REGS_rip(regs);
                    SAMPLE_RECORD_ipsr(psamp)          = (REGS_eflags(regs) & 0xffffffff) |
                        (((U64) SAMPLE_RECORD_csd(psamp).u2.s2.dpl) << 32);
                    SAMPLE_RECORD_ia64_pc(psamp)       = TRUE;
                }
                else {
                    SAMPLE_RECORD_eip(psamp)           = REGS_rip(regs);
                    SAMPLE_RECORD_eflags(psamp)        = REGS_eflags(regs);
                    SAMPLE_RECORD_ia64_pc(psamp)       = FALSE;

                    SEP_PRINT_DEBUG("SAMPLE_RECORD_eip(psamp) %x\n", SAMPLE_RECORD_eip(psamp));
                    SEP_PRINT_DEBUG("SAMPLE_RECORD_eflags(psamp) %x\n", SAMPLE_RECORD_eflags(psamp));
                }

                SAMPLE_RECORD_event_index(psamp) = DRV_EVENT_MASK_event_idx(&event_mask.eventmasks[i]);
                if (DRV_EVENT_MASK_precise(&event_mask.eventmasks[i])) {
                    if ( EVENT_DESC_pebs_offset(evt_desc) ||
                         EVENT_DESC_latency_offset_in_sample(evt_desc)) {
                         PEBS_Fill_Buffer((S8 *)psamp,
                                     evt_desc,
                                     DRV_CONFIG_virt_phys_translation(pcfg));
                    }
                    PEBS_Modify_IP((S8 *)psamp, is_64bit_addr);
                }
                if (DRV_CONFIG_collect_lbrs(pcfg) && (DRV_EVENT_MASK_lbr_capture(&event_mask.eventmasks[i]))) {
                    dispatch->read_lbrs(((S8 *)(psamp)+EVENT_DESC_lbr_offset(evt_desc)));
                }
                if (DRV_CONFIG_power_capture(pcfg)) {
                    dispatch->read_power(((S8 *)(psamp)+EVENT_DESC_power_offset_in_sample(evt_desc)));
                }
#if defined(BUILD_CHIPSET)
                if (DRV_CONFIG_enable_chipset(pcfg)) {
                    cs_dispatch->read_counters(((S8 *)(psamp)+DRV_CONFIG_chipset_offset(pcfg)));
                }
#endif
                if (DRV_CONFIG_event_based_counts(pcfg)) {
                    dispatch->read_counts(((S8 *)(psamp)+EVENT_DESC_ebc_offset(evt_desc)), DRV_EVENT_MASK_event_idx(&event_mask.eventmasks[i]));
                }
                for (dev_idx = 0; dev_idx < num_devices; dev_idx++) {
                    pcfg_unc = LWPMU_DEVICE_pcfg(&devices[dev_idx]);
                    dispatch_unc = LWPMU_DEVICE_dispatch(&devices[dev_idx]);
                    if (pcfg_unc && DRV_CONFIG_event_based_counts(pcfg_unc)) {
                        dispatch_unc->read_counts(((S8 *)(psamp)+DRV_CONFIG_results_offset(pcfg_unc)), dev_idx);
                        SAMPLE_RECORD_uncore_valid(psamp) = 1;

                        // skip first element because it's the group number
                        result_buffer = (U64*) ((S8*)(psamp) + DRV_CONFIG_results_offset(pcfg_unc));
                        for (event_idx = 1;
                             event_idx < LWPMU_DEVICE_num_events(&devices[dev_idx]) + 1; // need to account for 1st element being group id
                             event_idx++) {

                            // check for overflow
                            if (result_buffer[event_idx] < LWPMU_DEVICE_prev_val_per_thread(&devices[dev_idx])[this_cpu][event_idx]) {
                                // overflow occurred!  need to compensate
                                diff  = LWPMU_DEVICE_counter_mask(&devices[dev_idx])
                                      - LWPMU_DEVICE_prev_val_per_thread(&devices[dev_idx])[this_cpu][event_idx];
                                diff += result_buffer[event_idx];
                            }
                            else {
                                diff = result_buffer[event_idx] - LWPMU_DEVICE_prev_val_per_thread(&devices[dev_idx])[this_cpu][event_idx];
                            }
                            // accumulate current results into accumulator
                            LWPMU_DEVICE_acc_per_thread(&devices[dev_idx])[this_cpu][event_idx] += diff;                            
                            // update previous results array
                            LWPMU_DEVICE_prev_val_per_thread(&devices[dev_idx])[this_cpu][event_idx] = result_buffer[event_idx];
                            // update results buffer for this thread
                            result_buffer[event_idx] = LWPMU_DEVICE_acc_per_thread(&devices[dev_idx])[this_cpu][event_idx];
                        }
                    }
                }
            }
        }
    }
    if (DRV_CONFIG_pebs_mode(pcfg)) {
        PEBS_Reset_Index(this_cpu);
    }

    APIC_Ack_Eoi();

    // Reset the data counters
    if (CPU_STATE_trigger_count(&pcb[this_cpu]) == 0) {
        dispatch->swap_group(FALSE);
    }
    // Re-enable the counter control
    dispatch->restart(NULL);
    atomic_set(&CPU_STATE_in_interrupt(&pcb[this_cpu]), 0);

    return;
}

#endif


#if defined(DRV_IA64)

#define DISPATCH_read_ro(arg1, arg2, arg3)    \
    if (dispatch->read_ro) dispatch->read_ro((arg1), (arg2), (arg3))

/*
 * The parameters are different depending on if you are compiling IA32 or
 * Itanium(R)-based systems. In the end, what is really significant is that
 * for IA32, this routine is being called by assembly code, not the usual
 * Linux* OS interrupt handler. For IA32, we actually hijack the IDT directly
 * which lets us capture RO information which would otherwise be lost...
 *
 * @todo Is there anyway to make the func declaration the same? Is it worth it?
 *
 */
static void
pmi_Handler (
    IN int             irq,
    IN struct pt_regs *regs
)
{
    SampleRecordPC *psamp;
    CPU_STATE       pcpu;
    BUFFER_DESC     bd;
    DRV_MASKS_NODE  event_mask;
    U32             this_cpu;
    U32             i;
    U32             pid;
    U32             tid;
    U64             itc;          // interval time counter for IPF
    U32             desc_id;
    EVENT_DESC      evt_desc;
#if defined(SECURE_SEP)
    uid_t           l_uid;
#endif
    U32             accept_interrupt = 1;

    this_cpu = CONTROL_THIS_CPU();
    pcpu     = &pcb[this_cpu];
    bd       = &cpu_buf[this_cpu];
    SYS_Locked_Inc(&CPU_STATE_in_interrupt(pcpu));

    // Disable the counter control
    dispatch->freeze(NULL);

#if defined(SECURE_SEP)
    l_uid            = DRV_GET_UID(current);
    accept_interrupt = (l_uid == uid);
#endif

    if (GLOBAL_STATE_current_phase(driver_state) == DRV_STATE_RUNNING &&
            CPU_STATE_accept_interrupt(&pcb[this_cpu])) {

        dispatch->check_overflow(&event_mask);

        if (accept_interrupt) {
            UTILITY_Read_TSC(&itc);
            pid = GET_CURRENT_TGID();
            tid = current->pid;

            for (i = 0; i < event_mask.masks_num; i++) {
                desc_id   = COMPUTE_DESC_ID(DRV_EVENT_MASK_event_idx(&event_mask.eventmasks[i]));
                evt_desc  = desc_data[desc_id];
                psamp = (SampleRecordPC *)OUTPUT_Reserve_Buffer_Space(bd,
                                                 EVENT_DESC_sample_size(evt_desc));
                if (!psamp) {
                    continue;
                }

                // There could be fields in the sample which are not used;  therefore must zero.
                memset(psamp, 0, EVENT_DESC_sample_size(evt_desc));

                CPU_STATE_num_samples(pcpu)           += 1;
                /* Init bitfields. */
                SAMPLE_RECORD_cpu_and_os(psamp)        = 0;
                SAMPLE_RECORD_bit_fields2(psamp)       = 0;

                SAMPLE_RECORD_descriptor_id(psamp)     = desc_id;

                /* Build PC sample record based on addressing mode at the time of the
                   profile interrupt.

                   IPSR.is: 0=Itanium(R) processor, 1=IA32
                 */
                if (!(REGS_cr_ipsr(regs) & IA64_PSR_IS)) {
                    SAMPLE_RECORD_iip(psamp)           = REGS_cr_iip(regs);
                    SAMPLE_RECORD_ipsr(psamp)          = REGS_cr_ipsr(regs);
                    SAMPLE_RECORD_cs(psamp)            = 0;
                    SAMPLE_RECORD_ia64_pc(psamp)       = TRUE;
                }
                else {
                    unsigned long eflag, csd;

                    SAMPLE_RECORD_eip(psamp) = REGS_cr_iip(regs) & 0xffffffff;
                    asm("mov %0=ar.eflag;"  // get IA32 eflags
                            "mov %1=ar.csd;"    // get IA32 unscrambled code segment descriptor
                            :    "=r"(eflag), "=r"(csd));
                    SAMPLE_RECORD_eflags(psamp) = (U32) eflag;
                    SAMPLE_RECORD_csd(psamp).u1.lowWord  = csd;
                    SAMPLE_RECORD_csd(psamp).u2.highWord = csd >> 32;
                    SEP_PRINT_DEBUG("csd %lx\n", csd);
                    SAMPLE_RECORD_cs(psamp)            = (U32) regs->r17;
                    SAMPLE_RECORD_ia64_pc(psamp)       = FALSE;
                }

                SAMPLE_RECORD_cpu_num(psamp)           = (U16) this_cpu;
                SAMPLE_RECORD_tid(psamp)               = tid;
                SAMPLE_RECORD_pid_rec_index(psamp)     = pid;
                SAMPLE_RECORD_event_index(psamp)       = DRV_EVENT_MASK_event_idx(&event_mask.eventmasks[i]);
                SAMPLE_RECORD_pid_rec_index_raw(psamp) = 1;
                SAMPLE_RECORD_tsc(psamp)               = itc;

                if (!(REGS_cr_ipsr(regs) & IA64_PSR_IS)) {
                    SEP_PRINT_DEBUG("SAMPLE_RECORD_iip(psamp) %llx\n", SAMPLE_RECORD_iip(psamp));
                    SEP_PRINT_DEBUG("SAMPLE_RECORD_ipsr(psamp) %llx\n", SAMPLE_RECORD_ipsr(psamp));
                }
                else {
                    SEP_PRINT_DEBUG("SAMPLE_RECORD_eip(psamp) %x\n", SAMPLE_RECORD_eip(psamp));
                    SEP_PRINT_DEBUG("SAMPLE_RECORD_eflags(psamp) %x\n", SAMPLE_RECORD_eflags(psamp));
                    SEP_PRINT_DEBUG("SAMPLE_RECORD_csd(psamp).lowWord %x\n", SAMPLE_RECORD_csd(psamp).u1.lowWord);
                    SEP_PRINT_DEBUG("SAMPLE_RECORD_csd(psamp).highWord %x\n", SAMPLE_RECORD_csd(psamp).u2.highWord);
                }
                SEP_PRINT_DEBUG("SAMPLE_RECORD_cs(psamp) %x\n", SAMPLE_RECORD_cs(psamp));
                SEP_PRINT_DEBUG("SAMPLE_RECORD_cpu_num(psamp) %x\n", SAMPLE_RECORD_cpu_num(psamp));
                SEP_PRINT_DEBUG("SAMPLE_RECORD_tid(psamp) %x\n", SAMPLE_RECORD_tid(psamp));
                SEP_PRINT_DEBUG("SAMPLE_RECORD_pid_rec_index(psamp)  %x\n", SAMPLE_RECORD_pid_rec_index(psamp));

                if (DRV_CONFIG_collect_ro(pcfg)) {
                    if (DRV_EVENT_MASK_dear_capture(&event_mask.eventmasks[i]) == 1) {
                        DISPATCH_read_ro(((S8 *)(psamp) + EVENT_DESC_ro_offset(evt_desc)), EVENT_DESC_dear_offset(evt_desc), EVENT_DESC_dear_count(evt_desc));
                    }
                    if (DRV_EVENT_MASK_iear_capture(&event_mask.eventmasks[i]) == 1) {
                        DISPATCH_read_ro(((S8 *)(psamp) + EVENT_DESC_ro_offset(evt_desc)), EVENT_DESC_iear_offset(evt_desc), EVENT_DESC_iear_count(evt_desc));
                    }
                    if (DRV_EVENT_MASK_btb_capture(&event_mask.eventmasks[i]) == 1) {
                        DISPATCH_read_ro(((S8 *)(psamp) + EVENT_DESC_ro_offset(evt_desc)), EVENT_DESC_btb_offset(evt_desc), EVENT_DESC_btb_count(evt_desc));
                    }
                    if (DRV_EVENT_MASK_ipear_capture(&event_mask.eventmasks[i]) == 1) {
                        DISPATCH_read_ro(((S8 *)(psamp) + EVENT_DESC_ro_offset(evt_desc)), EVENT_DESC_ipear_offset(evt_desc), EVENT_DESC_ipear_count(evt_desc));
                    }
                }
            } // for
        }
    }

    // Re-enable the counter control
    dispatch->restart(NULL);

    atomic_set(&CPU_STATE_in_interrupt(&pcb[this_cpu]), 0);

    return;
}

#if defined(PERFMON_V2_ALT)

VOID
PMI_Interrupt_Handler (
  IN int             irq,
  IN void           *arg,
  IN struct pt_regs *regs
)
{
    pmi_Handler (irq, regs);

    return;
}

#elif defined(PERFMON_V2)

int
PMI_Interrupt_Handler (
    IN struct task_struct *task,
    IN void               *buf,
    IN pfm_ovfl_arg_t     *arg,
    IN struct pt_regs     *regs,
    IN unsigned long       stamp
)
{
    pmi_Handler (irq, regs);

    arg->ovfl_ctrl.bits.notify_user     = 0;
    arg->ovfl_ctrl.bits.block_task      = 0;
    arg->ovfl_ctrl.bits.mask_monitoring = 0;
    arg->ovfl_ctrl.bits.reset_ovfl_pmds = 1; /* Reset before returning from interrupt handler. */

    return (E_OS_OK);
}
#else

irqreturn_t
PMI_Interrupt_Handler (
    IN int             irq,
    IN void           *arg,
    IN struct pt_regs *regs
)
{
    pmi_Handler (irq, regs);

    return (irqreturn_t) IRQ_HANDLED;
}
#endif

#endif  // DRV_IA64
