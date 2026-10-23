#include "kernel.h"
#include "common.h"

extern char __bss[], __bss_end[], __stack_top[] ,__free_ram[], __free_ram_end[], __kernel_base[], _binary_shell_bin_start[], _binary_shell_bin_size[];
extern char _binary_spin_bin_start[], _binary_spin_bin_size[];
extern void main(void);

// Function declarations
paddr_t alloc_pages(uint32_t n);
void map_page(uint32_t *table1, uint32_t vaddr, paddr_t paddr, uint32_t flags);
void putchar(char ch);
void handle_trap(struct trap_frame *f);
void kernel_entry(void);
void switch_context(uint32_t *prev_sp, uint32_t *next_sp);
struct process *create_process(const void *image, size_t image_size);
void yield(void);
void delay(void);
void proc_a_entry(void);
void proc_b_entry(void);
void kernel_main(void);
void boot(void);
struct virtio_virtq *virtq_init(unsigned index);
void virtio_blk_init(void);
void fs_init(void);
void fs_flush(void);
struct file *fs_lookup(const char *filename);
static uint32_t *lookup_pte(uint32_t *table1, uint32_t vaddr);

extern struct file files[FILES_MAX];

//GLOBAL VARIABLES
struct process procs[PROCS_MAX]; // All process control structures.
struct process *current_proc; // Currently running process
struct process *idle_proc;    // Idle process

struct process *proc_a;
struct process *proc_b;

//MEMORY MANAGEMENT
paddr_t alloc_pages(uint32_t n) {
    static paddr_t next_paddr = (paddr_t) __free_ram;
    paddr_t paddr = next_paddr;
    next_paddr += n * PAGE_SIZE;

    if (next_paddr > (paddr_t) __free_ram_end) {
        PANIC("out of memory: requested %d pages, available space exceeded", n);
    }

    memset((void *) paddr, 0, n * PAGE_SIZE);
    return paddr;
}

void map_page(uint32_t *table1, uint32_t vaddr, paddr_t paddr, uint32_t flags) {
    if (!is_aligned(vaddr, PAGE_SIZE))
        PANIC("unaligned vaddr %x", vaddr);

    if (!is_aligned(paddr, PAGE_SIZE))
        PANIC("unaligned paddr %x", paddr);

    uint32_t vpn1 = (vaddr >> 22) & 0x3ff;
    if ((table1[vpn1] & PAGE_V) == 0) {
        // Create the 1st level page table if it doesn't exist.
        uint32_t pt_paddr = alloc_pages(1);
        table1[vpn1] = ((pt_paddr / PAGE_SIZE) << 10) | PAGE_V;
    }

    // Set the 2nd level page table entry to map the physical page.
    uint32_t vpn0 = (vaddr >> 12) & 0x3ff;
    uint32_t *table0 = (uint32_t *) ((table1[vpn1] >> 10) * PAGE_SIZE);
    table0[vpn0] = ((paddr / PAGE_SIZE) << 10) | flags | PAGE_V;
}

//SBI INTERFACE
struct sbiret sbi_call(long arg0, long arg1, long arg2, long arg3, long arg4,       //registers
                       long arg5, long fid, long eid) {
    register long a0 __asm__("a0") = arg0;
    register long a1 __asm__("a1") = arg1;
    register long a2 __asm__("a2") = arg2;
    register long a3 __asm__("a3") = arg3;
    register long a4 __asm__("a4") = arg4;
    register long a5 __asm__("a5") = arg5;
    register long a6 __asm__("a6") = fid;
    register long a7 __asm__("a7") = eid;

    __asm__ __volatile__("ecall"                                            //executes the SBI call, switches from S to M mode
                         : "=r"(a0), "=r"(a1)
                         : "r"(a0), "r"(a1), "r"(a2), "r"(a3), "r"(a4), "r"(a5),
                           "r"(a6), "r"(a7)
                         : "memory");
    return (struct sbiret){.error = a0, .value = a1};
}

void putchar(char ch) {
    sbi_call(ch, 0, 0, 0, 0, 0, 0, 1 );
}

long getchar(void) {
    struct sbiret ret = sbi_call(0, 0, 0, 0, 0, 0, 0, 2);
    return ret.error;
}

// TIMER (drives preemption)
static uint64_t read_time(void) {
    // On RV32 the 64-bit `time` counter is split in two CSRs; re-read the high
    // half to guard against a carry between the two reads.
    uint32_t hi, lo, hi2;
    do {
        hi  = READ_CSR(timeh);
        lo  = READ_CSR(time);
        hi2 = READ_CSR(timeh);
    } while (hi != hi2);
    return ((uint64_t) hi << 32) | lo;
}

// Ask the SBI firmware for a timer interrupt TIME_SLICE_TICKS from now. This
// also clears the pending supervisor timer interrupt (STIP).
void timer_arm(void) {
    uint64_t when = read_time() + TIME_SLICE_TICKS;
    sbi_call((uint32_t) when, (uint32_t) (when >> 32), 0, 0, 0, 0, 0, SBI_EID_TIME);
}

