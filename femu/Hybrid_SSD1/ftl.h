#ifndef __FEMU_FTL_H
#define __FEMU_FTL_H

#include "../nvme.h"


#define INVALID_PPA     (~(0ULL))
/* 表示无效的物理页地址，在某些情况下可能用于标识未写入的物理页。
 *这个宏定义表示一个所有位都是1的整数，相当于二进制中的所有位都是1。
 */
#define INVALID_LPN     (~(0ULL))
/* 表示无效的逻辑页地址，在某些情况下可能用于标识未写入的逻辑页。
 * 这个宏定义也表示一个所有位都是1的整数。
 */
#define UNMAPPED_PPA     (~(0ULL))
/* 表示未映射的物理页地址，通常用于表示写入新数据时的空闲页。
 * 这个宏定义也表示一个所有位都是1的整数。 
 */ 

enum {
    GC_Threshold_T = 5000000000, // idel time
};

enum {
    Hybrid_READ =  0, // 读取Hybrid闪存储器中的数据
    Hybrid_WRITE = 1, // 向Hybrid闪存储器中写入数据
    Hybrid_ERASE = 2, // 从Hybrid闪存储器中擦除数据

    /* SLC时间设置 */
    Hybrid_SLC_READ_LATENCY = SLC_PAGE_READ_LATENCY_NS,
    Hybrid_SLC_PROG_LATENCY = SLC_PAGE_WRITE_LATENCY_NS,
    Hybrid_SLC_ERASE_LATENCY = SLC_BLOCK_ERASE_LATENCY_NS,
    //Hybrid_SLC_CHANNEL_TRANS_LATENCY = SLC_CHNL_PAGE_TRANSFER_LATENCY_NS,

    /* TLC时间设置 */
    Hybrid_TLC_LOWER_READ_LATENCY = TLC_LOWER_PAGE_READ_LATENCY_NS,
    Hybrid_TLC_CENTER_READ_LATENCY = TLC_CENTER_PAGE_READ_LATENCY_NS,
    Hybrid_TLC_UPPER_READ_LATENCY = TLC_UPPER_PAGE_READ_LATENCY_NS,

    Hybrid_TLC_LOWER_PROG_LATENCY = TLC_LOWER_PAGE_WRITE_LATENCY_NS,
    Hybrid_TLC_CENTER_PROG_LATENCY = TLC_CENTER_PAGE_WRITE_LATENCY_NS,
    Hybrid_TLC_UPPER_PROG_LATENCY = TLC_UPPER_PAGE_WRITE_LATENCY_NS,

    Hybrid_TLC_ERASE_LATENCY = TLC_BLOCK_ERASE_LATENCY_NS,
    //Hybrid_TLC_CHANNEL_TRANS_LATENCY = TLC_CHNL_PAGE_TRANSFER_LATENCY_NS



    Hybrid_CHANNEL_TRANS_LATENCY = 30000,


    DRAM_READ_LATENCY = 2000, // DRAM读延迟（以4KB为单位）
    DRAM_WRITE_LATENCY = 0, // 暂且忽略DRAM写延迟（以4KB为单位）

}; // Hybrid快闪存储器操作的类型和操作所需的延迟时间

enum {
    USER_IO = 0, // 用户IO，即由应用程序发起的IO请求，比如读取、写入数据等
    GC_IO = 1, // 垃圾回收IO，即由SSD控制器自动发起的IO请求，用于回收所使用的NAND Flash中的闲置空间，以便提高SSD的性能和寿命
    PAGE_TRANS_IO = 2, // 页面迁移
}; // 表示三种不同的IO类型

enum {
	//定义了扇区的状态
    SEC_FREE = 0,
    SEC_INVALID = 1,
    SEC_VALID = 2, 

	//定义了页面的状态
    PG_FREE = 0, 
    PG_INVALID = 1, 
    PG_VALID = 2,
};

enum {
    HB_READ_BUFFER = 0,
    HB_SLC = 1,
    HB_TLC = 2,
}; // 定义space类型

enum {
    slc = 0,
    lsb = 1,
    csb = 2,
    msb = 3,
}; // 定义page类型

