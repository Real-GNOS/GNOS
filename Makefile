# Makefile — build the GNOS 64-bit kernel and boot it with Limine in QEMU.
#
# Pipeline:
#   kernel/*.c    --(gcc -m64) --> GNOSKr.elf      (64-bit kernel, Limine entry)
#   user/*.c      --(gcc -m64) --> *.elf          (user programs, all @0x400000)
#   user programs --(mke2fs)   -> initrd.img      (ext2 image)
#   +limine bins  --(xorriso)  -> gnos.iso       (hybrid BIOS+UEFI CD)
#   gnos.iso     --(qemu)     runs the OS
#
# Every user program is linked at the same address on purpose: each process
# has its own page tables, so they never collide.

BUILD := build
OVMF  := /usr/share/ovmf/OVMF.fd

# `all` (kernel + ISO) is defined further down; without this, make's default
# goal is the first target in the file (the mbedtls library) and a bare
# `make` would never rebuild the kernel.
.DEFAULT_GOAL := all

# Where the userland shipped in the image comes from.
#
#   minirootfs (default)
#                     tools/build-alpine-rootfs.sh unpacks Alpine's own
#                     minirootfs into build/alpine-rootfs and then lets apk
#                     install the extras into it.  The image's root filesystem
#                     IS Alpine's: /etc/os-release, the skeleton files, the
#                     CA bundle, the signing keys and a working /sbin/apk all
#                     come from there, and only GNOS's own binaries and the
#                     handful of files Alpine has no opinion about ride on
#                     top.  Alpine's musl-dev in that tree is also the sysroot
#                     src/usr's programs are compiled against.
#   alpine            the previous recipe: tools/install-alpine.sh stages
#                     build/alpine-root by unpacking individual .apk files.
#                     Kept because it is a useful comparison -- it ships
#                     Alpine's binaries but not Alpine's system.
#   src               the original recipe: build coreutils/bash/curl/nano/
#                     python/busybox/binutils/fastfetch/openrc from trees
#                     fetched by hand into build/*src/.  Also opt-in.
USERLAND ?= minirootfs

# The system compiler for the userland programs.  Must be gcc-13: the
# stock gcc 12.3 on this machine has a libcpp ICE (_cpp_process_line_notes,
# libcpp/lex.cc:1163) that randomly kills preprocessing of perfectly
# ordinary files (getty.c, ncurses's make_hash.c, ...).
CC      := gcc-13
AS      := nasm
LD      := ld
OBJCOPY := objcopy

# Architecture backend: src/arch/$(ARCH) supplies the descriptor tables, entry
# assembly and context switch.  Must be set before BASEFLAGS, which puts the
# arch tree on the include path.
ARCH ?= x86
ARCH_ROOT := src/arch

# The source tree follows the Linux top-level layout (arch/ drivers/ fs/
# include/ init/ ipc/ kernel/ lib/ mm/ net/ samples/ scripts/ sound/ usr/), so
# the include path and the vpath are derived from the directory list instead of
# being spelled out: every directory that holds a source or a header is on the
# search path.  This is only safe because header basenames are unique across
# the whole tree -- `make check-hdrs` asserts that.
#
# -prune, not -not -path: find must not descend into the vendored trees at all
# (src/LeonOS-4 alone is 164 MB) or they would land on the include path too.
PRUNE := -name vendor -prune -o -name rootfs -prune -o -name d -prune -o
SRCDIRS := $(sort $(shell find src $(PRUNE) \
             \( -name '*.[chS]' -o -name '*.asm' \) -printf '%h\n' | sort -u))

# Exactly one architecture backend is visible: an i386 build must never see
# x86 sources, and the other way round.
ARCH_DIRS   := $(filter $(ARCH_ROOT)/%,$(SRCDIRS))
ACTIVE_ARCH := $(filter $(ARCH_ROOT)/$(ARCH) $(ARCH_ROOT)/$(ARCH)/%,$(ARCH_DIRS))
OTHER_ARCH  := $(filter-out $(ACTIVE_ARCH),$(ARCH_DIRS))

# Kernel include path: everything except the foreign architectures and the
# userland, whose headers would otherwise leak into kernel objects.
KERNEL_DIRS := $(filter-out $(OTHER_ARCH) src/usr src/usr/%,$(SRCDIRS))
KINCS       := $(addprefix -I,$(KERNEL_DIRS))

# FatFs (src/vendor/fatfs/source) is third-party and pruned from the auto
# -I list, but its own headers must be visible to ff.c and to the GNOS
# port layer in src/fs/fatfs.
KINCS       += -Isrc/vendor/fatfs/source

# ACPICA: the OS services layer (src/drivers/acpi/acpica_osl.c) includes the
# vendored headers directly.
KINCS       += -Isrc/vendor/acpica/source/include -Isrc/vendor/acpica/source/include/platform
KINCS       += -Isrc/include/gnoslibc

# gcc defines __linux__ on this host, which makes acenv.h pick ACPICA's
# Linux glue (unistd.h and friends).  We are not Linux: drop the macro so it
# falls back to the generic gcc environment.
ACPICA_CFLAGS := -U__linux__ -Ulinux -U__unix__ -U_LINUX -D_GNOS_ -DACPI_MACHINE_WIDTH=64

# The two trees with their own consumers: headers shared between kernel and
# userland (sysnum.h, bootinfo.h), and the userland itself.
SHARED_DIR := src/include/uapi/linux
USER_DIR   := src/usr

# Common freestanding flags.  -mgeneral-regs-only keeps gcc away from
# SSE/MMX/x87 registers: the CPU arrives from Limine with CR4.OSFXSR clear,
# so any xmm instruction would raise #UD (and, with no IDT yet, triple-fault).
# -mno-red-zone is mandatory once interrupts can land on the kernel stack.
BASEFLAGS := -m64 -ffreestanding -nostdlib -fno-stack-protector -fno-builtin \
             -nostdinc -std=gnu11 -mno-red-zone -mgeneral-regs-only \
             -mno-sse -mno-sse2 -mno-mmx -mno-80387 -fvisibility=hidden \
             -Wall -Wextra -O2 -g $(KINCS)

# kernel: PIE so Limine can relocate it into the higher half
# build/.config (produced by `make config`) defines CONFIG_* macros.
# We convert them to -D flags: CONFIG_FOO=y -> -DCONFIG_FOO=1, CONFIG_FOO="bar" -> -DCONFIG_FOO='"bar"'
KCFLAGS := $(BASEFLAGS) -fpie -DSYSTRACE \
            $(shell if [ -f build/.config ]; then \
              sed -n 's/^CONFIG_\(.*\)=y/-DCONFIG_\1=1/p; s/^CONFIG_\(.*\)="\([^"]*\)"/-DCONFIG_\1=\"\2\"/p; s/^CONFIG_\(.*\)=\([0-9][0-9]*\)/-DCONFIG_\1=\2/p' build/.config; \
            fi)
# user programs: linked at a fixed address by $(USER_DIR)/user.ld
UCFLAGS := $(BASEFLAGS) -I$(USER_DIR) -fno-pie -fno-pic

# Limine binaries (copied from a local Limine install)
LIMINE_BIOS := limine/limine-bios-cd.bin
LIMINE_UEFI := limine/limine-uefi-cd.bin

# ---- artifacts ----
KRNL    := $(BUILD)/GNOSKr.elf
INITRD  := $(BUILD)/initrd.img
ISO     := $(BUILD)/gnos.iso
ISO_ROOT := $(BUILD)/iso

