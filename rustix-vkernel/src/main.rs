#![no_std]
#![no_main]
#![feature(alloc_error_handler)]
#![cfg_attr(target_arch = "x86_64", feature(abi_x86_interrupt))]
#![cfg_attr(not(feature = "compat-monitor"), allow(dead_code))]

use core::arch::global_asm;
use core::panic::PanicInfo;

#[no_mangle]
#[cfg(target_arch = "x86_64")]
static mut __stack_chk_guard: usize = 0x6a09_e667_f3bc_c909;

#[no_mangle]
#[inline(never)]
#[cfg(target_arch = "x86_64")]
extern "C" fn __stack_chk_fail() -> ! {
    unsafe {
        core::arch::asm!(
            "cli",
            "mov dx, 0x3f8",
            "mov al, 'S'",
            "out dx, al",
            "2:",
            "hlt",
            "jmp 2b",
            options(noreturn)
        );
    }
}

mod apps;
mod arch;
mod boot;
mod c_fastpath;
mod extfs;
mod fixed;
mod gui;
mod ipc;
mod initramfs;
mod kernel;
mod linux;
mod mm;
mod net;
mod services;
mod storage;
mod sync;
mod wallpaper;

mod print {
    use core::fmt::{self, Write};

    pub struct Buffer {
        buf: [u8; 1024],
        pos: usize,
    }

    impl Buffer {
        pub fn new() -> Self {
            Self {
                buf: [0; 1024],
                pos: 0,
            }
        }

        pub fn as_str(&self) -> &str {
            core::str::from_utf8(&self.buf[..self.pos]).unwrap_or("")
        }
    }

    impl Write for Buffer {
        fn write_str(&mut self, s: &str) -> fmt::Result {
            let bytes = s.as_bytes();
            let remaining = self.buf.len().saturating_sub(self.pos);
            let count = bytes.len().min(remaining);
            self.buf[self.pos..self.pos + count].copy_from_slice(&bytes[..count]);
            self.pos += count;
            Ok(())
        }
    }
}

#[repr(C, align(8))]
#[cfg(target_arch = "x86_64")]
struct MultibootHeader {
    magic: u32,
    architecture: u32,
    header_length: u32,
    checksum: u32,
    framebuffer_tag_type: u16,
    framebuffer_tag_flags: u16,
    framebuffer_tag_size: u32,
    framebuffer_width: u32,
    framebuffer_height: u32,
    framebuffer_depth: u32,
    framebuffer_padding: u32,
    end_tag_type: u16,
    end_tag_flags: u16,
    end_tag_size: u32,
}

#[cfg(feature = "compat-monitor")]
static VKERNEL: crate::sync::SpinMutex<crate::kernel::Kernel> =
    crate::sync::SpinMutex::new(crate::kernel::Kernel::new());

#[link_section = ".multiboot_header"]
#[no_mangle]
#[used]
#[cfg(target_arch = "x86_64")]
static MULTIBOOT_HEADER: MultibootHeader = MultibootHeader {
    magic: 0xE85250D6,
    architecture: 0,
    header_length: core::mem::size_of::<MultibootHeader>() as u32,
    checksum: (0x100000000u64 - (0xE85250D6u64 + core::mem::size_of::<MultibootHeader>() as u64))
        as u32,
    framebuffer_tag_type: 5,
    framebuffer_tag_flags: 0,
    framebuffer_tag_size: 20,
    framebuffer_width: 640,
    framebuffer_height: 480,
    framebuffer_depth: 32,
    framebuffer_padding: 0,
    end_tag_type: 0,
    end_tag_flags: 0,
    end_tag_size: 8,
};

