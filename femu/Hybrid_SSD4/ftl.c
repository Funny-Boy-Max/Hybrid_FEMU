#include "ftl.h"
#include <string.h>

// #define FEMU_DEBUG_FTL

static void *ftl_thread(void *arg);

static inline bool should_gc(struct ssd *ssd, int type)
{
    uint64_t current_t = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    switch (type) {
        case HB_READ_BUFFER:
            return ((current_t - ssd->last_access_t) > GC_Threshold_T && ssd->lm.rb_free_line_cnt <= ssd->sp.rb_gc_thres_lines);
        case HB_SLC:
            return ((current_t - ssd->last_access_t) > GC_Threshold_T && ssd->lm.slc_free_line_cnt <= ssd->sp.slc_gc_thres_lines);
        case HB_TLC:
            return ((current_t - ssd->last_access_t) > GC_Threshold_T && ssd->lm.tlc_free_line_cnt <= ssd->sp.tlc_gc_thres_lines);
        default:
            printf("Unsupported type !");
            return false;
    }
} // 判断是否要执行常态gc过程,当持续10s没有访问ssd，就判定可以发生gc

static inline bool should_gc_high(struct ssd *ssd, int type)
{
    switch (type) {
        case HB_READ_BUFFER:
            return (ssd->lm.rb_free_line_cnt <= ssd->sp.rb_gc_thres_lines_high);
            break;
        case HB_SLC:
            return (ssd->lm.slc_free_line_cnt <= ssd->sp.slc_gc_thres_lines_high);
            break;
        case HB_TLC:
            return (ssd->lm.tlc_free_line_cnt <= ssd->sp.tlc_gc_thres_lines_high);
            break;
        default:
            printf("Unsupported type !");
            exit(1);
            return false;
    }
} // 判断是否属于高压状态，即可用free line不多了为了后续不影响性能，需要释放无效空间（高压状态只执行gc，不执行其他操作）

static inline struct ppa get_maptbl_ent(struct ssd *ssd, uint64_t lpn)
{
    f_assert(lpn < ssd->sp.maptbl_len);
    return ssd->maptbl[lpn];
} // 获得逻辑页号对应的ssd片内物理地址

static inline struct ppa get_rb_maptbl_ent(struct ssd *ssd, uint64_t lpn)
{
    f_assert(lpn < ssd->sp.maptbl_len);
    return ssd->rb_maptbl[lpn];
} // 获得逻辑页号对应的ssd read buffer片内物理地址

static inline void set_maptbl_ent(struct ssd *ssd, uint64_t lpn, struct ppa *ppa)
{
    f_assert(lpn < ssd->sp.maptbl_len);
    f_assert(ppa->g.lun >= ssd->sp.rb_luns_per_ch);
    ssd->maptbl[lpn] = *ppa;
} // 设置页表映射，逻辑页号映射物理地址

static inline void set_rb_maptbl_ent(struct ssd *ssd, uint64_t lpn, struct ppa *ppa)
{
    f_assert(lpn < ssd->sp.maptbl_len);
    f_assert(ppa->g.lun < ssd->sp.rb_luns_per_ch);
    ssd->rb_maptbl[lpn] = *ppa;
} // 设置页表映射，逻辑页号映射read buffer物理地址

static uint64_t ppa2pgidx(struct ssd *ssd, struct ppa *ppa)
{
    struct Hybridparams *spp = &ssd->sp;
    uint64_t pgidx;

    if (ppa->g.lun < spp->rb_luns_per_ch) {
        pgidx = ppa->g.ch  * spp->pgs_per_ch     + \
                ppa->g.lun * spp->pgs_per_rb_lun + \
                ppa->g.pl  * spp->pgs_per_rb_pl  + \
                ppa->g.blk * spp->rb_pgs_per_blk + \
                ppa->g.pg;
    } else {
        if (ppa->g.blk < spp->slc_blks_per_pl) {
            pgidx = ppa->g.ch  * spp->pgs_per_ch      + \
                    (spp->rb_luns_per_ch * spp->pgs_per_rb_lun) + (ppa->g.lun - spp->rb_luns_per_ch) * spp->pgs_per_lun + \
                    ppa->g.pl  * spp->pgs_per_pl      + \
                    ppa->g.blk * spp->slc_pgs_per_blk + \
                    ppa->g.pg;
        } else {
            pgidx = ppa->g.ch  * spp->pgs_per_ch      + \
                    (spp->rb_luns_per_ch * spp->pgs_per_rb_lun) + (ppa->g.lun - spp->rb_luns_per_ch) * spp->pgs_per_lun + \
                    ppa->g.pl  * spp->pgs_per_pl      + \
                    (spp->slc_blks_per_pl * spp->slc_pgs_per_blk) + (ppa->g.blk - spp->slc_blks_per_pl) * spp->tlc_pgs_per_blk + \
                    ppa->g.pg;
        }
    }

    f_assert(pgidx < spp->tt_pgs);

    return pgidx;
} // 根据物理片内地址，得到反映射表中页面逻辑偏移

static inline uint64_t get_rmap_ent(struct ssd *ssd, struct ppa *ppa)
{
    uint64_t pgidx = ppa2pgidx(ssd, ppa);
    f_assert(pgidx < ssd->sp.tt_pgs);

    return ssd->rmap[pgidx];
} // 得到反映射表表项

/* set rmap[page_no(ppa)] -> lpn */
static inline void set_rmap_ent(struct ssd *ssd, uint64_t lpn, struct ppa *ppa)
{
    f_assert(lpn < ssd->sp.maptbl_len || lpn == INVALID_LPN);
    uint64_t pgidx = ppa2pgidx(ssd, ppa);
    f_assert(pgidx < ssd->sp.tt_pgs);

    ssd->rmap[pgidx] = lpn;
} // 构建反向映射表，根据ppa得到对应的物理页号偏移，偏移作为索引构建反映射

static inline int victim_line_cmp_pri(pqueue_pri_t next, pqueue_pri_t curr)
{
    return (next > curr);
} // 判断当前插入的线性块（line）和队列中的线性块的优先级大小关系

static inline pqueue_pri_t victim_line_get_pri(void *a)
{
    return ((struct line *)a)->vpc;
} // 获得当前有效页数目

static inline void victim_line_set_pri(void *a, pqueue_pri_t pri)
{
    ((struct line *)a)->vpc = pri;
} // 修改有效页数目

static inline size_t victim_line_get_pos(void *a)
{
    return ((struct line *)a)->pos;
} // 获取当前line在队列中的位置

static inline void victim_line_set_pos(void *a, size_t pos)
{
    ((struct line *)a)->pos = pos;
} // 设置line位置

static void ssd_init_lines(struct ssd *ssd) 
{
    struct Hybridparams *spp = &ssd->sp;
    struct line_mgmt *lm = &ssd->lm;
    struct line *line;

    /* 初始化line个数 */
    lm->rb_tt_lines = spp->rb_tt_lines;
    lm->slc_tt_lines = spp->slc_tt_lines;
    lm->tlc_tt_lines = spp->tlc_tt_lines;
    f_assert(lm->rb_tt_lines == spp->rb_blks_per_pl);
    f_assert(lm->slc_tt_lines == spp->slc_blks_per_pl);
    f_assert(lm->tlc_tt_lines == spp->tlc_blks_per_pl);
    /* 申请空间 */
    lm->rb_lines = g_malloc0(sizeof(struct line) * lm->rb_tt_lines);
    lm->slc_lines = g_malloc0(sizeof(struct line) * lm->slc_tt_lines);
    lm->tlc_lines = g_malloc0(sizeof(struct line) * lm->tlc_tt_lines);

    QTAILQ_INIT(&lm->rb_free_line_list); // 初始化空闲line双向链表
    QTAILQ_INIT(&lm->slc_free_line_list);
    QTAILQ_INIT(&lm->tlc_free_line_list);
    lm->rb_victim_line_pq = pqueue_init(spp->rb_tt_lines, victim_line_cmp_pri,
            victim_line_get_pri, victim_line_set_pri,
            victim_line_get_pos, victim_line_set_pos); // 初始化牺牲line队列，并定义操作函数
    lm->slc_victim_line_pq = pqueue_init(spp->slc_tt_lines, victim_line_cmp_pri,
            victim_line_get_pri, victim_line_set_pri,
            victim_line_get_pos, victim_line_set_pos);
    lm->tlc_victim_line_pq = pqueue_init(spp->tlc_tt_lines, victim_line_cmp_pri,
            victim_line_get_pri, victim_line_set_pri,
            victim_line_get_pos, victim_line_set_pos);
    QTAILQ_INIT(&lm->rb_full_line_list); // 初始化满line双向链表
    QTAILQ_INIT(&lm->slc_full_line_list);
    QTAILQ_INIT(&lm->tlc_full_line_list);

    lm->rb_free_line_cnt = 0;
    lm->slc_free_line_cnt = 0;
    lm->tlc_free_line_cnt = 0;

    /*初始化rb的管理line*/
    for (int i = 0; i < lm->rb_tt_lines; i++) {
        line = &lm->rb_lines[i];
        line->id = i; // lineID
        line->ipc = 0; // 无效页面数
        line->vpc = 0; // 有效页面数
        line->pos = 0;
        line->line_type = HB_READ_BUFFER; // rb
        /* initialize all the lines as free lines */
        QTAILQ_INSERT_TAIL(&lm->rb_free_line_list, line, entry);
        lm->rb_free_line_cnt++;
    }
    f_assert(lm->rb_free_line_cnt == lm->rb_tt_lines); // 调试
    lm->rb_victim_line_cnt = 0; // 设置牺牲line数量
    lm->rb_full_line_cnt = 0; // 设置满line数量

    /*初始化slc的管理line*/
    for (int i = 0; i < lm->slc_tt_lines; i++) {
        line = &lm->slc_lines[i];
        line->id = i;
        line->ipc = 0;
        line->vpc = 0;
        line->pos = 0;
        line->line_type = HB_SLC; // slc
        /* initialize all the lines as free lines */
        QTAILQ_INSERT_TAIL(&lm->slc_free_line_list, line, entry);
        lm->slc_free_line_cnt++;
    }
    f_assert(lm->slc_free_line_cnt == lm->slc_tt_lines);
    lm->slc_victim_line_cnt = 0;
    lm->slc_full_line_cnt = 0;

    /*初始化tlc的管理line*/
    for (int i = 0; i < lm->tlc_tt_lines; i++) {
        line = &lm->tlc_lines[i];
        line->id = i + lm->slc_tt_lines;
        line->ipc = 0;
        line->vpc = 0;
        line->pos = 0;
        line->line_type = HB_TLC; // tlc
        /* initialize all the lines as free lines */
        QTAILQ_INSERT_TAIL(&lm->tlc_free_line_list, line, entry);
        lm->tlc_free_line_cnt++;
    }
    f_assert(lm->tlc_free_line_cnt == lm->tlc_tt_lines);
    lm->tlc_victim_line_cnt = 0;
    lm->tlc_full_line_cnt = 0;
} // 初始化line管理结构，用于gc和写入【包括三个部分：read buffer层、slc层、tlc层】

static void ssd_init_write_pointer(struct ssd *ssd)
{
    struct write_pointer *rb_wpp = &ssd->rb_wp;
    struct write_pointer *slc_wpp = &ssd->slc_wp;
    struct write_pointer *tlc_wpp = &ssd->tlc_wp;
    struct line_mgmt *lm = &ssd->lm;
    struct line *curline = NULL;

    curline = QTAILQ_FIRST(&lm->rb_free_line_list); // 获取rb空闲line头
    QTAILQ_REMOVE(&lm->rb_free_line_list, curline, entry); // 移除当前line从free line集合中
    lm->rb_free_line_cnt--;
    /* wpp->curline is always our next-to-write super-block */
    rb_wpp->curline = curline;
    rb_wpp->ch = 0;
    rb_wpp->lun = 0;
    rb_wpp->pg = 0;
    rb_wpp->blk = curline->id;
    rb_wpp->pl = 0;
    f_assert(rb_wpp->blk < ssd->sp.rb_tt_lines);

    curline = QTAILQ_FIRST(&lm->slc_free_line_list);
    QTAILQ_REMOVE(&lm->slc_free_line_list, curline, entry); 
    lm->slc_free_line_cnt--;
    /* wpp->curline is always our next-to-write super-block */
    slc_wpp->curline = curline;
    slc_wpp->ch = 0;
    slc_wpp->lun = ssd->sp.rb_luns_per_ch;
    slc_wpp->pg = 0;
    slc_wpp->blk = curline->id;
    slc_wpp->pl = 0;
    f_assert(slc_wpp->blk < ssd->sp.slc_tt_lines);

    curline = QTAILQ_FIRST(&lm->tlc_free_line_list);
    QTAILQ_REMOVE(&lm->tlc_free_line_list, curline, entry);
    lm->tlc_free_line_cnt--;
    /* wpp->curline is always our next-to-write super-block */
    tlc_wpp->curline = curline;
    tlc_wpp->ch = 0;
    tlc_wpp->lun = ssd->sp.rb_luns_per_ch;
    tlc_wpp->pg = 0;
    tlc_wpp->blk = curline->id;
    tlc_wpp->pl = 0;
    f_assert(tlc_wpp->blk >= ssd->sp.slc_tt_lines && tlc_wpp->blk < ssd->sp.slc_tt_lines + ssd->sp.tlc_tt_lines);

    ssd->free_rb_pg_n = ssd->sp.tt_rb_pgs;
    ssd->free_slc_pg_n = ssd->sp.tt_slc_pgs;
    ssd->free_tlc_pg_n = ssd->sp.tt_tlc_pgs;

} // 初始化write point

static inline void check_addr(int a, int max)
{
    f_assert(a >= 0 && a < max);
} // 判断a是否超出范围

static struct line *get_next_free_line(struct ssd *ssd, int type)
{
    struct line_mgmt *lm = &ssd->lm;
    struct line *curline = NULL;

    switch (type) {
        case HB_READ_BUFFER:
            curline = QTAILQ_FIRST(&lm->rb_free_line_list);
            if (!curline) {
                ftl_err("No free read buffer lines left in [%s] !!!!\n", ssd->ssdname);
                return NULL;
            }

            QTAILQ_REMOVE(&lm->rb_free_line_list, curline, entry);
            lm->rb_free_line_cnt--;
            break;
        case HB_SLC:
            curline = QTAILQ_FIRST(&lm->slc_free_line_list);
            if (!curline) {
                ftl_err("No free slc lines left in [%s] !!!!\n", ssd->ssdname);
                return NULL;
            }

            QTAILQ_REMOVE(&lm->slc_free_line_list, curline, entry);
            lm->slc_free_line_cnt--;
            break;
        case HB_TLC:
            curline = QTAILQ_FIRST(&lm->tlc_free_line_list);
            if (!curline) {
                ftl_err("No free tlc lines left in [%s] !!!!\n", ssd->ssdname);
                return NULL;
            }

            QTAILQ_REMOVE(&lm->tlc_free_line_list, curline, entry);
            lm->tlc_free_line_cnt--;
            break;
    }
    return curline;
} // 用于SSD获取line队列中可用line

static void ssd_advance_write_pointer(struct ssd *ssd, int type)
{
    struct Hybridparams *spp = &ssd->sp;
    struct line_mgmt *lm = &ssd->lm;
    struct write_pointer *wpp = NULL;

    switch (type) {
        case HB_READ_BUFFER:
            wpp = &ssd->rb_wp;
            f_assert(wpp->lun >= 0 && wpp->lun < spp->rb_luns_per_ch);
            f_assert(wpp->blk < spp->rb_blks_per_pl);
            check_addr(wpp->ch, spp->nchs);
            wpp->ch++;
            if (wpp->ch == spp->nchs) {
                wpp->ch = 0;
                check_addr(wpp->lun, spp->rb_luns_per_ch);
                wpp->lun++;
                /* in this case, we should go to next lun */
                if (wpp->lun == spp->rb_luns_per_ch) {
                    wpp->lun = 0;
                    /* go to next page in the block */
                    check_addr(wpp->pg, spp->rb_pgs_per_blk);
                    wpp->pg++;
                    if (wpp->pg == spp->rb_pgs_per_blk) {
                        wpp->pg = 0;
                        /* move current line to {victim,full} line list */
                        if (wpp->curline->vpc == spp->rb_pgs_per_line) {
                            /* all pgs are still valid, move to full line list */
                            f_assert(wpp->curline->ipc == 0);
                            QTAILQ_INSERT_TAIL(&lm->rb_full_line_list, wpp->curline, entry);
                            lm->rb_full_line_cnt++;
                        } else {
                            f_assert(wpp->curline->vpc >= 0 && wpp->curline->vpc < spp->rb_pgs_per_line);
                            /* there must be some invalid pages in this line */
                            f_assert(wpp->curline->ipc > 0);
                            f_assert((wpp->curline->ipc + wpp->curline->vpc) == spp->rb_pgs_per_line);
                            pqueue_insert(lm->rb_victim_line_pq, wpp->curline);
                            lm->rb_victim_line_cnt++;
                        }
                        /* current line is used up, pick another empty line */
                        check_addr(wpp->blk, spp->rb_blks_per_pl);
                        wpp->curline = NULL;
                        wpp->curline = get_next_free_line(ssd, HB_READ_BUFFER);
                        if (!wpp->curline) {
                            /* TODO */
                            abort();
                        }
                        wpp->blk = wpp->curline->id;
                        check_addr(wpp->blk, spp->rb_blks_per_pl);
                        /* make sure we are starting from page 0 in the super block */
                        f_assert(wpp->pg == 0);
                        f_assert(wpp->lun == 0);
                        f_assert(wpp->ch == 0);
                        /* TODO: assume # of pl_per_lun is 1, fix later */
                        f_assert(wpp->pl == 0);
                    }
                }
            }
            break;
        case HB_SLC:
            wpp = &ssd->slc_wp;
            f_assert(wpp->lun >= spp->rb_luns_per_ch && wpp->lun < spp->rb_luns_per_ch + spp->luns_per_ch);
            f_assert(wpp->blk >= 0 && wpp->blk < spp->slc_blks_per_pl);
            check_addr(wpp->ch, spp->nchs);
            wpp->ch++;
            if (wpp->ch == spp->nchs) {
                wpp->ch = 0;
                check_addr(wpp->lun, spp->rb_luns_per_ch + spp->luns_per_ch);
                wpp->lun++;
                /* in this case, we should go to next lun */
                if (wpp->lun == spp->rb_luns_per_ch + spp->luns_per_ch) {
                    wpp->lun = spp->rb_luns_per_ch;
                    /* go to next page in the block */
                    check_addr(wpp->pg, spp->slc_pgs_per_blk);
                    wpp->pg++;
                    if (wpp->pg == spp->slc_pgs_per_blk) {
                        wpp->pg = 0;
                        /* move current line to {victim,full} line list */
                        if (wpp->curline->vpc == spp->slc_pgs_per_line) {
                            /* all pgs are still valid, move to full line list */
                            f_assert(wpp->curline->ipc == 0);
                            QTAILQ_INSERT_TAIL(&lm->slc_full_line_list, wpp->curline, entry);
                            lm->slc_full_line_cnt++;
                        } else {
                            f_assert(wpp->curline->vpc >= 0 && wpp->curline->vpc < spp->slc_pgs_per_line);
                            /* there must be some invalid pages in this line */
                            f_assert(wpp->curline->ipc > 0);
                            f_assert((wpp->curline->ipc + wpp->curline->vpc) == spp->slc_pgs_per_line);
                            pqueue_insert(lm->slc_victim_line_pq, wpp->curline);
                            lm->slc_victim_line_cnt++;
                        }
                        /* current line is used up, pick another empty line */
                        check_addr(wpp->blk, spp->slc_blks_per_pl);
                        wpp->curline = NULL;
                        wpp->curline = get_next_free_line(ssd, HB_SLC);
                        if (!wpp->curline) {
                            /* TODO */
                            abort();
                        }
                        wpp->blk = wpp->curline->id;
                        check_addr(wpp->blk, spp->slc_blks_per_pl);
                        /* make sure we are starting from page 0 in the super block */
                        f_assert(wpp->pg == 0);
                        f_assert(wpp->lun == spp->rb_luns_per_ch);
                        f_assert(wpp->ch == 0);
                        /* TODO: assume # of pl_per_lun is 1, fix later */
                        f_assert(wpp->pl == 0);
                    }
                }
            }
            break;
        case HB_TLC:
            wpp = &ssd->tlc_wp;
            f_assert(wpp->lun >= spp->rb_luns_per_ch && wpp->lun < spp->rb_luns_per_ch + spp->luns_per_ch);
            f_assert(wpp->blk >= spp->slc_blks_per_pl && wpp->blk < spp->slc_blks_per_pl + spp->tlc_blks_per_pl);
            check_addr(wpp->ch, spp->nchs);
            wpp->ch++;
            if (wpp->ch == spp->nchs) {
                wpp->ch = 0;
                check_addr(wpp->lun, spp->rb_luns_per_ch + spp->luns_per_ch);
                wpp->lun++;
                /* in this case, we should go to next lun */
                if (wpp->lun == spp->rb_luns_per_ch + spp->luns_per_ch) {
                    wpp->lun = spp->rb_luns_per_ch;
                    /* go to next page in the block */
                    check_addr(wpp->pg, spp->tlc_pgs_per_blk);
                    wpp->pg++;
                    if (wpp->pg == spp->tlc_pgs_per_blk) {
                        wpp->pg = 0;
                        /* move current line to {victim,full} line list */
                        if (wpp->curline->vpc == spp->tlc_pgs_per_line) {
                            /* all pgs are still valid, move to full line list */
                            f_assert(wpp->curline->ipc == 0);
                            QTAILQ_INSERT_TAIL(&lm->tlc_full_line_list, wpp->curline, entry);
                            lm->tlc_full_line_cnt++;
                        } else {
                            f_assert(wpp->curline->vpc >= 0 && wpp->curline->vpc < spp->tlc_pgs_per_line);
                            /* there must be some invalid pages in this line */
                            f_assert(wpp->curline->ipc > 0);
                            f_assert((wpp->curline->ipc + wpp->curline->vpc) == spp->tlc_pgs_per_line);
                            pqueue_insert(lm->tlc_victim_line_pq, wpp->curline);
                            lm->tlc_victim_line_cnt++;
                        }
                        /* current line is used up, pick another empty line */
                        check_addr(wpp->blk, spp->slc_blks_per_pl + spp->tlc_blks_per_pl);
                        wpp->curline = NULL;
                        wpp->curline = get_next_free_line(ssd, HB_TLC);
                        if (!wpp->curline) {
                            /* TODO */
                            abort();
                        }
                        wpp->blk = wpp->curline->id;
                        check_addr(wpp->blk, spp->slc_blks_per_pl + spp->tlc_blks_per_pl);
                        /* make sure we are starting from page 0 in the super block */
                        f_assert(wpp->pg == 0);
                        f_assert(wpp->lun == spp->rb_luns_per_ch);
                        f_assert(wpp->ch == 0);
                        /* TODO: assume # of pl_per_lun is 1, fix later */
                        f_assert(wpp->pl == 0);
                    }
                }
            }
            break;
    }
} // 获取新页【根据指定类型，去相应的ssd中提取新页】