KOBJS := $(BUILD)/kernel.o $(BUILD)/loader.o $(BUILD)/fbcon.o $(BUILD)/gfx.o \
         $(BUILD)/fbdev.o \
         $(BUILD)/drm_drv.o $(BUILD)/drm_file.o $(BUILD)/drm_auth.o \
         $(BUILD)/drm_ioctl.o $(BUILD)/drm_init.o $(BUILD)/drm_devtmpfs.o \
         $(BUILD)/intrusive_list.o $(BUILD)/rbtree.o \
         $(BUILD)/drm_idr.o $(BUILD)/drm_hashtab.o $(BUILD)/drm_rect.o \
         $(BUILD)/drm_mm.o $(BUILD)/drm_vsnprintf.o $(BUILD)/drm_print.o \
         $(BUILD)/drm_modeset_lock.o \
         $(BUILD)/drm_connector.o $(BUILD)/drm_crtc.o $(BUILD)/drm_encoder.o \
         $(BUILD)/drm_plane.o $(BUILD)/drm_mode_config.o \
         $(BUILD)/drm_mode_object.o $(BUILD)/drm_modes.o $(BUILD)/drm_blend.o \
         $(BUILD)/drm_atomic.o $(BUILD)/drm_atomic_helper.o \
         $(BUILD)/drm_atomic_uapi.o $(BUILD)/drm_vblank.o \
         $(BUILD)/drm_gem.o $(BUILD)/drm_framebuffer.o \
         $(BUILD)/drm_property.o $(BUILD)/drm_libc.o \
         $(BUILD)/drm_vbe.o $(BUILD)/drm_svga.o \
    $(BUILD)/kaslr.o \
                  $(BUILD)/subsys.o $(BUILD)/acpi.o $(BUILD)/sysfs.o \
         $(BUILD)/debugcon.o $(BUILD)/ext2.o $(BUILD)/panic.o \
         $(BUILD)/gdt.o $(BUILD)/idt.o $(BUILD)/isr.o \
         $(BUILD)/kstring.o $(BUILD)/vfs.o $(BUILD)/procfs.o $(BUILD)/debugfs.o $(BUILD)/tmpfs.o $(BUILD)/tty.o $(BUILD)/heap.o \
         $(BUILD)/pmm.o $(BUILD)/vmm.o $(BUILD)/slab.o $(BUILD)/proc.o $(BUILD)/cgroup.o $(BUILD)/ptrace.o \
        $(BUILD)/signal.o $(BUILD)/switch.o $(BUILD)/timer.o \
        $(BUILD)/syscall.o \
        $(BUILD)/smp.o $(BUILD)/ap_trampoline.o \
        $(BUILD)/lapic.o \
        $(BUILD)/net.o $(BUILD)/tcp.o $(BUILD)/sock.o \
         $(BUILD)/pci.o $(BUILD)/e1000.o $(BUILD)/audio.o \
        $(BUILD)/hda.o $(BUILD)/ata.o $(BUILD)/nvme.o $(BUILD)/cjkfont.o \
        $(BUILD)/cjkfont_data.o \
        $(BUILD)/input.o $(BUILD)/xhci.o $(BUILD)/usb_hid.o $(BUILD)/usb_msc.o \
        $(BUILD)/anonfd.o $(BUILD)/epoll.o $(BUILD)/timerfd.o $(BUILD)/signalfd.o \
        $(BUILD)/pty.o $(BUILD)/alsa.o $(BUILD)/klog.o $(BUILD)/cpuid.o $(BUILD)/pagecache.o \
        $(BUILD)/pidfd.o $(BUILD)/process_vm_access.o $(BUILD)/quota.o $(BUILD)/ff.o $(BUILD)/diskio.o $(BUILD)/ffsystem.o $(BUILD)/fatfs_vfs.o \
        $(BUILD)/secretmem.o $(BUILD)/landlock.o \
        $(BUILD)/unix.o \
        $(BUILD)/sysvipc.o \
        $(BUILD)/seccomp.o \
        $(BUILD)/module.o $(BUILD)/module_elf.o $(BUILD)/exports.o \
        $(BUILD)/crypto.o \
        $(BUILD)/scsi.o \
        $(BUILD)/vcs.o \
        $(BUILD)/iso9660.o \
        $(BUILD)/limine_requests.o \
        $(BUILD)/acpica_osl.o \
        $(BUILD)/acpica_dispatcher_dsargs.o \
        $(BUILD)/acpica_dispatcher_dscontrol.o \
        $(BUILD)/acpica_dispatcher_dsdebug.o \
        $(BUILD)/acpica_dispatcher_dsfield.o \
        $(BUILD)/acpica_dispatcher_dsinit.o \
        $(BUILD)/acpica_dispatcher_dsmethod.o \
        $(BUILD)/acpica_dispatcher_dsmthdat.o \
        $(BUILD)/acpica_dispatcher_dsobject.o \
        $(BUILD)/acpica_dispatcher_dsopcode.o \
        $(BUILD)/acpica_dispatcher_dspkginit.o \
        $(BUILD)/acpica_dispatcher_dsutils.o \
        $(BUILD)/acpica_dispatcher_dswexec.o \
        $(BUILD)/acpica_dispatcher_dswload.o \
        $(BUILD)/acpica_dispatcher_dswload2.o \
        $(BUILD)/acpica_dispatcher_dswscope.o \
        $(BUILD)/acpica_dispatcher_dswstate.o \
        $(BUILD)/acpica_events_evevent.o \
        $(BUILD)/acpica_events_evglock.o \
        $(BUILD)/acpica_events_evgpe.o \
        $(BUILD)/acpica_events_evgpeblk.o \
        $(BUILD)/acpica_events_evgpeinit.o \
        $(BUILD)/acpica_events_evgpeutil.o \
        $(BUILD)/acpica_events_evhandler.o \
        $(BUILD)/acpica_events_evmisc.o \
        $(BUILD)/acpica_events_evregion.o \
        $(BUILD)/acpica_events_evrgnini.o \
        $(BUILD)/acpica_events_evsci.o \
        $(BUILD)/acpica_events_evxface.o \
        $(BUILD)/acpica_events_evxfevnt.o \
        $(BUILD)/acpica_events_evxfgpe.o \
        $(BUILD)/acpica_events_evxfregn.o \
        $(BUILD)/acpica_executer_exconcat.o \
        $(BUILD)/acpica_executer_exconfig.o \
        $(BUILD)/acpica_executer_exconvrt.o \
        $(BUILD)/acpica_executer_excreate.o \
        $(BUILD)/acpica_executer_exdebug.o \
        $(BUILD)/acpica_executer_exdump.o \
        $(BUILD)/acpica_executer_exfield.o \
        $(BUILD)/acpica_executer_exfldio.o \
        $(BUILD)/acpica_executer_exmisc.o \
        $(BUILD)/acpica_executer_exmutex.o \
        $(BUILD)/acpica_executer_exnames.o \
        $(BUILD)/acpica_executer_exoparg1.o \
        $(BUILD)/acpica_executer_exoparg2.o \
        $(BUILD)/acpica_executer_exoparg3.o \
        $(BUILD)/acpica_executer_exoparg6.o \
        $(BUILD)/acpica_executer_exprep.o \
        $(BUILD)/acpica_executer_exregion.o \
        $(BUILD)/acpica_executer_exresnte.o \
        $(BUILD)/acpica_executer_exresolv.o \
        $(BUILD)/acpica_executer_exresop.o \
        $(BUILD)/acpica_executer_exserial.o \
        $(BUILD)/acpica_executer_exstore.o \
        $(BUILD)/acpica_executer_exstoren.o \
        $(BUILD)/acpica_executer_exstorob.o \
        $(BUILD)/acpica_executer_exsystem.o \
        $(BUILD)/acpica_executer_extrace.o \
        $(BUILD)/acpica_executer_exutils.o \
        $(BUILD)/acpica_hardware_hwacpi.o \
        $(BUILD)/acpica_hardware_hwesleep.o \
        $(BUILD)/acpica_hardware_hwgpe.o \
        $(BUILD)/acpica_hardware_hwpci.o \
        $(BUILD)/acpica_hardware_hwregs.o \
        $(BUILD)/acpica_hardware_hwsleep.o \
        $(BUILD)/acpica_hardware_hwtimer.o \
        $(BUILD)/acpica_hardware_hwvalid.o \
        $(BUILD)/acpica_hardware_hwxface.o \
        $(BUILD)/acpica_hardware_hwxfsleep.o \
        $(BUILD)/acpica_namespace_nsaccess.o \
        $(BUILD)/acpica_namespace_nsalloc.o \
        $(BUILD)/acpica_namespace_nsarguments.o \
        $(BUILD)/acpica_namespace_nsconvert.o \
        $(BUILD)/acpica_namespace_nsdump.o \
        $(BUILD)/acpica_namespace_nsdumpdv.o \
        $(BUILD)/acpica_namespace_nseval.o \
        $(BUILD)/acpica_namespace_nsinit.o \
        $(BUILD)/acpica_namespace_nsload.o \
        $(BUILD)/acpica_namespace_nsnames.o \
        $(BUILD)/acpica_namespace_nsobject.o \
        $(BUILD)/acpica_namespace_nsparse.o \
        $(BUILD)/acpica_namespace_nspredef.o \
        $(BUILD)/acpica_namespace_nsprepkg.o \
        $(BUILD)/acpica_namespace_nsrepair.o \
        $(BUILD)/acpica_namespace_nsrepair2.o \
        $(BUILD)/acpica_namespace_nssearch.o \
        $(BUILD)/acpica_namespace_nsutils.o \
        $(BUILD)/acpica_namespace_nswalk.o \
        $(BUILD)/acpica_namespace_nsxfeval.o \
        $(BUILD)/acpica_namespace_nsxfname.o \
        $(BUILD)/acpica_namespace_nsxfobj.o \
        $(BUILD)/acpica_parser_psargs.o \
        $(BUILD)/acpica_parser_psloop.o \
        $(BUILD)/acpica_parser_psobject.o \
        $(BUILD)/acpica_parser_psopcode.o \
        $(BUILD)/acpica_parser_psopinfo.o \
        $(BUILD)/acpica_parser_psparse.o \
        $(BUILD)/acpica_parser_psscope.o \
        $(BUILD)/acpica_parser_pstree.o \
        $(BUILD)/acpica_parser_psutils.o \
        $(BUILD)/acpica_parser_pswalk.o \
        $(BUILD)/acpica_parser_psxface.o \
        $(BUILD)/acpica_resources_rsaddr.o \
        $(BUILD)/acpica_resources_rscalc.o \
        $(BUILD)/acpica_resources_rscreate.o \
        $(BUILD)/acpica_resources_rsinfo.o \
        $(BUILD)/acpica_resources_rsio.o \
        $(BUILD)/acpica_resources_rsirq.o \
        $(BUILD)/acpica_resources_rslist.o \
        $(BUILD)/acpica_resources_rsmemory.o \
        $(BUILD)/acpica_resources_rsmisc.o \
        $(BUILD)/acpica_resources_rsserial.o \
        $(BUILD)/acpica_resources_rsutils.o \
        $(BUILD)/acpica_resources_rsxface.o \
        $(BUILD)/acpica_tables_tbdata.o \
        $(BUILD)/acpica_tables_tbfadt.o \
        $(BUILD)/acpica_tables_tbfind.o \
        $(BUILD)/acpica_tables_tbinstal.o \
        $(BUILD)/acpica_tables_tbprint.o \
        $(BUILD)/acpica_tables_tbutils.o \
        $(BUILD)/acpica_tables_tbxface.o \
        $(BUILD)/acpica_tables_tbxfload.o \
        $(BUILD)/acpica_tables_tbxfroot.o \
        $(BUILD)/acpica_utilities_utaddress.o \
        $(BUILD)/acpica_utilities_utalloc.o \
        $(BUILD)/acpica_utilities_utascii.o \
        $(BUILD)/acpica_utilities_utbuffer.o \
        $(BUILD)/acpica_utilities_utcache.o \
        $(BUILD)/acpica_utilities_utcksum.o \
        $(BUILD)/acpica_utilities_utclib.o \
        $(BUILD)/acpica_utilities_utcopy.o \
        $(BUILD)/acpica_utilities_utdebug.o \
        $(BUILD)/acpica_utilities_utdecode.o \
        $(BUILD)/acpica_utilities_utdelete.o \
        $(BUILD)/acpica_utilities_uterror.o \
        $(BUILD)/acpica_utilities_uteval.o \
        $(BUILD)/acpica_utilities_utexcep.o \
        $(BUILD)/acpica_utilities_utglobal.o \
        $(BUILD)/acpica_utilities_uthex.o \
        $(BUILD)/acpica_utilities_utids.o \
        $(BUILD)/acpica_utilities_utinit.o \
        $(BUILD)/acpica_utilities_utlock.o \
        $(BUILD)/acpica_utilities_utmath.o \
        $(BUILD)/acpica_utilities_utmisc.o \
        $(BUILD)/acpica_utilities_utmutex.o \
        $(BUILD)/acpica_utilities_utnonansi.o \
        $(BUILD)/acpica_utilities_utobject.o \
        $(BUILD)/acpica_utilities_utosi.o \
        $(BUILD)/acpica_utilities_utownerid.o \
        $(BUILD)/acpica_utilities_utpredef.o \
        $(BUILD)/acpica_utilities_utresdecode.o \
        $(BUILD)/acpica_utilities_utresrc.o \
        $(BUILD)/acpica_utilities_utstate.o \
        $(BUILD)/acpica_utilities_utstring.o \
        $(BUILD)/acpica_utilities_utstrsuppt.o \
        $(BUILD)/acpica_utilities_utstrtoul64.o \
        $(BUILD)/acpica_utilities_uttrack.o \
        $(BUILD)/acpica_utilities_utuuid.o \
        $(BUILD)/acpica_utilities_utxface.o \
        $(BUILD)/acpica_utilities_utxferror.o \
        $(BUILD)/acpica_utilities_utxfinit.o \
        $(BUILD)/acpica_utilities_utxfmutex.o \

# User programs: name -> build/<name>.elf, all linked from crt0 + ulib.
UPROGS  := init shell count ls cat tail tac rm mkdir touch scan dbgcat envtest
UELFS   := $(addprefix $(BUILD)/,$(addsuffix .elf,$(UPROGS)))
UCRT    := $(BUILD)/user/crt0.o $(BUILD)/user/ulib.o

# musl provides the C runtime for programs that want a real libc.  They are
# linked non-PIE at 0x400000 like every other user program, but from musl's
# crt1.o + libc.a instead of ulib.
#
# USERLAND=alpine points this at build/alpine-root/usr: Alpine's musl-dev
# ships include/, crt1.o, crti.o, crtn.o and libc.a, which is the whole of
# what the rules below consume.  MUSL_GCC stays aimed at the source tree --
# Alpine ships no musl-gcc and only dynhello (a USERLAND=src extra) needs one.
MUSL_SRC    := $(BUILD)/muslsrc/musl-1.2.5
ifeq ($(USERLAND),alpine)
MUSL_PREFIX := $(BUILD)/alpine-root/usr
else ifeq ($(USERLAND),minirootfs)
# The minirootfs tree carries Alpine's musl-dev, so it doubles as the sysroot.
MUSL_PREFIX := $(BUILD)/alpine-rootfs/usr
else
MUSL_PREFIX := $(BUILD)/muslsrc/musl
endif
MUSL_LIB  := $(MUSL_PREFIX)/lib
MUSL_CRT  := $(MUSL_PREFIX)/lib
MUSL_INC  := $(MUSL_PREFIX)/include
MUSL_GCC  := $(MUSL_PREFIX)/bin/musl-gcc

# Programs built against musl rather than ulib.
MUSLPROGS := hello mount coldplug chvt getty login agetty bgidm installer ttytest thrtest drmtest ptracetest insmod rmmod evtest eventest socktest ipctest wiggle nep1 cowtest
MUSL_OBJS := $(addprefix $(BUILD)/user/,$(addsuffix .o,$(MUSLPROGS)))
MUSL_ELFS := $(addprefix $(BUILD)/,$(addsuffix .elf,$(MUSLPROGS)))

# BusyBox: the first piece of real third-party userland.  Its source tree was
# fetched by hand into build/bbsrc and configured from allnoconfig with only
# the applets below turned on -- no shell yet: the toy shell still drives
# /etc/rc, and ash wants job control features the kernel is only now growing.
# Statically linked against musl, so the kernel only has to load ET_EXEC.
BB_SRC     := $(BUILD)/bbsrc/busybox-1.38.0
BB_BIN     := $(BB_SRC)/busybox
# Network applets (ifconfig/ping/wget/nc/route/udhcpc/hostname/getent) are
# turned on in $(BB_SRC)/.config and copied into /usr/bin below, so they show
# up under their own names and /bin/busybox.elf still works as the multicall
# dispatcher.  ping needs a raw socket and runs as root here, so it works
# without any setuid shimming.
BB_APPLETS := cat cp echo false head ls mkdir mv pwd rm true uname wc sh ash \
              ifconfig ping wget nc route hostname \
              env expr sleep test sort tr sed grep dirname basename \
              mktemp seq stat chmod ln readlink find date id kill ps printf

# GNU Bash — the shell this whole userland effort is aimed at.
#
# Configured by hand in the tree (it takes seconds; the recipe below only
# relinks) with:
#
#   ./configure CC=<musl-gcc> CFLAGS="-O2 -g -static -no-pie -fno-pie" \
#               LDFLAGS="-static -no-pie" --without-bash-malloc \
#               --disable-nls --enable-readline bash_cv_termcap_lib=gnutermcap
#
# Each of those matters.  -static because there is no dynamic loader and
# -no-pie because loader.c only accepts ET_EXEC.  --without-bash-malloc
# drops bash's own sbrk-based allocator in favour of musl's, which is the
# one this kernel's brk/mmap behaviour has actually been tested against.
# bash_cv_termcap_lib=gnutermcap picks the termcap bundled in lib/termcap
# rather than the host's ncurses, which musl-gcc cannot link against.
#
# Note there is no --host: musl-gcc produces binaries that run natively on
# the build machine, so configure's AC_TRY_RUN probes execute for real
# instead of falling back to cross-compilation guesses.
BASH_SRC := $(BUILD)/bashsrc/bash-5.3
BASH_BIN := $(BASH_SRC)/bash

# curl — the network client, built against musl and statically linked.
# Fetched into build/curlsrc and configured by hand with every optional
# dependency disabled (no TLS: the kernel has no crypto, and GNOS's network
# is HTTP to the QEMU slirp gateway).  Two things matter at link time:
#   - `-static` in LDFLAGS is swallowed by libtool as one of its own mode
#     flags (the same trap binutils fell into), so the binary comes out
#     dynamically linked against libc and dies on this kernel (no ld-musl).
#     CURL_LDFLAGS_BIN="-all-static" is what actually makes it static.
#   - the kernel's gcc 12.3 ICEs in libcpp on some inputs, so musl-gcc is
#     always pointed at gcc-13 via REALGCC (see dynhello).
CURL_BIN := $(BUILD)/curlsrc/curl-8.9.1/src/curl

# mbedtls 2.28.9 -- the TLS library curl links against, vendored in-tree
# (src/vendor/mbedtls, Apache-2.0).  It is compiled against
# musl's headers exactly like the musl programs, with the GNOS
# configuration: include/mbedtls/mbedtls_config.h *is* config-gnos.h (a
# client-only TLS 1.2 with ECDHE + AES-GCM + SHA-256 suites, entropy fed
# by the kernel's getrandom through mbedtls_hardware_poll).  mbedtls's own
# library Makefile builds the three split archives curl's configure probes
# for (libmbedtls/x509/crypto.a), and the result is copied into an
# installed prefix (include/ + lib/) curl's configure understands.
MBEDTLS_SRC    := src/vendor/mbedtls
MBEDTLS_INC    := $(MBEDTLS_SRC)/include
MBEDTLS_PREFIX := $(BUILD)/mbedtls-inst
MBEDTLS_LIBS   := $(MBEDTLS_PREFIX)/lib/libmbedtls.a \
                  $(MBEDTLS_PREFIX)/lib/libmbedx509.a \
                  $(MBEDTLS_PREFIX)/lib/libmbedcrypto.a
# Delayed expansion (not :=): MUSLCFLAGS is defined further down; with := it
# would expand empty here and mbedtls would build against glibc headers, whose
# -D_FILE_OFFSET_BITS=64 turns fopen into fopen64 -- a symbol musl lacks.
MBEDTLS_CFLAGS = $(MUSLCFLAGS) -I$(MBEDTLS_SRC) -I$(MBEDTLS_INC)

