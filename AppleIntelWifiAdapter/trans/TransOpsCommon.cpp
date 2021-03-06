//
//  TransOpsCommon.cpp
//  AppleIntelWifiAdapter
//
//  Created by 钟先耀 on 2020/2/5.
//  Copyright © 2020 钟先耀. All rights reserved.
//

#include "IWLTransOps.h"
#include "../fw/NotificationWait.hpp"


bool IWLTransOps::setHWRFKillState(bool state)
{
    bool rfkill_safe_init_done = trans->m_pDevice->rfkill_safe_init_done;
    bool unified = iwl_mvm_has_unified_ucode(trans->m_pDevice);
    if (state)
        set_bit(IWL_MVM_STATUS_HW_RFKILL, &trans->m_pDevice->status);
    else
        clear_bit(IWL_MVM_STATUS_HW_RFKILL, &trans->m_pDevice->status);
    
    //    iwl_mvm_set_rfkill_state(mvm);
    bool rfkill_state = iwl_mvm_is_radio_killed(trans->m_pDevice);
    if (rfkill_state) {
        IOLockWakeup(trans->m_pDevice->rx_sync_waitq, NULL, true);
    }
    
    
    /* iwl_run_init_mvm_ucode is waiting for results, abort it. */
    if (rfkill_safe_init_done)
                iwl_abort_notification_waits(&trans->m_pDevice->notif_wait);
        
    /*
     * Don't ask the transport to stop the firmware. We'll do it
     * after cfg80211 takes us down.
     */
        if (unified)
            return false;
    
    /*
     * Stop the device if we run OPERATIONAL firmware or if we are in the
     * middle of the calibrations.
     */
    return state && rfkill_safe_init_done;
}

void IWLTransOps::setRfKillState(bool state)
{
    IWL_WARN(0, "reporting RF_KILL (radio %s)\n",
             state ? "disabled" : "enabled");
    if (setHWRFKillState(state)) {
        stopDeviceDirectly();
    }
}

bool IWLTransOps::checkHWRFKill()
{
    bool hw_rfkill = trans->isRFKikkSet();
    bool prev = test_bit(STATUS_RFKILL_OPMODE, &trans->status);
    bool report;
    
    if (hw_rfkill) {
        set_bit(STATUS_RFKILL_HW, &trans->status);
        set_bit(STATUS_RFKILL_OPMODE, &trans->status);
    } else {
        clear_bit(STATUS_RFKILL_HW, &trans->status);
        if (trans->opmode_down)
            clear_bit(STATUS_RFKILL_OPMODE, &trans->status);
    }
    report = test_bit(STATUS_RFKILL_OPMODE, &trans->status);
    if (prev != report)
        setRfKillState(report);
    return hw_rfkill;
}

void IWLTransOps::irqRfKillHandle()
{
    struct isr_statistics *isr_stats = &trans->isr_stats;
    bool hw_rfkill, prev, report;
    
    IOLockLock(trans->mutex);
    prev = test_bit(STATUS_RFKILL_OPMODE, &trans->status);
    hw_rfkill = trans->isRFKikkSet();
    if (hw_rfkill) {
        set_bit(STATUS_RFKILL_OPMODE, &trans->status);
        set_bit(STATUS_RFKILL_HW, &trans->status);
    }
    if (trans->opmode_down)
        report = hw_rfkill;
    else
        report = test_bit(STATUS_RFKILL_OPMODE, &trans->status);
    
    IWL_WARN(0, "RF_KILL bit toggled to %s.\n",
             hw_rfkill ? "disable radio" : "enable radio");
    
    isr_stats->rfkill++;
    
    if (prev != report)
        setRfKillState(report);
    IOLockUnlock(trans->mutex);
    
    if (hw_rfkill) {
        if (test_and_clear_bit(STATUS_SYNC_HCMD_ACTIVE,
                               &trans->status))
            IWL_INFO(0,
                     "Rfkill while SYNC HCMD in flight\n");
        IOLockWakeup(trans->wait_command_queue, this, true);
    } else {
        clear_bit(STATUS_RFKILL_HW, &trans->status);
        if (trans->opmode_down)
            clear_bit(STATUS_RFKILL_OPMODE, &trans->status);
    }
}