static struct ppa get_new_page(struct ssd *ssd, int type)
{
    struct ppa ppa;
    struct write_pointer *wpp;
    switch (type) {
        case HB_READ_BUFFER:
            if (ssd->free_rb_pg_n > 0) {
                wpp = &ssd->rb_wp;
                ppa.ppa = 0;
                ppa.g.ch = (uint64_t)wpp->ch;
                ppa.g.lun = (uint64_t)wpp->lun;
                ppa.g.pg = (uint64_t)wpp->pg;
                ppa.g.blk = (uint64_t)wpp->blk;
                ppa.g.pl = (uint64_t)wpp->pl;
                f_assert(ppa.g.pl == 0);
                f_assert(ppa.g.ch < ssd->sp.nchs);
                f_assert(ppa.g.lun < ssd->sp.rb_luns_per_ch);
                f_assert(ppa.g.pg < ssd->sp.rb_pgs_per_blk);
                f_assert(ppa.g.blk >= 0 && ppa.g.blk < ssd->sp.rb_blks_per_pl);
            } else {
                printf("The number of free read buffer page isn't enough!\n");
                ppa.ppa = UNMAPPED_PPA;
            }
            break;
        case HB_SLC:
            if (ssd->free_slc_pg_n > 0) {
                wpp = &ssd->slc_wp;
                ppa.ppa = 0;
                ppa.g.ch = (uint64_t)wpp->ch;
                ppa.g.lun = (uint64_t)wpp->lun;
                ppa.g.pg = (uint64_t)wpp->pg;
                ppa.g.blk = (uint64_t)wpp->blk;
                ppa.g.pl = (uint64_t)wpp->pl;
                f_assert(ppa.g.pl == 0);
                f_assert(ppa.g.ch < ssd->sp.nchs);
                f_assert(ppa.g.lun >= ssd->sp.rb_luns_per_ch && ppa.g.lun < ssd->sp.rb_luns_per_ch + ssd->sp.luns_per_ch);
                f_assert(ppa.g.pg < ssd->sp.slc_pgs_per_blk);
                f_assert(ppa.g.blk >= 0 && ppa.g.blk < ssd->sp.slc_blks_per_pl);
            } else {
                printf("The number of free SLC page isn't enough!\n");
                if (ssd->free_tlc_pg_n > 0) {
                    wpp = &ssd->tlc_wp;
                    ppa.ppa = 0;
                    ppa.g.ch = (uint64_t)wpp->ch;
                    ppa.g.lun = (uint64_t)wpp->lun;
                    ppa.g.pg = (uint64_t)wpp->pg;
                    ppa.g.blk = (uint64_t)wpp->blk;
                    ppa.g.pl = (uint64_t)wpp->pl;
                    f_assert(ppa.g.pl == 0);
                    f_assert(ppa.g.ch < ssd->sp.nchs);
                    f_assert(ppa.g.lun >= ssd->sp.rb_luns_per_ch && ppa.g.lun < ssd->sp.rb_luns_per_ch + ssd->sp.luns_per_ch);
                    f_assert(ppa.g.pg < ssd->sp.tlc_pgs_per_blk);
                    f_assert(ppa.g.blk >= ssd->sp.slc_blks_per_pl && ppa.g.blk < ssd->sp.slc_blks_per_pl + ssd->sp.tlc_blks_per_pl);
                } else {
                    printf("The number of free TLC page isn't enough!\n");
                    ppa.ppa = UNMAPPED_PPA;
                }
            }
            break;
        case HB_TLC:
            if (ssd->free_tlc_pg_n > 0) {
                wpp = &ssd->tlc_wp;
                ppa.ppa = 0;
                ppa.g.ch = (uint64_t)wpp->ch;
                ppa.g.lun = (uint64_t)wpp->lun;
                ppa.g.pg = (uint64_t)wpp->pg;
                ppa.g.blk = (uint64_t)wpp->blk;
                ppa.g.pl = (uint64_t)wpp->pl;
                f_assert(ppa.g.pl == 0);
                f_assert(ppa.g.ch < ssd->sp.nchs);
                f_assert(ppa.g.lun >= ssd->sp.rb_luns_per_ch && ppa.g.lun < ssd->sp.rb_luns_per_ch + ssd->sp.luns_per_ch);
                f_assert(ppa.g.pg < ssd->sp.tlc_pgs_per_blk);
                f_assert(ppa.g.blk >= ssd->sp.slc_blks_per_pl && ppa.g.blk < ssd->sp.slc_blks_per_pl + ssd->sp.tlc_blks_per_pl);
            } else {
                printf("The number of free TLC page isn't enough!\n");
                ppa.ppa = UNMAPPED_PPA;
            }
            break;
    }
    return ppa;
} // 根据需求获取相应的空闲页面

static void check_params(struct Hybridparams *spp)
{
    /*
     * we are using a general write pointer increment method now, no need to
     * force luns_per_ch and nchs to be power of 2
     */

    //f_assert(is_power_of_2(spp->luns_per_ch));
    //f_assert(is_power_of_2(spp->nchs));
} // 判断初始化是否成功

static void ssd_init_params(struct Hybridparams *spp)
{
    /*一个channel中有read buffer LUN和普通LUN*/
    spp->secsz = 512; // 扇区大小512B
    spp->secs_per_pg = 8; // 页面包含8个扇区，即4KB页面大小
    spp->rb_pgs_per_blk = 32; // read buffer本质是SLC，用于做缓存
    spp->slc_pgs_per_blk = 32; // SLC block包含128个页面
    spp->tlc_pgs_per_blk = 384; // TLC block包含384个页面
    spp->rb_blks_per_pl = 6; // read buffer的LUN中一个pl有4个block
    spp->slc_blks_per_pl = 6; // 一个pl中有88个SLC block
    spp->tlc_blks_per_pl = 200; // 一个pl中有676个TLC block
    spp->rb_pls_per_lun = 1; // read buffer LUN中一个逻辑单元对应一个plane
    spp->pls_per_lun = 1; // LUN中一个逻辑单元对应一个plane
    spp->rb_luns_per_ch = 16; // 这里合并了chip(4)和die(4)层
    spp->luns_per_ch = 16; // 这里合并了chip(4)和die(4)层
    spp->nchs = 8;

    spp->waterline = 50;
    /* 设置预测相关参数 */
    spp->nhistorys = 1000;
    spp->class_flag = 0;
    spp->max_nclass = 1000;
    spp->length_vq = 20;
    spp->test_flag = false;

    spp->num = 0;
    spp->hit_num = 0;
    spp->rb_hit_num = 0;
    spp->slc_hit_num = 0;
    spp->tlc_hit_num = 0;
    spp->rb_gc_count = 0;
    spp->slc_gc_count = 0;
    spp->tlc_gc_count = 0;
    spp->w_num = 0;
    spp->rw_num = 0;
    spp->rb_w_num = 0;
    spp->slc_w_num = 0;
    spp->tlc_w_num = 0;
    spp->r_num = 0;
    spp->rr_num = 0;
    spp->rb_r_num = 0;
    spp->slc_r_num = 0;
    spp->tlc_r_num = 0;
    spp->pre_count = 0;
    spp->pre_hit_num = 0;

    /* Initialization delay time. */
    spp->tlc_upg_rd_lat = Hybrid_TLC_UPPER_READ_LATENCY;
    spp->tlc_cpg_rd_lat = Hybrid_TLC_CENTER_READ_LATENCY;
    spp->tlc_lpg_rd_lat = Hybrid_TLC_LOWER_READ_LATENCY;

    spp->tlc_upg_wr_lat = Hybrid_TLC_UPPER_PROG_LATENCY;
    spp->tlc_cpg_wr_lat = Hybrid_TLC_CENTER_PROG_LATENCY;
    spp->tlc_lpg_wr_lat = Hybrid_TLC_LOWER_PROG_LATENCY;

    spp->tlc_blk_er_lat = Hybrid_TLC_ERASE_LATENCY;

    spp->tlc_chnl_pg_xfer_lat = Hybrid_CHANNEL_TRANS_LATENCY;

    spp->slc_pg_rd_lat = Hybrid_SLC_READ_LATENCY;
    spp->slc_pg_wr_lat = Hybrid_SLC_PROG_LATENCY;
    spp->slc_blk_er_lat = Hybrid_SLC_ERASE_LATENCY;
    spp->slc_chnl_xfer_lat = Hybrid_CHANNEL_TRANS_LATENCY;

    spp->dram_pg_rd_lat = DRAM_READ_LATENCY;
    spp->dram_pg_wr_lat = DRAM_WRITE_LATENCY;

    /* calculated values */ // 计算相关参数
    spp->secs_per_rb_blk = spp->secs_per_pg * spp->rb_pgs_per_blk;
    spp->secs_per_slc_blk = spp->secs_per_pg * spp->slc_pgs_per_blk;
    spp->secs_per_tlc_blk = spp->secs_per_pg * spp->tlc_pgs_per_blk;
    spp->secs_per_rb_pl = spp->secs_per_rb_blk * spp->rb_blks_per_pl;
    spp->secs_per_pl = spp->secs_per_slc_blk * spp->slc_blks_per_pl + spp->secs_per_tlc_blk * spp->tlc_blks_per_pl;
    spp->secs_per_rb_lun = spp->secs_per_rb_pl * spp->rb_pls_per_lun;
    spp->secs_per_lun = spp->secs_per_pl * spp->pls_per_lun;
    spp->secs_per_ch = spp->secs_per_rb_lun * spp->rb_luns_per_ch + spp->secs_per_lun * spp->luns_per_ch;
    spp->tt_secs = spp->secs_per_ch * spp->nchs;

    spp->tt_rb_secs = spp->secs_per_rb_lun * spp->rb_luns_per_ch * spp->nchs;
    spp->tt_slc_secs = spp->secs_per_slc_blk * spp->slc_blks_per_pl * spp->pls_per_lun * spp->luns_per_ch * spp->nchs;
    spp->tt_tlc_secs = spp->secs_per_tlc_blk * spp->tlc_blks_per_pl * spp->pls_per_lun * spp->luns_per_ch * spp->nchs;
    spp->tt_store_secs = spp->tt_slc_secs + spp->tt_tlc_secs;

    spp->pgs_per_rb_pl = spp->rb_pgs_per_blk * spp->rb_blks_per_pl;
    spp->pgs_per_pl = spp->slc_pgs_per_blk * spp->slc_blks_per_pl + spp->tlc_pgs_per_blk * spp->tlc_blks_per_pl;
    spp->pgs_per_rb_lun = spp->pgs_per_rb_pl * spp->rb_pls_per_lun;
    spp->pgs_per_lun = spp->pgs_per_pl * spp->pls_per_lun;
    spp->pgs_per_ch = spp->pgs_per_rb_lun * spp->rb_luns_per_ch + spp->pgs_per_lun * spp->luns_per_ch;
    spp->tt_pgs = spp->pgs_per_ch * spp->nchs;

    spp->tt_rb_pgs = spp->pgs_per_rb_lun * spp->rb_luns_per_ch * spp->nchs;
    spp->tt_slc_pgs = spp->slc_pgs_per_blk * spp->slc_blks_per_pl * spp->pls_per_lun * spp->luns_per_ch * spp->nchs;
    spp->tt_tlc_pgs = spp->tlc_pgs_per_blk * spp->tlc_blks_per_pl * spp->pls_per_lun * spp->luns_per_ch * spp->nchs;
    spp->tt_store_pgs = spp->tt_slc_pgs + spp->tt_tlc_pgs;
    spp->maptbl_len = 250000000;

    spp->blks_per_rb_lun = spp->rb_blks_per_pl * spp->rb_pls_per_lun;
    spp->blks_per_lun = (spp->slc_blks_per_pl + spp->tlc_blks_per_pl) * spp->pls_per_lun;
    spp->blks_per_ch = spp->blks_per_rb_lun * spp->rb_luns_per_ch + spp->blks_per_lun * spp->luns_per_ch;
    spp->tt_blks = spp->blks_per_ch * spp->nchs;

    spp->tt_rb_blks = spp->blks_per_rb_lun * spp->rb_luns_per_ch * spp->nchs;
    spp->tt_slc_blks = spp->slc_blks_per_pl * spp->pls_per_lun * spp->luns_per_ch * spp->nchs;
    spp->tt_tlc_blks = spp->tlc_blks_per_pl * spp->pls_per_lun * spp->luns_per_ch * spp->nchs;
    spp->tt_store_blks = spp->tt_slc_blks + spp->tt_tlc_blks;

    spp->rb_pls_per_ch = spp->rb_pls_per_lun * spp->rb_luns_per_ch;
    spp->pls_per_ch = spp->pls_per_lun * spp->luns_per_ch;
    spp->tt_rb_pls = spp->rb_pls_per_ch * spp->nchs;
    spp->tt_pls = spp->pls_per_ch * spp->nchs;

    spp->tt_rb_luns = spp->rb_luns_per_ch * spp->nchs;
    spp->tt_luns = spp->luns_per_ch * spp->nchs;

    /* line is special, put it at the end */
    spp->rb_blks_per_line = spp->tt_rb_luns;
    spp->slc_blks_per_line = spp->tt_luns;
    spp->tlc_blks_per_line = spp->tt_luns;
    spp->rb_pgs_per_line = spp->rb_pgs_per_blk * spp->rb_blks_per_line;
    spp->slc_pgs_per_line = spp->slc_pgs_per_blk * spp->slc_blks_per_line;
    spp->tlc_pgs_per_line = spp->tlc_pgs_per_blk * spp->tlc_blks_per_line;
    spp->rb_secs_per_line = spp->secs_per_pg * spp->rb_pgs_per_line;
    spp->slc_secs_per_line = spp->secs_per_pg * spp->slc_pgs_per_line;
    spp->tlc_secs_per_line = spp->secs_per_pg * spp->tlc_pgs_per_line;
    spp->rb_tt_lines = spp->rb_blks_per_pl * spp->rb_pls_per_lun;
    spp->slc_tt_lines = spp->slc_blks_per_pl * spp->pls_per_lun;
    spp->tlc_tt_lines = spp->tlc_blks_per_pl * spp->pls_per_lun;

    spp->gc_thres_t = GC_Threshold_T;
    spp->gc_thres_pcent = 0.6;
    spp->gc_thres_pcent_high = 0.95;
    spp->rb_gc_thres_lines = (int)((1 - spp->gc_thres_pcent) * spp->rb_tt_lines + 0.999);
    spp->slc_gc_thres_lines = (int)((1 - spp->gc_thres_pcent) * spp->slc_tt_lines + 0.999);
    spp->tlc_gc_thres_lines = (int)((1 - spp->gc_thres_pcent) * spp->tlc_tt_lines + 0.999);
    spp->rb_gc_thres_lines_high = (int)((1 - spp->gc_thres_pcent_high) * spp->rb_tt_lines);
    spp->slc_gc_thres_lines_high = (int)((1 - spp->gc_thres_pcent_high) * spp->slc_tt_lines);
    spp->tlc_gc_thres_lines_high = (int)((1 - spp->gc_thres_pcent_high) * spp->tlc_tt_lines);
    spp->enable_gc_delay = true;

    check_params(spp);

    spp->SLC_lru_space_len = 513;
    spp->SLC_lru_capacity = 513;
    spp->DRAM_lru_space_len = 51;
    spp->DRAM_lru_capacity = 51;
} // 混合SSD的基本架构参数

static void ssd_init_hybrid_page(struct Hybrid_page *pg, struct Hybridparams *spp, int page_type)
{
    pg->nsecs = spp->secs_per_pg;
    pg->sec = g_malloc0(sizeof(Hybrid_sec_status_t) * pg->nsecs);
    for (int i = 0; i < pg->nsecs; i++) {
        pg->sec[i] = SEC_FREE;
    }
    pg->status = PG_FREE;
    pg->page_type = page_type; // slc:0 lsb:1 csb:2 msb:3
    pg->is_inlru = 0;
} // 根据页面类型，初始化Flash页面