enum {
    FEMU_ENABLE_GC_DELAY = 1, 
    FEMU_DISABLE_GC_DELAY = 2, 

    FEMU_ENABLE_DELAY_EMU = 3, 
    FEMU_DISABLE_DELAY_EMU = 4, 

    FEMU_RESET_ACCT = 5, 
    FEMU_ENABLE_LOG = 6, 
    FEMU_DISABLE_LOG = 7,
}; // FEMU模拟器的一些控制选项

#define BLK_BITS    (16) // 块（Block）号字段的位数
#define PG_BITS     (16) // 页（Page）号字段的位数
#define SEC_BITS    (8)  // 扇区号（Sector Number）字段所占用的位数
#define PL_BITS     (8)  // 平面号（Plane Number）字段所占用的位数，即一个盘片（Disk Platter）中包含多少个扇区（Sector）
#define LUN_BITS    (8)  // 逻辑单元号（LUN Number）字段所占用的位数，即一个物理设备（Physical Device）中包含多少个逻辑单元（Logical Unit）
#define CH_BITS     (7)  // 通道号（Channel Number）字段所占用的位数，即系统中包含多少个通道（Channel）

/* 物理地址结构 */
struct ppa {
    union {
        struct {
            uint64_t blk : BLK_BITS; // 块号
            uint64_t pg  : PG_BITS; // 页号
            uint64_t sec : SEC_BITS; // 扇区号
            uint64_t pl  : PL_BITS; // 平面号
            uint64_t lun : LUN_BITS; // 逻辑单元号（抽象出来的逻辑卷，由多个Flash芯片组成）[这里是Chip与Die的融合版本，应为这里都是并行的]
            uint64_t ch  : CH_BITS; // 通道号
            uint64_t rsv : 1; // 保留位
        } g;

        uint64_t ppa;
    };
}; // 定义了Flash存储器的物理地址结构（通道包含逻辑单元，逻辑单元包含平面，平面包含块，块包含页，页包含扇区）

typedef int Hybrid_sec_status_t; // 转意一下，表示Hybrid Flash 扇区状态的数据类型（nand_sec_status_t）

struct Hybrid_page {
    Hybrid_sec_status_t *sec; // 页内扇区状态
    int nsecs; // 页内扇区数
    int status; // 页状态
    int page_type; // [SLC:0 LSB:1 CSB:2 MSB:3]
    int is_inlru; // [在:1，不在:0]
}; // 页面状况结构体

struct Hybrid_block {
    struct Hybrid_page *pg; // 块内页面状况
    int npgs; // 块所包含的页数
    int ipc; /* invalid page count */ // 块内无效页数
    int vpc; /* valid page count */   // 块内有效页数
    int erase_cnt;					  //擦除计数
    int wp; /* current write pointer */ // 当前 NAND Flash 块的写入指针位置（即下一个可写入页面的位置）
    int blk_type; // [RB:0 SLC:1 TLC:2]
}; // 块状况结构体

struct Hybrid_plane {
    struct Hybrid_block *slc_blk; // 平面内SLC块 【原始数据存放位置】
    struct Hybrid_block *tlc_blk; // 平面内TLC块 【原始数据存放位置】
    int nslcblks; // 平面内SLC块数量
    int ntlcblks; // 平面内TLC块数量
}; // 平面状况结构体

struct Hybrid_rb_plane {
    struct Hybrid_block *rb_blk; // 平面内read buffer块 【数据备份存放位置，本质是SLC块】
    int nrbblks; // 平面内read buffer 块数量
}; // 平面状况结构体

struct Hybrid_lun {
    struct Hybrid_plane *pl; // 逻辑单元内的平面状态
    int npls; // 包含的平面数
    uint64_t next_lun_avail_time; // 当前 LUN 中下一次可用的时间（的时间戳）【当执行擦除/写入时，其他块禁止访问；或者容量达到阈值，等待释放】
    bool busy; // 当前 LUN 是否正在进行读/写操作
    uint64_t gc_endtime; // 当前 LUN 的 GC（垃圾回收）任务结束的时间（的时间戳）
}; // 逻辑单元状况结构体