void IWLTransOps::handleStopRFKill(bool was_in_rfkill)
{
    bool hw_rfkill;
    /*
     * Check again since the RF kill state may have changed while
     * all the interrupts were disabled, in this case we couldn't
     * receive the RF kill interrupt and update the state in the
     * op_mode.
     * Don't call the op_mode if the rkfill state hasn't changed.
     * This allows the op_mode to call stop_device from the rfkill
     * notification without endless recursion. Under very rare
     * circumstances, we might have a small recursion if the rfkill
     * state changed exactly now while we were called from stop_device.
     * This is very unlikely but can happen and is supported.
     */
    hw_rfkill = trans->isRFKikkSet();
    if (hw_rfkill) {
        set_bit(STATUS_RFKILL_HW, &trans->status);
        set_bit(STATUS_RFKILL_OPMODE, &trans->status);
    } else {
        clear_bit(STATUS_RFKILL_HW, &trans->status);
        clear_bit(STATUS_RFKILL_OPMODE, &trans->status);
    }
    if (hw_rfkill != was_in_rfkill)
        setRfKillState(hw_rfkill);
}

IWLTransOps::IWLTransOps(IWLTransport* trans) {
    this->trans = trans;
}

int IWLTransOps::startHW()
{
    int err;
    err = trans->prepareCardHW();
    if (err) {
        IWL_ERR(0, "Error while preparing HW: %d\n", err);
        return err;
    }
    err = trans->clearPersistenceBit();
    if (err) {
        return err;
    }
    trans->swReset();
    if ((err = forcePowerGating())) {
        return err;
    }
    err = apmInit();
    if (err) {
        return err;
    }
    trans->initMsix();
    
    
    /* From now on, the op_mode will be kept updated about RF kill state */
    trans->enableRFKillIntr();
    trans->opmode_down = false;
    /* Set is_down to false here so that...*/
    trans->is_down = false;
    checkHWRFKill();
    
    
    return 0;
}

bool IWLTransOps::fwRunning() {
    return test_bit(IWL_MVM_STATUS_FIRMWARE_RUNNING, &this->trans->status);
}

void IWLTransOps::sendRecoveryCmd(u32 flags) {
    u32 error_log_size = this->trans->m_pDevice->fw.ucode_capa.error_log_size;
    int ret;
    u32 resp;

    struct iwl_fw_error_recovery_cmd recovery_cmd = {
        .flags = cpu_to_le32(flags),
        .buf_size = 0,
    };
    
    struct iwl_host_cmd host_cmd = {
        .id = WIDE_ID(SYSTEM_GROUP, FW_ERROR_RECOVERY_CMD),
        .flags = CMD_WANT_SKB,
        .data = {&recovery_cmd, },
        .len = {sizeof(recovery_cmd), },
    };
    
    if (flags & ERROR_RECOVERY_UPDATE_DB) {
        /* no buf was allocated while HW reset */
        if (!trans->recovery_buf)
            return;

        host_cmd.data[1] = trans->recovery_buf;
        host_cmd.len[1] =  error_log_size;
        host_cmd.dataflags[1] = IWL_HCMD_DFL_NOCOPY;
        recovery_cmd.buf_size = cpu_to_le32(error_log_size);
    }

    ret = this->trans->sendCmd(&host_cmd);
    IOFree(trans->recovery_buf, error_log_size);
    trans->recovery_buf = NULL;

    if (ret) {
        IWL_ERR(mvm, "Failed to send recovery cmd %d\n", ret);
        return;
    }

    /* skb respond is only relevant in ERROR_RECOVERY_UPDATE_DB */
    if (flags & ERROR_RECOVERY_UPDATE_DB) {
        resp = le32_to_cpu(*(__le32 *)host_cmd.resp_pkt->data);
        if (resp)
            IWL_ERR(mvm,
                "Failed to send recovery cmd blob was invalid %d\n",
                resp);
    }
}