#[cfg(target_arch = "x86_64")]
global_asm!(
    r#"
    .equ EARLY_STACK_PHYS_TOP, 0x01000000
    .equ EARLY_STACK_TOP, 0xffffffff81000000
    .section .boot.text,"ax"
    .global _start_asm
    .extern _start

    .code32
_start_asm:
    cli
    mov esi, eax
    mov edi, ebx
    # Paging is not active yet. Using the truncated high-half address here
    # makes Multiboot2 fault as soon as the trampoline pushes a return frame.
    mov esp, EARLY_STACK_PHYS_TOP
    cld

    lea eax, [p3_table]
    or eax, 0x3
    mov dword ptr [p4_table], eax
    mov dword ptr [p4_table + 4], 0
    mov dword ptr [p4_table + 256 * 8], eax
    mov dword ptr [p4_table + 256 * 8 + 4], 0
    mov dword ptr [p4_table + 511 * 8], eax
    mov dword ptr [p4_table + 511 * 8 + 4], 0

    lea eax, [p2_table0]
    or eax, 0x3
    mov dword ptr [p3_table + 0], eax
    mov dword ptr [p3_table + 4], 0
    lea eax, [p2_table1]
    or eax, 0x3
    mov dword ptr [p3_table + 8], eax
    mov dword ptr [p3_table + 12], 0
    lea eax, [p2_table2]
    or eax, 0x3
    mov dword ptr [p3_table + 16], eax
    mov dword ptr [p3_table + 20], 0
    lea eax, [p2_table3]
    or eax, 0x3
    mov dword ptr [p3_table + 24], eax
    mov dword ptr [p3_table + 28], 0
    lea eax, [kernel_p2_table]
    or eax, 0x3
    mov dword ptr [p3_table + 510 * 8], eax
    mov dword ptr [p3_table + 510 * 8 + 4], 0

    xor ecx, ecx
1:
    mov eax, ecx
    shl eax, 21
    or eax, 0x83
    mov dword ptr [p2_table0 + ecx * 8], eax
    mov dword ptr [p2_table0 + ecx * 8 + 4], 0
    inc ecx
    cmp ecx, 512
    jne 1b

    xor ecx, ecx
2:
    mov eax, ecx
    add eax, 512
    shl eax, 21
    or eax, 0x83
    mov dword ptr [p2_table1 + ecx * 8], eax
    mov dword ptr [p2_table1 + ecx * 8 + 4], 0
    inc ecx
    cmp ecx, 512
    jne 2b

    xor ecx, ecx
3:
    mov eax, ecx
    add eax, 1024
    shl eax, 21
    or eax, 0x83
    mov dword ptr [p2_table2 + ecx * 8], eax
    mov dword ptr [p2_table2 + ecx * 8 + 4], 0
    inc ecx
    cmp ecx, 512
    jne 3b

    xor ecx, ecx
4:
    mov eax, ecx
    add eax, 1536
    shl eax, 21
    or eax, 0x83
    mov dword ptr [p2_table3 + ecx * 8], eax
    mov dword ptr [p2_table3 + ecx * 8 + 4], 0
    inc ecx
    cmp ecx, 512
    jne 4b

    xor ecx, ecx
5:
    mov eax, ecx
    shl eax, 21
    or eax, 0x83
    mov dword ptr [kernel_p2_table + ecx * 8], eax
    mov dword ptr [kernel_p2_table + ecx * 8 + 4], 0
    inc ecx
    cmp ecx, 512
    jne 5b

    lea eax, [p4_table]
    mov cr3, eax

    mov eax, cr4
    or eax, (1 << 5) | (1 << 4) | (1 << 9) | (1 << 10)
    mov cr4, eax

    mov ecx, 0xC0000080
    rdmsr
    or eax, 1 << 8
    wrmsr

    lgdt [gdt64_descriptor]

    mov eax, cr0
    and eax, 0xfffffff3
    or eax, (1 << 31) | (1 << 5) | (1 << 1)
    mov cr0, eax

    push 0x08
    mov eax, offset long_mode_start
    push eax
    retf

    .code64
long_mode_start:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov rsp, EARLY_STACK_TOP
    mov r12d, esi
    mov r13d, edi
    # Do not rely on every Multiboot/UEFI loader to materialize ELF NOBITS.
    # The active bootstrap page tables live in .boot.bss, so clear only the
    # high-half kernel BSS before any Rust static is observed.
    movabs rdi, offset bss_start
    movabs rcx, offset bss_end
    sub rcx, rdi
    xor eax, eax
    rep stosb
    rdtsc
    shl rdx, 32
    or rax, rdx
    movabs rbx, 0xa5a55a5ac3c33c3c
    xor rax, rbx
    movabs rbx, offset __stack_chk_guard
    mov qword ptr [rbx], rax
    mov edi, r12d
    mov esi, r13d
    movabs rax, offset _start
    call rax
2:
    hlt
    jmp 2b

    .align 8
gdt64:
    .quad 0x0000000000000000
    .quad 0x00AF9A000000FFFF
    .quad 0x00AF92000000FFFF
gdt64_descriptor:
    .word (3 * 8) - 1
    .long gdt64

    .section .boot.bss,"aw",@nobits
    .align 4096
p4_table:
    .skip 4096
p3_table:
    .skip 4096
p2_table0:
    .skip 4096
p2_table1:
    .skip 4096
p2_table2:
    .skip 4096
p2_table3:
    .skip 4096
kernel_p2_table:
    .skip 4096
    "#
);