static void ssd_init_hybrid_blk(struct Hybrid_block *blk, struct Hybridparams *spp, int blk_type)
{
    switch (blk_type) {
        case HB_READ_BUFFER:
            blk->npgs = spp->rb_pgs_per_blk;
            blk->pg = g_malloc0(sizeof(struct Hybrid_page) * blk->npgs);
            for (int i = 0; i < blk->npgs; i++) {
                ssd_init_hybrid_page(&blk->pg[i], spp, 0);
            }
            blk->ipc = 0;
            blk->vpc = 0;
            blk->erase_cnt = 0;
            blk->wp = 0;
            blk->blk_type = blk_type; // rb:0 slc:1 tlc:2
            break;
        case HB_SLC:
            blk->npgs = spp->slc_pgs_per_blk;
            blk->pg = g_malloc0(sizeof(struct Hybrid_page) * blk->npgs);
            for (int i = 0; i < blk->npgs; i++) {
                ssd_init_hybrid_page(&blk->pg[i], spp, 0);
            }
            blk->ipc = 0;
            blk->vpc = 0;
            blk->erase_cnt = 0;
            blk->wp = 0;
            blk->blk_type = blk_type;
            break;
        case HB_TLC:
            blk->npgs = spp->tlc_pgs_per_blk;
            blk->pg = g_malloc0(sizeof(struct Hybrid_page) * blk->npgs);
            for (int i = 0; i < blk->npgs; i++) {
                ssd_init_hybrid_page(&blk->pg[i], spp, (int)((i%3)+1));
            }
            blk->ipc = 0;
            blk->vpc = 0;
            blk->erase_cnt = 0;
            blk->wp = 0;
            blk->blk_type = blk_type;
            break;
    }
} // 初始化Hybrid Flash Block

static void ssd_init_hybrid_plane(void *pl, struct Hybridparams *spp, int type)
{
    struct Hybrid_rb_plane *rb_p = NULL;
    struct Hybrid_plane *p = NULL;
    if (type == HB_READ_BUFFER) {
        rb_p = (struct Hybrid_rb_plane *)pl;
        rb_p->nrbblks = spp->rb_blks_per_pl;
        rb_p->rb_blk = g_malloc0(sizeof(struct Hybrid_block) * rb_p->nrbblks);

        for (int i = 0; i < rb_p->nrbblks; i++) {
            ssd_init_hybrid_blk(&rb_p->rb_blk[i], spp, HB_READ_BUFFER);
        }
    } else {
        p = (struct Hybrid_plane *)pl;
        p->nslcblks = spp->slc_blks_per_pl;
        p->ntlcblks = spp->tlc_blks_per_pl;
        p->slc_blk = g_malloc0(sizeof(struct Hybrid_block) * p->nslcblks);
        p->tlc_blk = g_malloc0(sizeof(struct Hybrid_block) * p->ntlcblks);

        for (int i = 0; i < p->nslcblks; i++) {
            ssd_init_hybrid_blk(&p->slc_blk[i], spp, HB_SLC);
        }

        for (int i = 0; i < p->ntlcblks; i++) {
            ssd_init_hybrid_blk(&p->tlc_blk[i], spp, HB_TLC);
        }
    }
} // 初始化Hybrid Flash plane

static void ssd_init_hybrid_lun(void *lun, struct Hybridparams *spp, int type)
{
    struct Hybrid_rb_lun *rb_l = NULL;
    struct Hybrid_lun *l = NULL;
    if (type == HB_READ_BUFFER) {
        rb_l = (struct Hybrid_rb_lun *)lun;
        rb_l->npls = spp->rb_pls_per_lun;
        rb_l->pl = g_malloc0(sizeof(struct Hybrid_rb_plane) * rb_l->npls);

        for (int i = 0; i < rb_l->npls; i++) {
            ssd_init_hybrid_plane(&rb_l->pl[i], spp, HB_READ_BUFFER);
        }
        rb_l->next_lun_avail_time = 0;
        rb_l->gc_endtime = 0;
        rb_l->busy = false;
    } else {
        l = (struct Hybrid_lun *)lun;
        l->npls = spp->pls_per_lun;
        l->pl = g_malloc0(sizeof(struct Hybrid_plane) * l->npls);

        for (int i = 0; i < l->npls; i++) {
            ssd_init_hybrid_plane(&l->pl[i], spp, 1);
        }
        l->next_lun_avail_time = 0;
        l->gc_endtime = 0;
        l->busy = false;
    }
} // 初始化Hybrid Flash LUN

static void ssd_init_hybrid_ch(struct Hybrid_channel *ch, struct Hybridparams *spp)
{
    ch->nrb_luns = spp->rb_luns_per_ch;
    ch->nluns = spp->luns_per_ch;
    ch->rb_lun = g_malloc(sizeof(struct Hybrid_rb_lun) * ch->nrb_luns);
    ch->lun = g_malloc0(sizeof(struct Hybrid_lun) * ch->nluns);

    for (int i = 0; i < ch->nrb_luns; i++) {
        ssd_init_hybrid_lun(&ch->rb_lun[i], spp, HB_READ_BUFFER);
    }
    for (int i = 0; i < ch->nluns; i++) {
        ssd_init_hybrid_lun(&ch->lun[i], spp, 1);
    }

    ch->next_ch_avail_time = 0;
    ch->next_ch2_avail_time = 0;
    ch->gc_endtime = 0;
    ch->busy = false;
} // 初始化Hybrid Flash channel

static void ssd_init_maptbl(struct ssd *ssd)
{
    struct Hybridparams *spp = &ssd->sp;

    ssd->maptbl = g_malloc0(sizeof(struct ppa) * spp->maptbl_len);
    for (int i = 0; i < spp->maptbl_len; i++) {
        ssd->maptbl[i].ppa = UNMAPPED_PPA;
    }

    ssd->rb_maptbl = g_malloc0(sizeof(struct ppa) * spp->maptbl_len);
    for (int i = 0; i < spp->maptbl_len; i++) {
        ssd->rb_maptbl[i].ppa = UNMAPPED_PPA;
    }
} // 初始化Hybrid SSD的逻辑地址到物理地址的映射表（mapping table）

static void ssd_init_rmap(struct ssd *ssd)
{
    struct Hybridparams *spp = &ssd->sp;

    ssd->rmap = g_malloc(sizeof(uint64_t) * spp->tt_pgs);
    for (int i = 0; i < spp->tt_pgs; i++) {
        ssd->rmap[i] = INVALID_LPN;
    }
} // 初始化Hybrid SSD的物理地址到逻辑地址的映射表（reverse mapping table）

static void ssd_init_access_history(struct ssd *ssd)
{
    struct Hybridparams *spp = &ssd->sp;
    ssd->access_history = g_malloc0(sizeof(struct history_entry) * spp->nhistorys);
} /* Initialize Historical Tracking Queue. */

static void ssd_init_root(struct ssd *ssd)
{
    ssd->root = NULL;
}

static void ssd_init_address_cs(struct ssd *ssd)
{
    struct Hybridparams *spp = &ssd->sp;
    ssd->address_cs = g_malloc0(sizeof(struct class_space));
    ssd->address_cs->total_num = spp->max_nclass;
    ssd->address_cs->used_num = 0;
    ssd->address_cs->head = g_malloc0(sizeof(struct address_class) * ssd->address_cs->total_num);
} // 初始化class_space

static void ssd_init_vqueue(struct ssd *ssd)
{
    struct Hybridparams *spp = &ssd->sp;
    ssd->vqueue = g_malloc0(sizeof(struct view_queue));
    ssd->vqueue->total_num = spp->length_vq;
    ssd->vqueue->used_num = 0;
    ssd->vqueue->head = g_malloc0(sizeof(struct circulate_entry) * ssd->vqueue->total_num);
} // 初始化观测队列

static void init_slc_lru(struct ssd *ssd)
{
    struct Hybridparams *spp = &ssd->sp;
    struct lpn_lru *SLC_LRU = &ssd->SLC_LRU;
    struct one_page *one_page;
    SLC_LRU->space_length = spp->SLC_lru_space_len;
    SLC_LRU->capacity = spp->SLC_lru_capacity;

    SLC_LRU->length = 0;
    SLC_LRU->free_cnt = 0;

    SLC_LRU->one_page_space = g_malloc0(sizeof(struct one_page) * SLC_LRU->space_length);
    SLC_LRU->hash_map = g_malloc0(sizeof(struct one_page*) * SLC_LRU->capacity);
    SLC_LRU->head = SLC_LRU->tail = NULL;
    QTAILQ_INIT(&SLC_LRU->sl_free_page_list);

    /* 初始化one_page空间 */
    for (int i = 0; i < SLC_LRU->space_length; i++) {
        one_page = &SLC_LRU->one_page_space[i];
        one_page->prev = NULL;
        one_page->next = NULL;
        one_page->lpn = INVALID_LPN;
        one_page->id = i;
        one_page->hash_next = NULL;
        one_page->hash_pre = NULL;
        QTAILQ_INSERT_TAIL(&SLC_LRU->sl_free_page_list, one_page, entry);
        SLC_LRU->free_cnt++;
    }
    f_assert(SLC_LRU->free_cnt == spp->SLC_lru_space_len);
    /* 初始化hash_map空间 */
    for (int i = 0; i < SLC_LRU->capacity; i++) {
        SLC_LRU->hash_map[i] = NULL;
    }
} // 初始化LPN LRU

static void init_dram_lru(struct ssd *ssd)
{
    struct Hybridparams *spp = &ssd->sp;
    struct node_lru *DRAM_LRU = &ssd->DRAM_LRU;
    struct dram_node *dram_node;
    DRAM_LRU->space_length = spp->DRAM_lru_space_len;
    DRAM_LRU->capacity = spp->DRAM_lru_capacity;

    DRAM_LRU->length = 0;
    DRAM_LRU->free_cnt = 0;

    DRAM_LRU->node_space = g_malloc0(sizeof(struct dram_node) * DRAM_LRU->space_length);
    DRAM_LRU->hash_map = g_malloc0(sizeof(struct dram_node*) * DRAM_LRU->capacity);
    DRAM_LRU->head = DRAM_LRU->tail = NULL;
    QTAILQ_INIT(&DRAM_LRU->dl_free_page_list);

    /* 初始化dram_node空间 */
    for (int i = 0; i < DRAM_LRU->space_length; i++) {
        dram_node = &DRAM_LRU->node_space[i];
        dram_node->prev = NULL;
        dram_node->next = NULL;
        dram_node->lpn = INVALID_LPN;
        dram_node->next_avail_time = 0;
        dram_node->begin_state = false;
        dram_node->pre_state = false;
        dram_node->access_flag = false;
        dram_node->access_count = 0;
        dram_node->id = i;
        dram_node->hash_pre = NULL;
        dram_node->hash_next = NULL;
        QTAILQ_INSERT_TAIL(&DRAM_LRU->dl_free_page_list, dram_node, entry);
        DRAM_LRU->free_cnt++;
    }
    f_assert(DRAM_LRU->free_cnt == spp->DRAM_lru_space_len);
    /* 初始化hash_map空间 */
    for (int i = 0; i < DRAM_LRU->capacity; i++) {
        DRAM_LRU->hash_map[i] = NULL;
    }
} // 初始化DRAM LRU

void hbssd_init(FemuCtrl *n)
{
    struct ssd *ssd = n->ssd; // 获取ssd信息结构体
    struct Hybridparams *spp = &ssd->sp; // 获取一些ssd参数信息

    f_assert(ssd != NULL); // 调试测试信息
    //printf("hhhh!\n");

    ssd_init_params(spp); // 初始化SSD参数

    /* initialize ssd internal layout architecture */
    ssd->ch = g_malloc0(sizeof(struct Hybrid_channel) * spp->nchs); // 申请用于管理通道的空间
    for (int i = 0; i < spp->nchs; i++) {
        ssd_init_hybrid_ch(&ssd->ch[i], spp);
    }

    /* initialize maptbl */
    ssd_init_maptbl(ssd);

    /* initialize rmap */
    ssd_init_rmap(ssd);

    /* initialize all the lines */
    ssd_init_lines(ssd);

    /* initialize write pointer, this is how we allocate new pages for writes */
    ssd_init_write_pointer(ssd);

    /* initialize SLC_LRU */
    init_slc_lru(ssd);

    /* initialize DRAM_LRU */
    init_dram_lru(ssd);

    /* initialize access history */
    ssd_init_access_history(ssd);

    /* initialize address class tree */
    ssd_init_root(ssd);

    /* Create a fixed size space to save address classes */
    ssd_init_address_cs(ssd);

    /* Building an observation queue */
    ssd_init_vqueue(ssd);

    qemu_thread_create(&ssd->ftl_thread, "FEMU-FTL-Thread", ftl_thread, n, QEMU_THREAD_JOINABLE);
} // 针对 SSD 存储器进行初始化的函数，主要用于初始化 SSD 内部的各个模块和数据结构，为后续的数据读写操作做好准备；初始化根据的是FEMUCtrl参数

static inline bool valid_ppa(struct ssd *ssd, struct ppa *ppa)
{
    struct Hybridparams *spp = &ssd->sp;
    if (ppa->ppa == UNMAPPED_PPA) {
        return false;
    }
    int ch = ppa->g.ch;
    int lun = ppa->g.lun;
    int pl = ppa->g.pl;
    int blk = ppa->g.blk;
    int pg = ppa->g.pg;
    int sec = ppa->g.sec;

    if (ch >= 0 && ch < spp->nchs) {
        if (lun >= 0 && lun < ssd->ch[ch].nrb_luns) {
            if (pl >= 0 && pl < ssd->ch[ch].rb_lun[lun].npls) {
                if (blk >= 0 && blk < ssd->ch[ch].rb_lun[lun].pl[pl].nrbblks) {
                    if (pg >= 0 && pg < ssd->ch[ch].rb_lun[lun].pl[pl].rb_blk[blk].npgs) {
                        if (sec >= 0 && sec < ssd->ch[ch].rb_lun[lun].pl[pl].rb_blk[blk].pg[pg].nsecs) {
                            return true;
                        } else {
                            return false;
                        }
                    } else {
                        return false;
                    }
                } else {
                    return false;
                }
            } else {
                return false;
            }
        } else if (lun >= ssd->ch[ch].nrb_luns && lun < ssd->ch[ch].nrb_luns + ssd->ch[ch].nluns) {
            if (pl >= 0 && pl < ssd->ch[ch].lun[lun - ssd->ch[ch].nrb_luns].npls) {
                if (blk >= 0 && blk < ssd->ch[ch].lun[lun - ssd->ch[ch].nrb_luns].pl[pl].nslcblks) {
                    if (pg >= 0 && pg < ssd->ch[ch].lun[lun - ssd->ch[ch].nrb_luns].pl[pl].slc_blk[blk].npgs) {
                        if (sec >= 0 && sec < ssd->ch[ch].lun[lun - ssd->ch[ch].nrb_luns].pl[pl].slc_blk[blk].pg[pg].nsecs) {
                            return true;
                        } else {
                            return false;
                        }
                    } else {
                        return false;
                    }
                } else if (blk >= ssd->ch[ch].lun[lun - ssd->ch[ch].nrb_luns].pl[pl].nslcblks && blk < ssd->ch[ch].lun[lun - ssd->ch[ch].nrb_luns].pl[pl].nslcblks + ssd->ch[ch].lun[lun - ssd->ch[ch].nrb_luns].pl[pl].ntlcblks) {
                    if (pg >= 0 && pg < ssd->ch[ch].lun[lun - ssd->ch[ch].nrb_luns].pl[pl].tlc_blk[blk - ssd->ch[ch].lun[lun - ssd->ch[ch].nrb_luns].pl[pl].nslcblks].npgs) {
                        if (sec >= 0 && sec < ssd->ch[ch].lun[lun - ssd->ch[ch].nrb_luns].pl[pl].tlc_blk[blk - ssd->ch[ch].lun[lun - ssd->ch[ch].nrb_luns].pl[pl].nslcblks].pg[pg].nsecs) {
                            return true;
                        } else {
                            return false;
                        }
                    } else {
                        return false;
                    }
                } else {
                    return false;
                }
            } else {
                return false;
            }
        } else {
            return false;
        }
    } else {
        return false;
    }
} // 判断给定的物理页面地址（PPA）是否有效（需要根据PPA对应的页面类型）

static inline bool valid_lpn(struct ssd *ssd, uint64_t lpn) 
{
    return (lpn < ssd->sp.maptbl_len);
} // 判断给定的逻辑页面地址（Logical Page Number）是否合法

static inline bool mapped_ppa(struct ppa *ppa) 
{
    return !(ppa->ppa == UNMAPPED_PPA);
} // 判断给定物理地址PPA是否已经映射

static inline struct Hybrid_channel *get_ch(struct ssd *ssd, struct ppa *ppa)
{
    if (ppa->ppa == UNMAPPED_PPA || ppa->g.ch >= ssd->sp.nchs) {
        printf("get_ch err! ch:%d\n", (int)ppa->g.ch);
        exit(1);
    }
    return &(ssd->ch[ppa->g.ch]);
} // 获取给定物理页面PPA所对应的通道Channel结构

static inline struct Hybrid_rb_lun *get_rb_lun(struct ssd *ssd, struct ppa *ppa)
{
    struct Hybrid_channel *ch = get_ch(ssd, ppa);
    f_assert(ppa->g.lun >= 0 && ppa->g.lun < ch->nrb_luns);
    if (ppa->g.lun >= ssd->sp.rb_luns_per_ch) {
        printf("get_rb_lun err! rb_lun:%d", (int)ppa->g.lun);
        exit(1);
    }
    return &(ch->rb_lun[ppa->g.lun]);
} // 获取ppa对应的rb_lun结构体信息

static inline struct Hybrid_lun *get_lun(struct ssd *ssd, struct ppa *ppa)
{
    struct Hybrid_channel *ch = get_ch(ssd, ppa);
    f_assert(ppa->g.lun >= ch->nrb_luns && ppa->g.lun < ch->nrb_luns + ch->nluns);
    if (ppa->g.lun < ssd->sp.rb_luns_per_ch || ppa->g.lun >= ssd->sp.rb_luns_per_ch + ssd->sp.luns_per_ch) {
        printf("get_lun err! lun:%d", (int)ppa->g.lun);
        exit(1);
    }
    return &(ch->lun[ppa->g.lun - ch->nrb_luns]);
} // 获取给定页面地址PPA所对应的LUN结构

static inline struct Hybrid_rb_plane *get_rb_pl(struct ssd *ssd, struct ppa *ppa)
{
    struct Hybrid_rb_lun *rb_lun = get_rb_lun(ssd, ppa);
    if (ppa->g.pl >= ssd->sp.rb_pls_per_lun) {
        printf("get_rb_pl err! rb_pl:%d", (int)ppa->g.pl);
        exit(1);
    }
    return &(rb_lun->pl[ppa->g.pl]);
} // 根据ppa获取read buffer plane

static inline struct Hybrid_plane *get_pl(struct ssd *ssd, struct ppa *ppa)
{
    struct Hybrid_lun *lun = get_lun(ssd, ppa);
    if (ppa->g.pl >= ssd->sp.pls_per_lun) {
        printf("get_pl err! pl:%d", (int)ppa->g.pl);
        exit(1);
    }
    return &(lun->pl[ppa->g.pl]);
} // 根据ppa获取对应平面plane结构

static inline struct Hybrid_block *get_blk(struct ssd *ssd, struct ppa *ppa)
{
    int lun = ppa->g.lun;
    if (lun >= 0 && lun < ssd->sp.rb_luns_per_ch) {
        struct Hybrid_rb_plane *rb_pl = get_rb_pl(ssd, ppa);
        if (ppa->g.blk >= ssd->sp.rb_blks_per_pl) {
            printf("get_blk err! rb_blk:%d", (int)ppa->g.blk);
            exit(1);
        }
        return &(rb_pl->rb_blk[ppa->g.blk]);
    } else if (lun >= ssd->sp.rb_luns_per_ch && lun < ssd->sp.rb_luns_per_ch + ssd->sp.luns_per_ch) {
        struct Hybrid_plane *pl = get_pl(ssd, ppa);
        int blk = ppa->g.blk;
        if (blk >= 0 && blk < ssd->sp.slc_blks_per_pl) {
            return &(pl->slc_blk[blk]);
        } else if (blk >= ssd->sp.slc_blks_per_pl && blk < ssd->sp.slc_blks_per_pl + ssd->sp.tlc_blks_per_pl) {
            return &(pl->tlc_blk[blk - ssd->sp.slc_blks_per_pl]);
        } else {
            printf("get_blk err! blk:%d", (int)ppa->g.blk);
            exit(1);
        }
    }
    return NULL;
} // 获取给定物理PPA页面所对应的block结构