void IWLTransOps::restartNIC(bool fw_error) {
    
    //if(!this->trans->fw_resta)
    
    if(test_bit(IWL_MVM_STATUS_IN_HW_RESTART, &trans->m_pDevice->status)) {
        IWL_ERR(0, "failure while probing\n");
    } else if(test_bit(IWL_MVM_STATUS_HW_RESTART_REQUESTED, &trans->m_pDevice->status)) {
        IWL_WARN(0, "restart requested, but not started\n");
    } else if(trans->m_pDevice->cur_fw_img == IWL_UCODE_REGULAR &&
              !test_bit(STATUS_TRANS_DEAD, &trans->m_pDevice->status)) {
        if(trans->m_pDevice->fw.ucode_capa.error_log_size) {
            u32 src_size = trans->m_pDevice->fw.ucode_capa.error_log_size;
            u32 src_addr = trans->m_pDevice->fw.ucode_capa.error_log_addr;
            u8 *recover_buf = (u8*)IOMallocZero(src_size);

            if (recover_buf) {
                trans->recovery_buf = recover_buf;
                //mvm->error_recovery_buf = recover_buf;
               // iwl_trans_read_mem_bytes(mvm->trans,
               //              src_addr,
               //              recover_buf,
               //              src_size);
                trans->iwlReadMem(src_addr, &recover_buf, src_size);
            }
        }
        fwError();
        
        set_bit(IWL_MVM_STATUS_HW_RESTART_REQUESTED, &trans->m_pDevice->status);
        stopDevice();
        
        //enableDevice();
        
    }
}

static const struct {
    const char *name;
    uint8_t num;
} advanced_lookup[] = {
    { "NMI_INTERRUPT_WDG", 0x34 },
    { "SYSASSERT", 0x35 },
    { "UCODE_VERSION_MISMATCH", 0x37 },
    { "BAD_COMMAND", 0x38 },
    { "NMI_INTERRUPT_DATA_ACTION_PT", 0x3C },
    { "FATAL_ERROR", 0x3D },
    { "NMI_TRM_HW_ERR", 0x46 },
    { "NMI_INTERRUPT_TRM", 0x4C },
    { "NMI_INTERRUPT_BREAK_POINT", 0x54 },
    { "NMI_INTERRUPT_WDG_RXF_FULL", 0x5C },
    { "NMI_INTERRUPT_WDG_NO_RBD_RXF_FULL", 0x64 },
    { "NMI_INTERRUPT_HOST", 0x66 },
    { "NMI_INTERRUPT_ACTION_PT", 0x7C },
    { "NMI_INTERRUPT_UNKNOWN", 0x84 },
    { "NMI_INTERRUPT_INST_ACTION_PT", 0x86 },
    { "ADVANCED_SYSASSERT", 0 },
};

static const char *
iwm_desc_lookup(uint32_t num)
{
    int i;

    for (i = 0; i < ARRAY_SIZE(advanced_lookup) - 1; i++)
        if (advanced_lookup[i].num == num)
            return advanced_lookup[i].name;

    /* No entry matches 'num', so it is the last: ADVANCED_SYSASSERT */
    return advanced_lookup[i].name;
}

#define ERROR_START_OFFSET  (1 * sizeof(uint32_t))
#define ERROR_ELEM_SIZE     (7 * sizeof(uint32_t))

void print_umac(IWLTransport* trans) {
    struct iwl_umac_error_event_table t;
    uint32_t base;
    
    base = trans->m_pDevice->uc.uc_umac_error_event_table;
    
    if(base < 0x800000) {
        IWL_ERR(0, "Invalid error event table ptr: 0x%0x\n", base);
        return;
    }
    
    trans->iwlReadMem(base, &t, sizeof(t)/sizeof(uint32_t));
    
    if(!t.valid) {
        IWL_ERR(0, "Error log not found\n");
    }
    
    if (ERROR_START_OFFSET <= t.valid * ERROR_ELEM_SIZE) {
        IWL_ERR(0, "Start uMAC Error Log Dump:\n");
        IWL_ERR(0, "Status: 0x%x, count: %d\n",
            trans->m_pDevice->status, t.valid);
    }
    
    IWL_ERR(0, "0x%08X | %s\n", t.error_id,
        iwm_desc_lookup(t.error_id));
    IWL_ERR(0, "0x%08X | umac branchlink1\n", t.blink1);
    IWL_ERR(0, "0x%08X | umac branchlink2\n", t.blink2);
    IWL_ERR(0, "0x%08X | umac interruptlink1\n",
        t.ilink1);
    IWL_ERR(0, "0x%08X | umac interruptlink2\n",
        t.ilink2);
    IWL_ERR(0, "0x%08X | umac data1\n", t.data1);
    IWL_ERR(0, "0x%08X | umac data2\n", t.data2);
    IWL_ERR(0, "0x%08X | umac data3\n", t.data3);
    IWL_ERR(0, "0x%08X | umac major\n", t.umac_major);
    IWL_ERR(0, "0x%08X | umac minor\n", t.umac_minor);
    IWL_ERR(0, "0x%08X | frame pointer\n",
        t.frame_pointer);
    IWL_ERR(0, "0x%08X | stack pointer\n",
        t.stack_pointer);
    IWL_ERR(0, "0x%08X | last host cmd\n", t.cmd_header);
    IWL_ERR(0, "0x%08X | isr status reg\n",
        t.nic_isr_pref);
}