__attribute__((naked))
__attribute__((aligned(4)))
void kernel_entry(void) {
    __asm__ __volatile__(
        // Retrieve the kernel stack of the running process from sscratch.
        "csrrw sp, sscratch, sp\n"

        "addi sp, sp, -4 * 31\n"
        "sw ra,  4 * 0(sp)\n"
        "sw gp,  4 * 1(sp)\n"
        "sw tp,  4 * 2(sp)\n"
        "sw t0,  4 * 3(sp)\n"
        "sw t1,  4 * 4(sp)\n"
        "sw t2,  4 * 5(sp)\n"
        "sw t3,  4 * 6(sp)\n"
        "sw t4,  4 * 7(sp)\n"
        "sw t5,  4 * 8(sp)\n"
        "sw t6,  4 * 9(sp)\n"
        "sw a0,  4 * 10(sp)\n"
        "sw a1,  4 * 11(sp)\n"
        "sw a2,  4 * 12(sp)\n"
        "sw a3,  4 * 13(sp)\n"
        "sw a4,  4 * 14(sp)\n"
        "sw a5,  4 * 15(sp)\n"
        "sw a6,  4 * 16(sp)\n"
        "sw a7,  4 * 17(sp)\n"
        "sw s0,  4 * 18(sp)\n"
        "sw s1,  4 * 19(sp)\n"
        "sw s2,  4 * 20(sp)\n"
        "sw s3,  4 * 21(sp)\n"
        "sw s4,  4 * 22(sp)\n"
        "sw s5,  4 * 23(sp)\n"
        "sw s6,  4 * 24(sp)\n"
        "sw s7,  4 * 25(sp)\n"
        "sw s8,  4 * 26(sp)\n"
        "sw s9,  4 * 27(sp)\n"
        "sw s10, 4 * 28(sp)\n"
        "sw s11, 4 * 29(sp)\n"

        // Retrieve and save the sp at the time of exception.
        "csrr a0, sscratch\n"
        "sw a0,  4 * 30(sp)\n"

        // Reset the kernel stack.
        "addi a0, sp, 4 * 31\n"
        "csrw sscratch, a0\n"

        "mv a0, sp\n"
        "call handle_trap\n"

        // Restore registers
        "lw ra,  4 * 0(sp)\n"
        "lw gp,  4 * 1(sp)\n"
        "lw tp,  4 * 2(sp)\n"
        "lw t0,  4 * 3(sp)\n"
        "lw t1,  4 * 4(sp)\n"
        "lw t2,  4 * 5(sp)\n"
        "lw t3,  4 * 6(sp)\n"
        "lw t4,  4 * 7(sp)\n"
        "lw t5,  4 * 8(sp)\n"
        "lw t6,  4 * 9(sp)\n"
        "lw a0,  4 * 10(sp)\n"
        "lw a1,  4 * 11(sp)\n"
        "lw a2,  4 * 12(sp)\n"
        "lw a3,  4 * 13(sp)\n"
        "lw a4,  4 * 14(sp)\n"
        "lw a5,  4 * 15(sp)\n"
        "lw a6,  4 * 16(sp)\n"
        "lw a7,  4 * 17(sp)\n"
        "lw s0,  4 * 18(sp)\n"
        "lw s1,  4 * 19(sp)\n"
        "lw s2,  4 * 20(sp)\n"
        "lw s3,  4 * 21(sp)\n"
        "lw s4,  4 * 22(sp)\n"
        "lw s5,  4 * 23(sp)\n"
        "lw s6,  4 * 24(sp)\n"
        "lw s7,  4 * 25(sp)\n"
        "lw s8,  4 * 26(sp)\n"
        "lw s9,  4 * 27(sp)\n"
        "lw s10, 4 * 28(sp)\n"
        "lw s11, 4 * 29(sp)\n"
        "lw sp,  4 * 30(sp)\n"

        "sret\n"
    );
}