struct Hybrid_rb_lun {
    struct Hybrid_rb_plane *pl; // read buffer逻辑单元内的平面状态
    int npls; // 包含的平面数
    uint64_t next_lun_avail_time; // 当前 LUN 中下一次可用的时间（的时间戳）【当执行擦除/写入时，其他块禁止访问；或者容量达到阈值，等待释放】
    bool busy; // 当前 LUN 是否正在进行读/写操作
    uint64_t gc_endtime; // 当前 LUN 的 GC（垃圾回收）任务结束的时间（的时间戳）
}; // 逻辑单元状况结构体

struct Hybrid_channel {
    struct Hybrid_rb_lun *rb_lun; // 通道内Read buffer逻辑单元状态
    struct Hybrid_lun *lun; // 通道内逻辑单元状态
    int nrb_luns; // 通道连接的Read buffer 逻辑单元数量
    int nluns;  // 通道连接的逻辑单元数量
    uint64_t next_ch_avail_time; // 通道下一次可用的时间
    uint64_t next_ch2_avail_time;
    bool busy; // 该通道当前是否正在执行操作
    uint64_t gc_endtime; // 在通道上执行垃圾回收操作的结束时间
}; // 通道状况结构体

struct Hybridparams {
    int secsz;        /* sector size in bytes */ // 每个扇区大小
    int secs_per_pg;  /* # of sectors per page */ // 每个页面中扇区数目
    int rb_pgs_per_blk;  //每个rb块中页面数目
    int slc_pgs_per_blk; /* # of Hybrid slc pages per block */ // 每个slc块中页面数目
    int tlc_pgs_per_blk; /* # of Hybrid tlc pages per block */ // 每个tlc块中页面数目
    int rb_blks_per_pl;  // 每个平面中rb块数目
    int slc_blks_per_pl; /* # of slc blocks per plane */ // 每个平面中slc块数目
    int tlc_blks_per_pl; /* # of tlc blocks per plane */ // 每个平面中tlc块数目
    int rb_pls_per_lun;
    int pls_per_lun;  /* # of planes per LUN (Die) */ // 每个逻辑单元的平面数
    int rb_luns_per_ch; // 每个通道中的read buffer LUN数量
    int luns_per_ch;  /* # of LUNs per channel */ // 每个通道中的 LUN 数量
    int nchs;         /* # of channels in the SSD */ // 通道数量

    bool test_flag;
    int flag;

    /*用于计算命中率*/
    uint64_t num; // 记录总的读请求页面数量（以页为单位）
    uint64_t hit_num; // 记录命中的读请求页面数量（以页为单位）【DRAM命中页面数量】

    uint64_t rb_hit_num; // 记录read buffer命中的读请求页面数量（以页为单位）
    uint64_t slc_hit_num; // 记录SLC介质命中的读请求页面数量（以页为单位）
    uint64_t tlc_hit_num; // 记录TLC介质命中的读请求页面数量 (以页为单位)
    /*gc计数*/
    uint64_t rb_gc_count; // 记录read buffer 存储介质的gc次数
    uint64_t slc_gc_count; // 记录SLC 主存储介质的gc次数
    uint64_t tlc_gc_count; // 记录TLC主存储介质的gc次数
    /*用于计算写放大比率*/
    uint64_t w_num; // 记录写入请求数量（以页为单位）
    uint64_t rw_num; // 记录真实写入数量（以页为单位）
    uint64_t rb_w_num;
    uint64_t slc_w_num;
    uint64_t tlc_w_num;
    /*用于计算读取放大比率*/
    uint64_t r_num; // 记录读取请求数量（以页为单位）
    uint64_t rr_num; // 记录真实读取数量（以页为单位）
    uint64_t rb_r_num;
    uint64_t slc_r_num;
    uint64_t tlc_r_num;


    /* TLC芯片的参数设计 */
    int tlc_upg_rd_lat;
    int tlc_cpg_rd_lat;
    int tlc_lpg_rd_lat;

    int tlc_upg_wr_lat;
    int tlc_cpg_wr_lat;
    int tlc_lpg_wr_lat;

    int tlc_blk_er_lat;

    int tlc_chnl_pg_xfer_lat;

    /* SLC芯片的参数设计 */
    int slc_pg_rd_lat;
    int slc_pg_wr_lat;
    int slc_blk_er_lat;
    int slc_chnl_xfer_lat;

