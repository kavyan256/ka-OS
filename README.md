# Ka-OS

A minimal educational operating system written in C for RISC-V 32-bit. It started from the "OS in 1,000 Lines" tutorial and was extended with an ELF program loader, `exec`/`wait`, a file-based shell, memory reclamation and a modern-virtio disk driver.

## Features

### Core
- ✅ **Bare-metal boot** - boots through OpenSBI on QEMU's `virt` machine
- ✅ **Memory management** - page-based allocator with a free list, Sv32 paging, one address space per process
- ✅ **Process management** - up to 8 processes, round-robin scheduler, `exec` and `wait`
- ✅ **System calls** - user-to-kernel requests via `ecall` (see the table below)
- ✅ **Trap handling** - system calls, timer interrupts, and a user program that faults is killed instead of crashing the kernel
- ✅ **SBI interface** - console and timer through OpenSBI

### Beyond the tutorial
- 📦 **ELF loader** - programs are stored on disk as ELF files; the kernel validates the headers and maps each `PT_LOAD` segment (segments that share a page reuse it)
- 🚀 **`exec` / `wait`** - the shell runs any program on disk and waits for it to finish (polling); finished processes are reaped and their pages are returned
- 🧹 **Memory reclamation** - a finished process's pages go back to a free list, so running 1,000 programs in one boot works (tested)
- 🔄 **Preemptive multitasking** - supervisor timer interrupt, 50 ms slices; the kernel itself is not preemptible
- 🛡️ **Syscall hardening** - every user pointer is checked against the caller's page table
- 💾 **VirtIO block driver** - supports both legacy (v1) and modern (v2) devices
- 🗂️ **File system** - TAR image, up to 8 files of up to 8 KB, created and deleted at run time
- 💻 **Shell** - `help`, `hello`, `echo` (with `>` redirection), `write`, `rm`, `exit`, plus any program on the disk (`ls`, `cat`, ...)

## System Calls

| System Call | Number | Description |
|------------|--------|-------------|
| `SYS_PUTCHAR` | 1 | Output a character to the console |
| `SYS_GETCHAR` | 2 | Read a character from the console |
| `SYS_EXIT` | 3 | Terminate the current process |
| `SYS_READFILE` | 4 | Read a file |
| `SYS_WRITEFILE` | 5 | Write a file (creates it if it does not exist) |
| `SYS_GETPID` | 6 | Get the current process id |
| `SYS_LISTFILES` | 7 | Name and size of the n-th file |
| `SYS_DELETEFILE` | 8 | Delete a file |
| `SYS_GETARGS` | 9 | Copy this process's argument string into a buffer |
| `SYS_EXEC` | 10 | Start a program from disk; returns the child's pid |
| `SYS_WAIT` | 11 | Wait for a child to exit, then free its slot and memory |

Calling convention: syscall number in `a3`, arguments in `a0`-`a2`, result in `a0`.

## Architecture

### Memory Layout
- **Kernel**: identity-mapped, linked at `0x80200000`, with 64 MB of free RAM after it
- **User programs**: each process has its own page table; segments are loaded in `[0x1000000, 0x10FF000)`
- **User stack**: one page below `0x1100000` (programs also carry a 64 KB stack in their `.bss`)
- **VirtIO block device**: memory-mapped at `0x10001000`

### Process States
- `PROC_UNUSED` - slot available
- `PROC_RUNNABLE` - ready to run
- `PROC_EXITED` - finished, waiting for its parent to `wait` and reap it

### Program Format
Programs in `progs/` are built as small, stripped ELF files (about 1.5 KB each) and stored in the disk image next to the text files. `exec` checks the ELF header, maps each loadable segment, and starts the process at the entry point `0x1000000`. Arguments are passed as a string that the program reads with `getargs()`.

### File System
TAR-based, loaded into memory at boot:
- Up to 8 files (`FILES_MAX`), up to 8 KB each (`FILE_DATA_MAX`)
- The whole disk buffer is rewritten on every write or delete
- 512-byte sectors; the kernel panics with a clear message if the disk image is smaller than its buffer

## Building and Running

### Prerequisites
- **QEMU**: `qemu-system-riscv32`
- **LLVM/Clang** with **lld** and **llvm-objcopy**

```bash
sudo apt update && sudo apt install -y clang llvm lld qemu-system-riscv32
```