//CONTEXT SWITCHING
__attribute__((naked)) void switch_context(uint32_t *prev_sp,
                                           uint32_t *next_sp) {
    __asm__ __volatile__(
        // Save callee-saved registers onto the current process's stack.
        "addi sp, sp, -13 * 4\n" // Allocate stack space for 13 4-byte registers
        "sw ra,  0  * 4(sp)\n"   // Save callee-saved registers only
        "sw s0,  1  * 4(sp)\n"
        "sw s1,  2  * 4(sp)\n"
        "sw s2,  3  * 4(sp)\n"
        "sw s3,  4  * 4(sp)\n"
        "sw s4,  5  * 4(sp)\n"
        "sw s5,  6  * 4(sp)\n"
        "sw s6,  7  * 4(sp)\n"
        "sw s7,  8  * 4(sp)\n"
        "sw s8,  9  * 4(sp)\n"
        "sw s9,  10 * 4(sp)\n"
        "sw s10, 11 * 4(sp)\n"
        "sw s11, 12 * 4(sp)\n"

        // Switch the stack pointer.
        "sw sp, (a0)\n"         // *prev_sp = sp;
        "lw sp, (a1)\n"         // Switch stack pointer (sp) here

        // Restore callee-saved registers from the next process's stack.
        "lw ra,  0  * 4(sp)\n"  // Restore callee-saved registers only
        "lw s0,  1  * 4(sp)\n"
        "lw s1,  2  * 4(sp)\n"
        "lw s2,  3  * 4(sp)\n"
        "lw s3,  4  * 4(sp)\n"
        "lw s4,  5  * 4(sp)\n"
        "lw s5,  6  * 4(sp)\n"
        "lw s6,  7  * 4(sp)\n"
        "lw s7,  8  * 4(sp)\n"
        "lw s8,  9  * 4(sp)\n"
        "lw s9,  10 * 4(sp)\n"
        "lw s10, 11 * 4(sp)\n"
        "lw s11, 12 * 4(sp)\n"
        "addi sp, sp, 13 * 4\n"  // We've popped 13 4-byte registers from the stack
        "ret\n"
    );
}

// ↓ __attribute__((naked)) is very important!
__attribute__((naked)) void user_entry(void) {
    __asm__ __volatile__(
        "csrw sepc, %[sepc]        \n"
        "csrw sstatus, %[sstatus]  \n"
        "sret                      \n"
        :
        : [sepc] "r" (USER_BASE),
          [sstatus] "r" (SSTATUS_SPIE | SSTATUS_SUM)
    );
}

//PROCESS MANAGEMENT
static bool is_elf(const uint8_t *b, size_t size) {
    return size >= 4 && b[0] == 0x7f && b[1] == 'E' && b[2] == 'L' && b[3] == 'F';
}

// Copy the PT_LOAD segments of an ELF file into the process's address space.
// Returns false (loading nothing more) if the file is malformed.
static bool load_elf(uint32_t *page_table, const uint8_t *elf, size_t size) {
    const struct elf32_header *eh = (const struct elf32_header *) elf;

    // 1. Is this really a 32-bit little-endian RISC-V ELF?
    if (size < sizeof(*eh) ||
        eh->ident[0] != 0x7f || eh->ident[1] != 'E' ||
        eh->ident[2] != 'L'  || eh->ident[3] != 'F' ||
        eh->ident[4] != 1 || eh->ident[5] != 1 ||
        eh->machine != ELF_MACHINE_RISCV)
        return false;

    // 2. user_entry always starts at USER_BASE, so refuse anything else.
    if (eh->entry != USER_BASE)
        return false;

    // 3. Is the program-header table inside the file?
    if (eh->phentsize != sizeof(struct elf32_phdr) ||
        eh->phoff > size ||
        (size - eh->phoff) / sizeof(struct elf32_phdr) < eh->phnum)
        return false;

    for (int i = 0; i < eh->phnum; i++) {
        const struct elf32_phdr *ph =
            (const struct elf32_phdr *) (elf + eh->phoff) + i;
        if (ph->type != PT_LOAD)
            continue;

        // 4. Sanity checks, so a bad file can't make us read or map outside bounds.
        if (ph->filesz > ph->memsz ||
            ph->offset > size || ph->filesz > size - ph->offset ||
            ph->vaddr < USER_BASE || ph->vaddr >= USER_LIMIT ||
            ph->memsz > USER_LIMIT - ph->vaddr)
            return false;

        // 5. Walk every 4 KB page the segment touches.
        for (uint32_t page_va = ph->vaddr & ~(PAGE_SIZE - 1);
             page_va < ph->vaddr + ph->memsz; page_va += PAGE_SIZE) {

            // Another segment may already own this page. Reuse it.
            paddr_t page;
            uint32_t *pte = lookup_pte(page_table, page_va);
            if (pte && (*pte & PAGE_V)) {
                page = (*pte >> 10) * PAGE_SIZE;
            } else {
                page = alloc_pages(1);                  // comes back zeroed
                map_page(page_table, page_va, page,
                         PAGE_U | PAGE_R | PAGE_W | PAGE_X);
            }

            // Copy only the part of this page that comes from the file.
            // Anything past filesz stays zero (that's the .bss).
            uint32_t file_start = ph->vaddr;
            uint32_t file_end   = ph->vaddr + ph->filesz;
            uint32_t from = page_va > file_start ? page_va : file_start;
            uint32_t to   = page_va + PAGE_SIZE < file_end ? page_va + PAGE_SIZE : file_end;
            if (from < to)
                memcpy((void *) (page + (from - page_va)),
                       elf + ph->offset + (from - ph->vaddr), to - from);
        }
    }
    return true;
}

