#pragma once
#define SSTATUS_SPIE (1 << 5)
#define SSTATUS_SPP  (1 << 8)   // previous privilege: 1 = trap came from S-mode
#define SCAUSE_ECALL 8
#define SCAUSE_INTERRUPT (1u << 31)  // top bit of scause: interrupt, not exception
#define SCAUSE_S_TIMER   5
#define SIE_STIE     (1 << 5)   // enable supervisor timer interrupts

#define SBI_EID_TIME 0x54494D45 // "TIME" extension (sbi_set_timer)
#define TIME_SLICE_TICKS 500000 // 50 ms at QEMU virt's 10 MHz timebase
#define PROC_EXITED   2

#define PANIC(fmt, ...)                                                        \
    do {                                                                       \
        printf("PANIC: %s:%d: " fmt "\n", __FILE__, __LINE__, ##__VA_ARGS__);  \
        while (1) {}                                                           \
    } while (0)

struct sbiret {
    long error;
    long value;
};

#include "common.h"

struct trap_frame {
    uint32_t ra;
    uint32_t gp;
    uint32_t tp;
    uint32_t t0;
    uint32_t t1;
    uint32_t t2;
    uint32_t t3;
    uint32_t t4;
    uint32_t t5;
    uint32_t t6;
    uint32_t a0;
    uint32_t a1;
    uint32_t a2;
    uint32_t a3;
    uint32_t a4;
    uint32_t a5;
    uint32_t a6;
    uint32_t a7;
    uint32_t s0;
    uint32_t s1;
    uint32_t s2;
    uint32_t s3;
    uint32_t s4;
    uint32_t s5;
    uint32_t s6;
    uint32_t s7;
    uint32_t s8;
    uint32_t s9;
    uint32_t s10;
    uint32_t s11;
    uint32_t sp;
} __attribute__(()); //undid packed

//CONTROL AND STATUS REGISTERS
#define READ_CSR(reg)                                                          \
    ({                                                                         \
        unsigned long __tmp;                                                   \
        __asm__ __volatile__("csrr %0, " #reg : "=r"(__tmp));                  \
        __tmp;                                                                 \
    })

#define WRITE_CSR(reg, value)                                                  \
    do {                                                                       \
        uint32_t __tmp = (value);                                              \
        __asm__ __volatile__("csrw " #reg ", %0" ::"r"(__tmp));                \
    } while (0)


//PROCESS CONTROL BLOCKS
#define PROCS_MAX 8       // Maximum number of processes

#define PROC_UNUSED   0   // Unused process control structure
#define PROC_RUNNABLE 1   // Runnable process

struct process {
    int pid;
    int state;
    vaddr_t sp;
    uint32_t *page_table;
    char args[ARGS_MAX];     // argument string handed over by exec
    int ppid;                // pid of the process that exec'd this one
    uint8_t stack[8192];
};

//PAGE TABLE MACROS
#define SATP_SV32 (1u << 31)
#define PAGE_V    (1 << 0)   // "Valid" bit (entry is enabled)
#define PAGE_R    (1 << 1)   // Readable
#define PAGE_W    (1 << 2)   // Writable
#define PAGE_X    (1 << 3)   // Executable
#define PAGE_U    (1 << 4)   // User (accessible in user mode)

#define USER_BASE 0x1000000
#define USER_STACK_PAGES 1
#define USER_STACK_TOP (USER_BASE + 0x100000) 
// Programs may be loaded in [USER_BASE, USER_LIMIT); the stack page lies above.
#define USER_LIMIT     (USER_STACK_TOP - USER_STACK_PAGES * PAGE_SIZE)

// ELF32 (see the "elf" man page). Only what the loader needs.
#define ELF_MACHINE_RISCV 243
#define PT_LOAD           1

struct elf32_header {
    uint8_t  ident[16];   // 0x7f 'E' 'L' 'F', class (1 = 32-bit), endianness (1 = little)
    uint16_t type;
    uint16_t machine;     // 243 = RISC-V
    uint32_t version;
    uint32_t entry;       // address of the first instruction
    uint32_t phoff;       // where the program-header table starts in the file
    uint32_t shoff;
    uint32_t flags;
    uint16_t ehsize;
    uint16_t phentsize;   // size of one program header
    uint16_t phnum;       // how many program headers
    uint16_t shentsize, shnum, shstrndx;
} __attribute__((packed));

struct elf32_phdr {       // one row of the "instruction sheet"
    uint32_t type;        // PT_LOAD = "put this in memory"
    uint32_t offset;      // where its bytes start in the file
    uint32_t vaddr;       // where it goes in memory
    uint32_t paddr;
    uint32_t filesz;      // bytes to copy from the file
    uint32_t memsz;       // bytes it occupies in memory (>= filesz; the rest is zeros)
    uint32_t flags;
    uint32_t align;
} __attribute__((packed));

//virtio macros
#define SECTOR_SIZE       512
#define VIRTQ_ENTRY_NUM   16
#define VIRTIO_DEVICE_BLK 2
#define VIRTIO_BLK_PADDR  0x10001000
#define VIRTIO_REG_MAGIC         0x00
#define VIRTIO_REG_VERSION       0x04
#define VIRTIO_REG_DEVICE_ID     0x08
#define VIRTIO_REG_QUEUE_SEL     0x30
#define VIRTIO_REG_QUEUE_NUM_MAX 0x34
#define VIRTIO_REG_QUEUE_NUM     0x38
#define VIRTIO_REG_QUEUE_ALIGN   0x3c
#define VIRTIO_REG_QUEUE_PFN     0x40
#define VIRTIO_REG_QUEUE_READY   0x44
#define VIRTIO_REG_QUEUE_NOTIFY  0x50
#define VIRTIO_REG_DEVICE_STATUS 0x70
#define VIRTIO_REG_DEVICE_CONFIG 0x100
#define VIRTIO_STATUS_ACK       1
#define VIRTIO_STATUS_DRIVER    2
#define VIRTIO_STATUS_DRIVER_OK 4
#define VIRTIO_STATUS_FEAT_OK   8
#define VIRTQ_DESC_F_NEXT          1
#define VIRTQ_DESC_F_WRITE         2
#define VIRTQ_AVAIL_F_NO_INTERRUPT 1
#define VIRTIO_BLK_T_IN  0
#define VIRTIO_BLK_T_OUT 1

// Virtqueue Descriptor area entry.
struct virtq_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} __attribute__((packed));

// Virtqueue Available Ring.
struct virtq_avail {
    uint16_t flags;
    uint16_t index;
    uint16_t ring[VIRTQ_ENTRY_NUM];
} __attribute__((packed));

// Virtqueue Used Ring entry.
struct virtq_used_elem {
    uint32_t id;
    uint32_t len;
} __attribute__((packed));

// Virtqueue Used Ring.
struct virtq_used {
    uint16_t flags;
    uint16_t index;
    struct virtq_used_elem ring[VIRTQ_ENTRY_NUM];
} __attribute__((packed));

// Virtqueue.
struct virtio_virtq {
    struct virtq_desc descs[VIRTQ_ENTRY_NUM];
    struct virtq_avail avail;
    struct virtq_used used __attribute__((aligned(PAGE_SIZE)));
    int queue_index;
    volatile uint16_t *used_index;
    uint16_t last_used_index;
} __attribute__((packed));

// Virtio-blk request.
struct virtio_blk_req {
    uint32_t type;
    uint32_t reserved;
    uint64_t sector;
    uint8_t data[512];
    uint8_t status;
} __attribute__((packed));

//tar file system
#define FILES_MAX      8
#define FILE_DATA_MAX  8192   // room for a small program (ELF ~3 KB)
// Worst case in the tar image: one header sector + file data, per file.
#define DISK_MAX_SIZE  ((SECTOR_SIZE + FILE_DATA_MAX) * FILES_MAX)

struct tar_header {
    char name[100];
    char mode[8];
    char uid[8];
    char gid[8];
    char size[12];
    char mtime[12];
    char checksum[8];
    char type;
    char linkname[100];
    char magic[6];
    char version[2];
    char uname[32];
    char gname[32];
    char devmajor[8];
    char devminor[8];
    char prefix[155];
    char padding[12];
    char data[];      // Array pointing to the data area following the header
                      // (flexible array member)
} __attribute__((packed));

struct file {
    bool in_use;      // Indicates if this file entry is in use
    char name[100];   // File name
    char data[FILE_DATA_MAX];  // File content
    size_t size;      // File size
};

#define SSTATUS_SUM  (1 << 18)