void IWLTransOps::fwError() {
    if(test_bit(STATUS_TRANS_DEAD, &trans->status)) {
        return;
    }
    
    uint32_t base;
    struct iwl_error_event_table t;
    
    IWL_ERR(0, "Dumping device error log\n");
    base = this->trans->m_pDevice->uc.uc_error_event_table;
    
    if(base < 0x800000) {
        IWL_ERR(0, "Invalid error event table ptr: 0x%0x\n", base);
        return;
    }
    
    trans->iwlReadMem(base, &t, sizeof(t)/sizeof(uint32_t));
    
    if(!t.valid) {
        IWL_ERR(0, "Error log not found\n");
    }
    
    if (ERROR_START_OFFSET <= t.valid * ERROR_ELEM_SIZE) {
        IWL_ERR(0, "Start Error Log Dump:\n");
        IWL_ERR(0, "Status: 0x%x, count: %d\n",
            this->trans->m_pDevice->status, t.valid);
    }

    IWL_ERR(0, "%08X | %-28s\n", t.error_id,
        iwm_desc_lookup(t.error_id));
    IWL_ERR(0, "%08X | trm_hw_status0\n",
        t.trm_hw_status0);
    IWL_ERR(0, "%08X | trm_hw_status1\n",
        t.trm_hw_status1);
    IWL_ERR(0, "%08X | branchlink2\n", t.blink2);
    IWL_ERR(0, "%08X | interruptlink1\n", t.ilink1);
    IWL_ERR(0, "%08X | interruptlink2\n", t.ilink2);
    IWL_ERR(0, "%08X | data1\n", t.data1);
    IWL_ERR(0, "%08X | data2\n", t.data2);
    IWL_ERR(0, "%08X | data3\n", t.data3);
    IWL_ERR(0, "%08X | beacon time\n", t.bcon_time);
    IWL_ERR(0, "%08X | tsf low\n", t.tsf_low);
    IWL_ERR(0, "%08X | tsf hi\n", t.tsf_hi);
    IWL_ERR(0, "%08X | time gp1\n", t.gp1);
    IWL_ERR(0, "%08X | time gp2\n", t.gp2);
    IWL_ERR(0, "%08X | uCode revision type\n",
        t.fw_rev_type);
    IWL_ERR(0, "%08X | uCode version major\n",
        t.major);
    IWL_ERR(0, "%08X | uCode version minor\n",
        t.minor);
    IWL_ERR(0, "%08X | hw version\n", t.hw_ver);
    IWL_ERR(0, "%08X | board version\n", t.brd_ver);
    IWL_ERR(0, "%08X | hcmd\n", t.hcmd);
    IWL_ERR(0, "%08X | isr0\n", t.isr0);
    IWL_ERR(0, "%08X | isr1\n", t.isr1);
    IWL_ERR(0, "%08X | isr2\n", t.isr2);
    IWL_ERR(0, "%08X | isr3\n", t.isr3);
    IWL_ERR(0, "%08X | isr4\n", t.isr4);
    IWL_ERR(0, "%08X | last cmd Id\n", t.last_cmd_id);
    IWL_ERR(0, "%08X | wait_event\n", t.wait_event);
    IWL_ERR(0, "%08X | l2p_control\n", t.l2p_control);
    IWL_ERR(0, "%08X | l2p_duration\n", t.l2p_duration);
    IWL_ERR(0, "%08X | l2p_mhvalid\n", t.l2p_mhvalid);
    IWL_ERR(0, "%08X | l2p_addr_match\n",
        t.l2p_addr_match);
    IWL_ERR(0, "%08X | lmpm_pmg_sel\n", t.lmpm_pmg_sel);
    IWL_ERR(0, "%08X | timestamp\n", t.u_timestamp);
    IWL_ERR(0, "%08X | flow_handler\n", t.flow_handler);

    if(this->trans->m_pDevice->uc.uc_umac_error_event_table)
        print_umac(trans);
    
    
}