    /* DRAM读取延迟 */
    int dram_pg_rd_lat;
    int dram_pg_wr_lat;

    uint64_t gc_thres_t; // 垃圾回收阈值时间
    double gc_thres_pcent;
    double gc_thres_pcent_high; // 高阈值垃圾回收（garbage collection）的阈值百分比。它是 SSD 设备中的一个参数，用于在存储器中垃圾回收操作过多时触发高阈值垃圾回收
    int rb_gc_thres_lines;
    int slc_gc_thres_lines;
    int tlc_gc_thres_lines;
    int rb_gc_thres_lines_high;
    int slc_gc_thres_lines_high;
    int tlc_gc_thres_lines_high;
    bool enable_gc_delay; // 是否启用 gc 延迟
    
    /* 上一层的缓存、存储结构参数 */
    int rb_tt_lines; // read buffer line 的数量
    int slc_tt_lines; // slc io line 的数量
    int tlc_tt_lines; // tlc io line 的数量

    /* below are all calculated values */
    int secs_per_rb_blk;  // 每个RB块所包含的扇区数
    int secs_per_slc_blk; /* # of sectors per SLC block */ // 每个SLC块所包含的扇区数
    int secs_per_tlc_blk; /* # of sectors per TLC block */ // 每个TLC块所包含的扇区数
    int secs_per_rb_pl;
    int secs_per_pl;  /* # of sectors per plane */ // 每个 plane 所包含的扇区数
    int secs_per_rb_lun;
    int secs_per_lun; /* # of sectors per LUN */ // 每个 LUN（Logic Unit Number）所包含的扇区数
    int secs_per_ch;  /* # of sectors per channel */ // 每个通道所包含的扇区数
    int tt_secs;
    /* 上一层的缓存、存储结构参数 */
    int tt_rb_secs;
    int tt_slc_secs;
    int tt_tlc_secs;
    int tt_store_secs;

    int pgs_per_rb_pl;
    int pgs_per_pl;   /* # of pages per plane */ // 每个 plane 包含的页数
    int pgs_per_rb_lun;
    int pgs_per_lun;  /* # of pages per LUN (Die) */ // 每个 LUN 包含的页数
    int pgs_per_ch;   /* # of pages per channel */ // 每个通道包含的页数
    int tt_pgs;       /* total # of pages in the SSD */ // SSD 存储器的总页数[包括read buffer、slc、tlc]
    /* 上一层的缓存、存储结构参数 */
    int tt_rb_pgs;    /*SSD read buffer页面总数*/
    int tt_slc_pgs;
    int tt_tlc_pgs;
    int tt_store_pgs; // 除掉read buffer以外的slc和tlc容量的总和
    int maptbl_len;

    int blks_per_rb_lun;
    int blks_per_lun; /* # of blocks per LUN */ // 每个 LUN 中包含的块数
    int blks_per_ch;  /* # of blocks per channel */ // 每个通道中包含的块数
    int tt_blks;      /* total # of blocks in the SSD */ // SSD 存储器的总块数
    /* 上一层的缓存、存储结构参数 */
    int tt_rb_blks; // read buffer blk数量
    int tt_slc_blks;
    int tt_tlc_blks;
    int tt_store_blks;

    /* 上一层的缓存、存储结构参数 */
    int rb_secs_per_line;
    int slc_secs_per_line; // 每个垃圾回收操作对应的线性扇区数目
    int tlc_secs_per_line; // 
    int rb_pgs_per_line;
    int slc_pgs_per_line; // 每个垃圾回收操作对应的线性页面数目
    int tlc_pgs_per_line; // 
    int rb_blks_per_line;
    int slc_blks_per_line; // 每个垃圾回收操作对应的线性块数目
    int tlc_blks_per_line;

    int rb_pls_per_ch;
    int pls_per_ch;   /* # of planes per channel */ // 每个通道的平面planes数
    int tt_rb_pls;
    int tt_pls;       /* total # of planes in the SSD */ // SSD存储器的总平面数

    int tt_rb_luns;
    int tt_luns;      /* total # of LUNs in the SSD */