struct process *create_process(const void *image, size_t image_size) {
    // 1️⃣ Find an unused process slot
    struct process *proc = NULL;
    int i;
    for (i = 0; i < PROCS_MAX; i++) {
        if (procs[i].state == PROC_UNUSED) {
            proc = &procs[i];
            break;
        }
    }

    if (!proc)
        return NULL;     // table full (a syscall must not take the kernel down)

    // 2️⃣ Allocate a page table for this process
    uint32_t *page_table = (uint32_t *) alloc_pages(1);

    // 3️⃣ Map kernel pages (identity mapping)
    for (paddr_t paddr = (paddr_t) __kernel_base;
         paddr < (paddr_t) __free_ram_end; paddr += PAGE_SIZE)
        map_page(page_table, paddr, paddr, PAGE_R | PAGE_W | PAGE_X);

    // Map VirtIO block device for disk I/O
    map_page(page_table, VIRTIO_BLK_PADDR, VIRTIO_BLK_PADDR, PAGE_R | PAGE_W);

    // 4️⃣ Map user pages (if image provided)
    const uint8_t *bytes = image;
    if (is_elf(bytes, image_size)) {
        if (!load_elf(page_table, bytes, image_size))
            return NULL;    // bad ELF; the process slot is not claimed
    } else if (image && image_size > 0) {
        for (uint32_t off = 0; off < image_size; off += PAGE_SIZE) {
            paddr_t page = alloc_pages(1);

            // Handle the case where the data to be copied is smaller than the
            // page size.
            size_t remaining = image_size - off;
            size_t copy_size = PAGE_SIZE <= remaining ? PAGE_SIZE : remaining;

            // Fill and map the page.
            memcpy((void *) page, image + off, copy_size);
            map_page(page_table, USER_BASE + off, page,
                     PAGE_U | PAGE_R | PAGE_W | PAGE_X);
        }
    }

    // Map user stack
    for (int i = 0; i < USER_STACK_PAGES; i++) {
        paddr_t page = alloc_pages(1);
        map_page(page_table, USER_STACK_TOP - PAGE_SIZE * (i + 1), page,
                 PAGE_U | PAGE_R | PAGE_W);
    }

    // 5️⃣ Initialize the process stack
    uint32_t *sp = (uint32_t *) &proc->stack[sizeof(proc->stack)];

    // These values correspond to the callee-saved registers (s0–s11, ra)
    *--sp = 0;                // s11
    *--sp = 0;                // s10
    *--sp = 0;                // s9
    *--sp = 0;                // s8
    *--sp = 0;                // s7
    *--sp = 0;                // s6
    *--sp = 0;                // s5
    *--sp = 0;                // s4
    *--sp = 0;                // s3
    *--sp = 0;                // s2
    *--sp = 0;                // s1
    *--sp = 0;                // s0
    *--sp = (uint32_t) user_entry;    // ra — so the process starts at user_entry

    // 6️⃣ Initialize metadata
    proc->pid = i + 1;
    proc->state = PROC_RUNNABLE;
    proc->sp = (uint32_t) sp;
    proc->page_table = page_table;

    return proc;
}


//SCHEDULER
void yield(void) {
    // Find next runnable process
    struct process *next = idle_proc;
    for (int i = 0; i < PROCS_MAX; i++) {
        struct process *proc = &procs[(current_proc->pid + i) % PROCS_MAX];
        if (proc->state == PROC_RUNNABLE && proc->pid > 0) {
            next = proc;
            break;
        }
    }

    // If only current process is runnable, return
    if (next == current_proc)
        return;

    struct process *prev = current_proc;
    current_proc = next;

    // Switch MMU context to next process’s address space
    __asm__ __volatile__(
        "sfence.vma\n"
        "csrw satp, %[satp]\n"
        "sfence.vma\n"
        "csrw sscratch, %[sscratch]\n"
        :
        : [satp] "r" (SATP_SV32 | ((uint32_t) next->page_table / PAGE_SIZE)),
          [sscratch] "r" ((uint32_t) &next->stack[sizeof(next->stack)])
    );

    // Finally switch CPU context (stack + registers)
    switch_context(&prev->sp, &next->sp);
}

void delay(void) {
    for (int i = 0; i < 30000000; i++)
        __asm__ __volatile__("nop"); // do nothing
}

//SAMPLE PROCESS ENTRIES
void proc_a_entry(void) {
    printf("starting process A\n");
    while (1) {
        putchar('A');
        delay();
        yield();
    }
}

void proc_b_entry(void) {
    printf("starting process B\n");
    while (1) {
        putchar('B');
        delay();
        yield();
    }
}