static inline struct line *get_line(struct ssd *ssd, struct ppa *ppa)
{
    int lun = ppa->g.lun;
    if (lun >= 0 && lun < ssd->sp.rb_luns_per_ch) {
        f_assert(ppa->g.blk < ssd->sp.rb_blks_per_pl);
        if (ppa->g.blk >= ssd->sp.rb_blks_per_pl) {
            printf("err!\n");
            exit(1);
        }
        return &(ssd->lm.rb_lines[ppa->g.blk]);
    } else if (lun >= ssd->sp.rb_luns_per_ch && lun < ssd->sp.rb_luns_per_ch + ssd->sp.luns_per_ch) {
        int blk = ppa->g.blk;
        if (blk >= 0 && blk < ssd->sp.slc_blks_per_pl) {
            return &(ssd->lm.slc_lines[blk]);
        } else if (blk >= ssd->sp.slc_blks_per_pl && blk < ssd->sp.slc_blks_per_pl + ssd->sp.tlc_blks_per_pl) {
            return &(ssd->lm.tlc_lines[blk - ssd->sp.slc_blks_per_pl]);
        }
    }
    return NULL;
} // 获取给定物理页面地址(PPA)所对应的行(Line)结构

static inline struct Hybrid_page *get_pg(struct ssd *ssd, struct ppa *ppa)
{
    struct Hybrid_block *blk = get_blk(ssd, ppa);
    if (blk->blk_type == HB_READ_BUFFER) {
        if (ppa->g.pg >= ssd->sp.rb_pgs_per_blk) {
            printf("get_pg err! rb_pg:%d", (int)ppa->g.pg);
            exit(1);
        }
    } else if (blk->blk_type == HB_SLC) {
        if (ppa->g.pg >= ssd->sp.slc_pgs_per_blk) {
            printf("get_pg err! slc_pg:%d", (int)ppa->g.pg);
            exit(1);
        }
    } else if (blk->blk_type == HB_TLC) {
        if (ppa->g.pg >= ssd->sp.tlc_pgs_per_blk) {
            printf("get_pg err! tlc_pg:%d", (int)ppa->g.pg);
            exit(1);
        }
    } else {
        printf("blk err! blk_type:%d", (int)blk->blk_type);
        exit(1);
    }
    return &(blk->pg[ppa->g.pg]);
} // 获取给定物理页面地址(PPA)所对应的页面结构

static inline int get_lat(struct ssd *ssd, struct ppa *ppa, int type, int flag)
{
    struct Hybrid_page *pg = get_pg(ssd, ppa);
    int lat = 0;

    switch (type) {
        case Hybrid_READ:
            if (pg->page_type == slc) {
                lat = ssd->sp.slc_pg_rd_lat;
            } else if (pg->page_type == lsb) {
                lat = ssd->sp.tlc_lpg_rd_lat;
            } else if (pg->page_type == csb) {
                lat = ssd->sp.tlc_cpg_rd_lat;
            } else if (pg->page_type == msb) {
                lat = ssd->sp.tlc_upg_rd_lat;
            } else {
                ftl_err("1Unsupported page type appears:%d\n", pg->page_type);
                exit(1);
            }
            break;
        case Hybrid_WRITE:
            if (pg->page_type == slc) {
                lat = ssd->sp.slc_pg_wr_lat;
            } else if (pg->page_type == lsb) {
                lat = ssd->sp.tlc_lpg_wr_lat;
            } else if (pg->page_type == csb) {
                lat = ssd->sp.tlc_cpg_wr_lat;
            } else if (pg->page_type == msb) {
                lat = ssd->sp.tlc_upg_wr_lat;
            } else {
                printf("ch:%d lun:%d pl:%d blk:%d pg:%d sec:%d flag:%d\n", (int)ppa->g.ch, (int)ppa->g.lun, (int)ppa->g.pl, (int)ppa->g.blk, (int)ppa->g.pg, (int)ppa->g.sec, flag);
                ftl_err("2Unsupported page type appears:%d\n", pg->page_type);
                exit(1);
            }
            break;
        case Hybrid_ERASE:
            if (pg->page_type == slc) {
                lat = ssd->sp.slc_blk_er_lat;
            } else if (pg->page_type == lsb || pg->page_type == csb || pg->page_type == msb) {
                lat = ssd->sp.tlc_blk_er_lat;
            } else {
                ftl_err("3Unsupported page type appears:%d\n", pg->page_type);
                exit(1);
            }
            break;
    }
    return lat;
} // 根据指令类型以及访问的物理地址的到相应的延时

static inline struct one_page* is_inSLClru(struct ssd *ssd, uint64_t lpn)
{
    struct lpn_lru *lru = &ssd->SLC_LRU;
    int hash_key = lpn % lru->capacity;
    struct one_page *opage;
    for (opage = lru->hash_map[hash_key]; opage != NULL; opage = opage->hash_next) {
        if (lpn == opage->lpn) {
            return opage;
        }
    }
    return NULL;
} // 判断当前逻辑页面是否在SLC的read buffer中

static inline struct dram_node* is_indram(struct ssd *ssd, uint64_t lpn)
{
    struct node_lru *lru = &ssd->DRAM_LRU;
    int hash_key = lpn % lru->capacity;
    struct dram_node *dnode;
    for (dnode = lru->hash_map[hash_key]; dnode != NULL; dnode = dnode->hash_next) {
        if (lpn == dnode->lpn) {  
            return dnode;
        }
    }
    return NULL;
} // 判断当前逻辑页面是否在DRAM的LRU中

static inline int get_pg_type(struct ssd *ssd, struct ppa *ppa)
{
    struct Hybrid_page *pg = get_pg(ssd, ppa);
    return pg->page_type;
} // 根据PPA获取对应页面类型【SLC:0 LSB:1 CSB:2 MSB:3】

static inline int get_block_type(struct ssd *ssd, struct ppa *ppa)
{
    struct Hybrid_block *blk = get_blk(ssd, ppa);
    return blk->blk_type;
} // 根据PPA获取对应block类型【READ BUFFER:0 SLC:1 TLC:2】

static inline int get_line_type(struct ssd *ssd, struct ppa *ppa)
{
    struct line *line = get_line(ssd, ppa);
    return line->line_type;
} // 根据PPA获取对应line类型【read buffer:0 slc:1 tlc:2】

/* update SSD status about one page from PG_VALID -> PG_INVALID */
static void mark_page_invalid(struct ssd *ssd, struct ppa *ppa)
{
    struct line_mgmt *lm = &ssd->lm;
    struct Hybridparams *spp = &ssd->sp;
    struct Hybrid_block *blk = NULL;
    struct Hybrid_page *pg = NULL;
    bool was_full_line = false;
    struct line *line;
    int blk_type;
    int line_type;

    /* update corresponding page status */
    pg = get_pg(ssd, ppa);
    f_assert(pg->status == PG_VALID);
    pg->status = PG_INVALID;

    /* update corresponding block status */
    blk = get_blk(ssd, ppa);
    blk_type = get_block_type(ssd, ppa);
    if (blk_type == HB_READ_BUFFER) {
        f_assert(blk->ipc >= 0 && blk->ipc < spp->rb_pgs_per_blk);
    } else if (blk_type == HB_SLC) {
        f_assert(blk->ipc >= 0 && blk->ipc < spp->slc_pgs_per_blk);
    } else {
        f_assert(blk->ipc >= 0 && blk->ipc < spp->tlc_pgs_per_blk);
    }
    blk->ipc++;
    if (blk_type == HB_READ_BUFFER) {
        f_assert(blk->vpc > 0 && blk->vpc <= spp->rb_pgs_per_blk);
    } else if (blk_type == HB_SLC) {
        f_assert(blk->vpc > 0 && blk->vpc <= spp->slc_pgs_per_blk);
    } else {
        f_assert(blk->vpc > 0 && blk->vpc <= spp->tlc_pgs_per_blk);
    }
    blk->vpc--;

    /* update corresponding line status */
    line = get_line(ssd, ppa);
    line_type = get_line_type(ssd, ppa);
    if (line_type == HB_READ_BUFFER) {
        f_assert(line->ipc >= 0 && line->ipc < spp->rb_pgs_per_line);
        if (line->vpc == spp->rb_pgs_per_line) {
            f_assert(line->ipc == 0);
            was_full_line = true;
        }
    } else if (line_type == HB_SLC) {
        f_assert(line->ipc >= 0 && line->ipc < spp->slc_pgs_per_line);
        if (line->vpc == spp->slc_pgs_per_line) {
            f_assert(line->ipc == 0);
            was_full_line = true;
        }
    } else {
        f_assert(line->ipc >= 0 && line->ipc < spp->tlc_pgs_per_line);
        if (line->vpc == spp->tlc_pgs_per_line) {
            f_assert(line->ipc == 0);
            was_full_line = true;
        }
    }
    line->ipc++;
    if (line_type == HB_READ_BUFFER) {
        f_assert(line->vpc > 0 && line->vpc <= spp->rb_pgs_per_line);
        /* Adjust the position of the victime line in the pq under over-writes */
        if (line->pos) {
            /* Note that line->vpc will be updated by this call */
            pqueue_change_priority(lm->rb_victim_line_pq, line->vpc - 1, line);
        } else {
            line->vpc--;
        }
    } else if (line_type == HB_SLC) {
        f_assert(line->vpc > 0 && line->vpc <= spp->slc_pgs_per_line);
        /* Adjust the position of the victime line in the pq under over-writes */
        if (line->pos) {
            /* Note that line->vpc will be updated by this call */
            pqueue_change_priority(lm->slc_victim_line_pq, line->vpc - 1, line);
        } else {
            line->vpc--;
        }
    } else {
        f_assert(line->vpc > 0 && line->vpc <= spp->tlc_pgs_per_line);
        /* Adjust the position of the victime line in the pq under over-writes */
        if (line->pos) {
            /* Note that line->vpc will be updated by this call */
            pqueue_change_priority(lm->tlc_victim_line_pq, line->vpc - 1, line);
        } else {
            line->vpc--;
        }
    }

    if (was_full_line) {
        /* move line: "full" -> "victim" */
        if (line_type == HB_READ_BUFFER) {
            QTAILQ_REMOVE(&lm->rb_full_line_list, line, entry);
            lm->rb_full_line_cnt--;
            pqueue_insert(lm->rb_victim_line_pq, line);
            lm->rb_victim_line_cnt++;
        } else if (line_type == HB_SLC) {
            QTAILQ_REMOVE(&lm->slc_full_line_list, line, entry);
            lm->slc_full_line_cnt--;
            pqueue_insert(lm->slc_victim_line_pq, line);
            lm->slc_victim_line_cnt++;
        } else {
            QTAILQ_REMOVE(&lm->tlc_full_line_list, line, entry);
            lm->tlc_full_line_cnt--;
            pqueue_insert(lm->tlc_victim_line_pq, line);
            lm->tlc_victim_line_cnt++;
        }
    }

} // 将给定物理页面地址（PPA）所表示的页面标记为无效

static void mark_page_valid(struct ssd *ssd, struct ppa *ppa) 
{
    struct Hybrid_block *blk = NULL;
    struct Hybrid_page *pg = NULL;
    struct line *line;
    int blk_type;
    int line_type;

    /* update page status */
    pg = get_pg(ssd, ppa);
    f_assert(pg->status == PG_FREE);
    pg->status = PG_VALID;

    /* update corresponding block status */
    blk = get_blk(ssd, ppa);
    blk_type = get_block_type(ssd, ppa);
    if (blk_type == HB_READ_BUFFER) {
        f_assert(blk->vpc >= 0 && blk->vpc < ssd->sp.rb_pgs_per_blk);
    } else if (blk_type == HB_SLC) {
        f_assert(blk->vpc >= 0 && blk->vpc < ssd->sp.slc_pgs_per_blk);
    } else {
        f_assert(blk->vpc >= 0 && blk->vpc < ssd->sp.tlc_pgs_per_blk);
    }
    blk->vpc++;

    /* update corresponding line status */
    line = get_line(ssd, ppa);
    line_type = get_line_type(ssd, ppa);
    if (line_type == HB_READ_BUFFER) {
        f_assert(line->vpc >= 0 && line->vpc < ssd->sp.rb_pgs_per_line);
        ssd->free_rb_pg_n--;
    } else if (line_type == HB_SLC) {
        f_assert(line->vpc >= 0 && line->vpc < ssd->sp.slc_pgs_per_line);
        ssd->free_slc_pg_n--;
    } else {
        f_assert(line->vpc >= 0 && line->vpc < ssd->sp.tlc_pgs_per_line);
        ssd->free_tlc_pg_n--;
    }
    line->vpc++;
} // 将给定物理地址(PPA)对应页面标记为有效

static void mark_block_free(struct ssd *ssd, struct ppa *ppa)
{
    struct Hybrid_block *blk = get_blk(ssd, ppa);
    struct Hybrid_page *pg = NULL;
    int blk_type = get_block_type(ssd, ppa);

    for (int i = 0; i < blk->npgs; i++) {
        /* reset page status */
        pg = &blk->pg[i];
        f_assert(pg->nsecs == ssd->sp.secs_per_pg);
        pg->status = PG_FREE;
        pg->is_inlru = 0;
    }

    /* reset block status */
    if (blk_type == HB_READ_BUFFER) {
        f_assert(blk->npgs == ssd->sp.rb_pgs_per_blk);
        ssd->free_rb_pg_n += ssd->sp.rb_pgs_per_blk;
    } else if (blk_type == HB_SLC) {
        f_assert(blk->npgs == ssd->sp.slc_pgs_per_blk);
        ssd->free_slc_pg_n += ssd->sp.slc_pgs_per_blk;
    } else {
        f_assert(blk->npgs == ssd->sp.tlc_pgs_per_blk);
        ssd->free_tlc_pg_n += ssd->sp.tlc_pgs_per_blk;
    }

    blk->ipc = 0;
    blk->vpc = 0;
    blk->erase_cnt++;
} // 将给定物理页面地址（PPA）所在的块标记为空闲块

static uint64_t ssd_advance_status(struct ssd *ssd, struct ppa *ppa, struct nand_cmd *ncmd, int flag) 
{
    int c = ncmd->cmd;
    uint64_t cmd_stime = (ncmd->stime == 0) ? qemu_clock_get_ns(QEMU_CLOCK_REALTIME) : ncmd->stime;
    uint64_t nand_stime;
    uint64_t chnl_stime;
    struct Hybridparams *spp = &ssd->sp;
    struct Hybrid_channel *ch = get_ch(ssd, ppa);
    uint64_t lat = 0;

    if (ppa->g.lun < spp->rb_luns_per_ch) {
        struct Hybrid_rb_lun *rb_lun = get_rb_lun(ssd, ppa);
        switch (c) {
            case Hybrid_READ:
                /* read: perform NAND cmd first */
                nand_stime = (rb_lun->next_lun_avail_time < cmd_stime) ? cmd_stime : rb_lun->next_lun_avail_time;
                rb_lun->next_lun_avail_time = nand_stime + get_lat(ssd, ppa, c, flag);

                //lat = (ch->next_ch_avail_time < rb_lun->next_lun_avail_time) ? rb_lun->next_lun_avail_time : ch->next_ch_avail_time;
                //lat = lat + spp->slc_chnl_xfer_lat - cmd_stime;
                /* read: then data transfer through channel */
                chnl_stime = (ch->next_ch2_avail_time < rb_lun->next_lun_avail_time) ? rb_lun->next_lun_avail_time : ch->next_ch2_avail_time;
                ch->next_ch2_avail_time = chnl_stime + spp->slc_chnl_xfer_lat; // 这里SLC与TLC公用通道，所以通道传输时间一样
                lat = ch->next_ch2_avail_time - cmd_stime;
                if (ssd->sp.class_flag == 1) {
                    ssd->sp.rr_num++;
                    ssd->sp.rb_r_num++;
                }
                break;
            case Hybrid_WRITE:
                /* write: transfer data through channel first */
                chnl_stime = (ch->next_ch2_avail_time < cmd_stime) ? cmd_stime : ch->next_ch2_avail_time;
                ch->next_ch2_avail_time = chnl_stime + spp->slc_chnl_xfer_lat;

                /* write: then do NAND program */
                //nand_stime = (rb_lun->next_lun_avail_time < chnl_stime + spp->slc_chnl_xfer_lat) ? chnl_stime + spp->slc_chnl_xfer_lat : rb_lun->next_lun_avail_time;
                nand_stime = (rb_lun->next_lun_avail_time < ch->next_ch2_avail_time) ? ch->next_ch2_avail_time : rb_lun->next_lun_avail_time;
                rb_lun->next_lun_avail_time = nand_stime + get_lat(ssd, ppa, c, flag);
                lat = rb_lun->next_lun_avail_time - cmd_stime;
                if (ssd->sp.class_flag == 1) {
                    ssd->sp.rw_num++;
                    ssd->sp.rb_w_num++;
                }
                break;
            case Hybrid_ERASE:
                /* erase: only need to advance NAND status */
                nand_stime = (rb_lun->next_lun_avail_time < cmd_stime) ? cmd_stime : rb_lun->next_lun_avail_time;
                rb_lun->next_lun_avail_time = nand_stime + get_lat(ssd, ppa, c, flag);
                lat = rb_lun->next_lun_avail_time - cmd_stime;
                break;
            default:
                ftl_err("Unsupported Hybrid NAND command: 0x%x\n", c);
        }
    } else {
        struct Hybrid_lun *lun = get_lun(ssd, ppa);
        int b_t = get_block_type(ssd, ppa);
        switch (c) {
            case Hybrid_READ:
                /* read: perform NAND cmd first */
                nand_stime = (lun->next_lun_avail_time < cmd_stime) ? cmd_stime : lun->next_lun_avail_time;
                lun->next_lun_avail_time = nand_stime + get_lat(ssd, ppa, c, flag);

                /* read: then data transfer through channel */
                chnl_stime = (ch->next_ch_avail_time < lun->next_lun_avail_time) ? lun->next_lun_avail_time : ch->next_ch_avail_time;
                ch->next_ch_avail_time = chnl_stime + spp->slc_chnl_xfer_lat; // 这里SLC与TLC公用通道，所以通道传输时间一样
                lat = ch->next_ch_avail_time - cmd_stime;
                if (ssd->sp.class_flag == 1) {
                    ssd->sp.rr_num++;
                    if (b_t == HB_SLC) {
                        ssd->sp.slc_r_num++;
                    } else if (b_t == HB_TLC) {
                        ssd->sp.tlc_r_num++;
                    } else{
                        ftl_err("2type err!\n");
                    }
                }
                break;
            case Hybrid_WRITE:
                /* write: transfer data through channel first */
                chnl_stime = (ch->next_ch_avail_time < cmd_stime) ? cmd_stime : ch->next_ch_avail_time;
                ch->next_ch_avail_time = chnl_stime + spp->slc_chnl_xfer_lat;

                /* write: then do NAND program */
                nand_stime = (lun->next_lun_avail_time < ch->next_ch_avail_time) ? ch->next_ch_avail_time : lun->next_lun_avail_time;
                lun->next_lun_avail_time = nand_stime + get_lat(ssd, ppa, c, flag);
                lat = lun->next_lun_avail_time - cmd_stime;
                if (ssd->sp.class_flag == 1) {
                    ssd->sp.rw_num++;
                    if (b_t == HB_SLC) {
                        ssd->sp.slc_w_num++;
                    } else if (b_t == HB_TLC) {
                        ssd->sp.tlc_w_num++;
                    } else{
                        ftl_err("2type err!\n");
                    }
                }
                break;
            case Hybrid_ERASE:
                /* erase: only need to advance NAND status */
                nand_stime = (lun->next_lun_avail_time < cmd_stime) ? cmd_stime : lun->next_lun_avail_time;
                lun->next_lun_avail_time = nand_stime + get_lat(ssd, ppa, c, flag);
                lat = lun->next_lun_avail_time - cmd_stime;
                break;
            default:
                ftl_err("Unsupported Hybrid NAND command: 0x%x\n", c);
        }
    }
    return lat;
} // 更新计算相关延迟， 读写以及通道延迟