#[cfg(target_arch = "aarch64")]
global_asm!(include_str!("aarch64_boot.S"));

#[macro_export]
macro_rules! print {
    ($($arg:tt)*) => {{
        let mut buf = crate::print::Buffer::new();
        let _ = core::fmt::write(&mut buf, format_args!($($arg)*));
        $crate::arch::console_write(buf.as_str());
    }};
}

#[macro_export]
macro_rules! println {
    () => {
        $crate::print!("\n");
    };
    ($($arg:tt)*) => {{
        let mut buf = crate::print::Buffer::new();
        let _ = core::fmt::write(&mut buf, format_args!($($arg)*));
        let _ = core::fmt::Write::write_str(&mut buf, "\n");
        $crate::arch::console_write(buf.as_str());
    }};
}

fn kernel_main(protocol: u64, information: u64) -> ! {
    #[cfg(target_arch = "aarch64")]
    crate::arch::configure_boot(protocol, information);
    crate::arch::init(protocol, information);
    crate::println!(
        "[boot] protocol={:#x} information={:#x}",
        protocol,
        information
    );
    #[cfg(target_arch = "x86_64")]
    crate::println!(
        "[video] source={} mode={}x{}x{} graphics={}",
        crate::arch::video_source(),
        crate::arch::screen_width(),
        crate::arch::screen_height(),
        crate::arch::screen_bpp(),
        if crate::arch::graphics_mode_enabled() {
            "on"
        } else {
            "off"
        }
    );
    crate::println!(
        "[c-fastpath] ABI/self-test={}",
        if crate::c_fastpath::self_test() {
            "ok"
        } else {
            "failed"
        }
    );
    let memory = crate::mm::initialize(protocol, information);
    crate::println!(
        "[mm] source={} regions={} usable={} KiB free_frames={} numa={}/local:{}/fallback:{} bitmap={} high={} split={} ucopy={} uguard={} W^X={} kguard={} heap={} KiB/{} SMEP={} SMAP={}",
        memory.source.name(),
        memory.regions,
        memory.usable_bytes / 1024,
        memory.free_frames,
        memory.numa_nodes,
        memory.numa_local_allocations,
        memory.numa_fallback_allocations,
        if memory.allocator_self_test { "ok" } else { "failed" },
        if memory.high_half { "ok" } else { "failed" },
        if memory.user_isolation { "ok" } else { "failed" },
        if memory.user_copy { "ok" } else { "failed" },
        if memory.user_guard { "ok" } else { "failed" },
        if memory.write_xor_execute { "ok" } else { "failed" },
        if memory.stack_guard { "ok" } else { "failed" },
        memory.heap_bytes / 1024,
        if memory.heap_self_test { "ok" } else { "failed" },
        if memory.smep { "on" } else { "n/a" },
        if memory.smap { "on" } else { "n/a" },
    );
    #[cfg(all(target_arch = "x86_64", feature = "compat-monitor"))]
    crate::gui::init();
    #[cfg(target_arch = "aarch64")]
    {
        let hardware = crate::arch::hardware_info();
        crate::println!(
            "[arm64] MMU={} GICv2={} IRQs={} timer={} ctl={:#x} ppi={} masked={} ticks={} CPUs={} per-cpu-timer={} IPI={} irq-lock={}",
            if hardware.mmu { "on" } else { "off" },
            if hardware.gic_interrupt_lines != 0 {
                "on"
            } else {
                "off"
            },
            hardware.gic_interrupt_lines,
            if hardware.timer { "on" } else { "off" },
            hardware.timer_control,
            if hardware.timer_irq_enabled {
                "on"
            } else {
                "off"
            },
            if hardware.irq_masked { "yes" } else { "no" },
            hardware.timer_ticks,
            hardware.cpus_online,
            if hardware.per_cpu_timer { "ok" } else { "failed" },
            if hardware.tlb_ipi { "ok" } else { "failed" },
            if hardware.irq_lock { "ok" } else { "failed" },
        );
        if hardware.rp1.controller != 0 {
            crate::println!(
                "[rp1] pcie={:#x} link={} device={:04x}:{:04x} bdf={:02x}:{:02x}.{} bar0={:#x}",
                hardware.rp1.controller,
                if hardware.rp1.link_up { "up" } else { "down" },
                hardware.rp1.vendor,
                hardware.rp1.device,
                hardware.rp1.bus,
                hardware.rp1.slot,
                hardware.rp1.function,
                hardware.rp1.bar0,
            );
        }
        crate::println!(
            "[gic] dist={:#x} cpu={:#x} group={:#x} pending={:#x} highest={}",
            hardware.gic_state.distributor_control,
            hardware.gic_state.cpu_control,
            hardware.gic_state.group,
            hardware.gic_state.pending,
            hardware.gic_state.highest_pending & 0x3ff,
        );
        print_storage_summary();
    }
    #[cfg(target_arch = "x86_64")]
    {
        let hardware = crate::arch::initialize_runtime();
        crate::println!(
            "[irq] IDT={} syscall/sysret={} IST-guard={} LAPIC={} id={} IOAPIC={} routes={} timer-irq={}",
            if hardware.idt { "ok" } else { "failed" },
            if hardware.syscall { "ok" } else { "failed" },
            if hardware.exception_guard {
                "ok"
            } else {
                "failed"
            },
            if hardware.local_apic { "ok" } else { "failed" },
            hardware.apic_id,
            if hardware.ioapic { "ok" } else { "n/a" },
            hardware.ioapic_redirections,
            if hardware.timer_irq { "ok" } else { "failed" },
        );
        crate::println!(
            "[clock] source={} invariant-tsc={} TSC={} kHz HPET={} kHz APIC={} kHz tick={} Hz now={} ns",
            hardware.clock_source,
            if hardware.invariant_tsc { "yes" } else { "no" },
            hardware.tsc_hz / 1000,
            hardware.hpet_hz / 1000,
            hardware.apic_timer_hz / 1000,
            hardware.timer_hz,
            crate::arch::monotonic_time_ns(),
        );
        crate::println!(
            "[acpi/smp] ACPI={} root={} CPUs={}/{} attempted={} per-cpu={} irq-lock={}",
            hardware.acpi_revision,
            if hardware.acpi_xsdt { "XSDT" } else { "RSDT" },
            hardware.cpus_online,
            hardware.cpus_discovered,
            hardware.cpus_attempted,
            if hardware.per_cpu { "ok" } else { "failed" },
            if hardware.irq_lock { "ok" } else { "failed" },
        );
        print_storage_summary();

        #[cfg(not(feature = "compat-monitor"))]
        match crate::kernel::scheduler::initialize() {
            Ok(()) => {
                crate::net::init();
                let mut scheduler = crate::kernel::scheduler::summary();
                for _ in 0..100_000_000usize {
                    scheduler = crate::kernel::scheduler::summary();
                    if scheduler.context_switches >= 9
                        && scheduler.kernel_heartbeat != 0
                        && scheduler.user_heartbeat != 0
                        && scheduler.user_syscalls != 0
                        && scheduler.process_cycles != 0
                        && scheduler.exec_cycles != 0
                        && scheduler.signal_deliveries != 0
                        && scheduler.futex_wakeups != 0
                        && scheduler.policy_updates >= 3
                        && scheduler.ipc_deliveries >= 2
                        && scheduler.ipc_rejections != 0
                        && scheduler.address_spaces_reclaimed >= 2
                        && scheduler.task_table_entries == scheduler.threads
                    {
                        break;
                    }
                    core::hint::spin_loop();
                }
                crate::println!(
                    "[sched] preempt={} threads={} cpus={} switches={} migrations={} busiest={} kthread={} ring3={} user={} syscall={} fork-wait={} exec={} signal={} futex={} policy={} ipc-user={} ipc-deny={} ptable={} reclaim={}",
                    if scheduler.context_switches >= 9 && scheduler.kernel_heartbeat != 0 {
                        "ok"
                    } else {
                        "failed"
                    },
                    scheduler.threads,
                    scheduler.online_cpus,
                    scheduler.context_switches,
                    scheduler.migrations,
                    scheduler.busiest_cpu_ticks,
                    scheduler.kernel_heartbeat,
                    if scheduler.user_heartbeat != 0 { "ok" } else { "failed" },
                    scheduler.user_heartbeat,
                    if scheduler.user_syscalls != 0 { "ok" } else { "failed" },
                    if scheduler.process_cycles != 0 { "ok" } else { "failed" },
                    if scheduler.exec_cycles != 0 { "ok" } else { "failed" },
                    if scheduler.signal_deliveries != 0 { "ok" } else { "failed" },
                    if scheduler.futex_wakeups != 0 { "ok" } else { "failed" },
                    if scheduler.policy_updates >= 3 { "ok" } else { "failed" },
                    if scheduler.ipc_deliveries >= 2 { "ok" } else { "failed" },
                    if scheduler.ipc_rejections != 0 { "ok" } else { "failed" },
                    if scheduler.task_table_entries == scheduler.threads { "ok" } else { "failed" },
                    if scheduler.address_spaces_reclaimed >= 2 { "ok" } else { "failed" },
                );
            }
            Err(error) => crate::println!("[sched] initialization failed: {}", error),
        }
    }
    #[cfg(feature = "compat-monitor")]
    {
        let mut kernel = VKERNEL.lock();
        kernel.boot();
        kernel.run();
    }
    #[cfg(not(feature = "compat-monitor"))]
    {
        crate::println!("[microkernel] isolated Ring 3 services online; compatibility monitor=off");
        loop {
            crate::arch::wait_for_interrupt();
        }
    }
}