    int SLC_lru_space_len; // SLC LRU长度
    int SLC_lru_capacity; // SLC hash表长度
    int DRAM_lru_space_len; // DRAM LRU长度
    int DRAM_lru_capacity; // DRAM hash表长度



}; // (有待修改)

typedef struct line {
    int id;  /* line id, the same as corresponding block id */ // line的ID
    int ipc; /* invalid page count in this line */ // 无效页数目
    int vpc; /* valid page count in this line */ // 有效页数目
    QTAILQ_ENTRY(line) entry; /* in either {free,victim,full} list */ // line状态
    /* position in the priority queue for victim lines */
    size_t pos; //记录该 line 在优先级队列中的位置，用于垃圾回收时进行优先级排序
    int line_type; // [rb:0 SLC:1 TLC:2]
} line; // 按优先级排列的块双向列表（一个line一般对应一个块）【目前先考虑SLC Block和TLC Block统一在一个line中进行gc】

/* wp: record next write addr */
struct write_pointer {
    struct line *curline; // 当前数据所在的存储行（line）
    int ch; //  存储数据的通道编号(channel number)
    int lun; // 存储数据的LUN（Logical Unit Number），是存储层次结构中的一个逻辑存储单元
    int pg; // 存储数据的页编号(page number)
    int blk; // 存储数据的块编号(block number)
    int pl; // 存储数据的plane编号
}; // 存储系统中，写入位置【需要分别考虑SLC的write pointer和TLC的write pointer】

struct line_mgmt {
    struct line *rb_lines; // read buffer line 管理空间【SLC】
    struct line *slc_lines; // 管理SLC中除read buffer空间的SLC line
    struct line *tlc_lines; // 管理TLC的存储空间
    /* free line list, we only need to maintain a list of blk numbers */
    QTAILQ_HEAD(rb_free_line_list, line) rb_free_line_list;
    QTAILQ_HEAD(slc_free_line_list, line) slc_free_line_list; // 存储空闲存储行(line)的列表
    QTAILQ_HEAD(tlc_free_line_list, line) tlc_free_line_list;
    pqueue_t *rb_victim_line_pq;
    pqueue_t *slc_victim_line_pq; // 存储被选为牺牲行（vicitim line）的pq，通常使用优先队列(pq)实现，用于存储当前可以用来作为牺牲行的行，并按照一定的优先级进行排列
    pqueue_t *tlc_victim_line_pq;
    //QTAILQ_HEAD(victim_line_list, line) victim_line_list;
    QTAILQ_HEAD(rb_full_line_list, line) rb_full_line_list;
    QTAILQ_HEAD(slc_full_line_list, line) slc_full_line_list; // 存储已满存储行(line)的列表
    QTAILQ_HEAD(tlc_full_line_list, line) tlc_full_line_list;
    int rb_tt_lines;
    int slc_tt_lines; // 存储总的存储行(line)数量
    int tlc_tt_lines;
    int rb_free_line_cnt;
    int slc_free_line_cnt; // 存储空闲存储行(line)的数量
    int tlc_free_line_cnt;
    int rb_victim_line_cnt;
    int slc_victim_line_cnt; // 存储被选为牺牲行(victim line)的数量
    int tlc_victim_line_cnt;
    int rb_full_line_cnt;
    int slc_full_line_cnt; // 存储已满存储行(line)的数量
    int tlc_full_line_cnt;
}; // 管理存储系统中的存储行(line)

struct nand_cmd {
    int type; // 操作类型
    int cmd; // 具体指令
    int64_t stime; /* Coperd: request arrival time */ // 命令到达时间，通常用于记录命令请求的时间戳，用于计算请求到响应的时间间隔
}; // nand指令（用于计算处理延迟时间）

// 定义SLC读取双向链表节点
typedef struct one_page {
    struct one_page* prev;
    struct one_page* next;
    uint64_t lpn;
    int id;
    struct one_page* hash_next;
    struct one_page* hash_pre;
    QTAILQ_ENTRY(one_page) entry;
} one_page;