// USER POINTER VALIDATION
// Syscall arguments are untrusted addresses. SUM lets S-mode touch U pages, but
// does not stop a user from passing a kernel address or an unmapped one, so
// every pointer is checked against the caller's own page table first.
static uint32_t *lookup_pte(uint32_t *table1, uint32_t vaddr) {
    uint32_t pte1 = table1[(vaddr >> 22) & 0x3ff];
    if ((pte1 & PAGE_V) == 0)
        return NULL;
    uint32_t *table0 = (uint32_t *) ((pte1 >> 10) * PAGE_SIZE);
    return &table0[(vaddr >> 12) & 0x3ff];
}

// True if [addr, addr+len) lies in user space and every page is mapped with
// PAGE_U and the `need` permission (PAGE_R or PAGE_W).
static bool user_range_ok(uint32_t addr, uint32_t len, uint32_t need) {
    uint32_t end = addr + len;
    if (end < addr)                       // wrapped around 2^32
        return false;
    if (addr < USER_BASE || end > USER_STACK_TOP)
        return false;

    uint32_t want = PAGE_V | PAGE_U | need;
    for (uint32_t page = addr & ~(PAGE_SIZE - 1); page < end; page += PAGE_SIZE) {
        uint32_t *pte = lookup_pte(current_proc->page_table, page);
        if (!pte || (*pte & want) != want)
            return false;
    }
    return true;
}

// Copy a NUL-terminated string out of user space into `dst` (at most `cap`
// bytes including the NUL). Fails if unterminated or any byte is not readable.
static bool copy_user_string(char *dst, size_t cap, uint32_t uaddr) {
    for (size_t i = 0; i < cap; i++) {
        if (!user_range_ok(uaddr + i, 1, PAGE_R))
            return false;
        dst[i] = *(const char *) (uaddr + i);
        if (dst[i] == '\0')
            return true;
    }
    return false;
}

void handle_syscall(struct trap_frame *f) {
    switch (f->a3) {
        case SYS_PUTCHAR:
            putchar(f->a0);
            break;

        case SYS_GETCHAR:
            while (1) {
                long ch = getchar();
                if (ch >= 0) {
                    f->a0 = ch;
                    break;
                }

                yield();
            }
            break;
        
        case SYS_EXIT:
            printf("process %d exited\n", current_proc->pid);
            current_proc->state = PROC_EXITED;
            yield();
            PANIC("unreachable");

        case SYS_READFILE:
        case SYS_WRITEFILE: {
            bool is_write = f->a3 == SYS_WRITEFILE;
            char filename[sizeof(files[0].name)];
            if (!copy_user_string(filename, sizeof(filename), f->a0)) {
                f->a0 = -1;
                break;
            }

            struct file *file = fs_lookup(filename);
            if (!file && is_write) {
                // Create the file on first write (works on an empty disk).
                for (int i = 0; i < FILES_MAX; i++) {
                    if (!files[i].in_use) {
                        file = &files[i];
                        memset(file, 0, sizeof(*file));
                        file->in_use = true;
                        strcpy(file->name, filename);
                        break;
                    }
                }
                if (!file) {
                    printf("no free file slot for: %s\n", filename);
                    f->a0 = -1;
                    break;
                }
            }
            if (!file) {
                printf("file not found: %s\n", filename);
                f->a0 = -1;
                break;
            }

            int len = (int) f->a2;
            if (len < 0 || (is_write && len > (int) sizeof(file->data))) {
                f->a0 = -1;
                break;
            }
            // A read returns at most what the file holds, never the whole buffer.
            if (!is_write && len > (int) file->size)
                len = file->size;

            // The user buffer is the destination of a read, the source of a write.
            if (!user_range_ok(f->a1, len, is_write ? PAGE_R : PAGE_W)) {
                f->a0 = -1;
                break;
            }
            char *buf = (char *) f->a1;

            if (is_write) {
                memcpy(file->data, buf, len);
                file->size = len;
                fs_flush();
            } else {
                memcpy(buf, file->data, len);
            }

            f->a0 = len;
            break;
        }

        case SYS_LISTFILES: {
            // a0 = n: copy the name of the n-th existing file to a1 (cap a2),
            // return its size, or -1 when there is no n-th file.
            int n = (int) f->a0;
            int cap = (int) f->a2;
            f->a0 = -1;
            if (n < 0 || cap <= 0)
                break;
            for (int i = 0; i < FILES_MAX; i++) {
                if (!files[i].in_use || n--)
                    continue;
                int namelen = 0;
                while (files[i].name[namelen])
                    namelen++;
                if (namelen + 1 > cap || !user_range_ok(f->a1, namelen + 1, PAGE_W))
                    break;
                memcpy((char *) f->a1, files[i].name, namelen + 1);
                f->a0 = files[i].size;
                break;
            }
            break;
        }

        case SYS_DELETEFILE: {
            char filename[sizeof(files[0].name)];
            if (!copy_user_string(filename, sizeof(filename), f->a0)) {
                f->a0 = -1;
                break;
            }
            struct file *file = fs_lookup(filename);
            if (!file) {
                f->a0 = -1;
                break;
            }
            memset(file, 0, sizeof(*file));
            fs_flush();
            f->a0 = 0;
            break;
        }

        case SYS_EXEC: {
            char name[sizeof(files[0].name)];
            char args[ARGS_MAX];
            if (!copy_user_string(name, sizeof(name), f->a0) ||
                !copy_user_string(args, sizeof(args), f->a1)) {
                f->a0 = -1;
                break;
            }

            struct file *file = fs_lookup(name);
            if (!file || !is_elf((uint8_t *) file->data, file->size)) {
                f->a0 = -1;                   // no such file, or not a program
                break;
            }

            struct process *child = create_process(file->data, file->size);
            if (!child) {                     // bad ELF or no free slot
                f->a0 = -1;
                break;
            }
            strcpy(child->args, args);
            f->a0 = child->pid;               // success: the new process's id
            break;
        }

        case SYS_GETARGS: {
            int len = 0;
            while (current_proc->args[len])
                len++;
            if (f->a1 < (uint32_t) len + 1 || !user_range_ok(f->a0, len + 1, PAGE_W)) {
                f->a0 = -1;
                break;
            }
            memcpy((void *) f->a0, current_proc->args, len + 1);
            f->a0 = len;
            break;
        }

        case SYS_GETPID:
            f->a0 = current_proc->pid;
            break;

        default:
            PANIC("unexpected syscall a3=%x\n", f->a3);
    }
}