static void gc_read_page(struct ssd *ssd, struct ppa *ppa)
{
    /* advance ssd status, we don't care about how long it takes */
    if (ssd->sp.enable_gc_delay) {
        struct nand_cmd gcr;
        gcr.type = GC_IO;
        gcr.cmd = Hybrid_READ;
        gcr.stime = 0;
        ssd_advance_status(ssd, ppa, &gcr, 1);
    }
} // 处理GC过程中的页面读取

/* move valid page data (already in DRAM) from victim line to a new page */
static uint64_t gc_write_page(struct ssd *ssd, struct ppa *old_ppa)
{
    struct ppa new_ppa;
    struct Hybrid_channel *new_ch;
    struct Hybrid_page *pg;
    int lun = old_ppa->g.lun;
    if (lun >= 0 && lun < ssd->sp.rb_luns_per_ch) { // read buffer中需要gc write的页面只有is_inlru为1的页面，直接gc到read buffer中
        struct Hybrid_rb_lun *new_lun;
        uint64_t lpn = get_rmap_ent(ssd, old_ppa);
        struct Hybrid_page *page = get_pg(ssd, old_ppa);
        int block_type = get_block_type(ssd, old_ppa);
        if (block_type != HB_READ_BUFFER) {
            printf("1gc_write_page->old_ppa != HB_READ_BUFFER\n");
            exit(1);
        }

        f_assert(valid_lpn(ssd, lpn)); 
        if (page->is_inlru != 1 || block_type != HB_READ_BUFFER) {
            ftl_err("SLC/TLC PAGE IN SLC LRU!");
        }

        new_ppa = get_new_page(ssd, block_type);
        f_assert(mapped_ppa(&new_ppa));
    
        /* update maptbl */
        set_rb_maptbl_ent(ssd, lpn, &new_ppa);
        /* update rmap */
        set_rmap_ent(ssd, lpn, &new_ppa);

        mark_page_valid(ssd, &new_ppa);

        pg = get_pg(ssd, &new_ppa);
        pg->is_inlru = 1;

        /* need to advance the write pointer here */
        ssd_advance_write_pointer(ssd, HB_READ_BUFFER);

        if (ssd->sp.enable_gc_delay) {
            struct nand_cmd gcw;
            gcw.type = GC_IO;
            gcw.cmd = Hybrid_WRITE;
            gcw.stime = 0;
            ssd_advance_status(ssd, &new_ppa, &gcw, 2);
        }

        /* advance per-ch gc_endtime as well */
        new_ch = get_ch(ssd, &new_ppa);
        new_ch->gc_endtime = new_ch->next_ch_avail_time;

        new_lun = get_rb_lun(ssd, &new_ppa);
        new_lun->gc_endtime = new_lun->next_lun_avail_time;
        
    } else { // SLC/TLC中的gc迁移需要gc write的页面是全部迁移到TLC中
        struct Hybrid_lun *new_lun;
        uint64_t lpn = get_rmap_ent(ssd, old_ppa);
        struct Hybrid_page *page = get_pg(ssd, old_ppa);
        int block_type = get_block_type(ssd, old_ppa);

        f_assert(valid_lpn(ssd, lpn));
        if (page->is_inlru == 1 || block_type == HB_READ_BUFFER) {
            ftl_err("READ BUFFER page in SLC/TLC!");
        }

        new_ppa = get_new_page(ssd, HB_TLC);
        f_assert(mapped_ppa(&new_ppa));
        
        /* update maptbl */
        set_maptbl_ent(ssd, lpn, &new_ppa);
        /* update rmap */
        set_rmap_ent(ssd, lpn, &new_ppa);

        mark_page_valid(ssd, &new_ppa);

        /* need to advance the write pointer here */
        ssd_advance_write_pointer(ssd, HB_TLC);

        if (ssd->sp.enable_gc_delay) {
            struct nand_cmd gcw;
            gcw.type = GC_IO;
            gcw.cmd = Hybrid_WRITE;
            gcw.stime = 0;
            ssd_advance_status(ssd, &new_ppa, &gcw, 3);
        }

        /* advance per-ch gc_endtime as well */
        new_ch = get_ch(ssd, &new_ppa);
        new_ch->gc_endtime = new_ch->next_ch_avail_time;


        new_lun = get_lun(ssd, &new_ppa);
        new_lun->gc_endtime = new_lun->next_lun_avail_time;

    }

    return 0;
} // 在垃圾回收过程中写入页面

static struct line *select_victim_line(struct ssd *ssd, bool force, int line_type)
{
    struct line_mgmt *lm = &ssd->lm;
    struct line *victim_line = NULL;

    switch (line_type) {
        case HB_READ_BUFFER:
            victim_line = pqueue_peek(lm->rb_victim_line_pq);
            if (!victim_line) { // rb victim line 没有了，需要将full_line中不在lru的页面作废
                victim_line = QTAILQ_FIRST(&lm->rb_full_line_list);
                if (!victim_line) {
                    return NULL;
                }
                QTAILQ_REMOVE(&lm->rb_full_line_list, victim_line, entry);
                victim_line->pos = 0;
                lm->rb_full_line_cnt--;
                /* 无效页面数不超过1/8的line页面数，不予以GC */
                //if (!force && victim_line->ipc < ssd->sp.rb_pgs_per_line / 8) {
                    //return NULL;
                //}
            } else {
                /* 无效页面数不超过1/8的line页面数，不予以GC */
                //if (!force && victim_line->ipc < ssd->sp.rb_pgs_per_line / 8) {
                    //return NULL;
                //}
                pqueue_pop(lm->rb_victim_line_pq);
                victim_line->pos = 0;
                lm->rb_victim_line_cnt--;
            }
            break;
        case HB_SLC:
            victim_line = pqueue_peek(lm->slc_victim_line_pq);
            if (!victim_line) {
                victim_line = QTAILQ_FIRST(&lm->slc_full_line_list);
                if (!victim_line) {
                    return NULL;
                }
                QTAILQ_REMOVE(&lm->slc_full_line_list, victim_line, entry);
                victim_line->pos = 0;
                lm->slc_full_line_cnt--;
                /* 无效页面数不超过1/8的line页面数，不予以GC */
                //if (!force && victim_line->ipc < ssd->sp.slc_pgs_per_line / 8) {
                    //return NULL;
                //}
            } else {
                /* 无效页面数不超过1/8的line页面数，不予以GC */
                //if (!force && victim_line->ipc < ssd->sp.slc_pgs_per_line / 8) {
                    //return NULL;
                //}
                pqueue_pop(lm->slc_victim_line_pq);
                victim_line->pos = 0;
                lm->slc_victim_line_cnt--;
            }
            break;
        case HB_TLC:
            victim_line = pqueue_peek(lm->tlc_victim_line_pq);
            if (!victim_line) {
                return NULL;
            }
            /* 无效页面数不超过1/8的line页面数，不予以GC */
            //if (!force && victim_line->ipc < ssd->sp.tlc_pgs_per_line / 8) {
                //return NULL;
            //}
            pqueue_pop(lm->tlc_victim_line_pq);
            victim_line->pos = 0;
            lm->tlc_victim_line_cnt--;
            break;
        default:
            break;

    }
    /* victim_line is a danggling node now */
    return victim_line;
} // 选择受害行,因为line是以小根堆组织的，弹出头部的就可以了

/* here ppa identifies the block we want to clean */
static void clean_one_block(struct ssd *ssd, struct ppa *ppa, int block_type)
{
    struct Hybridparams *spp = &ssd->sp;
    struct Hybrid_page *pg_iter = NULL;
    struct Hybrid_block *blk = get_blk(ssd, ppa);
    int cnt = 0;
    switch (block_type) {
        case HB_READ_BUFFER:
            for (int pg = 0; pg < spp->rb_pgs_per_blk; pg++) {
                ppa->g.pg = pg;
                pg_iter = get_pg(ssd, ppa);
                /* there shouldn't be any free page in victim blocks */
                f_assert(pg_iter->status != PG_FREE);
                if (pg_iter->status == PG_VALID && pg_iter->is_inlru == 1) {
                    gc_read_page(ssd, ppa);
                    /* delay the maptbl update until "write" happens */
                    gc_write_page(ssd, ppa);
                    cnt++;
                } else {
                    // mark_page_invalid(ssd, &ppa);
                    if (pg_iter->status == PG_VALID) {
                        cnt++;
                        uint64_t pgidx = ppa2pgidx(ssd, ppa);
                        uint64_t lpn = ssd->rmap[pgidx];
                        ssd->rb_maptbl[lpn].ppa = UNMAPPED_PPA;
                        ssd->rmap[pgidx] = INVALID_LPN;
                    }
                }
            }
            break;
        case HB_SLC:
            for (int pg = 0; pg < spp->slc_pgs_per_blk; pg++) {
                ppa->g.pg = pg;
                pg_iter = get_pg(ssd, ppa);
                /* there shouldn't be any free page in victim blocks */
                f_assert(pg_iter->status != PG_FREE);
                if (pg_iter->status == PG_VALID) {
                    gc_read_page(ssd, ppa);
                    /* delay the maptbl update until "write" happens */
                    gc_write_page(ssd, ppa);
                    cnt++;
                }
            }
            break;
        case HB_TLC:
            for (int pg = 0; pg < spp->tlc_pgs_per_blk; pg++) {
                ppa->g.pg = pg;
                pg_iter = get_pg(ssd, ppa);
                /* there shouldn't be any free page in victim blocks */
                f_assert(pg_iter->status == PG_FREE);
                if (pg_iter->status == PG_VALID) {
                    gc_read_page(ssd, ppa);
                    /* delay the maptbl update until "write" happens */
                    gc_write_page(ssd, ppa);
                    cnt++;
                }
            }
            break;
    }
    f_assert(blk->vpc == cnt);
} // 清空给定的ppa的block，并将有效页面写到别处

static void mark_line_free(struct ssd *ssd, struct ppa *ppa)
{
    struct line_mgmt *lm = &ssd->lm;
    struct line *line = get_line(ssd, ppa);
    f_assert(line != NULL);
    line->ipc = 0;
    line->vpc = 0;
    /* move this line to free line list */
    if (line->line_type == HB_READ_BUFFER) {
        QTAILQ_INSERT_TAIL(&lm->rb_free_line_list, line, entry);
        lm->rb_free_line_cnt++;
    } else if (line->line_type == HB_SLC) {
        QTAILQ_INSERT_TAIL(&lm->slc_free_line_list, line, entry);
        lm->slc_free_line_cnt++;
    } else if (line->line_type == HB_TLC) {
        QTAILQ_INSERT_TAIL(&lm->tlc_free_line_list, line, entry);
        lm->tlc_free_line_cnt++;
    } else {
        ftl_err("1mark_line_free err!\n");
    }
} // 将给定物理页面对应的line标记为空闲

static int do_gc(struct ssd *ssd, bool force, int gc_type)
{
    struct line *victim_line = NULL;
    struct Hybridparams *spp = &ssd->sp;
    struct ppa ppa;
    int ch, lun;
    struct Hybrid_rb_lun *rb_lunp;
    struct Hybrid_lun *lunp;
    switch (gc_type) { /* gc rb */
        case HB_READ_BUFFER:
            victim_line = select_victim_line(ssd, force, HB_READ_BUFFER);
            if (!victim_line) {
                printf("1select_victim_line err!\n");
                exit(1);
                return -1;
            }
            ppa.g.blk = victim_line->id;
            f_assert(ppa.g.blk < ssd->sp.rb_blks_per_pl);
            ftl_debug("GC-ing line:%d,ipc=%d,victim=%d,full=%d,free=%d\n", ppa.g.blk,
                victim_line->ipc, ssd->lm.rb_victim_line_cnt, ssd->lm.rb_full_line_cnt,
                ssd->lm.rb_free_line_cnt);
            
            /* copy back valid data */ // 将受害行的有效数据迁移到别处，gc的一个line，是把所有block号相同的block给gc了
            for (ch = 0; ch < spp->nchs; ch++) {
                for (lun = 0; lun < spp->rb_luns_per_ch; lun++) {
                    ppa.g.ch = ch;
                    ppa.g.lun = lun;
                    ppa.g.pl = 0;
                    rb_lunp = get_rb_lun(ssd, &ppa);
                    clean_one_block(ssd, &ppa, HB_READ_BUFFER);// 将read buffer block中有效且在LRU中的页面迁移到read buffer中，其余作废
                    mark_block_free(ssd, &ppa); // 重置block

                    if (spp->enable_gc_delay) {
                        struct nand_cmd gce;
                        gce.type = GC_IO;
                        gce.cmd = Hybrid_ERASE;
                        gce.stime = 0; // 这里的时间根据当前执行gc的时间点为准
                        ssd_advance_status(ssd, &ppa, &gce, 4);
                    }

                    rb_lunp->gc_endtime = rb_lunp->next_lun_avail_time;
                }
            }
            if (spp->class_flag == 1) {
                spp->rb_gc_count += 1;
                // printf("rb gc times:%d\n", (int)spp->rb_gc_count);
            }
            break;
        case HB_SLC: /* gc slc */
            victim_line = select_victim_line(ssd, force, HB_SLC);
            if (!victim_line) {
                printf("2select_victim_line err!\n");
                exit(1);
                return -1;
            }
            //printf("gc_SLC!\n");
            ppa.g.blk = victim_line->id;
            f_assert(ppa.g.blk < ssd->sp.slc_blks_per_pl);
            ftl_debug("GC-ing line:%d,ipc=%d,victim=%d,full=%d,free=%d\n", ppa.g.blk,
                victim_line->ipc, ssd->lm.slc_victim_line_cnt, ssd->lm.slc_full_line_cnt,
                ssd->lm.slc_free_line_cnt);
            /* copy back valid data */ // 将受害行的有效数据迁移到别处，gc的一个line，是把所有block号相同的block给gc了
            for (ch = 0; ch < spp->nchs; ch++) {
                for (lun = spp->rb_luns_per_ch; lun < spp->rb_luns_per_ch + spp->luns_per_ch; lun++) {
                    ppa.g.ch = ch;
                    ppa.g.lun = lun;
                    ppa.g.pl = 0;
                    lunp = get_lun(ssd, &ppa);
                    clean_one_block(ssd, &ppa, HB_SLC); // 将SLC中有效页面迁移到TLC，回收SLC空间
                    mark_block_free(ssd, &ppa); // 重置block

                    if (spp->enable_gc_delay) {
                        struct nand_cmd gce;
                        gce.type = GC_IO;
                        gce.cmd = Hybrid_ERASE;
                        gce.stime = 0; // 这里的时间根据当前执行gc的时间点为准
                        ssd_advance_status(ssd, &ppa, &gce, 6);
                    }

                    lunp->gc_endtime = lunp->next_lun_avail_time;
                } 
            }
            if (spp->class_flag == 1) {
                spp->slc_gc_count += 1;
            }
            break;
        case HB_TLC: /* gc tlc */
            victim_line = select_victim_line(ssd, force, HB_TLC);
            if (!victim_line) {
                printf("3select_victim_line err!\n");
                exit(1);
                return -1;
            }
            ppa.g.blk = victim_line->id;
            f_assert(ppa.g.blk < ssd->sp.tlc_blks_per_pl);
            ftl_debug("GC-ing line:%d,ipc=%d,victim=%d,full=%d,free=%d\n", ppa.g.blk,
                victim_line->ipc, ssd->lm.tlc_victim_line_cnt, ssd->lm.tlc_full_line_cnt,
                ssd->lm.tlc_free_line_cnt);
            /* copy back valid data */ // 将受害行的有效数据迁移到别处，gc的一个line，是把所有block号相同的block给gc了
            for (ch = 0; ch < spp->nchs; ch++) {
                for (lun = spp->rb_luns_per_ch; lun < spp->rb_luns_per_ch + spp->luns_per_ch; lun++) {
                    ppa.g.ch = ch;
                    ppa.g.lun = lun;
                    ppa.g.pl = 0;
                    lunp = get_lun(ssd, &ppa);
                    clean_one_block(ssd, &ppa, HB_TLC); // 将TLC中有效页面迁移到TLC中，回收TLC空间
                    mark_block_free(ssd, &ppa); // 重置block

                    if (spp->enable_gc_delay) {
                        struct nand_cmd gce;
                        gce.type = GC_IO;
                        gce.cmd = Hybrid_ERASE;
                        gce.stime = 0;
                        ssd_advance_status(ssd, &ppa, &gce, 7);
                    }

                    lunp->gc_endtime = lunp->next_lun_avail_time;
                }
            }
            if (spp->class_flag == 1) {
                spp->tlc_gc_count += 1;
            }
            break;
        default:
            printf("ERR GC_TYPE!\n");
            exit(1);
    }
    /* update line status */
    mark_line_free(ssd, &ppa); // 将line置为free

    return 0;
} // 执行一次gc流程，gc一个line

static int compare(const void *a, const void *b) {
    struct history_entry *sa = (struct history_entry *)a;
    struct history_entry *sb = (struct history_entry *)b;
    return sa->start_LBA - sb->start_LBA;
}

static struct address_class* create_node(void *arg, uint64_t start_lba, uint64_t end_lba) {
    struct ssd *ssd = (struct ssd *)arg;
    if (ssd->address_cs->used_num < ssd->address_cs->total_num) {
        struct address_class* node = &ssd->address_cs->head[ssd->address_cs->used_num];
	    node->start_LBA = start_lba;
        node->end_LBA = end_lba;
	    node->type = ssd->address_cs->used_num;
        node->left = NULL;
        node->right = NULL;
        node->height = 1;
	    ssd->address_cs->used_num += 1;
	    return node;
    } else {
        ftl_err("Error!\n");
	    return NULL;
    }
}

// computing height of node 
static int height(struct address_class* node) {
    if (node == NULL) {
        return 0;
    }
    return node->height;
}

static void updateHeight(struct address_class* node) {
    node->height = 1 + (height(node->left) > height(node->right) ? height(node->left) : height(node->right));
}

static int balanceFactor(struct address_class* node) {
    if (node == NULL) {
        return 0;
    }
    return height(node->left) - height(node->right);
}

// Left-handed operation 
static struct address_class* leftRotate(struct address_class* node) {
    struct address_class* right = node->right;
    struct address_class* left = right->left;
    right->left = node;
    node->right = left;
    updateHeight(node);
    updateHeight(right);
    return right;
}

// Right-handed operation 
static struct address_class* rightRotate(struct address_class* node) {
    struct address_class* left = node->left;
    struct address_class* right = left->right;
    left->right = node;
    node->left = right;
    updateHeight(node);
    updateHeight(left);
    return left;
}

static void releaseTree(void *arg) {
    struct ssd *ssd = (struct ssd *)arg;
    ssd->root = NULL;
    ssd->address_cs->used_num = 0;
}

static struct address_class* insert_class(void *arg, struct address_class* root, uint64_t start_lba, uint64_t end_lba) {
    struct ssd *ssd = (struct ssd *)arg;
    if (root == NULL){
        root = create_node(ssd, start_lba, end_lba);
    } else if (start_lba < root->start_LBA) {
        root->left = insert_class(ssd, root->left, start_lba, end_lba);
    } else {
	    root->right = insert_class(ssd, root->right, start_lba, end_lba);
    }
    
    // update tree hight
    updateHeight(root);
    // computing balance factor 
    int bf = balanceFactor(root);
    // Rotation operation 
    if (bf > 1 && start_lba < root->left->start_LBA) {
        return rightRotate(root);
    }
    if (bf < -1 && start_lba > root->right->start_LBA) {
        return leftRotate(root);
    }
    if (bf > 1 && start_lba > root->left->start_LBA) {
        root->left = leftRotate(root->left);
	    return rightRotate(root);
    }
    if (bf < -1 && start_lba < root->right->start_LBA) {
        root->right = rightRotate(root->right);
	    return leftRotate(root);
    }
    return root;
}