fn print_storage_summary() {
    let storage = crate::storage::initialize();
    crate::println!(
        "[storage] ATA={} AHCI={} NVMe={} async-depth={} block-io={} async-io={} ext2/ext4={}",
        if storage.ata_present {
            "present"
        } else {
            "absent"
        },
        if storage.ahci_present {
            "present"
        } else {
            "absent"
        },
        if storage.nvme_present {
            "present"
        } else {
            "absent"
        },
        storage.async_queue_depth,
        if storage.block_self_test {
            "ok"
        } else {
            "failed"
        },
        if storage.async_self_test {
            "ok"
        } else {
            "failed"
        },
        if storage.ext_self_test {
            "ok"
        } else {
            "failed"
        },
    );
    let root = crate::initramfs::mount_root(&crate::services::VfsService::new());
    crate::println!(
        "[rootfs] initramfs={} entries={} bytes={} recovery={}",
        if root.mounted { "mounted" } else { "failed" },
        root.entries,
        root.bytes,
        if root.recovered { "ok" } else { "failed" },
    );
}

#[alloc_error_handler]
fn allocation_error(layout: core::alloc::Layout) -> ! {
    #[cfg(target_arch = "x86_64")]
    {
        crate::mm::heap::handle_oom(layout)
    }
    #[cfg(not(target_arch = "x86_64"))]
    {
        panic!("kernel allocation failed: {:?}", layout)
    }
}

#[cfg(target_arch = "x86_64")]
#[no_mangle]
pub extern "C" fn _start(protocol: u64, information: u64) -> ! {
    kernel_main(protocol, information)
}

#[cfg(target_arch = "aarch64")]
#[no_mangle]
pub extern "C" fn _start_rust(protocol: u64, information: u64) -> ! {
    kernel_main(protocol, information)
}

#[panic_handler]
fn panic(info: &PanicInfo) -> ! {
    crate::println!("\n=== v-kernel panic ===");
    if let Some(location) = info.location() {
        crate::println!("location: {}:{}", location.file(), location.line());
    }
    crate::println!("message: {}", info.message());

    loop {
        #[cfg(target_arch = "x86_64")]
        unsafe {
            core::arch::asm!("hlt");
        }
        #[cfg(target_arch = "aarch64")]
        unsafe {
            core::arch::asm!("wfi");
        }
    }
}