void handle_trap(struct trap_frame *f) {
    uint32_t scause = READ_CSR(scause);
    uint32_t stval = READ_CSR(stval);
    uint32_t user_pc = READ_CSR(sepc);
    uint32_t sstatus = READ_CSR(sstatus);

    // sepc is kept in a local, not re-read: yield() may run other processes
    // whose traps overwrite the sepc CSR before we get scheduled back.
    if (scause == (SCAUSE_INTERRUPT | SCAUSE_S_TIMER)) {
        // Preemption: time slice over. Re-arm (clears STIP) and give up the CPU.
        // sepc is NOT advanced: the interrupted instruction never completed.
        timer_arm();
        yield();
    } else if (scause == SCAUSE_ECALL) {
        handle_syscall(f);
        user_pc += 4;   // sepc points AT the ecall; step past it or it re-executes
    } else if (!(scause & SCAUSE_INTERRUPT) && !(sstatus & SSTATUS_SPP)) {
        // A user program faulted (bad pointer, illegal instruction, ...):
        // kill that process, not the whole kernel.
        printf("process %d killed: scause=%x, stval=%x, sepc=%x\n",
               current_proc->pid, scause, stval, user_pc);
        current_proc->state = PROC_EXITED;
        yield();
        PANIC("unreachable");
    } else {
        PANIC("unexpected trap scause=%x, stval=%x, sepc=%x\n", scause, stval, user_pc);
    }

    WRITE_CSR(sepc, user_pc);
}

uint32_t virtio_reg_read32(unsigned offset) {
    return *((volatile uint32_t *) (VIRTIO_BLK_PADDR + offset));
}

uint64_t virtio_reg_read64(unsigned offset) {
    return *((volatile uint64_t *) (VIRTIO_BLK_PADDR + offset));
}

void virtio_reg_write32(unsigned offset, uint32_t value) {
    *((volatile uint32_t *) (VIRTIO_BLK_PADDR + offset)) = value;
}

void virtio_reg_fetch_and_or32(unsigned offset, uint32_t value) {
    virtio_reg_write32(offset, virtio_reg_read32(offset) | value);
}

struct virtio_virtq *blk_request_vq;
struct virtio_blk_req *blk_req;
paddr_t blk_req_paddr;
uint64_t blk_capacity;

void virtio_blk_init(void) {
    if (virtio_reg_read32(VIRTIO_REG_MAGIC) != 0x74726976)
        PANIC("virtio: invalid magic value");
    if (virtio_reg_read32(VIRTIO_REG_VERSION) != 1)
        PANIC("virtio: invalid version");
    if (virtio_reg_read32(VIRTIO_REG_DEVICE_ID) != VIRTIO_DEVICE_BLK)
        PANIC("virtio: invalid device id");

    // 1. Reset the device.
    virtio_reg_write32(VIRTIO_REG_DEVICE_STATUS, 0);
    // 2. Set the ACKNOWLEDGE status bit: the guest OS has noticed the device.
    virtio_reg_fetch_and_or32(VIRTIO_REG_DEVICE_STATUS, VIRTIO_STATUS_ACK);
    // 3. Set the DRIVER status bit.
    virtio_reg_fetch_and_or32(VIRTIO_REG_DEVICE_STATUS, VIRTIO_STATUS_DRIVER);
    // 5. Set the FEATURES_OK status bit.
    virtio_reg_fetch_and_or32(VIRTIO_REG_DEVICE_STATUS, VIRTIO_STATUS_FEAT_OK);
    // 7. Perform device-specific setup, including discovery of virtqueues for the device
    blk_request_vq = virtq_init(0);
    // 8. Set the DRIVER_OK status bit.
    virtio_reg_write32(VIRTIO_REG_DEVICE_STATUS, VIRTIO_STATUS_DRIVER_OK);

    // Get the disk capacity.
    blk_capacity = virtio_reg_read64(VIRTIO_REG_DEVICE_CONFIG + 0) * SECTOR_SIZE;
    printf("virtio-blk: capacity is %d bytes\n", (uint32_t)blk_capacity);

    // Allocate a region to store requests to the device.
    blk_req_paddr = alloc_pages(align_up(sizeof(*blk_req), PAGE_SIZE) / PAGE_SIZE);
    blk_req = (struct virtio_blk_req *) blk_req_paddr;
}