// 定义页SLC LRU队列
struct lpn_lru {
    struct one_page* head;
    struct one_page* tail;
    int length; // LRU目前长度
    int space_length; // 节点空间长度
    uint64_t capacity; // 哈希计算表长度
    struct one_page** hash_map;
    struct one_page* one_page_space;
    QTAILQ_HEAD(sl_free_page_list, one_page) sl_free_page_list;
    int free_cnt;
};

// 定义DRAM读取双链表
typedef struct dram_node {
    struct dram_node* prev;
    struct dram_node* next;
    uint64_t lpn;
    uint64_t next_avail_time;
    int access_count;
    int id;
    struct dram_node* hash_next;
    struct dram_node* hash_pre;
    QTAILQ_ENTRY(dram_node) entry;
} dram_node;


// 定义DRAM LRU队列
struct node_lru {
    struct dram_node* head;
    struct dram_node* tail;
    int length;
    int space_length;
    uint64_t capacity;
    struct dram_node** hash_map;
    struct dram_node* node_space;
    QTAILQ_HEAD(dl_free_page_list, dram_node) dl_free_page_list;
    int free_cnt;
};

struct ssd {
    char *ssdname; // SSD 的名称
    struct Hybridparams sp; // SSD 的一些参数，包括通道数、封装类型、总空间大小等
    struct Hybrid_channel *ch; // 对应 SSD 的通道（Channel）数组，多个通道可以并行读写

    struct ppa *maptbl; /* page level mapping table */ // SSD 的页面映射表（Page-level Mapping Table），用于将逻辑地址和物理地址进行映射，管理 SSD 存储页面
    struct ppa *rb_maptbl; // read buffer页面映射表，用于将逻辑地址映射到read buffer物理地址

    uint64_t *rmap;     /* reverse mapptbl, assume it's stored in OOB */ // 逆向映射表（Reverse Mapping Table），用于从物理地址找到对应的逻辑地址，以支持读取操作

    struct write_pointer rb_wp; // read buffer 写指针
    struct write_pointer slc_wp; // 写指针（Write Pointer），指向当前可用的空闲页面，用于加速数据写入；
    struct write_pointer tlc_wp;

    struct line_mgmt lm; // 线性扇区（Line）管理模块，用于管理 SSD 存储器中的线性扇区，包括线性扇区的分配、擦除、垃圾回收等操作

    /* lockless ring for communication with NVMe IO thread */
	// 两个锁无环队列，用于从管理 SSD 存储的主控芯片（Control Chip）传递信息到 Flash Translation Layer（FTL）线程和 I/O Polling 线程
	struct rte_ring **to_ftl;
    struct rte_ring **to_poller;
	
    bool *dataplane_started_ptr; // 一个布尔值指针，表示数据平面（Data Plane）线程是否已经启动
    QemuThread ftl_thread; // FTL 线程，用于处理逻辑地址转换成物理页面地址，并执行实际的数据读写操作

    uint64_t last_access_t; // 记录SSD最近一次访问时间
    int free_rb_pg_n;
    int free_slc_pg_n;
    int free_tlc_pg_n;

    struct lpn_lru SLC_LRU;
    struct node_lru DRAM_LRU;

}; // 描述固态硬盘(SSD)的数据结构

void hbssd_init(FemuCtrl *n);

#define f_assert(expression) assert(expression)


#ifdef FEMU_DEBUG_FTL
#define ftl_debug(fmt, ...) \
    do { printf("[FEMU] FTL-Dbg: " fmt, ## __VA_ARGS__); } while (0)
#else
#define ftl_debug(fmt, ...) \
    do { } while (0)
#endif // 用于在FEMU的FTL层代码中输出调试信息

#define ftl_err(fmt, ...) \
    do { fprintf(stderr, "[FEMU] FTL-Err: " fmt, ## __VA_ARGS__); } while (0) // 用于在FEMU的FTL层代码中输出错误信息

#define ftl_log(fmt, ...) \
    do { printf("[FEMU] FTL-Log: " fmt, ## __VA_ARGS__); } while (0) // 用于在FEMU的FTL层代码中输出日志信息

/* FEMU assert() */
//#ifdef FEMU_DEBUG_FTL
//#define ftl_assert(expression) assert(expression)
//#else
//#define ftl_assert(expression) // 空白宏
//#endif

#endif