static void update_class(void *arg) {
    struct ssd *ssd = (struct ssd *)arg;
    int ii;
    uint64_t start_lba;
    uint64_t end_lba;
    bool flag = false;
    qsort(ssd->access_history, ssd->sp.nhistorys, sizeof(struct history_entry), compare);

    for (ii = 0 ; ii < ssd->sp.nhistorys ; ii++) {
        if (flag == false) {
	        start_lba = ssd->access_history[ii].start_LBA;
	        end_lba = ssd->access_history[ii].start_LBA + (uint64_t)ssd->access_history[ii].size;
	        flag = true;
	    } else {
            if (ssd->access_history[ii].start_LBA >= start_lba && ssd->access_history[ii].start_LBA <= end_lba) {
                if (end_lba < ssd->access_history[ii].start_LBA + (uint64_t)ssd->access_history[ii].size) {
                    end_lba = ssd->access_history[ii].start_LBA + (uint64_t)ssd->access_history[ii].size;
                }
            } else {
                ssd->root = insert_class(ssd, ssd->root, start_lba, end_lba);
                start_lba = ssd->access_history[ii].start_LBA;
                end_lba = ssd->access_history[ii].start_LBA + (uint64_t)ssd->access_history[ii].size;
            }
            if (ii == ssd->sp.nhistorys-1) {
                ssd->root = insert_class(ssd, ssd->root, start_lba, end_lba);
            } 
	    }
    }
}

static struct address_class* search_class(struct address_class *root, uint64_t start_LBA, uint64_t end_LBA) {
    if (root == NULL) {
        return NULL;
    } else {
        if (start_LBA >= root->start_LBA ) {
	        if (end_LBA <= root->end_LBA) {
	            return root;
	        } else {
	            return search_class(root->right, start_LBA, end_LBA);
	        }
	    } else {
	        return search_class(root->left, start_LBA, end_LBA);
	    }
    }
} // The search process needs to be optimized!

static int compare2(const void *a, const void *b) {
    struct circulate_entry *sa = (struct circulate_entry *)a;
    struct circulate_entry *sb = (struct circulate_entry *)b;
    return sa->start_LBA - sb->start_LBA;
}

/*
static void print_DRAM_LRU(struct ssd *ssd, FILE *log)
{
    struct node_lru *lru = &ssd->DRAM_LRU;
    for (struct dram_node *i = lru->head; i != NULL; i = i->next) {
        if (i == NULL) {
            ftl_err("print fail!\n");
        }
        fprintf(log, "%lu ", i->lpn);
    }
    fprintf(log, "\n");
}

static void print_SLC_LRU(struct ssd *ssd, FILE *log)
{
    struct lpn_lru *lru = &ssd->SLC_LRU;
    for (struct one_page *i = lru->head; i != NULL; i = i->next) {
        if (i == NULL) {
            ftl_err("print fail!\n");
        }
        fprintf(log, "%lu ", i->lpn);
    }
    fprintf(log, "\n");
}
*/

/*
static void insert_SLC_LRU_page(struct ssd *ssd, uint64_t lpn, uint64_t rt)
{
    struct lpn_lru *lru = &ssd->SLC_LRU;
    int hash_key = lpn % lru->capacity;
    int old_hash_key;
    struct Hybrid_page *hpage;
    struct one_page *opage;
    // DRAM 和 READ BUFFER中的数据不重复，所以当有页面从DRAM排出时，不会出现SLC LRU中也存在的情况
    // 判断1：查看页面是否在READ BUFFER中，若存在则报错，应为DRAM与READ BUFFER属于同一级别，数据不存在互相备份的情况
    // 将数据从DRAM中读出，并写入到READ BUFFER中
    while (should_gc_high(ssd, HB_READ_BUFFER)) { // gc read buffer
        // perform GC here until !should_gc(ssd, HB_READ_BUFFER) 
        //printf("1!\n");
        int r = do_gc(ssd, true, HB_READ_BUFFER);
        if (r == -1) 
            break;
    }
    struct ppa ppa = get_rb_maptbl_ent(ssd, lpn);
    if (mapped_ppa(&ppa)) {
        ftl_err("There is the page in READ BUFFER that are the same as in DRAM LRU!\n");
        exit(1);
    }
    struct ppa new_ppa = get_new_page(ssd, HB_READ_BUFFER);
    if (!mapped_ppa(&new_ppa) || !valid_ppa(ssd, &new_ppa)) {
        ftl_err("SLC READ BUFFER gc err!");
    }
    set_rb_maptbl_ent(ssd, lpn, &new_ppa);
    set_rmap_ent(ssd, lpn, &new_ppa);
    mark_page_valid(ssd, &new_ppa);
    ssd_advance_write_pointer(ssd, HB_READ_BUFFER);

    struct nand_cmd ptw;
    ptw.type = PAGE_TRANS_IO;
    ptw.cmd = Hybrid_WRITE;
    ptw.stime = rt;
    ssd_advance_status(ssd, &new_ppa, &ptw, 8);

    // 页面迁移到read buffer中之后，开始将SLC LRU内部结构对应到new_ppa中
    opage = is_inSLClru(ssd, lpn);
    if (opage != NULL) {
        ftl_err("There is the page in SLC LRU that are the same as in DRAM LRU!\n");
    }
    if (lru->free_cnt > 0) { // SLC LRU有空闲
        opage = QTAILQ_FIRST(&lru->sl_free_page_list);
        f_assert(opage != NULL);
        QTAILQ_REMOVE(&lru->sl_free_page_list, opage, entry);
        lru->free_cnt--;
        if (lru->head == NULL && lru->length == 0 && lru->tail == NULL) {
            opage->lpn = lpn;
            lru->head = lru->tail = opage;
            opage->next = opage->prev = NULL;
        } else {
            opage->lpn = lpn;
            lru->tail->next = opage;
            opage->prev = lru->tail;
            opage->next = NULL;
            lru->tail = opage;
        }
        lru->length++;
        f_assert((lru->length + lru->free_cnt) == lru->space_length);
    } else {
        opage = lru->head;
        old_hash_key = opage->lpn % lru->capacity;
        lru->head = lru->head->next;
        lru->head->prev = NULL;
        if (opage->hash_pre == NULL && opage->hash_next == NULL) {
            if (opage == lru->hash_map[old_hash_key]) {
                lru->hash_map[old_hash_key] = NULL;
            } else {
                ftl_err("SLC lru struct err3!\n");
            }
        } else if (opage->hash_pre == NULL) {
            if (opage == lru->hash_map[old_hash_key]) {
                lru->hash_map[old_hash_key] = opage->hash_next;
                opage->hash_next->hash_pre = NULL;
                opage->hash_next = NULL;
            } else {
                ftl_err("SLC lru struct err4!\n");
            }
        } else if (opage->hash_next == NULL) {
            if (opage->hash_pre != NULL) {
                opage->hash_pre->hash_next = NULL;
                opage->hash_pre = NULL;
            } else {
                ftl_err("SLC lru struct err5!\n");
            }
        } else {
            opage->hash_pre->hash_next = opage->hash_next;
            opage->hash_next->hash_pre = opage->hash_pre;
            opage->hash_pre = NULL;
            opage->hash_next = NULL;
        }

        struct ppa victim_ppa = get_rb_maptbl_ent(ssd, opage->lpn);
        f_assert(mapped_ppa(&victim_ppa));
        struct Hybrid_page *victim_pg = get_pg(ssd, &victim_ppa);
        f_assert(victim_pg->status == PG_VALID && victim_pg->is_inlru == 1);
        victim_pg->is_inlru = 0; // 将victim page对应的Hybrid_page结构体的队列标记位置0，以后gc时就直接作废页面
        
        //mark_page_invalid(ssd, &victim_ppa);
        //uint64_t pgidx = ppa2pgidx(ssd, &victim_ppa);
        //f_assert(opage->lpn == ssd->rmap[pgidx]);
        //ssd->rb_maptbl[opage->lpn].ppa = UNMAPPED_PPA;
        //ssd->rmap[pgidx] = INVALID_LPN;
        
        opage->lpn = lpn;
        lru->tail->next = opage;
        opage->prev = lru->tail;
        opage->next = NULL;
        lru->tail = opage;
    }
    hpage = get_pg(ssd, &new_ppa);
    f_assert(hpage->status == PG_VALID && hpage->is_inlru == 0);
    hpage->is_inlru = 1;
    // 修改hash_map链
    if (lru->hash_map[hash_key] == NULL) {
        lru->hash_map[hash_key] = opage;
        return;
    } // 如果哈希没有映射过，直接映射

    // 如果已经有哈希映射了，采用头插策略
    opage->hash_next = lru->hash_map[hash_key];
    opage->hash_pre = NULL;
    lru->hash_map[hash_key]->hash_pre = opage;
    lru->hash_map[hash_key] = opage;
    return;

} // DRAM中排出的页面加入到read buffer中
*/

static void insert_dram_lru_page(struct ssd *ssd, uint64_t lpn, uint64_t rt, bool is_pre)
{
    struct node_lru *lru = &ssd->DRAM_LRU;
    int hash_key = lpn % lru->capacity;
    int old_hash_key;
    struct dram_node *dnode;
    // 判断1：逻辑地址是否已经在DRAM LRU中，若在则直接从队列中迁移到队尾
    dnode = is_indram(ssd, lpn);
    if (dnode != NULL) {
        /*
        if (is_pre == true && dnode->pre_state == true) {
            return;
        } 
        */
        if (is_pre == false && dnode->pre_state == true) {
            f_assert(dnode->access_flag == false);
            dnode->pre_state = false;
            dnode->access_flag = true;
            return;
        }
        
        struct dram_node* pre_node = dnode->prev;
        struct dram_node* next_node = dnode->next;

        if (pre_node == NULL && next_node == NULL) {
            
        } else if (pre_node == NULL) {
            if (dnode == lru->head) {
                lru->head = next_node;
                next_node->prev = NULL;
                lru->tail->next = dnode;
                dnode->prev = lru->tail;
                dnode->next = NULL;
                lru->tail = dnode;
            } else {
                ftl_err("Dram lru struct err1!\n");
            }
        } else if (next_node == NULL) {
            if (dnode == lru->tail) {
                
            } else {
                ftl_err("Dram lru struct err2!\n");
            }
        } else {
            pre_node->next = next_node;
            next_node->prev = pre_node;
            lru->tail->next = dnode;
            dnode->prev = lru->tail;
            dnode->next = NULL;
            lru->tail = dnode;
        }
        
        if (is_pre == true && dnode->pre_state == false) {
            f_assert(dnode->access_flag == true);
            dnode->pre_state = true;
            dnode->access_flag = false;
        }
        return;
    } // 页面不在DRAM LRU中，则需先分配dnode，之后迁移页面到DRAM中

    if (lru->free_cnt > 0) { // DRAM LRU有剩余空间
        dnode = QTAILQ_FIRST(&lru->dl_free_page_list);
        if (dnode == NULL) {
            printf("1QTAILQ_FIRST err!");
            exit(1);
        }
        QTAILQ_REMOVE(&lru->dl_free_page_list, dnode, entry);
        lru->free_cnt--;
        if (lru->head == NULL && lru->tail == NULL && lru->length == 0) {
            dnode->lpn = lpn;
            lru->head = lru->tail = dnode;
            dnode->next = dnode->prev = NULL;
        } else {
            dnode->lpn = lpn;
            lru->tail->next = dnode;
            dnode->prev = lru->tail;
            dnode->next = NULL;
            lru->tail = dnode;
        }
        dnode->begin_state = is_pre;
        dnode->pre_state = is_pre;
        dnode->access_flag = !is_pre;
        lru->length++;
        f_assert((lru->length + lru->free_cnt) == lru->space_length);
    } else {
        dnode = lru->head;
        old_hash_key = dnode->lpn % lru->capacity;
        lru->head = lru->head->next;
        lru->head->prev = NULL;
        if (dnode->hash_pre == NULL && dnode->hash_next == NULL) {
            if (dnode == lru->hash_map[old_hash_key]) {
                lru->hash_map[old_hash_key] = NULL;
                dnode->hash_next = dnode->hash_pre = NULL;
            } else {
                ftl_err("Dram lru struct err3!\n");
            }
        } else if (dnode->hash_pre == NULL) {
            if (dnode == lru->hash_map[old_hash_key]) {
                lru->hash_map[old_hash_key] = dnode->hash_next;
                dnode->hash_next->hash_pre = NULL;
                dnode->hash_next = NULL;
                dnode->hash_pre = NULL;
            } else {
                ftl_err("Dram lru struct err4!\n");
            }
        } else if (dnode->hash_next == NULL) {
            if (dnode->hash_pre != NULL) {
                dnode->hash_pre->hash_next = NULL;
                dnode->hash_pre = NULL;
                dnode->hash_next = NULL;
            } else {
                ftl_err("Dram lru struct err5!\n");
            }
        } else {
            dnode->hash_pre->hash_next = dnode->hash_next;
            dnode->hash_next->hash_pre = dnode->hash_pre;
            dnode->hash_next = NULL;
            dnode->hash_pre = NULL;
        }

        /* 修改结构后，需要将原有页面写入到READ BUFFER中 */
        // 需要某种条件，进行迁移
        //struct ppa old_ppa = get_maptbl_ent(ssd, dnode->lpn);
        //int pg_type = get_block_type(ssd, &old_ppa);
        //if (pg_type == HB_TLC) {
        //    if (dnode->begin_state == true && dnode->pre_state == true && dnode->access_count == 0) {
        //        insert_SLC_LRU_page(ssd, dnode->lpn, rt + ssd->sp.dram_pg_rd_lat);
        //    } else if (dnode->begin_state == false && dnode->pre_state == true) {
        //        insert_SLC_LRU_page(ssd, dnode->lpn, rt + ssd->sp.dram_pg_rd_lat);
        //    }
        //}

        if (dnode->begin_state == true) {
            ssd->sp.pre_count ++;
            if (dnode->access_count > 0) {
                ssd->sp.pre_hit_num ++;
            } 
        }

        dnode->lpn = lpn;
        dnode->begin_state = is_pre;
        dnode->pre_state = is_pre;
        dnode->access_flag = !is_pre;
        dnode->access_count = 0;
        lru->tail->next = dnode;
        dnode->prev = lru->tail;
        dnode->next = NULL;
        lru->tail = dnode;
    }


    // 判断2：逻辑地址对应的数据不在DRAM LRU中，原始数据要不在SLC中，要不在TLC中；同时，原始数据有可能存在备份在READ BUFFER中
    // 策略 1、先查看页面在不在READ BUFFER，在的话则需要判断从READ BUFFER读的快，还是从SLC/TLC中读的快，并迁移到DRAM中
    //      2、若READ BUFFER中不存在备份，则去SLC/TLC中查找，若有原始数据，则读出返回给用户端，同时将数据读到DRAM中
    struct ppa rb_ppa = get_rb_maptbl_ent(ssd, lpn);
    struct ppa ppa = get_maptbl_ent(ssd, lpn);
    if (!mapped_ppa(&ppa) || !valid_ppa(ssd, &ppa)) {
        printf("%s,lpn(%" PRId64 " not mapped to valid ppa)\n", ssd->ssdname, lpn);
        return;
    }
    //  READ BUFFER中存在页面备份，则需要比较一下READ BUFFER页面获取速度和SLC/TLC获取页面的速度
    if (mapped_ppa(&rb_ppa)) { // READ BUFFER中有备份
        if (!valid_ppa(ssd, &rb_ppa)) {
            ftl_err("rb_maptbl map error!\n");
        }
        struct Hybrid_lun *lun = get_lun(ssd, &ppa);
        struct Hybrid_channel *ch1 = get_ch(ssd, &ppa);
        struct Hybrid_rb_lun *rb_lun = get_rb_lun(ssd, &rb_ppa);
        struct Hybrid_channel *ch2 = get_ch(ssd, &rb_ppa);

        uint64_t t1 = (lun->next_lun_avail_time < rt) ? rt : lun->next_lun_avail_time;
        t1 += get_lat(ssd, &ppa, Hybrid_READ, 13);
        t1 = (ch1->next_ch_avail_time < t1) ? t1 : ch1->next_ch_avail_time;
        t1 += ssd->sp.slc_chnl_xfer_lat;

        uint64_t t2 = (rb_lun->next_lun_avail_time < rt) ? rt : rb_lun->next_lun_avail_time;
        t2 += get_lat(ssd, &rb_ppa, Hybrid_READ, 14);
        t2 = (ch2->next_ch2_avail_time < t2) ? t2 : ch2->next_ch2_avail_time;
        t2 += ssd->sp.slc_chnl_xfer_lat;

        if (t2 <= t1) { // 从READ BUFFER中读的快，从READ BUFFER中读取页面，并将READ BUFFER中的页面作废
            // 先将数据读到DRAM中
            struct nand_cmd ptr;
            ptr.type = PAGE_TRANS_IO;
            ptr.cmd = Hybrid_READ;
            ptr.stime = rt;
            uint64_t lat = ssd_advance_status(ssd, &rb_ppa, &ptr, 9);
            dnode->next_avail_time = rt + lat + ssd->sp.dram_pg_wr_lat;
            if (ssd->sp.class_flag == 1 && is_pre == false) {
                ssd->sp.rb_hit_num++;
            }
        } else { // 从READ BUFFER中读的慢，从SLC/TLC中读取页面
            // 将数据读入到DRAM中
            struct nand_cmd ptr;
            ptr.type = PAGE_TRANS_IO;
            ptr.cmd = Hybrid_READ;
            ptr.stime = rt;
            uint64_t lat = ssd_advance_status(ssd, &ppa, &ptr, 10);
            dnode->next_avail_time = rt + lat + ssd->sp.dram_pg_wr_lat;
            int b_t = get_block_type(ssd, &ppa);
            if (ssd->sp.class_flag == 1 && is_pre == false) {
                if (b_t == HB_SLC) {
                    ssd->sp.slc_hit_num++;
                } else if (b_t == HB_TLC) {
                    ssd->sp.tlc_hit_num++;
                } else {
                    ftl_err("1type err!\n");
                }
            }
        }
        // 页面迁移到DRAM中后，需要作废READ BUFFER中原有的页面
        struct one_page *opage = is_inSLClru(ssd, lpn);
        struct lpn_lru *SLC_LRU = &ssd->SLC_LRU;
        if (opage != NULL) { // 页面在SLC LRU中，需要先修改结构后再作废物理页面
            int hk = opage->lpn % SLC_LRU->capacity;
            // 修改LRU结构体
            if (opage->prev == NULL && opage->next == NULL) { // 只有一个page
                f_assert(SLC_LRU->head == opage && SLC_LRU->tail == opage);
                SLC_LRU->head = SLC_LRU->tail = NULL;
            } else if (opage->prev == NULL) { // 在头部
                f_assert(SLC_LRU->head == opage);
                SLC_LRU->head = opage->next;
                SLC_LRU->head->prev = NULL;
                opage->next = NULL;
            } else if (opage->next == NULL) { //在尾部
                f_assert(SLC_LRU->tail == opage);
                SLC_LRU->tail = opage->prev;
                SLC_LRU->tail->next = NULL;
                opage->prev = NULL;
            } else { // 在内部
                opage->prev->next = opage->next;
                opage->next->prev = opage->prev;
                opage->next = NULL;
                opage->prev = NULL;
            }
            f_assert(SLC_LRU->length > 0);
            SLC_LRU->length--;
            // 修改hash_map数据结构
            if (opage->hash_pre == NULL && opage->hash_next == NULL) {
                f_assert(SLC_LRU->hash_map[hk] == opage);
                SLC_LRU->hash_map[hk] = NULL;
            } else if (opage->hash_pre == NULL) {
                f_assert(SLC_LRU->hash_map[hk] == opage);
                SLC_LRU->hash_map[hk] = opage->hash_next;
                opage->hash_next->hash_pre = NULL;
                opage->hash_next = NULL;
            } else if (opage->hash_next == NULL) {
                f_assert(opage->hash_pre != NULL);
                opage->hash_pre->hash_next = NULL;
                opage->hash_pre = NULL;
            } else {
                opage->hash_pre->hash_next = opage->hash_next;
                opage->hash_next->hash_pre = opage->hash_pre;
                opage->hash_next = NULL;
                opage->hash_pre = NULL;
            }
            opage->lpn = INVALID_LPN;
            QTAILQ_INSERT_TAIL(&SLC_LRU->sl_free_page_list, opage, entry);
            SLC_LRU->free_cnt++;
            f_assert((SLC_LRU->free_cnt + SLC_LRU->length) == SLC_LRU->space_length);
        }
        mark_page_invalid(ssd, &rb_ppa);
        uint64_t pgidx = ppa2pgidx(ssd, &rb_ppa);
        f_assert(lpn == ssd->rmap[pgidx]);
        ssd->rb_maptbl[lpn].ppa = UNMAPPED_PPA;
        ssd->rmap[pgidx] = INVALID_LPN; 
    } else { // READ BUFFER中无备份
        /* 把数据读到DRAM中 */
        struct nand_cmd ptr;
        ptr.type = PAGE_TRANS_IO;
        ptr.cmd = Hybrid_READ;
        ptr.stime = rt;
        uint64_t lat = ssd_advance_status(ssd, &ppa, &ptr, 11);
        dnode->next_avail_time = rt + lat + ssd->sp.dram_pg_wr_lat;
        int b_t = get_block_type(ssd, &ppa);
        if (ssd->sp.class_flag == 1 && is_pre == false) {
            if (b_t == HB_SLC) {
                ssd->sp.slc_hit_num++;
            } else if (b_t == HB_TLC) {
                ssd->sp.tlc_hit_num++;
            } else {
                ftl_err("1type err!\n");
            }
        }
    }

    /* 修改hash_map链 */
    if (lru->hash_map[hash_key] == NULL) {
        lru->hash_map[hash_key] = dnode;
        return;
    } // 如果哈希没有映射过，直接映射

    dnode->hash_next = lru->hash_map[hash_key];
    dnode->hash_pre = NULL;
    lru->hash_map[hash_key]->hash_pre = dnode;
    lru->hash_map[hash_key] = dnode;
    // 如果已经哈希映射了，就需要哈希链表，找到哈希尾部并插入
} // 将页面插入到DRAM中