$(MBEDTLS_LIBS): $(MBEDTLS_SRC)/library/Makefile $(wildcard $(MBEDTLS_SRC)/library/*.c) $(MBEDTLS_INC)/mbedtls/mbedtls_config.h $(MBEDTLS_INC)/mbedtls/config.h
	mkdir -p $(MBEDTLS_PREFIX)/lib
	$(MAKE) -C $(MBEDTLS_SRC)/library clean
	$(MAKE) -C $(MBEDTLS_SRC)/library -j"$(nproc)" \
	  CC=$(CC) AR=ar CFLAGS="$(MBEDTLS_CFLAGS) -O2 -g" \
	  libmbedtls.a libmbedx509.a libmbedcrypto.a
	cp -r $(MBEDTLS_INC) $(MBEDTLS_PREFIX)/
	cp $(MBEDTLS_SRC)/library/libmbedtls.a \
	   $(MBEDTLS_SRC)/library/libmbedx509.a \
	   $(MBEDTLS_SRC)/library/libmbedcrypto.a $(MBEDTLS_PREFIX)/lib/

# GNU nano — the editor, against a wide-char ncurses built by hand into
# build/nanosrc/ncstage (also musl, also static).  ncurses 6.4 needs its
# generated lib_gen.c rebuilt with gcc-13's cpp (the gcc 12 ICE strikes the
# MKlib_gen.sh pipeline too), and the initrd carries the xterm + linux
# terminfo entries it was installed with so TERM=xterm nano has a terminal
# description to talk to.
NANO_SRC := $(BUILD)/nanosrc
NANO_BIN := $(NANO_SRC)/nano-7.2/src/nano
NC_STAGE := $(NANO_SRC)/ncstage

# The desktop stack (wayland/wlroots 0.19/labwc 0.9 + friends), cross-built
# by hand into build/desk/stage with musl-gcc (see build/desk/env.sh).  The
# initrd's labwc section copies the compositor, xkb data and config out of
# this stage.
DESK_STAGE := $(BUILD)/desk/stage

# GNU coreutils 9.9 — the other half of what makes a shell prompt feel like a
# system.  Fetched into build/ccsrc and configured by hand with:
#
#   ./configure --host=x86_64-linux-musl --prefix=/usr --disable-nls \
#       --disable-libcap --without-selinux \
#       --enable-no-install-program=stdbuf HELP2MAN=: MAKEINFO=: \
#       CC=<musl-gcc> CFLAGS="-O2 -g -static -no-pie -fno-pie -isystem <stub>"
#
# Three of those are not obvious:
#
#   --enable-no-install-program=stdbuf drops libstdbuf.so.  It is the only
#   shared object coreutils builds, and musl-gcc's static-only setup cannot
#   produce one (`relocation R_X86_64_32 can not be used when making a shared
#   object`).  stdbuf is useless here anyway: it works by LD_PRELOAD.
#
#   HELP2MAN=: MAKEINFO=: because the man pages are generated by *running* each
#   freshly built binary with --help, and the texinfo manual needs makeinfo.
#   Neither ships in the image, so both are stubbed out to `true`.
#
#   -isystem $(CC_STUB) puts a fake <linux/version.h> reporting kernel 0.0.0 on
#   the include path.  gnulib probes it to decide whether copy_file_range(2),
#   renameat2(2) and friends are worth calling; claiming to be older than all
#   of them makes coreutils take its portable fallback paths, which is exactly
#   what this kernel implements.
#
# gnulib-tests is excluded from the build (SUBDIRS below) rather than fixed:
# it wants <linux/fs.h>, the test suite is not installed into the image, and
# building it would double the compile for nothing.
CC_SRC  := $(BUILD)/ccsrc/coreutils-9.9
CC_STUB := $(BUILD)/ccsrc/linux-stub/include
CC_BIN  := $(CC_SRC)/src/ls

# GNU binutils 2.47 — assembler, linker and the ELF inspection tools, so the
# guest can build and dissect its own binaries.  Fetched by hand into
# build/busrcc and configured with:
#
#   ./configure --host=x86_64-linux-musl CC=<musl-gcc> AR=gcc-ar \
#               RANLIB=gcc-ranlib CFLAGS="-O2 -static -no-pie -fno-pie" \
#               LDFLAGS="-static" --disable-nls --disable-werror \
#               --disable-gdb --disable-gprofng --without-debuginfod
#
# AR/RANLIB are gcc-ar/gcc-ranlib rather than bare ar because 2.47's
# configure probe for the libsframe archiver interface trips on plain ar.
#
# Two more hand edits live in the tree and must be re-applied after any
# re-configure:
#   - the libtool script in each subdirectory swallows `-static` as one of
#     its own mode flags and never passes it to the compiler driver, so the
#     tools come out dynamically linked against libc and die instantly on
#     this kernel (no ld-musl).  The `-static | -static-libtool-libs)`
#     branch in func_mode_link is patched to append " -static" to the
#     compile/finalize commands instead of `continue`.
#   - `disable-shared` in configure makes the bfd/opcodes/ctf libraries
#     static archives, otherwise libtool links the executables against
#     uninstalled .so files.
BU_SRC := $(BUILD)/busrcc/binutils-2.47

# musl programs see musl's headers and nothing else.  Two things matter here:
#   -nostdinc stays (BASEFLAGS already has it) so glibc's /usr/include cannot
#   leak in, and every kernel header directory is dropped because they shadow
#   musl's <stdint.h>, <stddef.h> and <syscall.h> -- plain -I beats -isystem.
# Two directories survive on purpose: the kernel/userland ABI headers
# (sysnum.h, bootinfo.h) and the arch backend headers (gdt.h, idt.h, lapic.h).
# Neither collides with anything musl ships.
#
# The list is derived from KINCS rather than spelled out, so it keeps working
# when directories move: MUSL_DROP is "every kernel include dir except the two
# that are allowed to survive".
#
# The SSE bans in BASEFLAGS come off too.  They exist so the *kernel* never
# touches xmm registers (it does not save its own FPU state across interrupts),
# but libc.a is compiled with SSE enabled: a caller built with -mno-sse hands
# doubles over under a different convention than printf expects and the value
# silently reads back as zero.  User mode is safe here -- vmm.c sets
# CR4.OSFXSR and proc.c fxsaves/fxrstors per process.
MUSL_KEEP := -I$(SHARED_DIR) $(addprefix -I,$(ACTIVE_ARCH))
MUSL_DROP := $(filter-out $(MUSL_KEEP),$(KINCS))
MUSLCFLAGS := $(filter-out $(MUSL_DROP) \
                           -mgeneral-regs-only \
                           -mno-sse -mno-sse2 -mno-mmx -mno-80387,$(BASEFLAGS)) \
              -isystem $(abspath $(MUSL_INC)) -I$(USER_DIR) -fno-pie -fno-pic

# Hardware handed to the guest beyond the PC platform minimum.  The e1000 is
# the NIC src/kernel/e1000.c drives; the AC97 is the codec src/kernel/audio.c
# drives.  `audiodev none` gives the codec a backend that consumes samples in
# real time without needing a sound card on the host -- which is exactly what
# the headless self-test wants: the DMA engine really runs, nobody hears it.
# `-nic none` is not redundant: without it QEMU also creates its *default*
# NIC, the guest sees two e1000s, and the driver binds to whichever it met
# first -- which is not the one attached to our netdev.
#
# Both sound cards are plugged in at once on purpose: they are two completely
# different programming models (see hda.h) and the kernel drives both, so
# leaving one out would mean half the audio code never runs in `make test`.
QEMU_NET   := -nic none -netdev user,id=net0 -device e1000,netdev=net0
QEMU_AUDIO := -audiodev none,id=snd0 \
              -device AC97,audiodev=snd0 \
              -device intel-hda -device hda-duplex,audiodev=snd0

# A hard disk for the guest to install onto.  `-cdrom` already occupies the
# secondary master (that is where the boot ISO is, and why ata.c has to detect
# and skip ATAPI devices), so this goes on the primary channel and comes up as
# /dev/sda.  `if=ide` and not virtio deliberately: the point of src/kernel/ata.c
# is to drive the 1986 interface every PC still emulates, so the guest sees a
# disk that needs no driver it does not already have.
#
# The image is sparse -- 256 MiB of address space costs a few kilobytes on the
# host until something writes to it -- and is *not* removed by `clean`: once
# the installer has put a system on it, that system is the interesting artifact.
DISK    := $(BUILD)/disk.img
DISK_MB ?= 256
QEMU_DISK := -drive file=$(DISK),format=raw,if=ide,index=0,media=disk

# The initrd's explicit size.  mke2fs -d auto-sizes to the content, which is
# exactly what must not happen: the kernel mounts this read-write in RAM and
# every byte the filesystem grows at runtime comes out of this headroom.
# The pruned rootfs is ~760 MiB (the Xfce/WebKit/codec stack is gone), so
# 1 GiB leaves the machine ~260 MiB of runtime headroom while halving the
# CD load time -- Limine reads every byte of this through the BIOS path.
INITRD_MB ?= 1024

# Number of virtual cores QEMU exposes.  The SMP bring-up path brings up
# every core Limine reports, so changing this also changes what smpinfo.elf
# (run from /etc/rc) must assert -- keep the two in sync.
SMP_CPUS ?= 4
QEMU_SMP := -smp $(SMP_CPUS)

# RAM the VM gets.  The initrd is now a 256 MiB ext2 image (Xfce desktop
# binaries are ~18 MiB each), so 512M leaves too little for the kernel plus
# the running session and the guest dies with "Failed to allocate memory".
QEMU_MEM ?= 8G
QEMU_MEM_ARG := -m $(QEMU_MEM)

# The SVGA II card is what the drm_svga driver probes for (15ad:0405).  It
# sits alongside the default stdvga rather than replacing it, so the
# bootloader framebuffer fbcon draws into stays exactly where it was and the
# SVGA VRAM is genuinely separate memory.  Override to "" to boot without it
# and exercise the console-framebuffer scanout path instead.
QEMU_SVGA ?= -device vmware-svga

QEMU_DEVICES := $(QEMU_NET) $(QEMU_AUDIO) $(QEMU_DISK) $(QEMU_SMP) $(QEMU_SVGA)

# The same hardware, but with a backend you can actually hear.  Override on
# the command line if PulseAudio is not what your desktop runs, e.g.
#   make guistart AUDIO_BACKEND=pipewire
#   make guistart AUDIO_BACKEND=alsa
AUDIO_BACKEND ?= pa
GUI_AUDIO := -audiodev $(AUDIO_BACKEND),id=snd0 \
             -device AC97,audiodev=snd0 \
             -device intel-hda -device hda-duplex,audiodev=snd0

.PHONY: all run run-uefi guistart headless clean distclean autoinstall alpine alpine-base config menuconfig
all: $(ISO)

# ---------- gnoscfg: C++20 Kconfig configuration tool (host native) -----
# The gnoscfg binary is a *host* tool (runs on the build machine, not inside
# the guest) that parses src/Kconfig and drives a menuconfig TUI
# using ncursesw.  It produces build/.config which `make` sources to set
# KCFLAGS for the kernel build.
GNOSCFG_SRC  := src/scripts/kconfig
GNOSCFG_BIN  := $(BUILD)/gnoscfg
GNOSCFG_OBJS := $(BUILD)/gnoscfg_kconfig.o $(BUILD)/gnoscfg_menu.o \
                $(BUILD)/gnoscfg_main.o
NC_INC       := $(NC_STAGE)/include/ncursesw
NC_LIBS      := $(NC_STAGE)/lib/libncursesw.a $(NC_STAGE)/lib/libtinfow.a

$(BUILD)/gnoscfg_kconfig.o: $(GNOSCFG_SRC)/kconfig.cpp $(GNOSCFG_SRC)/kconfig.h | $(BUILD)
	g++-13 -std=c++20 -O2 -g -I$(NC_INC) -I$(GNOSCFG_SRC) -c -o $@ $<

$(BUILD)/gnoscfg_menu.o: $(GNOSCFG_SRC)/menu.cpp $(GNOSCFG_SRC)/kconfig.h | $(BUILD)
	g++-13 -std=c++20 -O2 -g -I$(NC_INC) -I$(GNOSCFG_SRC) -c -o $@ $<

$(BUILD)/gnoscfg_main.o: $(GNOSCFG_SRC)/main.cpp $(GNOSCFG_SRC)/kconfig.h | $(BUILD)
	g++-13 -std=c++20 -O2 -g -I$(NC_INC) -I$(GNOSCFG_SRC) -c -o $@ $<

$(GNOSCFG_BIN): $(GNOSCFG_OBJS) $(NC_LIBS) | $(BUILD)
	g++-13 -std=c++20 -static -no-pie -L$(NC_STAGE)/lib -o $@ $(GNOSCFG_OBJS) \
	  $(NC_LIBS) -ltinfow -lm

config menuconfig: $(GNOSCFG_BIN) src/Kconfig
	$(GNOSCFG_BIN)
# ---------- Alpine software autoinstall -----------------------------------------
# This is where the userland comes from by default (see USERLAND at the top).
# `make autoinstall ALPINE_PKGS="htop curl"` pulls the named Alpine musl
# packages (and their dependencies) off dl-cdn.alpinelinux.org and unpacks
# them into $(ALPINE_ROOT); the initrd rule below folds that directory into
# the image whenever it exists, so a plain `make` afterwards ships them.
# See tools/install-alpine.sh for what is and is not done to each .apk.
ALPINE_ROOT := $(BUILD)/alpine-root

# The default set: everything that used to arrive from a hand-fetched source
# tree, plus the boot-time X session.  Only top-level names are listed --
# install-alpine.sh resolves each package's `depends` against the index.
#   musl          /lib/ld-musl-x86_64.so.1 and libc.musl-x86_64.so.1
#   musl-dev      headers + crt1.o/crti.o/crtn.o/libc.a that src/usr links
#   busybox       the 304 applet links (grep/sed/awk/hostname/ifconfig/...)
#   busybox-binsh /bin/sh
#   bash          the login shell named in /etc/passwd
#   coreutils, binutils   dd/ls/cat/head ... and ar/ld/objdump/strip
#   curl/nano/python3/fastfetch/openrc/ncurses-terminfo
#                 what the *src trees used to ship; openrc is what /etc/rc
#                 brings up at boot, terminfo is what xterm/TERM=xterm asks for
#   X set         xorg-server, its evdev input driver, twm, xterm, xeyes,
#                 xsetroot and mkfontscale (fonts.dir), the session /usr/rc runs
ALPINE_PKGS ?= musl musl-dev busybox busybox-binsh bash coreutils binutils \
               curl nano python3 fastfetch openrc ncurses-terminfo \
               xorg-server xf86-input-evdev xkbcomp xkeyboard-config \
               twm xterm xeyes xsetroot mkfontscale

# The X clients and fastfetch live in community rather than main.  Override on
# the command line (`make ALPINE_REPOS=main ...`) to pin it back to main only.
ALPINE_REPOS ?= main community
export ALPINE_REPOS

# The staged package set only changes when $(ALPINE_PKGS) does.  Rewrite this
# list only on a real change so its mtime stays put: otherwise every make would
# re-resolve the repository.  A fresh `build/` has neither this file nor
# build/alpine-root, and runs the installer exactly once.
ALPINE_LIST := $(BUILD)/.alpine-packages

$(ALPINE_LIST): FORCE | $(BUILD)
	@printf '%s\n' $(ALPINE_PKGS) > $@.tmp
	@cmp -s $@.tmp $@ 2>/dev/null && rm -f $@.tmp || mv -f $@.tmp $@

# The install record.  $(ALPINE_LIST) cannot carry the trigger itself: tar
# preserves each member's archive mtime (libc.a still dates from 2024), so a
# package-list prerequisite would always be "newer" and the installer would
# re-run on every make.  A stamp touched after the run has a stable, current
# mtime, and only a real change to the list or the script moves it.
ALPINE_STAMP := $(BUILD)/.alpine-install

$(ALPINE_STAMP): $(ALPINE_LIST) tools/install-alpine.sh | $(BUILD)
	tools/install-alpine.sh $(ALPINE_PKGS)
	@touch $@

# `make autoinstall` always re-resolves (a new upstream release should show up
# without editing anything), and refreshes the stamp so a following plain
# `make` does not install a second time.
autoinstall: $(ALPINE_LIST)
	tools/install-alpine.sh $(ALPINE_PKGS)
	@touch $(ALPINE_STAMP)

alpine: autoinstall

# The initrd depends on the staging directory so `make autoinstall` followed
# by a plain `make` re-folds the staged packages into the image; a fresh
# checkout without autoinstall still gets an (empty) directory to depend on.
$(ALPINE_ROOT):
	mkdir -p $(ALPINE_ROOT)

# ---- Alpine minirootfs base system (like Unixed-Kernel) -------------------
# Downloads Alpine minirootfs, installs a curated package set via apk in a
# bwrap/chroot sandbox, and populates build/alpine-rootfs/.  The initrd rule
# merges this tree on top of the FHS skeleton, giving us a real Alpine
# userland (openrc, udev, dbus, etc.) inside the GNOS kernel image.
ALPINE_ROOTFS := $(BUILD)/alpine-rootfs

alpine-base:
	tools/build-alpine-rootfs.sh

# Same producer skill as alpine-base, but as a real file target so the
# initrd rule can order itself after the tree exists.
$(ALPINE_ROOTFS):
	tools/build-alpine-rootfs.sh

# ---- hand-built third-party trees: USERLAND=src only -----------------------
# Under the default USERLAND=alpine none of this is required, and none of it
# may even be *visible*: every rule below lists $(MUSL_GCC), which Alpine does
# not provide (there is no musl-gcc package), so an exposed rule would leave
# `make` failing on "No rule to make target .../musl-gcc".  The variable
# definitions are hoisted above the conditional so both modes spell the same
# names -- only the rules are conditional.
FF_SRC  := $(BUILD)/ffsrc
FF_BIN  := $(BUILD)/ffbuild/fastfetch
FFLASH  := $(BUILD)/ffbuild/flashfetch
PY_VER  := 3.12.10
PY_SRC  := $(BUILD)/pysrc/Python-$(PY_VER)
PY_BIN  := $(PY_SRC)/python

ifeq ($(USERLAND),src)

# musl's headers only become usable after `make install` assembles them into a
# sysroot: bits/alltypes.h is generated by configure and lives in obj/include,
# so the raw source tree's include/ directory is incomplete on its own.  This
# rule has to sit below `all` -- make takes the first target in the file as the
# default goal.
$(MUSL_LIB)/libc.a $(MUSL_GCC):
	$(MAKE) -C $(MUSL_SRC) install

# BusyBox builds itself; this rule only fires when the binary is missing, so
# reconfiguring the tree by hand (make menuconfig in $(BB_SRC)) still works.
# AR=gcc-ar is not cosmetic: binutils 2.41's plain `ar` segfaults while
# auto-loading the LTO plugin on this host, and BusyBox archives every
# subdirectory into a .a before the final link.
#
# CONFIG_STATIC must be y in $(BB_SRC)/.config or the result is a dynamic
# PIE with an ld-musl interpreter -- which the kernel's loader cannot run
# (ET_EXEC only, no dynamic linker).  -no-pie -fno-pie on top because the
# host gcc defaults to PIE, and "-static -pie" is still an ET_DYN.
$(BB_BIN): $(BB_SRC)/.config | $(MUSL_GCC)
	$(MAKE) -C $(BB_SRC) CC=$(abspath $(MUSL_GCC)) HOSTCC=gcc AR=gcc-ar \
	  SKIP_STRIP=y CFLAGS_EXTRA="-fno-pie -no-pie" LDFLAGS_EXTRA="-static -no-pie"

# GNU Bash.  Like BusyBox, the tree is fetched and configured by hand (see
# the BASH_SRC comment above) and this rule only relinks it when the binary
# is missing, so a hand-run `make` inside the tree is never undone.
$(BASH_BIN): $(BASH_SRC)/Makefile | $(MUSL_GCC)
	$(MAKE) -C $(BASH_SRC)

# curl.  Configured by hand (see CURL_BIN above); this rule only relinks it
# when the binary is missing.  The link flags are forced on the command line
# because libtool eats -static from LDFLAGS (see the CURL_BIN comment).
$(CURL_BIN): $(BUILD)/curlsrc/curl-8.9.1/Makefile $(MBEDTLS_LIBS) | $(MUSL_GCC)
	REALGCC=gcc-13 $(MAKE) -C $(BUILD)/curlsrc/curl-8.9.1 CURL_LDFLAGS_BIN="-all-static"

# GNU nano against the ncursesw build in build/nanosrc/ncstage.  Same
# hand-configured-tree pattern; CFLAGS is forced on the command line so the
# stage's headers (and the linux uapi copy that musl's sys/vt.h needs) are
# always on the include path.
$(NANO_BIN): $(NANO_SRC)/nano-7.2/Makefile | $(MUSL_GCC)
	REALGCC=gcc-13 $(MAKE) -C $(NANO_SRC)/nano-7.2 CFLAGS="-O2 -g -static -fno-pie \
	  -isystem $(BUILD)/desk/stage/include"

# GNU coreutils.  Same deal: the tree is configured by hand (see CC_SRC above)
# and this only rebuilds when the binaries are gone.  SUBDIRS is overridden to
# skip gnulib-tests; "po ." is the pair that actually produces src/*.
$(CC_BIN): $(CC_SRC)/Makefile | $(MUSL_GCC)
	$(MAKE) -C $(CC_SRC) SUBDIRS="po ." HELP2MAN=: MAKEINFO=:

# GNU binutils.  Configured by hand (see BU_SRC above); this only relinks it
# when the tool binaries are gone.
$(BU_SRC)/binutils/readelf: $(BU_SRC)/Makefile | $(MUSL_GCC)
	$(MAKE) -C $(BU_SRC)

# fastfetch — the system-info tool GNOS exists to run.  Fetched into
# build/ffsrc (https://github.com/fastfetch-cli/fastfetch.git) and built by
# tools/build-fastfetch.sh with the locally built clang 24 (fastfetch needs
# C23, host gcc 12 cannot do it) against musl, statically, with every
# optional dependency disabled.  See that script for the full flag list.
#
# This rule must live below `all`: make takes the first target in the file
# as its default goal.
$(FF_BIN): $(FF_SRC)/CMakeLists.txt | $(MUSL_GCC)
	chmod +x tools/build-fastfetch.sh
	tools/build-fastfetch.sh

# CPython 3.12 — the musl interpreter.  Fetched into build/pysrc and
# configured by hand for x86_64-unknown-linux-musl (see build/pysrc/
# Python-3.12.10/README notes): cross-compiled with musl-gcc, shared
# extension modules, --disable-ipv6/ensurepip, readline/nis/_ctypes &
# friends trimmed (their dev headers do not exist in the musl sysroot).
# The result is dynamically linked against /lib/ld-musl-x86_64.so.1, which
# the initrd already ships, so this rule only relinks when the binary is
# missing, exactly like the bash/coreutils rules above.
$(PY_BIN): $(PY_SRC)/Makefile | $(MUSL_GCC)
	REALGCC=gcc-13 $(MAKE) -C $(PY_SRC) -j4

else  # USERLAND=alpine

# Alpine stages the sysroot.  These four files are what src/usr compiles and
# links against.  The rule has no normal prerequisite, so it fires only when
# its file is genuinely absent (a wiped build/alpine-root): tar members keep
# their archive mtimes, which are always older than anything this Makefile
# writes, and a package-list prerequisite would therefore re-run the installer
# on every single make.  A changed package list reaches the install through
# the order-only $(ALPINE_STAMP) instead, which does the installing first --
# by the time make checks these targets they exist, and the recipe is skipped.
# The fallback below only matters when the stamp is up to date but
# build/alpine-root has been deleted by hand.
ifeq ($(USERLAND),alpine)
$(MUSL_LIB)/libc.a $(MUSL_CRT)/crt1.o $(MUSL_CRT)/crti.o $(MUSL_CRT)/crtn.o: | $(ALPINE_STAMP)
	@[ -f $@ ] || tools/install-alpine.sh $(ALPINE_PKGS)
	@test -f $@ || { echo "install-alpine.sh did not stage $@" >&2; exit 1; }
else ifeq ($(USERLAND),minirootfs)
# Alpine's musl-dev already sits in the minirootfs tree; it is produced by
# tools/build-alpine-rootfs.sh, not install-alpine.sh, so there is nothing
# to fall back to here -- only a clear error naming the producer.
$(MUSL_LIB)/libc.a $(MUSL_CRT)/crt1.o $(MUSL_CRT)/crti.o $(MUSL_CRT)/crtn.o:
	@test -f $@ || { echo "$@ missing: run 'make alpine-base'" >&2; exit 1; }
endif

endif  # USERLAND

# Both directories are listed separately: `clean` leaves $(BUILD) standing (the
# third-party trees live there), so a rule keyed only on $(BUILD) would never
# fire again and $(BUILD)/user would stay missing.
$(BUILD) $(BUILD)/user $(BUILD)/modules:
	mkdir -p $@

# ---------- header dependencies ----------
# Without this every .o depends only on its .c, so editing a header rebuilds
# nothing that includes it.  That is not merely a stale-build annoyance here:
# proc.h defines proc_t, and half the kernel indexes into it, so a field added
# to that struct and recompiled into only one object gives two translation
# units two different memory layouts.  The result is a kernel that links
# cleanly and then corrupts user processes -- which is exactly how the last
# `-MMD`-less afternoon was spent.  -MP adds a phony target per header so a
# deleted or renamed header does not wedge make with "no rule to make target".
DEPFLAGS = -MMD -MP
DEPS := $(KOBJS:.o=.d) $(UOBJS:.o=.d) $(MUSL_OBJS:.o=.d) $(UCRT:.o=.d) \
        $(addprefix $(BUILD)/user/,$(addsuffix .d,$(UPROGS)))
-include $(DEPS)

# ---------- kernel (Limine entry point) ----------
# Sources are spread over the Linux-style subsystem trees (arch/ drivers/ fs/
# kernel/ mm/ net/ sound/ ...) while every .o lands flat in $(BUILD).  vpath
# lets the %.o rules below find a source by bare name no matter which directory
# it is in, which is why the directory list is derived from the tree itself.
# ARCH selects the backend tree; only one is searched, so an i386 build never
# sees x86 sources.
#
# Excluded on purpose: the userland (its init.c would otherwise compete with
# init/init.c for the bare name), the bootloader, the config tool and the
# sample modules, all of which are built by their own explicit rules.  init/
# itself stays in: kernel_entry (kernel.c) and the Limine request block live
# there and are ordinary kernel objects.
VPATH_SRC := $(filter-out src/usr/% src/bootloader/% \
                          src/scripts/kconfig/% src/samples/modules/%,$(KERNEL_DIRS))
vpath %.c   $(VPATH_SRC)
vpath %.asm $(VPATH_SRC)
vpath %.S   $(VPATH_SRC)

# The flat include path only works while header basenames are unique across the
# tree: two drv.h in different directories would let the include order decide
# which one wins, silently.
.PHONY: check-hdrs
check-hdrs:
	@dup=$$(find src $(PRUNE) -name '*.h' -printf '%f\n' | sort | uniq -d); \
	if [ -n "$$dup" ]; then echo "FATAL: duplicate header basename(s):"; \
	  echo "$$dup"; exit 1; fi; \
	echo "check-hdrs: header basenames are unique"


# The ACPICA OSL needs its own flags (see ACPICA_CFLAGS above).
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<

	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<

# FatFs core: vendored (pruned from the auto vpath), so it needs an explicit
# rule.  The port layer beside it (src/fs/fatfs) is ordinary kernel code.
$(BUILD)/ff.o: src/vendor/fatfs/source/ff.c | $(BUILD)
	$(CC) $(KCFLAGS) $(DEPFLAGS) -c -o $@ $<

# ACPICA core: vendored (pruned from vpath), one explicit rule per file.
$(BUILD)/acpica_dispatcher_dsargs.o: src/vendor/acpica/source/components/dispatcher/dsargs.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_dispatcher_dscontrol.o: src/vendor/acpica/source/components/dispatcher/dscontrol.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_dispatcher_dsdebug.o: src/vendor/acpica/source/components/dispatcher/dsdebug.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_dispatcher_dsfield.o: src/vendor/acpica/source/components/dispatcher/dsfield.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_dispatcher_dsinit.o: src/vendor/acpica/source/components/dispatcher/dsinit.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_dispatcher_dsmethod.o: src/vendor/acpica/source/components/dispatcher/dsmethod.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_dispatcher_dsmthdat.o: src/vendor/acpica/source/components/dispatcher/dsmthdat.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_dispatcher_dsobject.o: src/vendor/acpica/source/components/dispatcher/dsobject.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_dispatcher_dsopcode.o: src/vendor/acpica/source/components/dispatcher/dsopcode.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_dispatcher_dspkginit.o: src/vendor/acpica/source/components/dispatcher/dspkginit.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_dispatcher_dsutils.o: src/vendor/acpica/source/components/dispatcher/dsutils.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_dispatcher_dswexec.o: src/vendor/acpica/source/components/dispatcher/dswexec.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_dispatcher_dswload.o: src/vendor/acpica/source/components/dispatcher/dswload.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_dispatcher_dswload2.o: src/vendor/acpica/source/components/dispatcher/dswload2.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_dispatcher_dswscope.o: src/vendor/acpica/source/components/dispatcher/dswscope.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_dispatcher_dswstate.o: src/vendor/acpica/source/components/dispatcher/dswstate.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_events_evevent.o: src/vendor/acpica/source/components/events/evevent.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_events_evglock.o: src/vendor/acpica/source/components/events/evglock.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_events_evgpe.o: src/vendor/acpica/source/components/events/evgpe.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_events_evgpeblk.o: src/vendor/acpica/source/components/events/evgpeblk.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_events_evgpeinit.o: src/vendor/acpica/source/components/events/evgpeinit.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_events_evgpeutil.o: src/vendor/acpica/source/components/events/evgpeutil.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_events_evhandler.o: src/vendor/acpica/source/components/events/evhandler.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_events_evmisc.o: src/vendor/acpica/source/components/events/evmisc.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_events_evregion.o: src/vendor/acpica/source/components/events/evregion.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_events_evrgnini.o: src/vendor/acpica/source/components/events/evrgnini.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_events_evsci.o: src/vendor/acpica/source/components/events/evsci.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_events_evxface.o: src/vendor/acpica/source/components/events/evxface.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_events_evxfevnt.o: src/vendor/acpica/source/components/events/evxfevnt.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_events_evxfgpe.o: src/vendor/acpica/source/components/events/evxfgpe.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_events_evxfregn.o: src/vendor/acpica/source/components/events/evxfregn.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_executer_exconcat.o: src/vendor/acpica/source/components/executer/exconcat.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_executer_exconfig.o: src/vendor/acpica/source/components/executer/exconfig.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_executer_exconvrt.o: src/vendor/acpica/source/components/executer/exconvrt.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_executer_excreate.o: src/vendor/acpica/source/components/executer/excreate.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_executer_exdebug.o: src/vendor/acpica/source/components/executer/exdebug.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_executer_exdump.o: src/vendor/acpica/source/components/executer/exdump.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_executer_exfield.o: src/vendor/acpica/source/components/executer/exfield.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_executer_exfldio.o: src/vendor/acpica/source/components/executer/exfldio.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_executer_exmisc.o: src/vendor/acpica/source/components/executer/exmisc.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_executer_exmutex.o: src/vendor/acpica/source/components/executer/exmutex.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_executer_exnames.o: src/vendor/acpica/source/components/executer/exnames.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_executer_exoparg1.o: src/vendor/acpica/source/components/executer/exoparg1.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_executer_exoparg2.o: src/vendor/acpica/source/components/executer/exoparg2.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_executer_exoparg3.o: src/vendor/acpica/source/components/executer/exoparg3.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_executer_exoparg6.o: src/vendor/acpica/source/components/executer/exoparg6.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_executer_exprep.o: src/vendor/acpica/source/components/executer/exprep.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_executer_exregion.o: src/vendor/acpica/source/components/executer/exregion.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_executer_exresnte.o: src/vendor/acpica/source/components/executer/exresnte.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_executer_exresolv.o: src/vendor/acpica/source/components/executer/exresolv.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_executer_exresop.o: src/vendor/acpica/source/components/executer/exresop.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_executer_exserial.o: src/vendor/acpica/source/components/executer/exserial.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_executer_exstore.o: src/vendor/acpica/source/components/executer/exstore.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_executer_exstoren.o: src/vendor/acpica/source/components/executer/exstoren.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_executer_exstorob.o: src/vendor/acpica/source/components/executer/exstorob.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_executer_exsystem.o: src/vendor/acpica/source/components/executer/exsystem.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_executer_extrace.o: src/vendor/acpica/source/components/executer/extrace.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_executer_exutils.o: src/vendor/acpica/source/components/executer/exutils.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_hardware_hwacpi.o: src/vendor/acpica/source/components/hardware/hwacpi.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_hardware_hwesleep.o: src/vendor/acpica/source/components/hardware/hwesleep.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_hardware_hwgpe.o: src/vendor/acpica/source/components/hardware/hwgpe.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_hardware_hwpci.o: src/vendor/acpica/source/components/hardware/hwpci.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_hardware_hwregs.o: src/vendor/acpica/source/components/hardware/hwregs.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_hardware_hwsleep.o: src/vendor/acpica/source/components/hardware/hwsleep.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_hardware_hwtimer.o: src/vendor/acpica/source/components/hardware/hwtimer.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_hardware_hwvalid.o: src/vendor/acpica/source/components/hardware/hwvalid.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_hardware_hwxface.o: src/vendor/acpica/source/components/hardware/hwxface.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_hardware_hwxfsleep.o: src/vendor/acpica/source/components/hardware/hwxfsleep.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_namespace_nsaccess.o: src/vendor/acpica/source/components/namespace/nsaccess.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_namespace_nsalloc.o: src/vendor/acpica/source/components/namespace/nsalloc.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_namespace_nsarguments.o: src/vendor/acpica/source/components/namespace/nsarguments.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_namespace_nsconvert.o: src/vendor/acpica/source/components/namespace/nsconvert.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_namespace_nsdump.o: src/vendor/acpica/source/components/namespace/nsdump.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_namespace_nsdumpdv.o: src/vendor/acpica/source/components/namespace/nsdumpdv.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_namespace_nseval.o: src/vendor/acpica/source/components/namespace/nseval.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_namespace_nsinit.o: src/vendor/acpica/source/components/namespace/nsinit.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_namespace_nsload.o: src/vendor/acpica/source/components/namespace/nsload.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_namespace_nsnames.o: src/vendor/acpica/source/components/namespace/nsnames.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_namespace_nsobject.o: src/vendor/acpica/source/components/namespace/nsobject.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_namespace_nsparse.o: src/vendor/acpica/source/components/namespace/nsparse.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_namespace_nspredef.o: src/vendor/acpica/source/components/namespace/nspredef.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_namespace_nsprepkg.o: src/vendor/acpica/source/components/namespace/nsprepkg.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_namespace_nsrepair.o: src/vendor/acpica/source/components/namespace/nsrepair.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_namespace_nsrepair2.o: src/vendor/acpica/source/components/namespace/nsrepair2.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_namespace_nssearch.o: src/vendor/acpica/source/components/namespace/nssearch.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_namespace_nsutils.o: src/vendor/acpica/source/components/namespace/nsutils.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_namespace_nswalk.o: src/vendor/acpica/source/components/namespace/nswalk.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_namespace_nsxfeval.o: src/vendor/acpica/source/components/namespace/nsxfeval.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_namespace_nsxfname.o: src/vendor/acpica/source/components/namespace/nsxfname.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_namespace_nsxfobj.o: src/vendor/acpica/source/components/namespace/nsxfobj.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_parser_psargs.o: src/vendor/acpica/source/components/parser/psargs.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_parser_psloop.o: src/vendor/acpica/source/components/parser/psloop.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_parser_psobject.o: src/vendor/acpica/source/components/parser/psobject.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_parser_psopcode.o: src/vendor/acpica/source/components/parser/psopcode.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_parser_psopinfo.o: src/vendor/acpica/source/components/parser/psopinfo.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_parser_psparse.o: src/vendor/acpica/source/components/parser/psparse.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_parser_psscope.o: src/vendor/acpica/source/components/parser/psscope.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_parser_pstree.o: src/vendor/acpica/source/components/parser/pstree.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_parser_psutils.o: src/vendor/acpica/source/components/parser/psutils.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_parser_pswalk.o: src/vendor/acpica/source/components/parser/pswalk.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_parser_psxface.o: src/vendor/acpica/source/components/parser/psxface.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_resources_rsaddr.o: src/vendor/acpica/source/components/resources/rsaddr.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_resources_rscalc.o: src/vendor/acpica/source/components/resources/rscalc.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_resources_rscreate.o: src/vendor/acpica/source/components/resources/rscreate.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_resources_rsdump.o: src/vendor/acpica/source/components/resources/rsdump.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_resources_rsdumpinfo.o: src/vendor/acpica/source/components/resources/rsdumpinfo.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_resources_rsinfo.o: src/vendor/acpica/source/components/resources/rsinfo.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_resources_rsio.o: src/vendor/acpica/source/components/resources/rsio.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_resources_rsirq.o: src/vendor/acpica/source/components/resources/rsirq.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_resources_rslist.o: src/vendor/acpica/source/components/resources/rslist.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_resources_rsmemory.o: src/vendor/acpica/source/components/resources/rsmemory.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_resources_rsmisc.o: src/vendor/acpica/source/components/resources/rsmisc.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_resources_rsserial.o: src/vendor/acpica/source/components/resources/rsserial.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_resources_rsutils.o: src/vendor/acpica/source/components/resources/rsutils.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_resources_rsxface.o: src/vendor/acpica/source/components/resources/rsxface.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_tables_tbdata.o: src/vendor/acpica/source/components/tables/tbdata.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_tables_tbfadt.o: src/vendor/acpica/source/components/tables/tbfadt.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_tables_tbfind.o: src/vendor/acpica/source/components/tables/tbfind.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_tables_tbinstal.o: src/vendor/acpica/source/components/tables/tbinstal.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_tables_tbprint.o: src/vendor/acpica/source/components/tables/tbprint.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_tables_tbutils.o: src/vendor/acpica/source/components/tables/tbutils.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_tables_tbxface.o: src/vendor/acpica/source/components/tables/tbxface.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_tables_tbxfload.o: src/vendor/acpica/source/components/tables/tbxfload.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_tables_tbxfroot.o: src/vendor/acpica/source/components/tables/tbxfroot.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_utilities_utaddress.o: src/vendor/acpica/source/components/utilities/utaddress.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_utilities_utalloc.o: src/vendor/acpica/source/components/utilities/utalloc.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_utilities_utascii.o: src/vendor/acpica/source/components/utilities/utascii.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_utilities_utbuffer.o: src/vendor/acpica/source/components/utilities/utbuffer.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_utilities_utcache.o: src/vendor/acpica/source/components/utilities/utcache.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_utilities_utcksum.o: src/vendor/acpica/source/components/utilities/utcksum.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_utilities_utclib.o: src/vendor/acpica/source/components/utilities/utclib.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_utilities_utcopy.o: src/vendor/acpica/source/components/utilities/utcopy.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_utilities_utdebug.o: src/vendor/acpica/source/components/utilities/utdebug.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_utilities_utdecode.o: src/vendor/acpica/source/components/utilities/utdecode.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_utilities_utdelete.o: src/vendor/acpica/source/components/utilities/utdelete.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_utilities_uterror.o: src/vendor/acpica/source/components/utilities/uterror.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_utilities_uteval.o: src/vendor/acpica/source/components/utilities/uteval.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_utilities_utexcep.o: src/vendor/acpica/source/components/utilities/utexcep.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_utilities_utglobal.o: src/vendor/acpica/source/components/utilities/utglobal.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_utilities_uthex.o: src/vendor/acpica/source/components/utilities/uthex.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_utilities_utids.o: src/vendor/acpica/source/components/utilities/utids.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_utilities_utinit.o: src/vendor/acpica/source/components/utilities/utinit.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_utilities_utlock.o: src/vendor/acpica/source/components/utilities/utlock.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_utilities_utmath.o: src/vendor/acpica/source/components/utilities/utmath.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_utilities_utmisc.o: src/vendor/acpica/source/components/utilities/utmisc.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_utilities_utmutex.o: src/vendor/acpica/source/components/utilities/utmutex.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_utilities_utnonansi.o: src/vendor/acpica/source/components/utilities/utnonansi.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_utilities_utobject.o: src/vendor/acpica/source/components/utilities/utobject.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_utilities_utosi.o: src/vendor/acpica/source/components/utilities/utosi.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_utilities_utownerid.o: src/vendor/acpica/source/components/utilities/utownerid.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_utilities_utpredef.o: src/vendor/acpica/source/components/utilities/utpredef.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_utilities_utprint.o: src/vendor/acpica/source/components/utilities/utprint.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_utilities_utresdecode.o: src/vendor/acpica/source/components/utilities/utresdecode.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_utilities_utresrc.o: src/vendor/acpica/source/components/utilities/utresrc.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_utilities_utstate.o: src/vendor/acpica/source/components/utilities/utstate.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_utilities_utstring.o: src/vendor/acpica/source/components/utilities/utstring.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_utilities_utstrsuppt.o: src/vendor/acpica/source/components/utilities/utstrsuppt.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_utilities_utstrtoul64.o: src/vendor/acpica/source/components/utilities/utstrtoul64.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_utilities_uttrack.o: src/vendor/acpica/source/components/utilities/uttrack.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_utilities_utuuid.o: src/vendor/acpica/source/components/utilities/utuuid.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_utilities_utxface.o: src/vendor/acpica/source/components/utilities/utxface.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_utilities_utxferror.o: src/vendor/acpica/source/components/utilities/utxferror.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_utilities_utxfinit.o: src/vendor/acpica/source/components/utilities/utxfinit.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<
$(BUILD)/acpica_utilities_utxfmutex.o: src/vendor/acpica/source/components/utilities/utxfmutex.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<

$(BUILD)/acpica_osl.o: src/drivers/acpi/acpica_osl.c | $(BUILD)
	$(CC) $(KCFLAGS) $(ACPICA_CFLAGS) $(DEPFLAGS) -c -o $@ $<

$(BUILD)/%.o: %.c | $(BUILD)
	$(CC) $(KCFLAGS) $(DEPFLAGS) -c -o $@ $<

$(BUILD)/%.o: %.asm | $(BUILD)
	$(AS) -f elf64 -o $@ $<

# GNU-as sources, for the one thing nasm cannot do here: .incbin of a file the
# C compiler must not see.  Run through the C driver so the preprocessor and
# the kernel's flags apply.
$(BUILD)/%.o: %.S | $(BUILD)
	$(CC) $(KCFLAGS) $(DEPFLAGS) -c -o $@ $<

# The font blob is checked in, so this is a dependency rather than a rule that
# fires: `make cjkfont` regenerates it deliberately, a normal build never does.
$(BUILD)/cjkfont_data.o: src/drivers/video/fbdev/cjkfont.bin

# The CJK bitmap is stapled into the kernel image by GNU-as (.incbin of a 1 MiB
# binary the C compiler must never see).  It is assembled into its own object,
# cjkfont_data.o; cjkfont.c compiles to cjkfont.o.  Keeping them separate is
# what stops the two competing for the same cjkfont.o target.
$(BUILD)/cjkfont_data.o: src/drivers/video/fbdev/cjkfont_data.S | $(BUILD)
	$(CC) $(KCFLAGS) $(DEPFLAGS) -c -o $@ $<

.PHONY: cjkfont
cjkfont:
	python3 tools/mkcjkfont.py

$(KRNL): $(KOBJS) linker.ld
	$(CC) $(KCFLAGS) -static-pie -Wl,-T,linker.ld -Wl,-z,max-page-size=0x1000 \
	  -Wl,--build-id=none -o $@ $(KOBJS)

# ---------- loadable kernel modules (.ko) ----------
# src/samples/modules/*.c -> build/modules/*.ko.  These are plain ET_REL
# objects (no -fpie, no -fpic): the loader maps and relocates them itself.
# The module toolchain is the kernel's minus the PIE codegen; -fno-pie /
# -fno-pic undo what BASEFLAGS -fpie would otherwise add.
KMSRC := $(wildcard src/samples/modules/*.c)
KMODS := $(patsubst src/samples/modules/%.c,$(BUILD)/modules/%.ko,$(KMSRC))

$(BUILD)/modules/%.o: src/samples/modules/%.c \
	  src/samples/modules/module_info.h src/kernel/module/module.h \
	  src/lib/kstring.h | $(BUILD)/modules
	$(CC) $(BASEFLAGS) -fno-pie -fno-pic -fno-stack-protector \
	  -fno-asynchronous-unwind-tables -fno-omit-frame-pointer \
	  $(DEPFLAGS) -c -o $@ $<

$(BUILD)/modules/%.ko: $(BUILD)/modules/%.o
	$(LD) -m elf_x86_64 -r --build-id=none -o $@ $<

# ---------- user programs ----------
$(BUILD)/user/%.o: src/usr/%.c | $(BUILD)/user
	$(CC) $(UCFLAGS) $(DEPFLAGS) -c -o $@ $<

# musl programs compile against musl's headers (see MUSLCFLAGS).  These are
# static pattern rules, which beat the generic ulib ones above for exactly the
# targets named in MUSL_OBJS / MUSL_ELFS and leave every other program alone.
$(MUSL_OBJS): $(BUILD)/user/%.o: src/usr/%.c $(MUSL_LIB)/libc.a | $(BUILD)/user
	$(CC) $(MUSLCFLAGS) $(DEPFLAGS) -c -o $@ $<

$(BUILD)/user/%.o: src/usr/%.asm | $(BUILD)/user
	$(AS) -f elf64 -o $@ $<

$(BUILD)/%.elf: $(BUILD)/user/%.o $(UCRT) src/usr/user.ld
	$(LD) -m elf_x86_64 -T src/usr/user.ld -z max-page-size=0x1000 \
	  --build-id=none -o $@ $(UCRT) $<

# musl-linked program: crt1.o, libc.a, and the .init/.fini glue from crti/crtn.
# Order matters: crt1.o references __libc_start_main (in libc.a, so it comes
# after), and the program object references printf (also in libc.a).  crtn.o
# closes the .init/.fini sections opened by crti.o, so it is last.
$(MUSL_ELFS): $(BUILD)/%.elf: $(BUILD)/user/%.o \
              $(MUSL_CRT)/crt1.o $(MUSL_CRT)/crti.o $(MUSL_CRT)/crtn.o \
              $(MUSL_LIB)/libc.a src/usr/user.ld
	$(LD) -m elf_x86_64 -T src/usr/user.ld -z max-page-size=0x1000 \
	  --build-id=none -static -nostdlib -o $@ \
	  $(MUSL_CRT)/crt1.o $(MUSL_CRT)/crti.o $< \
	  $(MUSL_LIB)/libc.a $(MUSL_CRT)/crtn.o

# dynhello — the one *dynamically linked* program on the system.  musl-gcc
# links it as a PIE (ET_DYN) with a PT_INTERP pointing at
# /lib/ld-musl-x86_64.so.1; the kernel loader maps the PIE, reads the
# interpreter path, loads the linker, and hands the entry point to it, the
# way a Linux loader would.  Everything else stays -static: this file exists
# to prove the ET_DYN + PT_INTERP path, not to be a production choice.
# dynhello exists only in USERLAND=src: its link goes through $(MUSL_GCC),
# which Alpine does not ship.  Alpine's own binaries are ET_DYN with the same
# PT_INTERP, so the path it proves is exercised by every package either way.
ifeq ($(USERLAND),src)
$(BUILD)/dynhello.elf: src/usr/dynhello.c $(MUSL_GCC)
	REALGCC=gcc-13 $(MUSL_GCC) -O2 -g -o $@ $<
	file $@
endif

# ---------- initrd (ext2 image holding the user programs) ----------
# mke2fs -d fills the image straight from a staging directory, so this needs
# neither a loopback mount nor root.  The feature set is trimmed on purpose:
# ^dir_index keeps every directory a plain linear list, which is all the
# kernel driver knows how to rewrite.
# USERLAND=alpine gets the userland from build/alpine-root -- and from
# $(ALPINE_STAMP), whose rule is what runs install-alpine.sh when the package
# list or the script changes (see the autoinstall block above).  The staging
# directory is listed too: it is created/refreshed by an install, so its mtime
# moves and the initrd re-folds the new tree into the image.  USERLAND=src
# still needs every hand-built tree listed below.
ifeq ($(USERLAND),alpine)
INITRD_ULDEPS := $(ALPINE_ROOT) $(ALPINE_STAMP)
else ifeq ($(USERLAND),minirootfs)
# The rootfs tree is the userland; depending on it re-folds a refreshed
# build/alpine-rootfs into the image.  Empty target list entry when the
# directory already exists (a directory with no prerequisites is always
# up to date); a missing one builds it via the rule below.
INITRD_ULDEPS := $(ALPINE_ROOTFS)
else
INITRD_ULDEPS := $(BB_BIN) $(BASH_BIN) $(CC_BIN) $(FF_BIN) $(CURL_BIN) \
                 $(NANO_BIN) $(PY_BIN) $(BUILD)/dynhello.elf
endif

$(INITRD): $(UELFS) $(MUSL_ELFS) $(KRNL) $(KMODS) $(INITRD_ULDEPS) \
           $(BUILD)/.kcmd src/etc/inittab | $(BUILD)
	rm -rf $(BUILD)/initrd-root
	mkdir -p $(BUILD)/initrd-root
	# ---- FHS skeleton (empty dirs are harmless placeholders for now) ----
	mkdir -p $(BUILD)/initrd-root/bin
	mkdir -p $(BUILD)/initrd-root/sbin
	mkdir -p $(BUILD)/initrd-root/etc
	mkdir -p $(BUILD)/initrd-root/dev
	mkdir -p $(BUILD)/initrd-root/proc
	mkdir -p $(BUILD)/initrd-root/debug
	mkdir -p $(BUILD)/initrd-root/sys
	mkdir -p $(BUILD)/initrd-root/tmp
	# var/run comes from Alpine as a symlink to /run; pre-creating it as a
	# real directory makes "cp -a" fail with "not a directory".
	# mkdir -p $(BUILD)/initrd-root/var/run
	mkdir -p $(BUILD)/initrd-root/usr/bin
	mkdir -p $(BUILD)/initrd-root/usr/sbin
	mkdir -p $(BUILD)/initrd-root/usr/lib
	mkdir -p $(BUILD)/initrd-root/lib
	mkdir -p $(BUILD)/initrd-root/lib/apk/db
	mkdir -p $(BUILD)/initrd-root/var/cache/apk
	mkdir -p $(BUILD)/initrd-root/etc/apk/keys
	# apk's state db must exist (an empty file): `apk update` opens it
	# read-only and reports ENOENT instead of treating it as fresh.
	# /etc/apk/world is the other half of the state: without it apk says
	# "Unable to read database state" even with the db present.
	touch $(BUILD)/initrd-root/lib/apk/db/installed
	touch $(BUILD)/initrd-root/lib/apk/db/lock
	touch $(BUILD)/initrd-root/etc/apk/world
	mkdir -p $(BUILD)/initrd-root/root
	mkdir -p $(BUILD)/initrd-root/home
	mkdir -p $(BUILD)/initrd-root/mnt
	mkdir -p $(BUILD)/initrd-root/run
	# ---- /sys: the static sysfs tree the DRM client tooling reads ---------
	# There is no sysfs backend, so the handful of nodes fastfetch's GPU
	# detection consults are laid down here, by hand, exactly as the Linux
	# sysfs would lay them out for a QEMU bochs VGA behind bochsdrm.  They
	# describe the real hardware: 1234:1111 stdvga, class 03/00, on
	# 0000:00:02.0 -- the device GNOS's drm.c actually drives.
	mkdir -p $(BUILD)/initrd-root/sys/class/drm/card0
	mkdir -p $(BUILD)/initrd-root/sys/class/drm/renderD128
	mkdir -p $(BUILD)/initrd-root/sys/devices/0000:00:02.0/drm/renderD128
	# /sys/dev/char/<maj>:<min> is where libdrm maps an fd back to its
	# /dev node: drmGetDeviceNameFromFd2() fstats the fd and reads
	# DEVNAME= out of /sys/dev/char/N/uevent.  drmNodeIsDRM() additionally
	# stats /sys/dev/char/N/device/drm, so the char node needs its 'device'
	# symlink and the PCI device needs a drm/ subdir, exactly as Linux lays
	# it out.  Without all three wlroots refuses the whole DRM backend.
	mkdir -p $(BUILD)/initrd-root/sys/dev/char/226:0
	mkdir -p $(BUILD)/initrd-root/sys/dev/char/226:128
	echo 'DEVNAME=dri/card0' > $(BUILD)/initrd-root/sys/dev/char/226:0/uevent
	echo 'DEVNAME=dri/renderD128' > $(BUILD)/initrd-root/sys/dev/char/226:128/uevent
	ln -sfn ../../../devices/0000:00:02.0 \
	  $(BUILD)/initrd-root/sys/dev/char/226:0/device
	ln -sfn ../../../devices/0000:00:02.0 \
	  $(BUILD)/initrd-root/sys/dev/char/226:128/device
	mkdir -p $(BUILD)/initrd-root/sys/devices/0000:00:02.0/drm/card0
	echo 'pci:v00001234d00001111sv00001AF4sd00001100bc03sc00' \
	  > $(BUILD)/initrd-root/sys/devices/0000:00:02.0/modalias
	ln -sfn ../../../bus/pci/drivers/bochsdrm \
	  $(BUILD)/initrd-root/sys/devices/0000:00:02.0/driver
	ln -sfn ../../../devices/0000:00:02.0 \
	  $(BUILD)/initrd-root/sys/class/drm/card0/device
	ln -sfn ../../../devices/0000:00:02.0 \
	  $(BUILD)/initrd-root/sys/class/drm/renderD128/device
	# ---- user programs live in /bin ----
	for p in $(UPROGS); do \
	  cp $(BUILD)/$$p.elf $(BUILD)/initrd-root/bin/$$p.elf; \
	done
	cp $(BUILD)/ls.elf $(BUILD)/initrd-root/bin/dir.elf   # "dir" is another name for ls
	cp $(BUILD)/init.elf $(BUILD)/initrd-root/init.elf    # kernel loads /init.elf at root
	# ---- musl-linked programs also live in /bin ----
	for p in $(MUSLPROGS); do \
	  cp $(BUILD)/$$p.elf $(BUILD)/initrd-root/bin/$$p.elf; \
	done
	# ---- the dynamic loader -----------------------------------------------
ifeq ($(USERLAND),src)
	# dynhello.elf is an ET_DYN with PT_INTERP=/lib/ld-musl-x86_64.so.1, so
	# the initrd must carry the interpreter at exactly that path.  musl's
	# libc.so *is* the dynamic linker (the two names are the same file in a
	# musl install), so a plain copy provides both the interpreter and the
	# libc.so it is asked to load.  Alpine's binaries are linked the same
	# way, so USERLAND=alpine gets the same file from the musl package.
	cp $(BUILD)/dynhello.elf $(BUILD)/initrd-root/bin/dynhello.elf
endif
	cp $(MUSL_LIB)/libc.so $(BUILD)/initrd-root/lib/ld-musl-x86_64.so.1
	cp $(MUSL_LIB)/libc.so $(BUILD)/initrd-root/lib/libc.so
	# Alpine packages staged by `make autoinstall` ride along on every image
	# build.  Their binaries link against /lib/ld-musl-x86_64.so.1, which
	# the copy just above provides, so they run unmodified.
	if [ -d $(BUILD)/alpine-root ]; then \
	    cp -af $(BUILD)/alpine-root/. $(BUILD)/initrd-root/; \
	fi
	# Alpine minirootfs base (Unixed-Kernel style): the full Alpine userland
	# (openrc services, udev rules, dbus configs, ...) layers on top.
	if [ -d $(BUILD)/alpine-rootfs ]; then \
	    cp -af $(BUILD)/alpine-rootfs/. $(BUILD)/initrd-root/; \
	fi
	# ---- prune the desktop/media dead weight ---------------------------
	# WebKit, the media codecs, GTK and the Xfce session cannot run on this
	# kernel and serve nothing here.  Limine loads the whole initrd through
	# the BIOS CD path, so every megabyte cut is seconds of boot time.
	# Xorg, its data files (/usr/share/X11: xkb, locale, xorg.conf.d) and
	# the core fonts are deliberately absent from this list -- those are
	# exactly what the boot-time X session in /usr/rc needs.
	rm -rf $(BUILD)/initrd-root/usr/lib/libwebkit2gtk-4.1* \
	       $(BUILD)/initrd-root/usr/lib/libjavascriptcoregtk-4.1* \
	       $(BUILD)/initrd-root/usr/libexec/webkit2gtk-4.1 \
	       $(BUILD)/initrd-root/usr/lib/libx265* \
	       $(BUILD)/initrd-root/usr/lib/libavcodec* \
	       $(BUILD)/initrd-root/usr/lib/libavformat* \
	       $(BUILD)/initrd-root/usr/lib/libavutil* \
	       $(BUILD)/initrd-root/usr/lib/libswresample* \
	       $(BUILD)/initrd-root/usr/lib/libswscale* \
	       $(BUILD)/initrd-root/usr/lib/libpostproc* \
	       $(BUILD)/initrd-root/usr/lib/libgtk-3* \
	       $(BUILD)/initrd-root/usr/lib/libgdk* \
	       $(BUILD)/initrd-root/usr/libexec/upower* \
	       $(BUILD)/initrd-root/usr/share/icons \
	       $(BUILD)/initrd-root/usr/share/themes \
	       $(BUILD)/initrd-root/etc/xdg/xfce4 \
	       $(BUILD)/initrd-root/usr/bin/xfce4-* \
	       $(BUILD)/initrd-root/usr/bin/startxfce4
	find $(BUILD)/initrd-root/usr/bin -name "xfce4-*" -delete 2>/dev/null || true
	find $(BUILD)/initrd-root -name "*.Xauthority" -delete 2>/dev/null || true
	# `mount` is invoked by its bare name from OpenRC's init.sh and service
	# scripts, so it must sit on PATH as /bin/mount (not /bin/mount.elf).  The
	# rest of the musl programs are only ever called by absolute path.
	#
	# Alpine lays these same names down as absolute busybox links
	# (/bin/mount -> /bin/busybox), and `cp -a alpine-root/.` above copies
	# them verbatim: writing through such a link from the *host* would follow
	# it to the host's /bin/busybox (EPERM) instead of replacing the guest's
	# entry.  Remove the links first, exactly like the /bin/sh handling in
	# USERLAND=src below; the copies are the GNOS programs the boot scripts
	# actually call.
	rm -f $(BUILD)/initrd-root/bin/mount \
	      $(BUILD)/initrd-root/sbin/getty \
	      $(BUILD)/initrd-root/bin/login \
	      $(BUILD)/initrd-root/usr/bin/chvt \
	      $(BUILD)/initrd-root/sbin/insmod \
	      $(BUILD)/initrd-root/sbin/rmmod
	cp $(BUILD)/mount.elf $(BUILD)/initrd-root/bin/mount
	# getty, login and chvt are named without the .elf suffix: /etc/inittab
	# style callers, the shell and /etc/issue all refer to them by the names
	# every other Unix uses, and `login` in particular is what getty execs by
	# a compiled-in absolute path.
	cp $(BUILD)/getty.elf $(BUILD)/initrd-root/sbin/getty
	cp $(BUILD)/agetty.elf $(BUILD)/initrd-root/sbin/agetty
	cp $(BUILD)/wiggle.elf $(BUILD)/initrd-root/sbin/wiggle
	cp $(BUILD)/nep1.elf $(BUILD)/initrd-root/bin/nep1
	cp $(BUILD)/login.elf $(BUILD)/initrd-root/bin/login
	cp $(BUILD)/bgidm.elf $(BUILD)/initrd-root/bin/bgidm
	cp $(BUILD)/chvt.elf  $(BUILD)/initrd-root/usr/bin/chvt
	# ---- loadable kernel modules: /lib/modules, like every Linux ---------
	mkdir -p $(BUILD)/initrd-root/lib/modules
	for k in $(KMODS); do \
	  cp $$k $(BUILD)/initrd-root/lib/modules/; \
	done
	# insmod/rmmod are typed by name at the shell, no .elf suffix (and
	# /sbin, like the Linux kmod tools they stand in for).
	cp $(BUILD)/insmod.elf $(BUILD)/initrd-root/sbin/insmod
	cp $(BUILD)/rmmod.elf  $(BUILD)/initrd-root/sbin/rmmod
	# The installer is typed by name at the shell, exactly like the rest of
	# a Unix tool set -- no .elf suffix.
	cp $(BUILD)/installer.elf $(BUILD)/initrd-root/bin/installer
ifeq ($(USERLAND),src)
	# ---- BusyBox: the multi-call binary, plus one file per applet ----
	# BusyBox picks its applet from basename(argv[0]) -- names that start with
	# "busybox" fall through to the multi-call dispatcher instead -- so every
	# applet needs a file of its own.  They are copies, not hard links: the
	# ext2 driver has never been run against an inode with nlink > 1.
	# /usr/bin keeps them out of the way of the toy ls/cat/rm in /bin, which
	# PATH finds first.
	cp $(BB_BIN) $(BUILD)/initrd-root/bin/busybox.elf
	for a in $(BB_APPLETS); do \
	  cp $(BB_BIN) $(BUILD)/initrd-root/usr/bin/$$a; \
	done
	# /bin/sh and /bin/ash are busybox multi-call names: invoked as "sh"
	# (or "ash") busybox dispatches to its ash applet, which is the system
	# shell.  They live in /bin so #!/bin/sh shebangs and the init PATH find
	# them.
	# Alpine's busybox-binsh ships /bin/sh as an absolute symlink to
	# /bin/busybox; copying over it would follow the link and try to write
	# the *host's* /bin/busybox, so remove the link first.
	rm -f $(BUILD)/initrd-root/bin/sh $(BUILD)/initrd-root/bin/ash
	cp $(BB_BIN) $(BUILD)/initrd-root/bin/sh
	cp $(BB_BIN) $(BUILD)/initrd-root/bin/ash
	# ---- GNU Bash ----
	# Stripped on the way in: the unstripped binary is 4.4 MB of mostly
	# DWARF, and every byte of it would be read off the initrd at exec time.
	cp $(BASH_BIN) $(BUILD)/initrd-root/bin/bash
	strip $(BUILD)/initrd-root/bin/bash
	# ---- GNU coreutils ----
	# Every program coreutils built, into /usr/bin.  This lands *after* the
	# BusyBox loop above, so for the ~30 names both provide (cat, cp, ls, rm,
	# sort, ...) the GNU one wins and BusyBox's applet stays reachable through
	# /bin/busybox.elf's multi-call dispatcher.  That is the intended order:
	# BusyBox was scaffolding to get a userland booting at all, GNU coreutils
	# is the thing GNOS is supposed to run.
	#
	# Stripped like bash, and for the same reason -- 33 MB of binaries becomes
	# 12 MB, all of it DWARF that nothing in the image can read.  `ginstall` is
	# coreutils' build-time name for install(1) (autoconf renames it to dodge
	# the host's install script); it goes in under its real name.  `getlimits`
	# is a helper for coreutils' own test suite and has no business shipping.
	for f in $(CC_SRC)/src/*; do \
	  [ -f "$$f" ] && [ -x "$$f" ] || continue; \
	  n=$${f##*/}; \
	  case "$$n" in *.o|*.sh|*.pl|getlimits) continue;; esac; \
	  head -c4 "$$f" | grep -q ELF || continue; \
	  [ "$$n" = ginstall ] && n=install; \
	  cp "$$f" $(BUILD)/initrd-root/usr/bin/$$n; \
	  strip $(BUILD)/initrd-root/usr/bin/$$n; \
	done
	# ---- GNU binutils ----
	# The ELF toolchain the guest ships with.  `as`/`ld` come from their
	# build-time names (as-new/ld-new); ld is copied twice so that the
	# ld.bfd spelling that many build scripts probe for works too.
	cp $(BU_SRC)/binutils/ar        $(BUILD)/initrd-root/usr/bin/ar
	cp $(BU_SRC)/binutils/addr2line $(BUILD)/initrd-root/usr/bin/addr2line
	cp $(BU_SRC)/binutils/cxxfilt   $(BUILD)/initrd-root/usr/bin/c++filt
	cp $(BU_SRC)/binutils/elfedit   $(BUILD)/initrd-root/usr/bin/elfedit
	cp $(BU_SRC)/binutils/nm-new    $(BUILD)/initrd-root/usr/bin/nm
	cp $(BU_SRC)/binutils/objcopy   $(BUILD)/initrd-root/usr/bin/objcopy
	cp $(BU_SRC)/binutils/objdump   $(BUILD)/initrd-root/usr/bin/objdump
	cp $(BU_SRC)/binutils/ranlib    $(BUILD)/initrd-root/usr/bin/ranlib
	cp $(BU_SRC)/binutils/readelf   $(BUILD)/initrd-root/usr/bin/readelf
	cp $(BU_SRC)/binutils/size      $(BUILD)/initrd-root/usr/bin/size
	cp $(BU_SRC)/binutils/strings   $(BUILD)/initrd-root/usr/bin/strings
	cp $(BU_SRC)/binutils/strip-new $(BUILD)/initrd-root/usr/bin/strip
	cp $(BU_SRC)/gas/as-new         $(BUILD)/initrd-root/usr/bin/as
	cp $(BU_SRC)/ld/ld-new          $(BUILD)/initrd-root/usr/bin/ld
	cp $(BU_SRC)/ld/ld-new          $(BUILD)/initrd-root/usr/bin/ld.bfd
	cp $(BU_SRC)/gprof/gprof        $(BUILD)/initrd-root/usr/bin/gprof
	strip $(BUILD)/initrd-root/usr/bin/ar \
	      $(BUILD)/initrd-root/usr/bin/addr2line \
	      $(BUILD)/initrd-root/usr/bin/c++filt \
	      $(BUILD)/initrd-root/usr/bin/elfedit \
	      $(BUILD)/initrd-root/usr/bin/nm \
	      $(BUILD)/initrd-root/usr/bin/objcopy \
	      $(BUILD)/initrd-root/usr/bin/objdump \
	      $(BUILD)/initrd-root/usr/bin/ranlib \
	      $(BUILD)/initrd-root/usr/bin/readelf \
	      $(BUILD)/initrd-root/usr/bin/size \
	      $(BUILD)/initrd-root/usr/bin/strings \
	      $(BUILD)/initrd-root/usr/bin/strip \
	      $(BUILD)/initrd-root/usr/bin/as \
	      $(BUILD)/initrd-root/usr/bin/ld \
	      $(BUILD)/initrd-root/usr/bin/ld.bfd \
	      $(BUILD)/initrd-root/usr/bin/gprof
	# ---- fastfetch ----
	# The system-info tool itself, plus its single-threaded flashfetch
	# sibling.  Stripped like everything else: the unstripped pair is
	# mostly DWARF.
	cp $(FF_BIN) $(BUILD)/initrd-root/usr/bin/fastfetch
	cp $(FFLASH) $(BUILD)/initrd-root/usr/bin/flashfetch
	strip $(BUILD)/initrd-root/usr/bin/fastfetch \
	      $(BUILD)/initrd-root/usr/bin/flashfetch
	# ---- curl ----
	# The network client that proves the TCP/UDP stack end to end.  Stripped
	# on the way in like everything else.
	cp $(CURL_BIN) $(BUILD)/initrd-root/usr/bin/curl
	strip $(BUILD)/initrd-root/usr/bin/curl
	# curl's CA store: the host's root bundle, at the path curl was
	# configured with --with-ca-bundle.  Without it https fails loudly;
	# `curl -k` remains the escape hatch.
	mkdir -p $(BUILD)/initrd-root/etc/ssl/certs
	cp /etc/ssl/certs/ca-certificates.crt \
	   $(BUILD)/initrd-root/etc/ssl/certs/ca-bundle.pem
	# ---- nano + terminfo ----
	# The editor, plus the xterm and linux terminfo entries it needs when
	# TERM=xterm (getty sets TERM=linux by default, so nano must have that
	# entry too).  The whole ncurses terminfo tree is 6.7 MB; x/ and linux/
	# are the only entries this machine will ever ask for.
	cp $(NANO_BIN) $(BUILD)/initrd-root/usr/bin/nano
	strip $(BUILD)/initrd-root/usr/bin/nano
	mkdir -p $(BUILD)/initrd-root/usr/share/terminfo
	cp -a $(NC_STAGE)/share/terminfo/x $(BUILD)/initrd-root/usr/share/terminfo/
	mkdir -p $(BUILD)/initrd-root/usr/share/terminfo/l
	cp -a $(NC_STAGE)/share/terminfo/l/linux $(BUILD)/initrd-root/usr/share/terminfo/l/
	# v/ is ncurses's fallback terminal (vt220) when TERM is unset, so it
	# rides along too.
	mkdir -p $(BUILD)/initrd-root/usr/share/terminfo/v
	cp -a $(NC_STAGE)/share/terminfo/v $(BUILD)/initrd-root/usr/share/terminfo/
	# ---- python3 (musl CPython 3.12) ----
	# The interpreter is dynamically linked against musl, so it needs its
	# stdlib beside it: the pure-python Lib/ tree under /usr/lib/python3.12
	# and the compiled extension modules in lib-dynload/ (static builds
	# could not dlopen those; this one can).
	mkdir -p $(BUILD)/initrd-root/usr/lib/python3.12/lib-dynload
	cp $(PY_BIN) $(BUILD)/initrd-root/usr/bin/python3.12
	strip $(BUILD)/initrd-root/usr/bin/python3.12
	ln -sf python3.12 $(BUILD)/initrd-root/usr/bin/python3
	ln -sf python3.12 $(BUILD)/initrd-root/usr/bin/python
	cp -a $(PY_SRC)/Lib/. $(BUILD)/initrd-root/usr/lib/python3.12/
	cp $(PY_SRC)/Modules/*.so \
	   $(BUILD)/initrd-root/usr/lib/python3.12/lib-dynload/
endif  # USERLAND=src: busybox/bash/coreutils/binutils/fastfetch/curl/nano/python
	# ---- desktop stack (labwc/xfce) DISABLED for headless ISO -----------
	# To re-enable: un-comment the labwc/xfce sections above this line.
	
	cp src/etc/inittab $(BUILD)/initrd-root/etc/inittab  # busybox init rewires openrc
	# Standard Alpine runlevel set: hostname/sysctl/bootmisc/localmount in the
	# boot runlevel, the local(8) script dir at default.  Without the runlevel
	# symlinks the openrc runlevels stay empty and "openrc boot/default"
	# starts nothing; this mirrors what apk's maintainer scripts put there on
	# a fully provisioned Alpine.  /run/openrc is openrc's state dir and must
	# exist before "openrc sysinit" runs; /run is plain ext2 in the image.
	mkdir -p $(BUILD)/initrd-root/etc/local.d \
	         $(BUILD)/initrd-root/etc/runlevels/sysinit \
	         $(BUILD)/initrd-root/etc/runlevels/boot \
	         $(BUILD)/initrd-root/etc/runlevels/default \
	         $(BUILD)/initrd-root/run/openrc \
	         $(BUILD)/initrd-root/run/lock
	for s in hostname sysctl bootmisc localmount; do \
	  ln -sf /etc/init.d/$$s $(BUILD)/initrd-root/etc/runlevels/boot/$$s; \
	done
	ln -sf /etc/init.d/local $(BUILD)/initrd-root/etc/runlevels/default/local
	# startxfce: post-login desktop launcher (see /root/.profile).  Installed
	# executable because the kernel honours the #! line only on a real exec.
	cp src/usr/startxfce $(BUILD)/initrd-root/usr/bin/startxfce
	chmod 755 $(BUILD)/initrd-root/usr/bin/startxfce
	# Static system config (hosts, resolv.conf, nsswitch, services, protocols,
	# passwd/group, hostname).  These make the BusyBox network tools and the
	# C resolver actually work: ping/wget do DNS via /etc/resolv.conf, getent
	# reads /etc/passwd, and `hostname` uses /etc/hostname.
	cp -a src/rootfs/etc/. $(BUILD)/initrd-root/etc/
	# Guest-side helper scripts (start-xfce, ...).
	cp -a src/rootfs/sbin/. $(BUILD)/initrd-root/sbin/
	cp -a src/rootfs/bin/. $(BUILD)/initrd-root/bin/
	# ---- the /usr/bin spelling of the tools /etc/rc calls by path ------
	# Alpine lays coreutils out as /bin/<name> -> ../usr/bin/coreutils and
	# puts busybox's applets under /bin as well, while the hand-built
	# coreutils (USERLAND=src) put every name in /usr/bin.  /etc/rc calls
	# dd, ls, cat and hostname by their /usr/bin path, so point that spelling
	# at whichever of the two is actually there -- and do nothing at all when
	# /usr/bin already has the file, which is the USERLAND=src case.
	for c in dd ls cat hostname; do \
	  if [ ! -e $(BUILD)/initrd-root/usr/bin/$$c ] && \
	     [ -e $(BUILD)/initrd-root/bin/$$c ]; then \
	    ln -sf ../../bin/$$c $(BUILD)/initrd-root/usr/bin/$$c; \
	  fi; \
	done
	# Kernel command line carrier: `make KCMD="single"` drops the words into
	# /cmdline at the initrd root; the kernel reads that file before PID 1
	# (Limine does not forward conf cmdline: to direct-protocol kernels).
	@if [ -n "$(KCMD)" ]; then \
	    echo "$(KCMD)" > $(BUILD)/initrd-root/cmdline; \
	fi
	# ---- root home: ~/.bashrc is sourced by the interactive login shell ----
	cp -a src/rootfs/root/. $(BUILD)/initrd-root/root/
	# ---- OpenRC 0.56 tree ------------------------------------------------
	# The full install (built separately with meson + musl, see
	# build/orcsrc/) drops in here: /sbin/openrc (+ openrc-run, rc-status,
	# start-stop-daemon, ...), /etc/init.d/*, /etc/runlevels/*, /etc/conf.d/*,
	# /etc/rc.conf, and /usr/libexec/rc/{bin,sbin,sh}.  It is what the kernel's
	# mount/getrandom/symlink/rename support exists to serve, so it rides along
	# in the initrd and /etc/rc brings its runlevels up at boot.
	mkdir -p $(BUILD)/initrd-root/dev/shm $(BUILD)/initrd-root/run/lock
ifeq ($(USERLAND),src)
	cp -a build/orcsrc/openrc-install/bin/.    $(BUILD)/initrd-root/bin/    2>/dev/null || true
	cp -a build/orcsrc/openrc-install/sbin/.   $(BUILD)/initrd-root/sbin/
	cp -a build/orcsrc/openrc-install/etc/.    $(BUILD)/initrd-root/etc/
	cp -a build/orcsrc/openrc-install/usr/.    $(BUILD)/initrd-root/usr/
endif
	# devfs would mount a tmpfs over /dev and hide the static character
	# devices the kernel already provides (null, tty, ...); drop it from the
	# sysinit runlevel so the rest of OpenRC can run headless.
	rm -f $(BUILD)/initrd-root/etc/runlevels/sysinit/devfs
	# ---- the ordinary user's home ----------------------------------------
	mkdir -p $(BUILD)/initrd-root/home/elaina
	cp -a src/rootfs/home/elaina/. $(BUILD)/initrd-root/home/elaina/ 2>/dev/null || true
	# ---- boot payload (what an installed machine boots from) ------------
	# Every system image carries the whole boot chain inside it, so a root
	# partition dumped onto a disk by the installer is already bootable: the
	# disk's Limine reads /boot/limine/limine.sys (stage 2) and
	# /boot/limine/limine.conf at boot, and the kernel_image file it points
	# at is /GNOSKr.elf at the volume root.  Stage 1 (limine-bios.sys)
	# handled the MBR when the installer ran.  The kernel's own modules are
	# not copied -- installed boots have no initrd, and the root filesystem
	# itself is the module, read off partition 1 by the kernel at boot.
	mkdir -p $(BUILD)/initrd-root/boot/limine
	cp limine/limine-bios.sys $(BUILD)/initrd-root/boot/limine/limine.sys
	cp $(KRNL) $(BUILD)/initrd-root/GNOSKr.elf
	printf 'timeout: 1\n\n/GNOS\n    protocol: limine\n    kernel_path: boot():/GNOSKr.elf\n' > $(BUILD)/initrd-root/boot/limine/limine.conf
	dd if=/dev/zero of=$@ bs=1M count=64 2>/dev/null
	# ---- ownership and modes ---------------------------------------------
	# mke2fs -d copies the *build user's* uid/gid onto every inode, which on
	# a machine whose developer is uid 1000 means shipping an image where
	# /etc/shadow and /bin/bash belong to an ordinary user.  That was
	# harmless while the kernel reported st_uid = 0 for everything; now that
	# it reads i_uid for real, it would hand the whole system away.
	#
	# fakeroot is what fixes it: chown(2) inside it is remembered in a side
	# table that mke2fs's stat(2) then sees, so the image comes out
	# root-owned without this build needing to be root.  Everything that has
	# to differ from root:root -- the user's home, the shadow file's mode --
	# is set in the same shell, after the blanket chown.
	fakeroot -- sh -c '\
	  chown -R 0:0 $(BUILD)/initrd-root; \
	  chmod 0700 $(BUILD)/initrd-root/root; \
	  chmod 0600 $(BUILD)/initrd-root/etc/shadow; \
	  chmod 0644 $(BUILD)/initrd-root/etc/passwd $(BUILD)/initrd-root/etc/group; \
	  chmod 1777 $(BUILD)/initrd-root/tmp; \
	  chown -R 1000:1000 $(BUILD)/initrd-root/home/elaina; \
	  chmod 0755 $(BUILD)/initrd-root/home/elaina; \
	  mke2fs -q -t ext2 -b 1024 -I 256 \
	         -O ^resize_inode,^dir_index,^ext_attr \
	         -d $(BUILD)/initrd-root -F $@ $(INITRD_MB)M'

# ---------- Limine hybrid ISO ----------
# The bootloader config inside the ISO is generated from the same single
# entry every time, plus an optional kernel command line.  `make KCMD=single
# build/gnos.iso` (or any boot target) puts `cmdline: single` in the conf,
# Limine hands it to the kernel, and the kernel passes the word to /init.elf
# -> single-user root bash.  The .kcmd stamp makes the ISO rebuild only when
# KCMD actually changes value.
KCMD ?=

.PHONY: FORCE
FORCE:

$(BUILD)/.kcmd: FORCE
	@old="$$(cat $@ 2>/dev/null || true)"; \
	if [ "$$old" != "$(KCMD)" ]; then \
	    echo "$(KCMD)" > $@; \
	    echo "GNOS kernel cmdline: '$(KCMD)'"; \
	fi

$(ISO): $(KRNL) $(INITRD) $(BUILD)/.kcmd $(LIMINE_BIOS) $(LIMINE_UEFI) | $(BUILD)
	mkdir -p $(ISO_ROOT)
	cp $(KRNL)      $(ISO_ROOT)/GNOSKr.elf
	cp $(INITRD)    $(ISO_ROOT)/initrd.img
	printf 'timeout: 1\n\n/GNOS\n    protocol: limine\n    kernel_path: boot():/GNOSKr.elf\n    module_path: boot():/initrd.img\n' > $(ISO_ROOT)/limine.conf
	@if [ -n "$(KCMD)" ]; then echo "    cmdline: $(KCMD)" >> $(ISO_ROOT)/limine.conf; fi
	cp $(LIMINE_BIOS) $(ISO_ROOT)/limine-bios-cd.bin
	cp $(LIMINE_UEFI) $(ISO_ROOT)/limine-uefi-cd.bin
	cp limine/limine-bios.sys $(ISO_ROOT)/limine-bios.sys
	xorriso -as mkisofs -b limine-bios-cd.bin -no-emul-boot \
	  -boot-load-size 4 -boot-info-table \
	  --efi-boot limine-uefi-cd.bin -efi-boot-part --efi-boot-image \
	  -r -J -o $@ $(ISO_ROOT)

# ---------- run ----------
# The disk is created on demand and never rebuilt once it exists -- `make run`
# twice in a row must find whatever the guest wrote the first time.  The rule
# lives down here rather than beside its variables because make builds the
# *first* target in the file when given no arguments, and that has to stay
# `all`.
$(DISK):
	@mkdir -p $(BUILD)
	qemu-img create -f raw $@ $(DISK_MB)M

run: $(ISO) $(DISK)
	qemu-system-x86_64 -cdrom $(ISO) $(QEMU_MEM_ARG) -display gtk $(QEMU_DEVICES)

run-uefi: $(ISO) $(DISK)
	qemu-system-x86_64 -cdrom $(ISO) $(QEMU_MEM_ARG) -bios $(OVMF) -display gtk \
	  $(QEMU_DEVICES)

# guistart — the "just show me the OS" target.
#
# Differences from `run`, all of them about being in front of a human:
#   - the audio backend is a real one, so both self-test tones (AC97 first,
#     then HDA) are audible;
#   - the debug console is teed to build/dbg.log as well, so the boot messages
#     that scroll past the framebuffer are still there afterwards;
#   - -no-reboot turns a triple fault into a stopped VM you can look at
#     instead of an endless reboot loop.
# The ISO is a prerequisite, so this rebuilds anything stale first.
guistart: $(ISO) $(DISK)
	@echo "GNOS: booting in a window (audio backend: $(AUDIO_BACKEND));"
	@echo "      boot log is also being written to $(BUILD)/dbg.log"
	qemu-system-x86_64 -cdrom $(ISO) $(QEMU_MEM_ARG) \
	  $(QEMU_NET) $(GUI_AUDIO) $(QEMU_DISK) $(QEMU_SMP) \
	  -device isa-debugcon,chardev=dbg -chardev file,id=dbg,path=$(BUILD)/dbg.log \
	  -display gtk -no-reboot


# clang-format gate: every first-party kernel/userland C+header obeys the
# repo-level .clang-format (Google base, BreakBeforeBraces=Linux, 4-space
# indent, 100 columns).  Vendor trees keep their upstream formatting.
.PHONY: check-format
check-format:
	@find src -name '*.c' -o -name '*.h' | grep -v '/vendor/' | \
	  xargs clang-format --dry-run -Werror --style=file
	@echo "check-format: clang-format clean"

# `make test` — the headless self-test: run qemu for 20s, then hunt the
# debug console's PASS/FAIL and fault markers for the binary that ran.
# headless leaves the process on after its tests, so timeout kills it.
.PHONY: test
test: $(ISO) $(DISK)
	@echo "GNOS: headless self-test (20 s)..."
	@rm -f $(BUILD)/dbg.log
	@timeout 20 $(MAKE) headless || true
	@grep -E 'PASS|FAIL' $(BUILD)/dbg.log >/dev/null || \
	  { echo "test: no PASS/FAIL lines in $(BUILD)/dbg.log"; exit 1; }
	@grep 'FAIL' $(BUILD)/dbg.log && { echo "test: FAIL present"; exit 1; } || true
	@echo "test: PASS lines present, no FAIL"

# The one-stop "is this commit acceptable?" gate used by CI and by hand.
.PHONY: check
check: check-format check-hdrs test
	@echo "check: all gates passed"

# headless — guistart without the window: the headless self-test target.
# The debug console is teed to build/dbg.log, exactly like guistart, so
# `make headless; grep PASS build/dbg.log` is the whole verification loop.
headless: $(ISO) $(DISK)
	@echo "GNOS: booting headless; log is $(BUILD)/dbg.log"
	qemu-system-x86_64 -cdrom $(ISO) $(QEMU_MEM_ARG) \
	  $(QEMU_NET) $(QEMU_DISK) \
	  -device isa-debugcon,chardev=dbg -chardev file,id=dbg,path=$(BUILD)/dbg.log \
	  -display none -no-reboot -smp 4

# `clean` deliberately spares the third-party source trees under $(BUILD).
# musl was fetched and built by hand and there is no rule to get it back, so a
# plain `rm -rf build` would destroy the toolchain irrecoverably.  Everything
# this Makefile knows how to rebuild is listed explicitly instead.
THIRD_PARTY := $(BUILD)/muslsrc $(BUILD)/bbsrc $(BUILD)/bashsrc \
               $(BUILD)/orcsrc $(BUILD)/ccsrc $(BUILD)/busrcc

clean:
	rm -rf $(BUILD)/user $(BUILD)/initrd-root $(BUILD)/modules $(ISO_ROOT)
	rm -f  $(BUILD)/*.o $(BUILD)/*.elf $(BUILD)/*.img $(BUILD)/*.iso \
	       $(BUILD)/*.log $(BUILD)/gnoscfg build/.config

# Nuke everything, third-party trees included.  Only useful if you are prepared
# to re-fetch musl by hand -- see THIRD_PARTY above.
distclean:
	rm -rf $(BUILD)