### Build and run

```bash
./run.sh
```

`run.sh` builds the shell and the preemption demo program, builds the programs in `progs/` as ELF files, packs them together with `disk/*.txt` into `disk.tar`, builds the kernel, and starts QEMU. Quit QEMU with `Ctrl-A` then `X`.

Options:

```bash
DEMO=1 ./run.sh            # two CPU-bound programs (spin.c) that never yield; output shows AAAA BBBB AAAA...
VIRTIO_MODERN=1 ./run.sh   # present a modern (v2) virtio disk instead of a legacy one
```

### Using the shell

```
>> help
>> ls
>> cat hello.txt
>> echo hello world > notes.txt
>> write todo.txt buy milk
>> rm notes.txt
>> run cat todo.txt
```

Any word that is not a built-in is run as a program from the disk, and the shell waits for it to finish.

### Adding a program

Create `progs/hello.c`, add its name to the `for prog in ...` loop in `run.sh`, and run `./run.sh`:

```c
#include "user.h"

void main(void) {
    printf("hello from a program on disk!\n");
}
```

### Adding files to the disk
Put `.txt` files in `disk/` before running; they are included automatically. (`disk/` may be empty; files can also be created from inside the OS.)

## File Structure

```
├── kernel.c / kernel.h   # kernel: processes, memory, syscalls, ELF loader, virtio, file system
├── kernel.ld             # kernel linker script
├── user.c / user.h       # user-space library (syscall wrappers)
├── user.ld               # user program linker script (programs start at 0x1000000)
├── shell.c               # the shell (embedded in the kernel image)
├── progs/                # programs stored on the disk: ls.c, cat.c
├── spin.c                # CPU-bound program for the preemption demo
├── common.c / common.h   # shared helpers (printf, string functions, syscall numbers)
├── run.sh                # build and run script
├── disk/                 # .txt files that go into the disk image
└── build/                # generated: program ELFs and the disk staging folder
```

## Technical Details

### Traps
- All registers are saved on the process's kernel stack
- Syscall number in `a3`, arguments in `a0`, `a1`, `a2`, result in `a0`
- `sepc` is advanced by 4 after `ecall`

### VirtIO Block Device
- One virtqueue, a 3-descriptor chain per request, polled (no interrupts)
- Legacy devices: one `QueuePFN` write. Modern devices: feature negotiation (`VIRTIO_F_VERSION_1`), then the descriptor, available and used areas are registered separately and the queue is switched on with `QueueReady`

### Page Tables (Sv32)
- Two levels, 4 KB pages, a separate address space per process
- The kernel is mapped (without the user bit) in every address space
- When a process is reaped, only pages with the user bit set are freed; the shared kernel mappings never are

## Debugging

QEMU logs are written to `qemu.log`: unimplemented instructions, guest errors, interrupts and CPU resets.

## Roadmap

- [ ] **Blocking `wait`** - `wait` currently *polls*: the parent keeps getting scheduled, checks whether the child has exited, and yields again. Replace it with a `PROC_WAITING` state that the scheduler skips, and wake the parent from both exit paths (`SYS_EXIT` and the fault-kill path in `handle_trap`).
- [ ] **Increasing process ids** - pids are slot numbers plus one, so a pid is reused as soon as its slot is freed.
- [ ] **Orphans** - a process whose parent never calls `wait` is never reaped.
- [ ] **Exit status** - `exit()` carries no code, so `wait` cannot report success or failure.
- [ ] **Arguments on the stack** - programs read arguments with `getargs()` instead of receiving `argv`.
- [ ] **Directories**, `cd` and `pwd`.

## Limitations

- At most 8 processes at a time
- The kernel itself is not preemptible (interrupts are only taken in user mode)
- Single core
- No dynamic memory allocation in user space
- At most 8 files of 8 KB each; the whole disk buffer is rewritten on every change
- Programs are linked at one fixed address and loaded with read/write/execute on every page

## Learning Resources

- "OS in 1,000 Lines" tutorial (the starting point)
- RISC-V Privileged Specification
- VirtIO Specification v1.x
- TAR file format (USTAR)
- Operating Systems: Three Easy Pieces

## License

Educational project - feel free to use and modify for learning purposes.

## Acknowledgments

Based on the "OS in 1,000 Lines" tutorial and inspired by minimal OS implementations for RISC-V.