static void mode_predict(struct ssd *ssd, int index, int64_t time) {
    struct Hybridparams *spp = &ssd->sp;
    struct view_queue *vqueue = ssd->vqueue;

    qsort(vqueue->head, vqueue->used_num, sizeof(struct circulate_entry), compare2);

    uint64_t start_address = 0;
    uint64_t end_address = 0;
    //int size1 = 0;
    uint64_t pre_start_lpn = 0;
    uint64_t pre_end_lpn = 0;
    struct ppa ppa;
    //bool next_section_flag = false; // 如果存在当前请求段出现在两大段之间，直接预取两大段第二个大段的后一个连续地址
    int secter_begin = 0;
    int secter_end = 0;
    for (int i = 0; i < vqueue->used_num; i++) {
        if (i == 0) {
            start_address = vqueue->head[i].start_LBA;
            secter_begin = i;
            end_address = vqueue->head[i].end_LBA;
            secter_end = i;
            //size1 = vqueue->head[i].end_LBA - vqueue->head[i].start_LBA;
        } else if (i > 0 && i < vqueue->used_num) {
            if (vqueue->head[i].start_LBA >= start_address && vqueue->head[i].start_LBA <= end_address) { // 组合成一个分区
                //size1 = vqueue->head[i].end_LBA - vqueue->head[i].start_LBA;
                if (end_address < vqueue->head[i].end_LBA) {
                    end_address = vqueue->head[i].end_LBA;
                    secter_end = i;
                }
            } else { // 一个分区已经组合完毕，需判断分区与当前请求是否交叉，分为五种情况：在分区前[无连接]、在分区前[有连接]、在分区中、在分区下[上连接，下无连接]、在分区下[上有连接下有连接]
                if (ssd->access_history[index].start_LBA < start_address && (ssd->access_history[index].start_LBA + ssd->access_history[index].size) < start_address) { // 在分区前[无连接]
                    return; // 不做任何操作
                } else if (ssd->access_history[index].start_LBA < start_address && (ssd->access_history[index].start_LBA + ssd->access_history[index].size) >= start_address) { // 在分区前[有连接]
                    pre_start_lpn = (ssd->access_history[index].start_LBA - ssd->access_history[index].size) / spp->secs_per_pg;
                    pre_end_lpn = (ssd->access_history[index].start_LBA - 1) / spp->secs_per_pg;
                    for (uint64_t lpn = pre_start_lpn; lpn <= pre_end_lpn; lpn++) {
                        ppa = get_maptbl_ent(ssd, lpn);
                        if (!mapped_ppa(&ppa) || !valid_ppa(ssd, &ppa)) {
                            continue;
                        }
                        insert_dram_lru_page(ssd, lpn, time, true);
                        
                    }
                    pre_start_lpn =  end_address / spp->secs_per_pg;
                    pre_end_lpn = (end_address + (vqueue->head[secter_end].end_LBA - vqueue->head[secter_end].start_LBA) - 1) / spp->secs_per_pg;
                    for (uint64_t lpn = pre_start_lpn; lpn <= pre_end_lpn; lpn++) {
                        ppa = get_maptbl_ent(ssd, lpn);
                        if (!mapped_ppa(&ppa) || !valid_ppa(ssd, &ppa)) {
                            continue;
                        }
                        insert_dram_lru_page(ssd, lpn, time, true);
                        
                    }
                    return;
                } else if (ssd->access_history[index].start_LBA >= start_address && (ssd->access_history[index].start_LBA + ssd->access_history[index].size) <= end_address) { // 在分区中
                    return;
                } else if (ssd->access_history[index].start_LBA >= start_address && ssd->access_history[index].start_LBA <= end_address && (ssd->access_history[index].start_LBA + ssd->access_history[index].size) > end_address && (ssd->access_history[index].start_LBA + ssd->access_history[index].size) < vqueue->head[i].start_LBA) { // 在分区下[上连接，下无连接]
                    pre_start_lpn = (start_address - (vqueue->head[secter_begin].end_LBA - vqueue->head[secter_begin].start_LBA)) / spp->secs_per_pg;
                    pre_end_lpn = (start_address - 1) / spp->secs_per_pg;
                    for (uint64_t lpn = pre_start_lpn; lpn <= pre_end_lpn; lpn++) {
                        ppa = get_maptbl_ent(ssd, lpn);
                        if (!mapped_ppa(&ppa) || !valid_ppa(ssd, &ppa)) {
                            continue;
                        }
                        insert_dram_lru_page(ssd, lpn, time, true);
                        
                    }
                    pre_start_lpn = (ssd->access_history[index].start_LBA + ssd->access_history[index].size) / spp->secs_per_pg;
                    pre_end_lpn = (ssd->access_history[index].start_LBA + ssd->access_history[index].size + ssd->access_history[index].size - 1) / spp->secs_per_pg;
                    for (uint64_t lpn = pre_start_lpn; lpn <= pre_end_lpn; lpn++) {
                        ppa = get_maptbl_ent(ssd, lpn);
                        if (!mapped_ppa(&ppa) || !valid_ppa(ssd, &ppa)) {
                            continue;
                        }
                        insert_dram_lru_page(ssd, lpn, time, true);
                        
                    }
                    return;
                } else if (ssd->access_history[index].start_LBA >= start_address && ssd->access_history[index].start_LBA <= end_address && (ssd->access_history[index].start_LBA + ssd->access_history[index].size) >= vqueue->head[i].start_LBA) { // 在分区下[上有连接下有连接]
                    return;
                } else if (ssd->access_history[index].start_LBA > end_address) {
                    start_address = vqueue->head[i].start_LBA;
                    secter_begin = i;
                    end_address = vqueue->head[i].end_LBA;
                    secter_end = i;
                    continue;
                } else {
                    ftl_err("mode predict err1!\n");
                }
            }
        }
    }
    // 对最后一个类区域进行匹配
    if (ssd->access_history[index].start_LBA < start_address && (ssd->access_history[index].start_LBA + ssd->access_history[index].size) < start_address) { // 在分区前[无连接]
        return; // 不做任何操作
    } else if (ssd->access_history[index].start_LBA < start_address && (ssd->access_history[index].start_LBA + ssd->access_history[index].size) >= start_address) { // 在分区前[有连接]
        pre_start_lpn = (ssd->access_history[index].start_LBA - ssd->access_history[index].size) / spp->secs_per_pg;
        pre_end_lpn = (ssd->access_history[index].start_LBA - 1) / spp->secs_per_pg;
        for (uint64_t lpn = pre_start_lpn; lpn <= pre_end_lpn; lpn++) {
            ppa = get_maptbl_ent(ssd, lpn);
            if (!mapped_ppa(&ppa) || !valid_ppa(ssd, &ppa)) {
                continue;
            }
            insert_dram_lru_page(ssd, lpn, time, true);
            
        }
        pre_start_lpn =  end_address / spp->secs_per_pg;
        pre_end_lpn = (end_address + (vqueue->head[secter_end].end_LBA - vqueue->head[secter_end].start_LBA) - 1) / spp->secs_per_pg;
        for (uint64_t lpn = pre_start_lpn; lpn <= pre_end_lpn; lpn++) {
            ppa = get_maptbl_ent(ssd, lpn);
            if (!mapped_ppa(&ppa) || !valid_ppa(ssd, &ppa)) {
                continue;
            }
            insert_dram_lru_page(ssd, lpn, time, true);
            
        }
        return;
    } else if (ssd->access_history[index].start_LBA >= start_address && (ssd->access_history[index].start_LBA + ssd->access_history[index].size) <= end_address) { // 在分区中
        return;
    } else if (ssd->access_history[index].start_LBA >= start_address && ssd->access_history[index].start_LBA <= end_address && (ssd->access_history[index].start_LBA + ssd->access_history[index].size) > end_address) { // 在分区下[上连接]
        pre_start_lpn = (start_address - (vqueue->head[secter_begin].end_LBA - vqueue->head[secter_begin].start_LBA)) / spp->secs_per_pg;
        pre_end_lpn = (start_address - 1) / spp->secs_per_pg;
        for (uint64_t lpn = pre_start_lpn; lpn <= pre_end_lpn; lpn++) {
            ppa = get_maptbl_ent(ssd, lpn);
            if (!mapped_ppa(&ppa) || !valid_ppa(ssd, &ppa)) {
                continue;
            }
            insert_dram_lru_page(ssd, lpn, time, true);
            
        }
        pre_start_lpn = (ssd->access_history[index].start_LBA + ssd->access_history[index].size) / spp->secs_per_pg;
        pre_end_lpn = (ssd->access_history[index].start_LBA + ssd->access_history[index].size + ssd->access_history[index].size - 1) / spp->secs_per_pg;
        for (uint64_t lpn = pre_start_lpn; lpn <= pre_end_lpn; lpn++) {
            ppa = get_maptbl_ent(ssd, lpn);
            if (!mapped_ppa(&ppa) || !valid_ppa(ssd, &ppa)) {
                continue;
            }
            insert_dram_lru_page(ssd, lpn, time, true);
            
        }
        return;
    } else if (ssd->access_history[index].start_LBA > end_address) {
        return;
    } else {
        ftl_err("mode predict err1!\n");
    }
    return;
}

// 地址区域分区类别预取
static void class_predict(struct ssd *ssd, int index, int64_t time) {
    struct Hybridparams *spp = &ssd->sp;
    struct address_class *pre_class = search_class(ssd->root, ssd->access_history[index].start_LBA, (ssd->access_history[index].start_LBA + ssd->access_history[index].size));
    if (pre_class == NULL) {
        return;
    }
    uint64_t class_start_lpn = pre_class->start_LBA / spp->secs_per_pg;
    uint64_t class_end_lpn = (pre_class->end_LBA - 1) / spp->secs_per_pg;
    uint64_t start_lpn = ssd->access_history[index].start_LBA / spp->secs_per_pg;
    uint64_t end_lpn = (ssd->access_history[index].start_LBA + ssd->access_history[index].size - 1) / spp->secs_per_pg;
    uint64_t pre_start_lpn, pre_end_lpn;
    if ((class_end_lpn - class_start_lpn + 1) > ssd->sp.waterline){
        if ((start_lpn + end_lpn) >= (class_start_lpn + class_end_lpn)) {// 数据块在class空间中部的下面，采用2:3，即想上去30页，向下取20页
            struct ppa ppa;
            pre_start_lpn = start_lpn - (int)(0.6*ssd->sp.waterline);
            pre_end_lpn = start_lpn - 1;
            for (uint64_t lpn = pre_start_lpn; lpn <= pre_end_lpn; lpn++) {
                if (lpn >= class_start_lpn && lpn <= class_end_lpn) {
                    ppa = get_maptbl_ent(ssd, lpn);
                    if (!mapped_ppa(&ppa) || !valid_ppa(ssd, &ppa)) {
                        continue;
                    }
                    insert_dram_lru_page(ssd, lpn, time, true);
                    
                }
            }
            
            pre_start_lpn = end_lpn;
            pre_end_lpn = end_lpn + (int)(0.4*ssd->sp.waterline) - 1;
            for (uint64_t lpn = pre_start_lpn; lpn <= pre_end_lpn; lpn++) {
                if (lpn >= class_start_lpn && lpn <= class_end_lpn) {
                    ppa = get_maptbl_ent(ssd, lpn);
                    if (!mapped_ppa(&ppa) || !valid_ppa(ssd, &ppa)) {
                        continue;
                    }
                    insert_dram_lru_page(ssd, lpn, time, true);
                    
                }
            }
            return;
        } else {// 数据块在class空间中部的上面，采用3:2，即向上去20页，向下取30页
            struct ppa ppa;
            pre_start_lpn = start_lpn - (int)(0.4*ssd->sp.waterline);
            pre_end_lpn = start_lpn - 1;
            for (uint64_t lpn = pre_start_lpn; lpn <= pre_end_lpn; lpn++) {
                if (lpn >= class_start_lpn && lpn <= class_end_lpn) {
                    ppa = get_maptbl_ent(ssd, lpn);
                    if (!mapped_ppa(&ppa) || !valid_ppa(ssd, &ppa)) {
                        continue;
                    }
                    insert_dram_lru_page(ssd, lpn, time, true);
                    
                }
            }
            pre_start_lpn = end_lpn;
            pre_end_lpn = end_lpn + (int)(0.6*ssd->sp.waterline) - 1;
            for (uint64_t lpn = pre_start_lpn; lpn <= pre_end_lpn; lpn++) {
                if (lpn >= class_start_lpn && lpn <= class_end_lpn) {
                    ppa = get_maptbl_ent(ssd, lpn);
                    if (!mapped_ppa(&ppa) || !valid_ppa(ssd, &ppa)) {
                        continue;
                    }
                    insert_dram_lru_page(ssd, lpn, time, true);
                    
                }
            }
            return;
        }
    } else {
        pre_start_lpn = class_start_lpn;
        pre_end_lpn = class_end_lpn;
        struct ppa ppa;
        for (uint64_t lpn = pre_start_lpn; lpn <= pre_end_lpn; lpn++) {
            ppa = get_maptbl_ent(ssd, lpn);
            if (!mapped_ppa(&ppa) || !valid_ppa(ssd, &ppa)) {
                continue;
            }
            insert_dram_lru_page(ssd, lpn, time, true);
            
        }
        return;
    }
}

// 顺序预取
static void sequential_predict(struct ssd *ssd, int index, int64_t time) {
    struct Hybridparams *spp = &ssd->sp;
    uint64_t pre_start_lpn = (ssd->access_history[index].start_LBA + ssd->access_history[index].size) / spp->secs_per_pg;
    uint64_t pre_end_lpn = (ssd->access_history[index].start_LBA + ssd->access_history[index].size * 2 -1) / spp->secs_per_pg;
    struct ppa ppa;
    for (uint64_t lpn = pre_start_lpn; lpn <= pre_end_lpn; lpn++) {
        ppa = get_maptbl_ent(ssd, lpn);
        if (!mapped_ppa(&ppa) || !valid_ppa(ssd, &ppa)) {
            continue;
        }
        insert_dram_lru_page(ssd, lpn, time, true);
        
    }
    return;
}

static void insert_history2vqueue(void *arg, int index, NvmeRequest *req) {
    struct ssd *ssd = (struct ssd *)arg;
    struct Hybridparams *spp = &ssd->sp;
    struct view_queue *vqueue = ssd->vqueue;
    vqueue->used_num = 0;
    // 构造观测队列，用于模式预测
    if (index >= spp->length_vq) {
        for (int i = 0; i < spp->length_vq; i++) {
            vqueue->head[i].start_LBA = ssd->access_history[index - spp->length_vq + i].start_LBA;
            vqueue->head[i].end_LBA = ssd->access_history[index - spp->length_vq + i].start_LBA + ssd->access_history[index - spp->length_vq + i].size;
            vqueue->used_num += 1;
        }
    } else {
        int j = 0;
        for (int i = spp->nhistorys - (spp->length_vq - index); i < spp->nhistorys; i++) {
            vqueue->head[j].start_LBA = ssd->access_history[i].start_LBA;
            vqueue->head[j].end_LBA = ssd->access_history[i].start_LBA + ssd->access_history[i].size;
            vqueue->used_num += 1;
            j++;
        }
        for (int i = 0; i < index; i++) {
            vqueue->head[j].start_LBA = ssd->access_history[i].start_LBA;
            vqueue->head[j].end_LBA = ssd->access_history[i].start_LBA + ssd->access_history[i].size;
            vqueue->used_num += 1;
            j++;
        }
        if (j != vqueue->total_num || vqueue->used_num != vqueue->total_num) {
            ftl_err("vqueue is err1!\n");
        }
    }
    sequential_predict(ssd, index, 0);
    class_predict(ssd, index, 0);
    mode_predict(ssd, index, 0); // 根据v_queue识别模式，预取依据具体模式可能访问的4KB页
    
}

static uint64_t ssd_read(struct ssd *ssd, NvmeRequest *req, FILE *r_lat_log_file, FILE *hit)
{
    struct Hybridparams *spp = &ssd->sp;
    uint64_t lba = req->slba;
    int nsecs = req->nlb;
    struct ppa ppa;
    uint64_t start_lpn = lba / spp->secs_per_pg;
    uint64_t end_lpn = (lba + nsecs - 1) / spp->secs_per_pg;
    uint64_t lpn;
    uint64_t sublat, maxlat = 0;
    uint64_t cur_t, t;

    if (end_lpn >= spp->maptbl_len) {
        ftl_err("start_lpn=%"PRIu64",tt_pgs=%d\n", start_lpn, ssd->sp.maptbl_len);
    }

    /* normal IO read path */
    for (lpn = start_lpn; lpn <= end_lpn; lpn++) {
        if (ssd->sp.class_flag == 1) {
            spp->num += 1;
        }

        ppa = get_maptbl_ent(ssd, lpn);
        if (!mapped_ppa(&ppa) || !valid_ppa(ssd, &ppa)) {
            continue;
        }

        struct dram_node *dnode = is_indram(ssd, lpn);
        if (dnode != NULL) {
            if (ssd->sp.class_flag == 1) {
                spp->hit_num += 1;
            }
            insert_dram_lru_page(ssd, lpn, req->stime, false);
            dnode->access_count++;
            // cur_t = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
            cur_t = req->stime;
            t = (dnode->next_avail_time < cur_t) ? cur_t : dnode->next_avail_time;
            dnode->next_avail_time = t + ssd->sp.dram_pg_rd_lat;
            sublat = dnode->next_avail_time - req->stime;
        } else {
            insert_dram_lru_page(ssd, lpn, req->stime, false);
            dnode = is_indram(ssd, lpn);
            if (dnode == NULL) {
                ftl_err("Err! LPN not inserted into DRAM! \n");
            }
            dnode->access_count++;
            // cur_t = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
            cur_t = req->stime;
            t = (dnode->next_avail_time < cur_t) ? cur_t : dnode->next_avail_time;
            dnode->next_avail_time = t + ssd->sp.dram_pg_rd_lat;
            sublat = dnode->next_avail_time - req->stime;
        }
        if (spp->class_flag == 1) {
            spp->r_num += 1;
        }
        maxlat = (sublat > maxlat) ? sublat : maxlat;
    }
    if (ssd->sp.class_flag == 1) {
        fprintf(r_lat_log_file, "%lu, %lu\n", maxlat, end_lpn - start_lpn + 1);
        double hit_p = (double)spp->hit_num / spp->num * 100;
        fprintf(hit, "%lf\n", hit_p);
    }
    /*
    if (spp->test_flag == true) {
        print_DRAM_LRU(ssd, d_log);
        print_SLC_LRU(ssd, s_log);
    }
    */
    ssd->last_access_t = (ssd->last_access_t > req->stime) ? ssd->last_access_t : req->stime;

    return maxlat;
} // 执行SSD的读操作