struct virtio_virtq *virtq_init(unsigned index) {
    // Allocate a region for the virtqueue.
    paddr_t virtq_paddr = alloc_pages(align_up(sizeof(struct virtio_virtq), PAGE_SIZE) / PAGE_SIZE);
    struct virtio_virtq *vq = (struct virtio_virtq *) virtq_paddr;
    vq->queue_index = index;
    vq->used_index = (volatile uint16_t *) &vq->used.index;
    // 1. Select the queue writing its index (first queue is 0) to QueueSel.
    virtio_reg_write32(VIRTIO_REG_QUEUE_SEL, index);
    // 5. Notify the device about the queue size by writing the size to QueueNum.
    virtio_reg_write32(VIRTIO_REG_QUEUE_NUM, VIRTQ_ENTRY_NUM);
    // 6. Notify the device about the used alignment by writing its value in bytes to QueueAlign.
    virtio_reg_write32(VIRTIO_REG_QUEUE_ALIGN, 0);
    // 7. Write the physical number of the first page of the queue to the QueuePFN register.
    virtio_reg_write32(VIRTIO_REG_QUEUE_PFN, virtq_paddr);
    return vq;
}

// Notifies the device that there is a new request. `desc_index` is the index
// of the head descriptor of the new request.
void virtq_kick(struct virtio_virtq *vq, int desc_index) {
    vq->avail.ring[vq->avail.index % VIRTQ_ENTRY_NUM] = desc_index;
    vq->avail.index++;
    __sync_synchronize();
    virtio_reg_write32(VIRTIO_REG_QUEUE_NOTIFY, vq->queue_index);
    vq->last_used_index++;
}

// Returns whether there are requests being processed by the device.
bool virtq_is_busy(struct virtio_virtq *vq) {
    return vq->last_used_index != *vq->used_index;
}

// Reads/writes from/to virtio-blk device.
void read_write_disk(void *buf, unsigned sector, int is_write) {
    if (sector >= blk_capacity / SECTOR_SIZE) {
        printf("virtio: tried to read/write sector=%d, but capacity is %d\n",
              sector, blk_capacity / SECTOR_SIZE);
        return;
    }

    // Construct the request according to the virtio-blk specification.
    blk_req->sector = sector;
    blk_req->type = is_write ? VIRTIO_BLK_T_OUT : VIRTIO_BLK_T_IN;
    if (is_write)
        memcpy(blk_req->data, buf, SECTOR_SIZE);

    // Construct the virtqueue descriptors (using 3 descriptors).
    struct virtio_virtq *vq = blk_request_vq;
    vq->descs[0].addr = blk_req_paddr;
    vq->descs[0].len = sizeof(uint32_t) * 2 + sizeof(uint64_t);
    vq->descs[0].flags = VIRTQ_DESC_F_NEXT;
    vq->descs[0].next = 1;

    vq->descs[1].addr = blk_req_paddr + offsetof(struct virtio_blk_req, data);
    vq->descs[1].len = SECTOR_SIZE;
    vq->descs[1].flags = VIRTQ_DESC_F_NEXT | (is_write ? 0 : VIRTQ_DESC_F_WRITE);
    vq->descs[1].next = 2;

    vq->descs[2].addr = blk_req_paddr + offsetof(struct virtio_blk_req, status);
    vq->descs[2].len = sizeof(uint8_t);
    vq->descs[2].flags = VIRTQ_DESC_F_WRITE;

    // Notify the device that there is a new request.
    virtq_kick(vq, 0);

    // Wait until the device finishes processing.
    while (virtq_is_busy(vq))
        ;

    // virtio-blk: If a non-zero value is returned, it's an error.
    if (blk_req->status != 0) {
        printf("virtio: warn: failed to read/write sector=%d status=%d\n",
               sector, blk_req->status);
        return;
    }

    // For read operations, copy the data into the buffer.
    if (!is_write)
        memcpy(buf, blk_req->data, SECTOR_SIZE);
}

struct file files[FILES_MAX];
uint8_t disk[DISK_MAX_SIZE];

