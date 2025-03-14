#include "../nvme.h"
#include "./ftl.h"

static void hb_init_ctrl_str(FemuCtrl *n)
{
    static int fsid_vhb = 0;
    const char *vhbssd_mn = "FEMU Hybrid-SSD Controller";
    const char *vhbssd_sn = "vHSSD";

    nvme_set_ctrl_name(n, vhbssd_mn, vhbssd_sn, &fsid_vhb);
} // 用于初始化FEMU的黑盒模式控制器的名称和序列号

/* hb <=> hybrid */
static void hb_init(FemuCtrl *n, Error **errp)
{
    struct ssd *ssd = n->ssd = g_malloc0(sizeof(struct ssd));

    hb_init_ctrl_str(n);

    ssd->dataplane_started_ptr = &n->dataplane_started;
    ssd->ssdname = (char *)n->devname;
    femu_debug("Starting FEMU in Hybrid-SSD mode ...\n");
    hbssd_init(n);
} // FEMU启动Hybrid-black模式，初始化SSD设备，并为SSD设备设置名称和相关信息。

static void hb_flip(FemuCtrl *n, NvmeCmd *cmd)
{
    struct ssd *ssd = n->ssd;
    int64_t cdw10 = le64_to_cpu(cmd->cdw10);

    switch (cdw10) {
        case FEMU_ENABLE_GC_DELAY:
            ssd->sp.enable_gc_delay = true;
            femu_log("%s,FEMU GC Delay Emulation [Enabled]!\n", n->devname);
            break;
        case FEMU_DISABLE_GC_DELAY:
            ssd->sp.enable_gc_delay = false;
            femu_log("%s,FEMU GC Delay Emulation [Disabled]!\n", n->devname);
            break;
        case FEMU_ENABLE_DELAY_EMU:
            ssd->sp.slc_pg_rd_lat = Hybrid_SLC_READ_LATENCY;
            ssd->sp.tlc_lpg_rd_lat = Hybrid_TLC_LOWER_READ_LATENCY;
            ssd->sp.tlc_cpg_rd_lat = Hybrid_TLC_CENTER_READ_LATENCY;
            ssd->sp.tlc_upg_rd_lat = Hybrid_TLC_UPPER_READ_LATENCY;

            ssd->sp.slc_pg_wr_lat = Hybrid_SLC_PROG_LATENCY;
            ssd->sp.tlc_lpg_wr_lat = Hybrid_TLC_LOWER_PROG_LATENCY;
            ssd->sp.tlc_cpg_wr_lat = Hybrid_TLC_CENTER_PROG_LATENCY;
            ssd->sp.tlc_upg_wr_lat = Hybrid_TLC_UPPER_PROG_LATENCY;

            ssd->sp.slc_blk_er_lat = Hybrid_SLC_ERASE_LATENCY;
            ssd->sp.tlc_blk_er_lat = Hybrid_TLC_ERASE_LATENCY;

            ssd->sp.slc_chnl_xfer_lat = Hybrid_CHANNEL_TRANS_LATENCY;
            ssd->sp.tlc_chnl_pg_xfer_lat = Hybrid_CHANNEL_TRANS_LATENCY;
            femu_log("%s,FEMU Delay Emulation [Enabled]!\n", n->devname);
            break;
        case FEMU_DISABLE_DELAY_EMU:
            ssd->sp.slc_pg_rd_lat = 0;
            ssd->sp.tlc_lpg_rd_lat = 0;
            ssd->sp.tlc_cpg_rd_lat = 0;
            ssd->sp.tlc_upg_rd_lat = 0;

            ssd->sp.slc_pg_wr_lat = 0;
            ssd->sp.tlc_lpg_wr_lat = 0;
            ssd->sp.tlc_cpg_wr_lat = 0;
            ssd->sp.tlc_upg_wr_lat = 0;

            ssd->sp.slc_blk_er_lat = 0;
            ssd->sp.tlc_blk_er_lat = 0;

            ssd->sp.slc_chnl_xfer_lat = 0;
            ssd->sp.tlc_chnl_pg_xfer_lat = 0;
            femu_log("%s,FEMU Delay Emulation [Disabled]!\n", n->devname);
            break;
        case FEMU_RESET_ACCT:
            n->nr_tt_ios = 0; // 清空总体访问请求计数
            n->nr_tt_late_ios = 0; // 清空总体访问延迟计数
            femu_log("%s,Reset tt_late_ios/tt_ios,%lu/%lu\n", n->devname,
                n->nr_tt_late_ios, n->nr_tt_ios);
            break;
        case FEMU_ENABLE_LOG:
            n->print_log = true;
            femu_log("%s,Log print [Enabled]!\n", n->devname);
            break;
        case FEMU_DISABLE_LOG:
            n->print_log = false;
            femu_log("%s,Log print [Disabled]!\n", n->devname);
            break;
        default:
            printf("FEMU:%s,Not implemented flip cmd (%lu)\n", n->devname, cdw10);
    }
} // 实现针对admin cmd的处理函数

static uint16_t hb_nvme_rw(FemuCtrl *n, NvmeNamespace *ns, NvmeCmd *cmd, NvmeRequest *req)
{
    return nvme_rw(n, ns, cmd, req);
} // 在FEMU的Hybrid模式下进行NVMe读写操作

static uint16_t hb_io_cmd(FemuCtrl *n, NvmeNamespace *ns, NvmeCmd *cmd, NvmeRequest *req)
{
    switch (cmd->opcode) {
        case NVME_CMD_READ:
        case NVME_CMD_WRITE:
            return hb_nvme_rw(n, ns, cmd, req);
        default:
            return NVME_INVALID_OPCODE | NVME_DNR;
    }
} // 用于在FEMU的黑盒模式下进行NVMe I/O操作

static uint16_t hb_admin_cmd(FemuCtrl *n, NvmeCmd *cmd)
{
    switch (cmd->opcode) {
        case NVME_ADM_CMD_FEMU_FLIP:
            hb_flip(n, cmd);
            return NVME_SUCCESS;
        default:
            return NVME_INVALID_OPCODE | NVME_DNR;  
    }
} // 在FEMU的Hybrid模式下进行NVMe管理操作

int nvme_register_hbssd(FemuCtrl *n)
{
    n->ext_ops = (FemuExtCtrlOps) {
        .state            = NULL,
        .init             = hb_init,
        .exit             = NULL,
        .rw_check_req     = NULL,
        .admin_cmd        = hb_admin_cmd,
        .io_cmd           = hb_io_cmd,
        .get_log          = NULL,
    };

    return 0;
} // 注册Hybrid的SSD控制器