static uint64_t ssd_write(struct ssd *ssd, NvmeRequest *req, FILE *r_lat_log_file, FILE *hit, FILE *w_lat_log_file, FILE *count)
{
    uint64_t lba = req->slba; // 起始逻辑地址 
    struct Hybridparams *spp = &ssd->sp; // ssd相关参数
    int len = req->nlb; // 扇区块数
    uint64_t start_lpn = lba / spp->secs_per_pg; // 起始物理页号（Logical Page Number）
    uint64_t end_lpn = (lba + len -1) / spp->secs_per_pg; // 结束物理页号（end Logical Page Number）
    struct ppa ppa; // 物理地址
    uint64_t lpn; // 逻辑页编号
    uint64_t curlat = 0, maxlat = 0; // 分别表示当前访问的延迟和最大访问延迟
    int r; // 结果标记位，判断操作是否成功

    if (start_lpn == 0 && len == 2 && spp->test_flag == false) {
        spp->test_flag = true;
        return maxlat;
    } else if (start_lpn == 0 && len == 2 && spp->test_flag == true) {
        //sync();
        fprintf(count, "rb_gc_count = %lu slc_gc_count = %lu tlc_gc_count = %lu\n", spp->rb_gc_count, spp->slc_gc_count, spp->tlc_gc_count);
        fprintf(count, "pre_count = %lu pre_hit_num = %lu\n", spp->pre_count, spp->pre_hit_num);
        fprintf(count, "w_num = %lu rw_num = %lu rb_w_num = %lu slc_w_num = %lu tlc_w_num = %lu\n", spp->w_num, spp->rw_num, spp->rb_w_num, spp->slc_w_num, spp->tlc_w_num);
        fprintf(count, "num = %lu hit_num = %lu r_num = %lu rr_num = %lu rb_r_num = %lu slc_r_num = %lu tlc_r_num = %lu\n", spp->num, spp->hit_num, spp->r_num, spp->rr_num, spp->rb_r_num, spp->slc_r_num, spp->tlc_r_num);
        fprintf(count, "rb_hit_num = %lu slc_hit_num = %lu tlc_hit_num = %lu\n", spp->rb_hit_num, spp->slc_hit_num, spp->tlc_hit_num);
        //printf("gc_count = %lu\n", spp->gc_count);
        fclose(count);
        fclose(r_lat_log_file);
        fclose(hit);
        fclose(w_lat_log_file);
        spp->test_flag = false;
        spp->class_flag = 0;
        return maxlat;
    }

    if (end_lpn >= spp->maptbl_len) {
        ftl_err("start_lpn=%"PRIu64",tt_pgs=%d\n", start_lpn, ssd->sp.maptbl_len);
    } // 判断页面访问是否合法，有没有超出范围


    while (should_gc_high(ssd, HB_SLC)) {
        /* perform GC here until !should_gc(ssd, SLC) */
        r = do_gc(ssd, true, HB_SLC);
        if (r == -1)
            break;
    } // 如果SLC空间不足，执行一次 line gc过程，释放SLC空间

    while (should_gc_high(ssd, HB_TLC)) {
        /* perform GC here until !should_gc(ssd, TLC) */
        r = do_gc(ssd, true, HB_TLC);
        if (r == -1)
            break;
    } // 如果TLC空间不足，执行一次line gc过程，释放TLC空间

    for (lpn = start_lpn; lpn <= end_lpn; lpn++) {
        // 写页面前，需要先实现数据一致性，将DRAM 和READ BUFFER中的页面作废
        struct dram_node *dnode;
        struct one_page *opage;
        ppa = get_rb_maptbl_ent(ssd, lpn);
        dnode = is_indram(ssd, lpn);
        opage = is_inSLClru(ssd, lpn);
        if (dnode != NULL) { // DRAM中有页面，需要置废
            f_assert(opage == NULL && !mapped_ppa(&ppa));
            struct node_lru *lru = &ssd->DRAM_LRU;
            int hash_key = dnode->lpn % lru->capacity;
            struct dram_node *pre_node = dnode->prev;
            struct dram_node *next_node = dnode->next;

            // 修改LRU结构体
            if (pre_node == NULL && next_node == NULL) { // LRU中只有这一个node
                f_assert(lru->head == dnode && lru->tail == dnode);
                lru->head = lru->tail = NULL;
            } else if (pre_node == NULL) { // 在LRU头部
                f_assert(lru->head == dnode);
                lru->head = dnode->next;
                lru->head->prev = NULL;
                dnode->next = NULL;
            } else if (next_node == NULL) { // 在LRU尾部
                f_assert(lru->tail == dnode);
                lru->tail = dnode->prev;
                lru->tail->next = NULL;
                dnode->prev = NULL;
            } else { // 在LRU中
                dnode->prev->next = dnode->next;
                dnode->next->prev = dnode->prev;
                dnode->next = NULL;
                dnode->prev = NULL;
            }
            f_assert(lru->length > 0);
            dnode->begin_state = false;
            dnode->pre_state = false;
            dnode->access_flag = false;
            lru->length--;
            // 修改hash_map结构
            if (dnode->hash_pre == NULL && dnode->hash_next == NULL) { // hash_map只有一个
                f_assert(lru->hash_map[hash_key] == dnode);
                lru->hash_map[hash_key] = NULL;
                dnode->hash_pre = dnode->hash_next = NULL;
            } else if (dnode->hash_pre == NULL) { // 在hash_map头部
                f_assert(lru->hash_map[hash_key] == dnode);
                lru->hash_map[hash_key] = dnode->hash_next;
                dnode->hash_next->hash_pre = NULL;
                dnode->hash_next = NULL;
                dnode->hash_pre = NULL;
            } else if (dnode->hash_next == NULL) { // 在hash_map尾部
                f_assert(dnode->hash_pre != NULL);
                dnode->hash_pre->hash_next = NULL;
                dnode->hash_pre = NULL;
                dnode->hash_next = NULL;
            } else {
                dnode->hash_pre->hash_next = dnode->hash_next;
                dnode->hash_next->hash_pre = dnode->hash_pre;
                dnode->hash_next = NULL;
                dnode->hash_pre = NULL;
            }
            dnode->lpn = INVALID_LPN;
            dnode->next_avail_time = 0;
            dnode->access_count = 0;
            QTAILQ_INSERT_TAIL(&lru->dl_free_page_list, dnode, entry);
            lru->free_cnt++;
            f_assert((lru->free_cnt + lru->length) == lru->space_length);
        } else if (mapped_ppa(&ppa)) { // DRAM中没有页面，就去READ BUFFER中查找，若有则置废
            f_assert(dnode == NULL);
            if (opage != NULL) {
                struct lpn_lru *lru = &ssd->SLC_LRU;
                int hash_key = opage->lpn % lru->capacity;
                struct one_page *pre_page = opage->prev;
                struct one_page *next_page = opage->next;

                // 修改LRU结构体
                if (pre_page == NULL && next_page == NULL) { // LRU只有一个page
                    f_assert(lru->head == opage && lru->tail == opage);
                    lru->head = lru->tail = NULL;
                } else if (pre_page == NULL) { // 在LRU头部
                    f_assert(lru->head == opage);
                    lru->head = opage->next;
                    lru->head->prev = NULL;
                    opage->next = NULL;
                } else if (next_page == NULL) { // 在LRU尾部
                    f_assert(lru->tail == opage);
                    lru->tail = opage->prev;
                    lru->tail->next = NULL;
                    opage->prev = NULL;
                } else { // 在LRU内部
                    opage->prev->next = opage->next;
                    opage->next->prev = opage->prev;
                    opage->next = NULL;
                    opage->prev = NULL;
                }
                f_assert(lru->length > 0);
                lru->length--;
                // 修改hash_map数据结构
                if (opage->hash_pre == NULL && opage->hash_next == NULL) { // hash_map只有一个
                    f_assert(lru->hash_map[hash_key] == opage);
                    lru->hash_map[hash_key] = NULL;
                    opage->hash_pre = opage->hash_next = NULL;
                } else if (opage->hash_pre == NULL) { // 在hash_map头部
                    f_assert(lru->hash_map[hash_key] == opage);
                    lru->hash_map[hash_key] = opage->hash_next;
                    opage->hash_next->hash_pre = NULL;
                    opage->hash_next = NULL;
                    opage->hash_pre = NULL;
                } else if (opage->hash_next == NULL) { // 在hash_map尾部
                    f_assert(opage->hash_pre != NULL);
                    opage->hash_pre->hash_next = NULL;
                    opage->hash_pre = NULL;
                    opage->hash_next = NULL;
                } else {
                    opage->hash_pre->hash_next = opage->hash_next;
                    opage->hash_next->hash_pre = opage->hash_pre;
                    opage->hash_next = NULL;
                    opage->hash_pre = NULL;
                }
                opage->lpn = INVALID_LPN;
                QTAILQ_INSERT_TAIL(&lru->sl_free_page_list, opage, entry);
                lru->free_cnt++;
                f_assert((lru->free_cnt + lru->length) == lru->space_length);

                //struct Hybrid_page *hpage = get_pg(ssd, &ppa);
                //hpage->is_inlru = 0;
            }
            // 将READ BUFFER页面映射作废
            mark_page_invalid(ssd, &ppa);
            uint64_t pgidx = ppa2pgidx(ssd, &ppa);
            f_assert(lpn == ssd->rmap[pgidx]);
            ssd->rb_maptbl[lpn].ppa = UNMAPPED_PPA;
            ssd->rmap[pgidx] = INVALID_LPN;
        }
        ppa = get_maptbl_ent(ssd, lpn); // 获取页面位置
        if (mapped_ppa(&ppa)) { // 如果页面是旧页面，需要先把页面无效，然后另外开辟物理位置，进行写入
            /* update old page information first */
            mark_page_invalid(ssd, &ppa);
            set_rmap_ent(ssd, INVALID_LPN, &ppa);
        }
        /* new write */
        ppa = get_new_page(ssd, HB_SLC); // 获取新的物理地址， SLC
        f_assert(mapped_ppa(&ppa));
        /* update maptbl */
        set_maptbl_ent(ssd, lpn, &ppa); // 更新页表
        /* update rmap */
        set_rmap_ent(ssd, lpn, &ppa); // 更新反向映射表
        mark_page_valid(ssd, &ppa); // 更新页面有效
        /* need to advance the write pointer here */
        if (ppa.g.blk < ssd->sp.slc_blks_per_pl) {
            ssd_advance_write_pointer(ssd, HB_SLC);
        } else if (ppa.g.blk >= ssd->sp.slc_blks_per_pl && ppa.g.blk < ssd->sp.slc_blks_per_pl + ssd->sp.tlc_blks_per_pl) {
            ssd_advance_write_pointer(ssd, HB_TLC);
        }

        struct nand_cmd swr;
        swr.type = USER_IO;
        swr.cmd = Hybrid_WRITE;
        swr.stime = req->stime;
        /* get latency statistics */
        curlat = ssd_advance_status(ssd, &ppa, &swr, 12);
        if (ssd->sp.class_flag == 1) {
            spp->w_num += 1;
        }
        maxlat = (curlat > maxlat) ? curlat : maxlat;
    }

    if (ssd->sp.class_flag == 1) {
        fprintf(w_lat_log_file, "%lu, %lu\n", maxlat, end_lpn - start_lpn + 1);
    }

    ssd->last_access_t = (ssd->last_access_t > req->stime) ? ssd->last_access_t : req->stime;
    //printf("wlat:%d\n", (int)maxlat);
    return maxlat;
} // 执行SSD的写写操作

static uint64_t char2uint64_t(char *p, size_t len) {
    uint64_t lba = 0;
    for (int i = 0; i < (int)len; i++) {
        if (p[i] == '\0') break;
        lba = lba * 10 + (p[i] - '0');
    }
    return lba;
}

static void *ftl_thread(void *arg)
{
    FemuCtrl *n = (FemuCtrl *)arg; // 解析参数，FEMU的控制参数
    struct ssd *ssd = n->ssd; // ssd信息
    NvmeRequest *req = NULL; // NVMe请求
    uint64_t lat = 0; // 指令的执行时间（latency），单位是ns
    int rc; // 主要用于记录FEMU的队列操作函数执行结果
    int i;

    FILE *w_lat_log_file = fopen("w_lat_log", "w");
    FILE *r_lat_log_file = fopen("r_lat_log", "w");
    FILE *hit = fopen("hit_log","w");
    FILE *count = fopen("count_log","w");
    FILE *w_log = fopen("w_log", "w");
    FILE *test_log = fopen("test_log", "w");

    FILE *workload_file1 = fopen("workload/WDEV_0.txt", "r");
    FILE *workload_file2 = fopen("workload/WDEV_0.txt", "r");
    if (workload_file1 == NULL || workload_file2 == NULL) {
        ftl_err("file err!\n");
        exit(1);
    }
    char line[100]; // 用于存储workload每一行记录的信息

    // FILE *d_log = fopen("dram_log", "w");
    // FILE *s_log = fopen("slc_log", "w");

    ssd->last_access_t = 0;
    int index = 0;

    while (!*(ssd->dataplane_started_ptr)) {
        usleep(100000);
    } // 等待SSD数据平面启动

    /* FIXME: not safe, to handle ->to_ftl and ->to_poller gracefully */
	// 一个 NvmeRequest 结构体指针 n 中的命令请求分发给 SSD 模拟线程和轮询线程
    ssd->to_ftl = n->to_ftl; // FTL 线程队列，存放的是FTL请求和指令
    ssd->to_poller = n->to_poller; // 轮询线程队列，存放FTL请求和指令的处理结果
    //以上两个队列位于主线程和FTL线程之间，用于通信

    while (1) {
        for (i = 1; i <= n->num_poller; i++) { // 由于处理线程有很多
            if (!ssd->to_ftl[i] || !femu_ring_count(ssd->to_ftl[i])) {
                continue;
            }
            rc = femu_ring_dequeue(ssd->to_ftl[i], (void *)&req, 1); // 从to_ftl队列中获取一个条请求
            if (rc != 1) {
                printf("FEMU: FTL to_ftl dequeue failed\n");
                printf("%d\n", rc);
            }

            f_assert(req != NULL);
            if (req->slba == 0 && (req->nlb == 1 || req->nlb == 2 || req->nlb == 3)) {
                if (req->nlb == 1) { // 初始化warm
                    if(req->cmd.opcode == NVME_CMD_WRITE) {
                        if (fgets(line, sizeof(line), workload_file1) != NULL) {
                            char *p = strtok(line, " ");
                            f_assert(p != NULL);
                            p = strtok(NULL, " ");
                            f_assert(p != NULL);
                            p = strtok(NULL, " ");
                            f_assert(p != NULL);
                            req->slba = char2uint64_t(p, strlen(p));
                            p = strtok(NULL, " ");
                            f_assert(p != NULL);
                            req->nlb = char2uint64_t(p, strlen(p));
                            p = strtok(NULL, " ");
                            f_assert(p != NULL);

                            lat = ssd_write(ssd, req, r_lat_log_file, hit, w_lat_log_file, count);
                            fprintf(w_log, "%lu , %d\n", req->slba, req->nlb);
                        }
                        
                    } else {
                        ftl_err("1cmd err!");
                    }
                } else {
                    if (req->nlb == 3) {
                        if (fgets(line, sizeof(line), workload_file2) != NULL) {
                            char *p = strtok(line, " ");
                            f_assert(p != NULL);
                            p = strtok(NULL, " ");
                            f_assert(p != NULL);
                            p = strtok(NULL, " ");
                            f_assert(p != NULL);
                            req->slba = char2uint64_t(p, strlen(p));
                            p = strtok(NULL, " ");
                            f_assert(p != NULL);
                            req->nlb = char2uint64_t(p, strlen(p));
                            p = strtok(NULL, " ");
                            f_assert(p != NULL);   
                        }
                    }

                    switch (req->cmd.opcode) {
                        case NVME_CMD_WRITE: // 写操作
                            lat = ssd_write(ssd, req, r_lat_log_file, hit, w_lat_log_file, count);
                            break;
                        case NVME_CMD_READ: // 读操作
                            lat = ssd_read(ssd, req, r_lat_log_file, hit);
                            if (ssd->sp.test_flag == true){
                                ssd->access_history[index].start_LBA = req->slba;
                                ssd->access_history[index].size = req->nlb;
                                //if (ssd->sp.class_flag == 1 && lat != ssd->sp.dram_pg_rd_lat) {
                                if (ssd->sp.class_flag == 1) {
                                    insert_history2vqueue(ssd, index, req); // Insert instruction information into the observation queue and perform prediction operations
                                }
                                index += 1;
                            }
                            break;
                        case NVME_CMD_DSM:
                            lat = 0;
                            break;
                        default:
                            // ftl_err("FTL received unkown request type, ERROR\n");
                            ;
                    }
                    fprintf(test_log, "%lu , %d , 0x%02X\n", req->slba, req->nlb, req->cmd.opcode);
                }
                
            }
            
            //printf("rb_line:%d\n", ssd->lm.rb_free_line_cnt);
            req->reqlat = lat;
            req->expire_time += lat;

            rc = femu_ring_enqueue(ssd->to_poller[i], (void *)&req, 1);

            if (index >= ssd->sp.nhistorys) {
	            index = 0;
		        if (ssd->sp.class_flag == 1) {
		            releaseTree(ssd);
		        }
		        update_class(ssd);
		        ssd->sp.class_flag = 1;
	        } /* Periodic update address class. */
            
            if (rc != 1) {
                ftl_err("FTL to_poller enqueue failed\n");
            }

            while (should_gc_high(ssd, HB_SLC)) { // gc SLC
                // perform GC here until !should_gc(ssd, SLC)
                int r = do_gc(ssd, true, HB_SLC);
                if (r == -1)
                    break;
            } // 如果SLC空闲空间不足，执行一次 line gc过程，释放空间

            while (should_gc_high(ssd, HB_TLC)) { // gc TLC
                // perform GC here until !should_gc(ssd, TLC)
                int r = do_gc(ssd, true, HB_TLC);
                if (r == -1)
                    break;
            } // 如果TLC空闲空间不足，执行一次 line gc过程，释放空间
        }
        /* clean one line if needed (in the background) */
        // 根据时间差来在空闲时间进行gc
        /* 
            if (should_gc(ssd, HB_READ_BUFFER)) {
                //printf("HB_READ_BUFFER\n");
                do_gc(ssd, false, HB_READ_BUFFER);
            }
         */
        /*
        if (should_gc(ssd, HB_SLC)) {
            //printf("HB_SLC\n");
            do_gc(ssd, false, HB_SLC);
        }
        if (should_gc(ssd, HB_TLC)) {
            //printf("HB_TLC\n");
            do_gc(ssd, false, HB_TLC);
        }
        */
    }

    return NULL;
}