int oct2int(char *oct, int len) {
    int dec = 0;
    for (int i = 0; i < len; i++) {
        if (oct[i] < '0' || oct[i] > '7')
            break;

        dec = dec * 8 + (oct[i] - '0');
    }
    return dec;
}

void fs_init(void) {
    if (blk_capacity < sizeof(disk))
        PANIC("disk image is %d bytes but the kernel needs %d (see run.sh)",
              (uint32_t) blk_capacity, (uint32_t) sizeof(disk));

    for (unsigned sector = 0; sector < sizeof(disk) / SECTOR_SIZE; sector++)
        read_write_disk(&disk[sector * SECTOR_SIZE], sector, false);

    unsigned off = 0;
    for (int i = 0; i < FILES_MAX; i++) {
        struct tar_header *header = (struct tar_header *) &disk[off];
        if (header->name[0] == '\0')
            break;

        if (strcmp(header->magic, "ustar") != 0)
            PANIC("invalid tar header: magic=\"%s\"", header->magic);

        int filesz = oct2int(header->size, sizeof(header->size));
        if (filesz > (int) sizeof(files[i].data))
            PANIC("file %s too large: %d bytes", header->name, filesz);
        struct file *file = &files[i];
        file->in_use = true;
        strcpy(file->name, header->name);
        memcpy(file->data, header->data, filesz);
        file->size = filesz;
        printf("file: %s, size=%d\n", file->name, file->size);

        off += align_up(sizeof(struct tar_header) + filesz, SECTOR_SIZE);
    }
}

void fs_flush(void) {
    // Copy all file contents into `disk` buffer.
    memset(disk, 0, sizeof(disk));
    unsigned off = 0;
    for (int file_i = 0; file_i < FILES_MAX; file_i++) {
        struct file *file = &files[file_i];
        if (!file->in_use)
            continue;

        struct tar_header *header = (struct tar_header *) &disk[off];
        memset(header, 0, sizeof(*header));
        strcpy(header->name, file->name);
        strcpy(header->mode, "000644");
        strcpy(header->magic, "ustar");
        strcpy(header->version, "00");
        header->type = '0';

        // Turn the file size into an octal string.
        int filesz = file->size;
        for (int i = sizeof(header->size); i > 0; i--) {
            header->size[i - 1] = (filesz % 8) + '0';
            filesz /= 8;
        }

        // Calculate the checksum.
        int checksum = ' ' * sizeof(header->checksum);
        for (unsigned i = 0; i < sizeof(struct tar_header); i++)
            checksum += (unsigned char) disk[off + i];

        for (int i = 5; i >= 0; i--) {
            header->checksum[i] = (checksum % 8) + '0';
            checksum /= 8;
        }

        // Copy file data.
        memcpy(header->data, file->data, file->size);
        off += align_up(sizeof(struct tar_header) + file->size, SECTOR_SIZE);
    }

    // Write `disk` buffer into the virtio-blk.
    for (unsigned sector = 0; sector < sizeof(disk) / SECTOR_SIZE; sector++)
        read_write_disk(&disk[sector * SECTOR_SIZE], sector, true);

    printf("wrote %d bytes to disk\n", sizeof(disk));
}

struct file *fs_lookup(const char *filename) {
    for (int i = 0; i < FILES_MAX; i++) {
        struct file *file = &files[i];
        if (file->in_use && !strcmp(file->name, filename))
            return file;
    }

    return NULL;
}













void kernel_main(void) {
    memset(__bss, 0, (size_t) __bss_end - (size_t) __bss);
    printf("Kernel started\nWelcome to my Ka-OS,\n\n");
    WRITE_CSR(stvec, (uint32_t) kernel_entry);

    virtio_blk_init();
    fs_init();

    idle_proc = create_process(NULL, 0); // updated!
    idle_proc->pid = 0; // idle
    current_proc = idle_proc;

#ifdef PREEMPT_DEMO
    // Two CPU-bound programs that never call yield(): only the timer can
    // make them take turns. Build with: DEMO=1 ./run.sh
    create_process(_binary_spin_bin_start, (size_t) _binary_spin_bin_size);
    create_process(_binary_spin_bin_start, (size_t) _binary_spin_bin_size);
#else
    create_process(_binary_shell_bin_start, (size_t) _binary_shell_bin_size);
#endif

    // Enable supervisor timer interrupts. They can only fire while a process
    // runs in U-mode: the kernel keeps sstatus.SIE = 0, so it is never preempted.
    WRITE_CSR(sie, READ_CSR(sie) | SIE_STIE);
    timer_arm();

    yield();
    
    PANIC("switched to idle process");
}

__attribute__((section(".text.boot")))
__attribute__((naked))
void boot(void) {
    __asm__ __volatile__(
        "mv sp, %[stack_top]\n" // Set the stack pointer
        "j kernel_main\n"       // Jump to the kernel main function
        :
        : [stack_top] "r" (__stack_top) // Pass the stack top address as %[stack_top]
    );
}