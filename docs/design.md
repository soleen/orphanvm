# Orphaned Virtual Machines: In-Kernel CPU Preservation and Caretaker

**Authors:** Pasha Tatashin, `<add-names>`
**Contributors:** `<add-names>`
**Date:** 9/28/2026
**Status:** Draft

## 1. Introduction and Background

**Live Update** is a kernel capability that enables updating the full host
software stack across a reboot into a new kernel image while preserving and
keeping alive selected resources, such as memory and devices. KHO and LUO were
introduced to facilitate this capability in the Linux kernel. For virtual
machines, preserving guest backing memory (`memfd` / `guest_memfd`) and
pass-through devices eliminates the need for live migration, which requires
copying gigabytes of guest memory over the network and maintaining spare host
capacity across the fleet. Additionally, pass-through accelerators such as GPUs
and TPUs are extremely expensive, making any downtime critical for both AI
inference and training workloads, yet they do not support live migration; for
guest VMs using such devices, Live Update is the only non-destructive way to
update the full host software stack.

The need for frequent host updates is also growing due to LLM-assisted software
engineering. In recent years, LLM-based analysis has uncovered a number of
security vulnerabilities across the virtualization stack, including the Linux
kernel. At the same time, engineers use LLMs to develop fixes, build new
features, and review code, increasing the rate of both vulnerability discovery
and code changes. This trend is expected to accelerate as LLMs become more
capable. Because kernel live patching cannot be used for all types of fixes
(such as data structure changes, low-level assembly updates, or cross-subsystem
changes), Live Update becomes the only viable path to deploy these updates
without terminating running VMs.

However, a Live Update that preserves only memory and PCI devices still leaves a
service blackout window. Even as ongoing device preservation efforts ensure that
pass-through devices and the PCI complex are not reset and continue operating
normally (including in-flight DMA) across live update, vCPUs in a standard VM
live update still rely on the classical userspace suspend and resume flow:
before the old VMM exits, all guest vCPUs are paused and their state is
serialized to userspace, and they remain paused while the outgoing kernel
reboots into the incoming kernel and the new VMM spawns and restores them. This
transition still stalls guest vCPU execution for several seconds.

**Orphaned Virtual Machines** eliminate this CPU blackout window by treating
physical CPUs as another resource type, alongside memory and PCI devices, that
can be preserved during a live update, decoupling vCPU execution from the host
OS lifecycle. While management CPUs reboot into the incoming kernel, preserved
physical CPUs running guest vCPUs remain isolated from the host OS and continue
executing guest instructions inside an isolated in-kernel execution environment
(**The KVM Caretaker**) until the incoming kernel and VMM are ready to reclaim
them. To support this, this effort introduces in-kernel `vcpufd` serialization
and preservation via LUO (`KVM_CAP_VCPU_PRESERVE`) together with the KVM
Caretaker, which is also extended with a minimal set of VM-exit handlers for
proof-of-concept purposes.

### 1.1 Evolution from the Initial Proposal to RFCv1

The [initial design proposal](https://lore.kernel.org/all/afEwWZksU0Fw61oT@plex)
(posted without implementation) proposed loading a standalone, userspace-supplied
bare-metal ELF binary into KVM via a `KVM_SET_CARETAKER` ioctl. In that initial
proposal, the Caretaker permanently interposed on the hardware VM-exit vector
(`HOST_RIP`) throughout the VM's lifetime, acting as a fast-path shim that
forwarded exits back to KVM via physical function pointers when attached, and
spinning in a detached loop during `kexec`.

Following upstream community feedback on that thread, the architecture was
redesigned for the [RFCv1 patch series](https://lore.kernel.org/all/20260920193650.3373435-1-pasha.tatashin@soleen.com)
described in this document:

1. **Built-in vs. External Caretaker**: Instead of parsing and validating a
   userspace ELF payload, the preserved execution code is proposed to be part of
   the kernel source tree and therefore compiled into the kernel image inside
   dedicated linker sections (`.text.cpu_preserved` and `.data.cpu_preserved`).
2. **Zero Overhead During Normal Operation**: Standard `KVM_RUN` execution is
   untouched while the host is running normally. Hardware VM-exit vectors
   (`HOST_RIP`, `VBAR_EL2`, `HOST_CR3`) are reprogrammed to point to the
   Caretaker only at the moment the live update session enters the preservation
   phase, and are restored to standard KVM handlers immediately upon adoption in
   the incoming kernel.
3. **Layered Subsystem Decomposition**: Rather than embedding physical CPU
   offlining, page-table isolation, and scheduling inside KVM, the current
   design separates the mechanism into generic kernel subsystems (`cpu_preserve`
   and `oncore`) that KVM (`caretaker`) consumes as a client. This keeps core CPU
   lifecycle management in `kernel/liveupdate/` and leaves the door open for
   future non-KVM on-core workloads (such as polling kernel drivers or isolated
   user tasks).
4. **Strict KHO ABI Separation**: In-kernel `vcpufd` preservation via LUO
   (`KVM_CAP_VCPU_PRESERVE`) provides a versioned, uAPI-backed serialization
   format (`include/linux/kho/abi/`) across `kexec`. The incoming kernel never
   dereferences outgoing-kernel internal data structures (`struct kvm_vcpu`,
   `struct loaded_vmcs`, etc.), allowing live updates across kernels with
   different struct layouts or compiler configurations.

> **Note on Proposal Status:** This document describes the current design
> corresponding to the [RFCv1 patch series](https://lore.kernel.org/all/20260920193650.3373435-1-pasha.tatashin@soleen.com).
> Several aspects, most notably how to structure VM-exit handling and share
> low-level code with KVM without duplicating hypervisor logic, as well as open
> items from the initial LKML discussion, remain under active discussion and are
> covered in [Section 7](#7-open-design-challenges-and-topics-for-further-discussion).

### 1.2 Memory and Device Preservation

This document focuses on **CPU and KVM execution state preservation**
(`vmfd`/Stage-2 MMU, `cpu_preserve`, `oncore`, and `vcpufd`/Caretaker). Within
the broader Live Update project, guest memory and pass-through device
preservation are developed as companion efforts:

- **Guest Memory and Base `vmfd` Preservation**:
  The [`guest_memfd` preservation series](https://lore.kernel.org/all/20260728121138.1103610-1-tarunsahu@google.com/)
  (`[PATCH v4 00/11] liveupdate: kvm: Guest_memfd preservation`) introduces
  `guest_memfd` support as well as the base KVM `vmfd` LUO preservation
  interfaces (`KVM_CAP_LUO` / `virt/kvm/kvm_luo.c`) that this design expands
  upon for secondary MMU and `vcpufd` preservation. Although RFCv1 was based on
  top of that series for proof-of-concept purposes, OrphanVM does not depend on
  `guest_memfd` and can also use standard preserved `memfd` or HugeTLB backing
  memory.
- **Pass-Through Device and DMA Preservation**:
  Full pass-through device preservation across `kexec` is currently under review
  across three companion series:
  - [PCI core support for Live Update](https://lore.kernel.org/all/20260918200640.887030-1-dmatlack@google.com/)
    (`[PATCH v9 00/13] PCI: liveupdate: PCI core support for Live Update`)
  - [Base Live Update support for VFIO/PCI](https://lore.kernel.org/all/20260714151505.3466855-1-vipinsh@google.com/)
    (`[PATCH v5 00/20] vfio/pci: Base Live Update support for VFIO`)
  - [IOMMU live update state preservation](https://lore.kernel.org/all/20260921004834.2601285-1-skhawaja@google.com/#t)
    (`[PATCH v5 00/18] iommu: Add live update state preservation`)
- **Missing Piece: Posted Interrupt Table Preservation**:
  Combined with preserved Stage-2/TDP page tables, the PCI, VFIO, and IOMMU
  series allow guest polling I/O, direct BAR MMIO, and ongoing device DMA to
  continue across `kexec` without VM-exits. However, for OrphanVM to receive
  pass-through device interrupts during the `kexec` gap without host IRQ
  handlers or `irqfd`, it must work with hardware direct interrupt injection
  (Intel VT-d Posted Interrupts, AMD AVIC, ARM64 GICv4.1 vLPIs): the IOMMU
  Interrupt Remapping Table Entries (IRTEs) and per-vCPU Posted Interrupt
  Descriptors (`pi_desc` on Intel, AVIC backing page on AMD, and GICv4 vPE
  tables on ARM64) must remain preserved across KHO and pointed at the preserved
  physical CPU. Preserving these posted interrupt tables still needs to be
  implemented as part of the device preservation effort, and there is no patch
  series for it yet.

---

## 2. Architectural Overview

The current design decomposes continuous vCPU execution across `kexec` into four
layers, ordered from bottom to top:

```
+-------------------------------------------------------------------+
| Layer 4: KVM Caretaker Engine (virt/kvm/caretaker.c, arch/...)    |
|   - Wraps preserved vCPUs as oncore_job instances                 |
|   - Lockless cross-kernel state machine (PAUSED/RUNNING/STOPPED)  |
|   - Arch guest entry/exit loop (Intel VMX, AMD SVM, ARM64 VHE)    |
+-------------------------------------------------------------------+
| Layer 3: On-Core Execution & Scheduler (kernel/liveupdate/oncore) |
|   - Workload-agnostic session (oncore_session) & job (oncore_job) |
|   - Cooperative / time-sliced round-robin FIFO runqueue           |
|   - Multiplexes M jobs across N preserved physical CPUs           |
+-------------------------------------------------------------------+
| Layer 2: Physical CPU Preservation (kernel/liveupdate/cpu_preserve|
|   - .text.cpu_preserved & .data.cpu_preserved outside KHO Scratch |
|   - CPU hotplug interception & !cpu_present(cpu) SMP isolation    |
|   - Isolated page tables (cpu_preserved_as) & stack context       |
+-------------------------------------------------------------------+
| Layer 1: In-RAM KVM & vCPU Preservation via LUO & KHO             |
|   - vmfd & secondary MMU (TDP / Stage-2) folio preservation       |
|   - vcpufd architectural state serialization (kvm_vcpu_arch_ser)  |
+-------------------------------------------------------------------+
```

This layering allows incremental upstreaming:

- **Layer 1** is useful on its own without any physical CPU preservation: it
  provides in-kernel `vcpufd` and secondary MMU preservation across `kexec`
  (RAM-only suspend/resume without round-tripping vCPU state through userspace).
- **Layers 2 and 3** provide generic infrastructure for keeping physical CPUs
  alive in an isolated address space across `kexec` and scheduling bounded work
  on them.
- **Layer 4** connects preserved KVM vCPUs to the `oncore` scheduler when both
  `vcpufd`s and physical CPUs are preserved within the same LUO session.

### 2.1 End-to-End VMM Orchestration Flow

The kernel interfaces are VMM-agnostic and operate through standard
KVM and LUO file-descriptor preservation ioctls:

1. **Creation & Capability Enablement**:
   The userspace VMM creates `vmfd` and guest backing `memfd`, enables
   `KVM_CAP_LUO`, `KVM_CAP_VCPU_PRESERVE`, and `KVM_CAP_CARETAKER` via
   `KVM_ENABLE_CAP`, and creates `vcpufd`s. Normal `KVM_RUN` execution proceeds
   without any Caretaker interposition.
2. **LUO Session Registration & Isolation**:
   Prior to a live update, the VMM pauses userspace-emulated devices, stops its
   `KVM_RUN` threads, and registers its resources into a LUO session via
   `LIVEUPDATE_SESSION_PRESERVE_FD`:
   - Guest memory `memfd` descriptors
   - `vmfd`, preserving KVM metadata and secondary MMU page tables in KHO memory
   - Physical CPU descriptors (`/sys/devices/system/cpu/cpu<N>/preserve`),
     offlining the target physical cores via CPU hotplug into the isolated
     `oncore_session` execution loop
   - Each `vcpufd`, serializing architectural state into KHO memory and queuing
     the vCPU onto the `oncore_session` runqueue so it immediately resumes guest
     execution on the preserved physical CPUs
3. **The Kexec Gap**:
   The VMM process exits and the host executes `kexec -e`. Management CPUs reboot
   into the incoming kernel while the preserved physical CPUs (`!cpu_present`)
   continue executing guest vCPUs inside the Caretaker's isolated address space.
4. **Reclamation**:
   Once the incoming kernel boots, the new VMM instance opens the preserved LUO
   session and issues `LIVEUPDATE_SESSION_RETRIEVE_FD` for the `memfd`, `vmfd`,
   and `vcpufd` tokens. Retrieving each `vcpufd` atomically detaches the vCPU
   from the Caretaker (`KVM_CARETAKER_STOPPED`) and synchronizes its updated
   architectural state into the new kernel's `struct kvm_vcpu`. Issuing
   `LIVEUPDATE_SESSION_FINISH` releases the preserved physical CPUs back to the
   host Linux scheduler via `add_cpu()`, and the VMM resumes normal `KVM_RUN`
   threads.

---

## 3. Layer 1: In-RAM vCPU and Secondary MMU Preservation via LUO

Before a vCPU can execute across `kexec` (or even be suspended and resumed in RAM
without physical CPU preservation), both its architectural register state and the
VM's secondary page tables must survive the reboot in KHO-preserved memory. This
layer expands upon the base KVM `vmfd` preservation infrastructure
(`KVM_CAP_LUO` in `virt/kvm/kvm_luo.c`) introduced by the
[`guest_memfd` preservation series](https://lore.kernel.org/all/20260728121138.1103610-1-tarunsahu@google.com/).

### 3.1 In-Kernel `vcpufd` Preservation (`KVM_CAP_VCPU_PRESERVE`)

In a traditional live update, the userspace VMM must extract all vCPU state via
dozens of `KVM_GET_*` ioctls prior to `kexec`, serialize that state into a file
or memory buffer, and re-issue `KVM_SET_*` ioctls after `kexec`.

In RFCv1, `vcpufd` is registered directly with LUO via
`LIVEUPDATE_SESSION_PRESERVE_FD` (`"kvm_vcpu_luo_v1"`), governed by the
`KVM_CAP_VCPU_PRESERVE` capability:

- **Top-Level ABI (`include/linux/kho/abi/kvm.h`)**:
  `struct kvm_vcpu_ser` records the `vcpu_id`, `flags` (such as
  `KVM_VCPU_LUO_FLAG_CARETAKER`), the physical address of the preserved `kvm_run`
  page (`kvm_run_phys`), the physical address of the architecture-specific state
  buffer (`arch_state_phys`), and, when Caretaker is enabled, the physical
  address of the Caretaker control block (`caretaker_phys`).
- **uAPI-Backed Architectural State (`struct kvm_vcpu_arch_ser`)**:
  To avoid exposing internal kernel structures across `kexec`, the kernel
  serializes vCPU state into KHO-preserved pages using existing KVM uAPI
  structures:
  - **x86 (`include/linux/kho/abi/kvm_x86.h`)**: Contains `kvm_regs`,
    `kvm_sregs`, `kvm_xsave` (4KB aligned for hardware `XSAVE64`/`XRSTOR64`),
    `kvm_xcrs`, `kvm_debugregs`, `kvm_mp_state`, `kvm_vcpu_events`,
    `kvm_lapic_state`, `tsc_khz`, `kvm_cpuid_entry2[]`, and a dynamic array of
    `kvm_msr_entry[]` covering KVM's `msrs_to_save_all` and `emulated_msrs`.
  - **ARM64 (`include/linux/kho/abi/kvm_arm64.h`)**: Contains `kvm_regs`,
    `kvm_mp_state`, `kvm_vcpu_events`, `kvm_vcpu_init` (the guest feature
    bitmap), and a dynamically sized array of `struct kvm_one_reg_ser` (`id` and
    64-bit `val`). During `.preserve()`, the kernel enumerates all architectural
    registers via `kvm_arm_copy_reg_indices()` and reads/writes them using
    in-kernel accessors (`kvm_arm_get_reg_kernel()` / `kvm_arm_set_reg_kernel()`)
    separated from `copy_to_user()`/`copy_from_user()`.

When a LUO session contains preserved `vmfd` and `vcpufd` descriptors *without*
preserved physical CPUs, `.preserve()` serializes the vCPU into
`struct kvm_vcpu_arch_ser` and `.retrieve()` restores it into the newly allocated
`struct kvm_vcpu` in the incoming kernel. When physical CPUs *are* present in the
LUO session, this same `struct kvm_vcpu_arch_ser` buffer serves as the handoff
area between the outgoing kernel, the Caretaker, and the incoming kernel.

### 3.2 Secondary MMU (TDP / Stage-2) KHO Preservation

During the `kexec` blackout window, the host KVM MMU fault handler is offline.
For the guest to continue accessing its memory without triggering EPT Violations,
NPT faults, or Stage-2 translation faults, the existing secondary page tables
must remain intact in physical memory across `kexec`:

- **x86 TDP MMU (`arch/x86/kvm/mmu/kho.c`)**:
  `kvm_mmu_preserve_kho()` walks all valid TDP MMU roots and active MMU pages.
  Because `kho_preserve_folio()` may allocate memory and sleep, preservation is
  performed in two phases: first counting and recording page physical addresses
  into pre-allocated `struct kvm_kho_folios_ser` pages while holding `mmu_lock`,
  and then invoking `kho_preserve_folio()` outside `mmu_lock`.
- **ARM64 Stage-2 MMU (`arch/arm64/kvm/kvm_luo.c`)**:
  `kvm_arch_vm_luo_preserve()` walks the guest's `struct kvm_s2_mmu` page table
  (`mmu->pgt`) using `kvm_pgtable_walk()` with a `KVM_PGTABLE_WALK_TABLE_PRE`
  visitor (`stage2_kho_visitor()`), preserving every non-leaf Stage-2 page-table
  folio in `struct kvm_kho_folios_ser` alongside the PGD physical address
  (`pgd_phys`) and `vtcr` configuration.

---

## 4. Layer 2: Physical CPU Preservation (`cpu_preserve`)

The `cpu_preserve` subsystem (`kernel/liveupdate/cpu_preserve.c`,
`CONFIG_LIVEUPDATE_CPU`) provides the generic mechanism for detaching a physical
CPU from the outgoing Linux kernel, keeping it executing in a self-contained
memory environment across `kexec`, and returning it to the incoming Linux kernel
when the live update completes.

### 4.1 Preserved Sections and Relocation Outside KHO Scratch

All code and static data executed by a preserved CPU during the `kexec` window
are placed in dedicated linker sections:

- `.text.cpu_preserved` (`__cpu_preserved_text`)
- `.data.cpu_preserved` (`__cpu_preserved_data`)

Translation units targeting these sections are compiled with strict flags to
prevent implicit dependencies on the host kernel runtime:

- `-fno-stack-protector` (no `%gs:0x28` or `sp_el0` stack canary references)
- `-fno-jump-tables` (prevents switch statements from generating jump tables in
  standard `.rodata` outside `.text.cpu_preserved`)
- `-mbranch-protection=none` on ARM64
- Disabled instrumentation (`KCOV_INSTRUMENT := n`, `GCOV_PROFILE := n`,
  `KCSAN_SANITIZE := n`, `KASAN_SANITIZE := n`, `UBSAN_SANITIZE := n`, and no
  `-pg`/`ftrace` flags)
- `objtool` exemptions for `.text.cpu_preserved` from retpoline and return-thunk
  rewriting (`__x86_indirect_thunk_*` / `__x86_return_thunk`), because those
  thunks reside in standard `.text` and are overwritten during `kexec`.

#### Relocating Outside KHO Scratch Memory

A hazard with KHO is that the compiled kernel image itself resides in physical
memory that KHO designates as **KHO Scratch** (memory that the incoming kernel
is permitted to overwrite during early boot decompression and initialization).
Consequently, preserving the physical pages of the outgoing kernel's
`.text.cpu_preserved` section in-place is unsafe.

To solve this, during late boot initialization
(`cpu_preserved_init_runtime_buffer()`), `cpu_preserve`:

1. Allocates physical pages from the buddy allocator (`alloc_pages()`), which
   are guaranteed to be outside the KHO Scratch regions.
2. Copies the compiled contents of `.text.cpu_preserved` and
   `.data.cpu_preserved` into those newly allocated physical pages.
3. Remaps the outgoing kernel's virtual address ranges
   (`__cpu_preserved_text_start..end` and `__cpu_preserved_data_start..end`) to
   point to the new physical pages (splitting any 2MB/contpte kernel mappings
   into 4KB PTEs and setting `PAGE_KERNEL_ROX` / `PAGE_KERNEL` permissions).
4. Marks those physical pages as preserved in KHO (`kho_preserve_pages()`).

As a result, normal C symbol references and direct calls within
`.text.cpu_preserved` and `.data.cpu_preserved` continue to use their linked
kernel virtual addresses, while backed by safe physical pages that survive
`kexec`.

### 4.2 Lifecycle and `!cpu_present(cpu)` SMP Isolation

Each non-boot physical CPU exposes a sysfs control file:
`/sys/devices/system/cpu/cpu<N>/preserve`.

To preserve a CPU for a live update, userspace opens this file and registers the
file descriptor with a LUO session via `LIVEUPDATE_SESSION_PRESERVE_FD`
(`"cpu_fh_v1"`, `struct cpu_preserved_file_ser`).

```
Outgoing Kernel                      kexec                     Incoming Kernel
---------------                      -----                     ---------------
open(/sys/.../cpuN/preserve)
LIVEUPDATE_SESSION_PRESERVE_FD
  -> remove_cpu(cpu)
  -> cpuhp_ap_report_dead()
  -> cpu_preserved_park():
       switch SP & CR3/TTBR1
       complete(&parked_done)
  -> set_cpu_present(cpu, false)
                                 cpu_preserved_main()
                                 runs continuously in    early boot:
                                 isolated address space    cpu_preserved_flb_retrieve()
                                                           set_cpu_present(cpu, false)
                                                         smp_init():
                                                           skips !cpu_present(cpu)
                                                         LIVEUPDATE_SESSION_FINISH:
                                                           signal exit_requested
                                                           set_cpu_present(cpu, true)
                                                           add_cpu(cpu) -> online
```

1. **Hotplug Interception**:
   During `cpu_preserve_luo_preserve()`, the kernel marks the CPU preserved and
   calls `remove_cpu(cpu)`. Standard Linux CPU hotplug migrates all tasks, timers,
   and interrupts off the core until the CPU reaches the terminal offline hook in
   `cpuhp_ap_report_dead()`. Instead of invoking `arch_cpu_idle_dead()` (which
   would place the core in an ACPI/PSCI sleep state), `cpuhp_ap_report_dead()`
   calls `cpu_preserved_park()`.
2. **Isolating via `!cpu_present(cpu)`**:
   Once the CPU has switched onto its preserved stack and isolated page tables
   and signaled `parked_done`, the outgoing kernel calls
   `set_cpu_present(cpu, false)`.
   - In the **outgoing kernel**, marking the CPU neither online nor present
     ensures that `reboot` / `kexec` shutdown paths (`smp_send_stop()`,
     `native_stop_other_cpus()`) skip sending `REBOOT_VECTOR` or `STOP` IPIs to
     the preserved core.
   - Across `kexec`, a minimal LUO FLB global structure (`"cpu_flb_v1"`,
     `struct cpu_preserved_global_ser`) carries the `cpumask` of preserved CPUs.
     During early boot in the **incoming kernel**, `cpu_preserved_flb_retrieve()`
     reads this mask and calls `set_cpu_present(cpu, false)` *before*
     `smp_init()` runs. Because `smp_init()` only brings up CPUs in
     `cpu_present_mask`, the incoming kernel skips sending `INIT`/`SIPI` (x86)
     or `CPU_ON` PSCI calls (ARM64) to preserved cores without requiring
     architecture-specific changes in the SMP boot path.
3. **Reclamation**:
   When the live update completes (`LIVEUPDATE_SESSION_FINISH`) or is cancelled,
   `cpu_preserve` sets `exit_requested` on the preserved CPU, sends a wakeup IPI
   (`arch_cpu_preserved_kick()`), waits for the CPU to exit
   `cpu_preserved_main()` into its terminal offline halt state, restores
   `set_cpu_present(cpu, true)`, and calls `add_cpu(cpu)` to bring the core back
   into the host scheduler.

### 4.3 Isolated Address Space (`struct cpu_preserved_as`) and Stack Context

Before `kexec` overwrites the outgoing kernel's page tables, each preserved CPU
switches its MMU root (`CR3` on x86, `TTBR1_EL1`/`TTBR0_EL2` on ARM64) to an
isolated address space (`struct cpu_preserved_as`).

The isolated page tables map **only**:
- `.text.cpu_preserved` (`PAGE_KERNEL_ROX`)
- `.data.cpu_preserved` (`PAGE_KERNEL`)
- The preserved `pcpus` status array and per-CPU preserved stacks
- Explicitly mapped workload buffers registered into the session's address space
  via `oncore_session_map_range()` (`cpu_preserved_as_map()`)

No other host kernel memory (neither the kernel linear direct map `PAGE_OFFSET`,
nor `vmalloc`, nor normal `.text`/`.data`) is mapped in `cpu_preserved_as`. Page
tables are constructed using `kernel_ident_mapping_init()` (extended with
`force_pte = true` and `offset = va - pa` for 4KB page granularity) on x86 and
`trans_pgd_map_range()` on ARM64. All page-table pages allocated for the isolated
address space are recorded in `struct cpu_preserved_as_ser` so the incoming
kernel can adopt (`cpu_preserved_as_adopt()`) and free them
(`cpu_preserved_as_destroy()`) after the CPU returns to normal operation.

To avoid relying on host per-CPU offset registers (`%gs` on x86 or
`TPIDR_EL1`/`TPIDR_EL2` on ARM64), which may be clobbered by guest execution or
differ across kernels, per-CPU metadata (`struct cpu_preserved_stack_context`) is
placed at the base of the power-of-two aligned preserved stack (4KB on x86, 16KB
on ARM64) and located in $O(1)$ time by masking the stack pointer:
`sp & ~(CPU_PRESERVED_STACK_SIZE - 1)`.

---

## 5. Layer 3: On-Core Execution and Scheduling Framework (`oncore`)

The `oncore` subsystem (`kernel/liveupdate/oncore.c`, `include/linux/oncore.h`,
`CONFIG_LIVEUPDATE_ONCORE`) sits between raw physical CPU preservation and
higher-level workloads such as KVM.

### 5.1 Sessions and Jobs

- **`struct oncore_session`**: Bound 1:1 to a `struct luo_session`. It owns the
  session's `struct cpu_preserved_as` isolated address space, tracks the
  `cpumask` of physical CPUs preserved in that session, and embeds a shared
  `struct oncore_runqueue`.
- **`struct oncore_job`**: Represents an independent unit of work (such as a
  single preserved KVM vCPU) queued onto an `oncore_session`. A job defines three
  callbacks that must reside in `.text.cpu_preserved`:
  - `enum oncore_exit_reason (*run)(struct oncore_job *job, u64 deadline)`
  - `void (*pause)(struct oncore_job *job)`
  - `void (*resume)(struct oncore_job *job)`

### 5.2 Cooperative / Time-Sliced Round-Robin Scheduling

Each preserved physical CPU in an `oncore_session` executes `oncore_cpu_run()`
from inside `cpu_preserved_main()`:

1. **Hardware Counter Deadlines**:
   Because standard kernel timers, `jiffies`, and softirqs are unavailable,
   `oncore` measures scheduling quantums (`oncore.quantum_ms`, defaulting to
   10ms) directly in hardware counter ticks (`rdtsc()` on x86,
   `__arch_counter_get_cntpct()` on ARM64) and passes an absolute `deadline` tick
   count to `job->run(job, deadline)`. The workload is responsible for returning
   at or before `deadline` (for example, by arming the VMX preemption timer,
   host LAPIC timer, or ARM64 EL2 physical timer).
2. **1:1 Fast-Path Continuation vs. $M > N$ Multiplexing**:
   - When $M \le N$ (e.g., 4 vCPUs on 4 preserved physical CPUs) and the shared
     runqueue is empty (`!rq->head`), the CPU keeps `current_job` loaded across
     consecutive quantums without acquiring runqueue locks or invoking
     `job->pause()` / `job->resume()`.
   - When $M > N$ (oversubscription), jobs rotate through the FIFO runqueue: at
     the end of a quantum, the CPU calls `job->pause(current_job)` (saving hardware
     context), enqueues `current_job` at the tail, dequeues the next runnable job,
     and calls `job->resume(next_job)`.
   - The dequeue logic (`runqueue_dequeue_for_cpu()`) prioritizes: (a) jobs with
     `total_runs == 0` to prevent starvation during initial startup, (b) jobs
     whose `preferred_cpu` matches the current physical CPU to preserve cache and
     VMCS/VMCB locality, and (c) work-stealing from the head of the queue.
3. **Exit Reasons and Low-Power Stall Backoff**:
   - `ONCORE_EXIT_HEALTHY`: The job ran until its quantum deadline and remains
     runnable.
   - `ONCORE_EXIT_STALL`: The job encountered a condition it cannot resolve
     during the `kexec` window (such as an unhandled VM-exit) and yielded early.
     The job remains in the runqueue so it can be re-polled or aborted when the
     incoming kernel attaches; if all queued jobs return `ONCORE_EXIT_STALL`
     across a full pass, the physical CPU enters a bounded low-power wait
     (`arch_cpu_preserved_park_wait()`, ~1ms `tpause`/`mwait`/`wfe`/timer sleep)
     to avoid burning power and memory bandwidth.
   - `ONCORE_EXIT_ABORT`: The job has been reclaimed by the incoming kernel (or
     cancelled) and is permanently removed from the runqueue.

---

## 6. Layer 4: KVM Caretaker Architecture (`caretaker`)

The KVM Caretaker engine (`virt/kvm/caretaker.c`, `include/linux/kvm_caretaker.h`,
`CONFIG_KVM_CARETAKER`) bridges KVM `vcpufd` preservation (Layer 1) with the
`oncore` scheduler (Layer 3).

When `KVM_CAP_CARETAKER` is enabled on a VM and a `vcpufd` is preserved into a
LUO session that also contains preserved physical CPUs, KVM initializes a
`struct kvm_caretaker_vcpu` and registers an `oncore_job` with the session.

### 6.1 Cross-Kexec ABI Invariant vs. Private Runtime Pages

A design rule in RFCv1 is that **the incoming kernel must never read or depend
on outgoing-kernel internal structures**. Only structures defined in
`include/linux/kho/abi/` may cross the `kexec` boundary:

- **Cross-Kernel KHO ABI (`include/linux/kho/abi/kvm.h`)**:
  - `struct kvm_caretaker_cb_ser`: Contains the lockless `state`
    (`enum kvm_caretaker_state`), the current physical CPU (`pcpu`), and embedded
    telemetry (`struct kvm_caretaker_telemetry_ser`).
  - `struct kvm_caretaker_arch_ser`: Contains `struct kvm_caretaker_cb_ser cb` at
    offset 0, plus only the minimal physical addresses required by the incoming
    kernel during hardware state adoption (for example, `vmcs_pa` on Intel VMX).
- **Outgoing-Kernel Private Runtime Page**:
  Each architecture allocates a KHO-preserved runtime context page (`struct
  caretaker_x86_page`, `struct caretaker_vmx_page`, `struct caretaker_svm_page`,
  or `struct caretaker_arm64_page`) that is mapped into the `oncore_session`'s
  isolated address space (`cpu_preserved_as`).
  This page embeds `struct kvm_caretaker_arch_ser abi` at **offset 0**,
  while the rest of the page holds private runtime state (saved host registers,
  standalone GDT/IDT/TSS, exception stacks, scratch variables) that is accessed
  *exclusively* by the outgoing kernel's `.text.cpu_preserved` code. When the
  incoming kernel boots, it casts `caretaker_phys` only to
  `struct kvm_caretaker_cb_ser *` (or `struct kvm_caretaker_arch_ser *`) and
  frees the raw page once the vCPU transitions to `KVM_CARETAKER_STOPPED`.

### 6.2 Lockless Cross-Kernel State Machine

Handoff between the preserved physical CPU (running the outgoing kernel's
`.text.cpu_preserved` code) and the incoming kernel (running `.retrieve()` in a
normal task context) is coordinated via atomic `cmpxchg()` transitions on
`cb->state`:

```
                      oncore_job->run() / resume()
              +------------------------------------------+
              |                                          v
    +-----------------------+                  +-----------------------+
    | KVM_CARETAKER_PAUSED  |                  | KVM_CARETAKER_RUNNING |
    |          (0)          |                  |          (1)          |
    +-----------------------+                  +-----------------------+
       |                 ^                        |                 |
       |                 | detach_serialize()     |                 |
       |                 +------------------------+                 |
       |                   quantum end / pause()                    |
       |                                                            |
       | Incoming kernel attach:            Incoming kernel attach: |
       | cmpxchg(PAUSED -> STOPPED)         cmpxchg(RUNNING ->      |
       | (Immediate 0ns reclaim)                    STOPPING)       |
       |                                    + kick(cb->pcpu)        |
       |                                                            v
       |                                       +-----------------------+
       |                                       | KVM_CARETAKER_STOPPING|
       |                                       |          (2)          |
       |                                       +-----------------------+
       |                                                            |
       |                                      Caretaker VM-exits,   |
       |                                      detach_serialize(),   |
       |                                      sets STOPPED          |
       v                                                            v
    +------------------------------------------------------------------+
    |                     KVM_CARETAKER_STOPPED (3)                    |
    |          (ser->arch_state is complete; incoming KVM owns vCPU)   |
    +------------------------------------------------------------------+
```

A key invariant of this state machine is **proactive serialization at quantum
boundaries**:

- Before transitioning from `KVM_CARETAKER_RUNNING` to `KVM_CARETAKER_PAUSED`
  (whether due to `oncore` quantum rotation or an `ONCORE_EXIT_STALL`), the
  preserved CPU calls `ops->detach_serialize(c_vcpu, ser->arch_state)`, flushing
  any live hardware registers back into the uAPI-backed `struct kvm_vcpu_arch_ser`
  buffer, followed by `smp_wmb()`.
- Therefore, whenever `cb->state == KVM_CARETAKER_PAUSED`, `ser->arch_state` is
  guaranteed to hold the complete, up-to-date architectural state of the vCPU.
- When the incoming kernel calls `kvm_caretaker_wait_for_attach()`:
  - **Fast Path (`PAUSED -> STOPPED`)**: If `cb->state` is `PAUSED`, a single
    `cmpxchg(&cb->state, KVM_CARETAKER_PAUSED, KVM_CARETAKER_STOPPED)` claims the
    vCPU immediately without waiting for or kicking any physical CPU. When the
    `oncore` scheduler next inspects the job, it observes `STOPPED` and returns
    `ONCORE_EXIT_ABORT`.
  - **Active Path (`RUNNING -> STOPPING -> STOPPED`)**: If `cb->state` is
    `RUNNING`, the incoming kernel executes
    `cmpxchg(&cb->state, KVM_CARETAKER_RUNNING, KVM_CARETAKER_STOPPING)` and
    sends a physical IPI (`arch_cpu_preserved_kick(pcpu)`). The IPI forces an
    immediate VM-exit on the preserved CPU; the Caretaker loop sees `STOPPING`,
    calls `ops->detach_serialize(c_vcpu, ser->arch_state)`, sets `cb->state` to
    `KVM_CARETAKER_STOPPED`, and returns `ONCORE_EXIT_ABORT`.

### 6.3 Architecture Backends in RFCv1

In RFCv1, each supported architecture implements `struct kvm_caretaker_ops`
(`init_vcpu`, `prepare_vcpu`, `unprepare_vcpu`, `sync_vcpu`, `destroy_vcpu`, and
`.text.cpu_preserved` callbacks `vcpu_load`, `vcpu_put`, `vcpu_run`,
`handle_exit`, and `detach_serialize`):

- **x86 Common (`arch/x86/kvm/caretaker.c`)**:
  Allocates `struct caretaker_x86_page`, builds standalone GDT, TSS, and IDT
  tables (so any NMI or exception in host mode lands in a self-contained handler
  rather than the torn-down Linux IDT), manages guest FPU state via `XRSTOR64` /
  `XSAVE64` directly against `ser->arch_state->xsave.region`, and programs the
  host local APIC timer to service guest `MSR_IA32_TSC_DEADLINE` or `APIC_TMICT`
  deadlines.
- **Intel VMX (`arch/x86/kvm/vmx/caretaker.c`, `caretaker_vmenter.S`)**:
  Preserves `vmcs01` (`vmcs`, `msr_bitmap`, `pml_pg`, `ve_info`) across `kexec`,
  reprograms `HOST_CR3` to the isolated `cpu_preserved_as` CR3 and `HOST_RIP` to
  `vmx_caretaker_exit_handler`, and uses the hardware VMX preemption timer to
  enforce `oncore` quantum deadlines. Upon adoption in the incoming kernel
  (`vmx_caretaker_sync_vcpu`), because `struct loaded_vmcs` is internal to each
  kernel build, the incoming kernel allocates a new `loaded_vmcs`, temporarily
  loads the preserved `abi->vmcs_pa` via `vmptrld`, copies the guest-visible
  VMCS fields (`vmx_caretaker_guest_fields[]`) into the new VMCS, and applies
  `ser->arch_state`.
- **AMD SVM (`arch/x86/kvm/svm/caretaker.c`, `caretaker_vmenter.S`)**:
  Copies `vmcb01` into the preserved `caretaker_svm_page->vmcb` and allocates a
  dedicated preserved `hsave_area` (`MSR_VM_HSAVE_PA`). Uses the host LAPIC timer
  combined with `INTERCEPT_INTR` to preempt guest execution at the `oncore`
  quantum deadline, and synchronizes dirty VMCB save-area fields (`rip`, `rsp`,
  `rax`, `rflags`, control registers, segment descriptors, `v_tpr`) back into
  `ser->arch_state` on detach.
- **ARM64 VHE (`arch/arm64/kvm/caretaker.c`, `caretaker_vmenter.S`)**:
  Installs a standalone EL2 vector table (`caretaker_hyp_vector` in `VBAR_EL2`),
  context-switches EL1 system registers, FP/SIMD state, Pointer Authentication
  keys, Stage-2 MMU registers (`VTCR_EL2`, `VTTBR_EL2`), VGICv3 CPU interface
  registers (`ICH_LR<n>_EL2`, `ICH_AP*R<n>_EL2`, `ICH_VMCR_EL2`, `ICH_HCR_EL2`),
  and the virtual timer (`CNTV_CTL_EL0`, `CNTV_CVAL_EL0`). Enforces `oncore`
  quantums via the EL2 physical timer (`CNTHP_CVAL_EL2` / `CNTHP_CTL_EL2`) and
  writes dirty hardware state back into the `kvm_one_reg` entries of
  `ser->arch_state` before transitioning out of `RUNNING`.

### 6.4 Gap Telemetry and Observability

Because standard host tracing (`ftrace`, `perf`, `bpf`) cannot run on isolated
CPUs during `kexec`, `struct kvm_caretaker_telemetry_ser` in the KHO ABI records
per-vCPU activity during the gap:

- Execution counts (`entries`, `pause_count`, `resume_count`, `quantum_exits`)
- Hardware timestamp counter accumulations (`run_ticks`, `pause_ticks`,
  `stall_ticks`)
- Per-reason exit counters (`exit_counts[64]`) and `last_exit_reason`

When the incoming kernel adopts the vCPU, it copies this telemetry from the KHO
ABI struct and exposes it under
`/sys/kernel/debug/kvm/<vm>/vcpu<N>/caretaker_telemetry`.

---

## 7. Open Design Challenges and Topics for Further Discussion

While the [RFCv1 patch series](https://lore.kernel.org/all/20260920193650.3373435-1-pasha.tatashin@soleen.com)
demonstrates end-to-end continuous vCPU execution across `kexec` on Intel VMX,
AMD SVM, and ARM64 VHE, it is an initial proof-of-concept. Several architectural
questions, both from the [initial proposal discussion](https://lore.kernel.org/all/afEwWZksU0Fw61oT@plex)
and from prototyping RFCv1, still need to be addressed or discussed further with
upstream maintainers.

### 7.1 VM-Exit Handling Strategy and Sharing Code with KVM

A central open design question is how the Caretaker should enter the guest and
handle VM-exits without duplicating KVM into a second in-kernel hypervisor.

In the [initial LKML discussion](https://lore.kernel.org/all/afEwWZksU0Fw61oT@plex),
Paolo Bonzini noted that the Caretaker is the non-preemptible inner part of
`vcpu_enter_guest()`, and that `vmx_exit_handlers_fastpath()` /
`svm_exit_handlers_fastpath()` already provide a blueprint for exits that can
be handled with interrupts disabled.

#### Specific VM-Exits Are Not Architectural (Zero-Exit Baseline)

First, it is important to separate the **Caretaker execution mechanism** from
**which specific VM-exits are emulated during the gap**.

In RFCv1, the architecture backends implement a small set of exit handlers
(such as 8250 COM1 UART TX/RX polling, `CPUID`, `RDTSC`, and basic MSR or GICv3
SGI accesses) so that interactive console demos and test workloads can run
during `kexec`. However, those specific exit handlers are **not** essential to
the core architecture and do not need to be part of initial upstreaming.

An initial upstream merge can implement **zero VM-exit emulation**:
- As long as a guest vCPU is executing compute instructions in guest mode (or
  handling timers and interrupts in silicon via hardware virtualization
  features), it continues running across `kexec` without exiting.
- If the guest triggers *any* synchronous VM-exit (other than `oncore` quantum
  preemption), the Caretaker does not attempt to decode or emulate the instruction;
  it serializes state, returns `ONCORE_EXIT_STALL`, and waits in a low-power
  loop until the incoming kernel finishes booting and reclaims the vCPU, where
  standard KVM handles the pending exit normally.

Even with a zero-exit or minimal-exit baseline, however, the Caretaker still
requires world-switch assembly (`caretaker_vmenter.S`), guest/host register context
switching, and hardware control structure (`VMCS` / `VMCB` / EL2 sysreg)
management. In RFCv1, this code is duplicated rather than shared with
`vcpu_enter_guest()` and `vmx/svm_exit_handlers_fastpath()`.

#### Why Sharing Code Directly with KVM Is Problematic

Directly calling `vcpu_enter_guest()`, `vmx_vcpu_run()`, `svm_vcpu_run()`, or
`vmx/svm_exit_handlers_fastpath()` from the Caretaker runs into four
constraints:

1. **Section and Compiler Isolation (`.text.cpu_preserved`)**:
   Any code executed while the outgoing kernel is torn down must reside in
   `.text.cpu_preserved` (so it is relocated outside KHO Scratch and mapped into
   `cpu_preserved_as`) and must be compiled without stack protectors, jump
   tables, ftrace/mcount hooks, sanitizers, or retpolines/return thunks.
   Annotating existing functions across `arch/x86/kvm/` or `arch/arm64/kvm/` with
   `__cpu_preserved_text` is fragile: if a shared KVM function makes even one
   transitive call to an unannotated helper (`WARN_ON_ONCE()`, `printk()`,
   tracepoints, `static_branch_unlikely()`, `rcu_read_lock()`, or a
   compiler-generated out-of-line library routine), the preserved CPU will jump
   into unmapped memory during `kexec` and fault.
2. **Deep Coupling to `struct kvm_vcpu` and `struct kvm`**:
   KVM's entry/exit paths and fastpath handlers take `struct kvm_vcpu *` and
   dereference `vcpu->kvm`, `vcpu->arch`, `mmu`, `apic`, `memslots`, and host
   kernel heap pointers (`kmalloc`/`vmalloc`). Those structures live in the
   host kernel's linear direct map (`PAGE_OFFSET`), which is intentionally
   unmapped in `cpu_preserved_as` to guarantee memory isolation between the
   preserved CPUs and the booting kernel.
3. **Host OS Runtime Assumptions**:
   Standard KVM code assumes a valid Linux task context (`current`), host per-CPU
   variables (`%gs` on x86, `TPIDR_EL1` on ARM64), preemption tracking, lockdep,
   and RCU. On a preserved CPU that is offline and `!cpu_present(cpu)`, running
   on a standalone 4KB/16KB stack across `kexec`, none of those facilities exist.
4. **Cross-Kernel Struct Layout Drift**:
   Internal kernel structures (`struct kvm_vcpu`, `struct vcpu_vmx`,
   `struct vcpu_svm`) are not an ABI and can change size or field offsets between
   the outgoing and incoming kernels. While the outgoing kernel's
   `.text.cpu_preserved` only executes until `KVM_CARETAKER_STOPPED`, the handoff
   boundary to the incoming kernel must still translate into a fixed KHO ABI
   (`struct kvm_vcpu_arch_ser`).

#### Candidate Approaches for Code Sharing

We must converge with upstream maintainers on how to balance code reuse against
isolation safety. The primary options under consideration are:

- **Option 1: Strict Zero/Minimal Exit Policy + Hardware Virtualization Offload**
  - Keep the Caretaker restricted to world-switch entry/exit and quantum
    preemption, and rely on hardware virtualization features (Intel APICv / Posted
    Interrupts / IPI virtualization, AMD AVIC, ARM64 GICv4.1 direct vLPI/vSGI
    injection, and direct timer passthrough) to keep guests running without VM-exits.
    Any guest action that forces a software VM-exit stalls
    (`ONCORE_EXIT_STALL`) until the incoming kernel re-attaches.
  - *Trade-off*: Eliminates exit-handler duplication and keeps the preserved
    attack surface minimal, though each architecture still needs a small
    world-switch assembly stub (or shared macro) and workloads without hardware
    interrupt/timer offload will stall upon their first timer or IPI exit.
- **Option 2: Refactoring Low-Level World-Switch Macros and Stateless Primitives**
  - Factor KVM's low-level assembly (`vmx/vmenter.S`, `svm/vmenter.S`, ARM64 hyp
    entry) and leaf register/VMCS/VMCB helpers so they operate on a compact,
    self-contained register/hardware-context struct (embedded in both
    `struct kvm_vcpu_arch` and the Caretaker page) rather than taking a full
    `struct kvm_vcpu *`.
  - Share those routines via assembly macros or `static __always_inline` header
    helpers that compile into both standard `.text` and `.text.cpu_preserved`,
    paired with `objtool` verification that `.text.cpu_preserved` contains no
    relocations to external sections.
  - *Trade-off*: Addresses duplicate `vmenter.S` and low-level register
    save/restore logic without pulling `struct kvm_vcpu` or host heap dependencies
    into the isolated address space, at the cost of refactoring KVM's low-level
    entry/exit assembly.
- **Option 3: Adopting the ARM64 KVM `nvhe` Dual-Compilation Model**
  - ARM64 KVM already solves a similar isolation problem for nVHE/pKVM EL2
    (`arch/arm64/kvm/hyp/nvhe/`), where shared KVM C/assembly files are compiled
    a second time with dedicated flags (`-fno-stack-protector`, disabled
    instrumentation, symbol prefixing) into an isolated linker section
    (`.hyp.text`) with build-time checks prohibiting references to host kernel
    symbols.
  - Applying this pattern to `.text.cpu_preserved` would allow compiling shared,
    self-contained KVM world-switch and fastpath translation units into both KVM
    and Caretaker with compiler- and `objtool`-enforced isolation.
  - *Trade-off*: Established upstream pattern on ARM64, though introducing a
    similar split-compilation boundary into `arch/x86/kvm/` requires refactoring.
- **Option 4: Mapping Outgoing `struct kvm_vcpu` into `cpu_preserved_as` During the Gap**
  - Because the code running in `.text.cpu_preserved` during the `kexec` window
    belongs to the *outgoing* kernel image, its struct layout matches the
    outgoing kernel's `struct kvm_vcpu`. If `struct kvm_vcpu` (and its immediate
    sub-structures) are preserved in KHO and mapped into `cpu_preserved_as`,
    outgoing `.text.cpu_preserved` code can operate on `struct kvm_vcpu *`
    during the gap and serialize into the versioned KHO ABI
    (`struct kvm_vcpu_arch_ser`) only when transitioning to `KVM_CARETAKER_STOPPED`
    for the incoming kernel.
  - *Trade-off*: Avoids inventing parallel per-arch context structs for the
    runtime loop, but still requires auditing every shared function to ensure it
    never chases unmapped pointers (`vcpu->kvm`, `memslots`), touches `current` or
    per-CPU variables, or invokes unpreserved kernel helpers.

### 7.2 Unresolved Items from the Initial LKML Discussion

Several points raised during the [initial proposal discussion](https://lore.kernel.org/all/afEwWZksU0Fw61oT@plex)
were either deferred in RFCv1 or implemented differently and warrant further
discussion:

1. **Pre-`kexec` Memory Invalidation and MMU Notifiers While Detached**:
   Once `LIVEUPDATE_SESSION_PRESERVE_FD` is called on a `vcpufd`, the vCPU
   begins executing inside the Caretaker on an offlined (`!cpu_present`)
   physical CPU while the outgoing kernel and userspace are still running prior
   to `kexec`. As Paolo Bonzini pointed out, if a buggy or malicious userspace
   process performs an operation that invalidates guest mappings before `kexec`
   (such as `madvise(MADV_FREE)` or `munmap()`), standard KVM MMU notifiers and
   `kvm_flush_remote_tlbs()` will not kick the detached Caretaker CPU. The
   outgoing kernel's MMU notifier path must be wired to force an immediate
   VM-exit via IPI (`KVM_CARETAKER_STOPPING`) and abort Caretaker execution if
   secondary page tables are invalidated while detached.
2. **Kernel Remote Trace Ring Buffers vs. Custom Telemetry Struct**:
   RFCv1 uses a custom `struct kvm_caretaker_telemetry_ser` in the KHO ABI
   exposed via debugfs (`caretaker_telemetry`). In the initial discussion, Paolo
   suggested using the kernel's remote trace ring buffers (introduced for
   hypervisor/pKVM tracing) instead: the outgoing kernel allocates a remote ring
   buffer preserved via KHO, the Caretaker acts as a lockless producer writing
   trace events during the gap, and the incoming kernel attaches the buffer back
   to the tracing subsystem upon retrieval.
3. **`HLT` / Idle Exit Handling and `oncore` $M > N$ Multiplexing vs. 1:1 Pinning**:
   In the initial discussion, it was suggested that initial support should assume
   a 1:1 mapping of active vCPUs to isolated physical CPUs, and that `HLT` exits
   during the gap should either be skipped or have their hardware intercepts
   disabled (`HLT`/`PAUSE`/`MONITOR`/`MWAIT`, e.g. via
   `KVM_CAP_X86_DISABLE_EXITS`). RFCv1 instead introduced the `oncore` scheduler
   to support $M > N$ oversubscription and kept `HLT`/`WFI` intercepts enabled
   so an idle vCPU yields its quantum to other runnable vCPUs on the same pCPU.
   Whether $M > N$ multiplexing during the brief `kexec` window justifies the
   complexity of `oncore`, versus enforcing 1:1 vCPU-to-pCPU pinning (and leaving
   any excess vCPUs suspended in RAM across `kexec`) with idle intercepts
   disabled, should be decided with maintainers.
4. **Moving APIC Emulation into Standard KVM Fastpaths**:
   For multi-vCPU guests without hardware IPI virtualization (Intel APICv/IPIv,
   AMD AVIC), moving a subset of APIC emulation (such as `APIC_ICR` / IPI
   delivery) into `vmx_exit_handlers_fastpath()` and
   `svm_exit_handlers_fastpath()` was proposed as a standalone improvement that
   benefits both normal KVM and the Caretaker. In RFCv1, ARM64 emulates
   `ICC_SGI1R_EL1` in the Caretaker, whereas x86 currently treats x2APIC
   `APIC_ICR` writes as a blocking exit (`ONCORE_EXIT_STALL`).
5. **Decoupling Asynchronous vCPU Execution from `kexec` (`KVM_RUN_ASYNC` / VMM Upgrade)**:
   Paolo Bonzini and David Woodhouse discussed decoupling detached vCPU execution
   from `kexec` so that a userspace VMM can detach, restart or upgrade itself,
   and re-attach to running vCPUs without a kernel reboot, potentially staged as:
   (a) `ioctl(KVM_RUN_ASYNC)` running the vCPU in a `struct vhost_task` kernel
   thread, (b) `vmfd`/`vcpufd` handoff to a new `mm`, (c) ASI during normal
   `KVM_RUN` (enabled via a module parameter), and (d) `kexec` serialization on
   top. RFCv1 supports intra-kernel start/cancel loops through LUO session
   preservation and retrieval, but does not implement `KVM_RUN_ASYNC`
   (`vhost_task`) or always-on ASI during normal `KVM_RUN`.
6. **Cross-Kernel Feature and Capability Negotiation on Reattachment**:
   When the incoming kernel adopts a preserved VM during `.retrieve()`, it needs
   formal feature negotiation to verify that the new kernel and KVM module
   support all hardware capabilities and register states used by the preserved
   VM, terminating the VM (or aborting before the point of no return) if an
   incompatibility is detected.

### 7.3 Bare-Metal Platform Considerations

Running preserved physical CPUs across a bare-metal `kexec` introduces two
platform-level considerations beyond virtualized test environments:

1. **CPU Enumeration Stability and Hardware Identifier Validation**:
   - **Logical CPU Stability Across `kexec`**: RFCv1 indexes `pcpus[]` by Linux
     logical CPU ID (`0 .. nr_cpu_ids - 1`) and passes logical CPU bitmasks in
     `cpu_flb_v1` and `oncore_session_ser`. Just as PCI/VFIO Live Update
     (`pci_flb_v1`) relies on PCI `domain:bus:dev.fn` (BDF) stability across
     `kexec`, logical CPU numbering is invariant across a standard `kexec`
     because platform firmware does not re-run and the incoming kernel parses
     the exact same in-memory ACPI MADT or Device Tree tables in the same
     deterministic order (with the same bootstrap processor, CPU 0).
   - **How Hardware ID Validation Can Be Added**: To guard against kernel
     command-line changes (such as `maxcpus=` or `possible_cpus=`) or future
     changes in kernel topology enumeration order between the outgoing and
     incoming kernels, `struct cpu_preserved_pcpu_ser` (`cpu_flb_v1`) and
     `struct cpu_preserved_file_ser` (`cpu_fh_v1`) can record the hardware CPU
     identifier alongside the logical CPU index (`x2APIC ID` /
     `cpu_physical_id(cpu)` on x86, and `MPIDR_EL1 & MPIDR_HWID_BITMASK` /
     `cpu_logical_map(cpu)` on ARM64). During `cpu_preserved_flb_retrieve()` and
     `cpu_preserve_retrieve()`, the incoming kernel can cross-check that its
     `setup_arch()` logical-to-physical mapping for `cpu` matches the preserved
     hardware ID before adopting the core.

2. **Firmware and Platform Events (SMIs, MCEs, and ARM64 SErrors)**:
   - **C-State Control and Standalone Exception Vectors**: Deep C-state
     transitions that could depend on ACPI power management state are avoided by
     using bounded `tpause`/`mwait`/`wfe` waits during `ONCORE_EXIT_STALL` and
     requesting maximum autonomous performance (`MSR_HWP_REQUEST`,
     `MSR_IA32_ENERGY_PERF_BIAS`, `MSR_AMD_CPPC_REQ`) before entering the
     preserved runtime. Both architectures switch to standalone descriptor and
     vector tables in `.text.cpu_preserved` / `.data.cpu_preserved`
     (`x86_preserved_idt` on x86, `caretaker_hyp_vector` in `VBAR_EL2` on
     ARM64) so an asynchronous interrupt or exception during `kexec` never
     vectors into torn-down host kernel text.
   - **How Remaining Bare-Metal Event Corner Cases Can Be Solved**:
     - *SMIs (x86)*: System Management Interrupts are serviced transparently by
       platform firmware in SMM (out of BIOS-reserved SMRAM) and resume via
       `RSM`. On Intel VMX, SMM returns directly to the guest or Caretaker. On
       AMD SVM, if `INTERCEPT_SMI` is inherited from `vmcb01`, the CPU triggers
       a `VMEXIT` (`SVM_EXIT_SMI`) after `RSM` completes; adding `SVM_EXIT_SMI`
       to `svm_caretaker_decode_exit()` (or clearing `INTERCEPT_SMI` in the
       Caretaker VMCB) allows the Caretaker to resume the guest immediately
       rather than treating the exit as unhandled (`ONCORE_EXIT_STALL`).
     - *MCEs and Synchronous Host Faults (x86)*: In RFCv1, vectors `0..31` in
       `x86_preserved_idt` return immediately via `iretq`. While `NMI` (vector
       2) must `iretq` because x86 uses NMI IPIs to wake preserved cores, a
       Machine Check Exception (`#MC`, vector 18) or synchronous fault (`#DF`,
       `#GP`, `#PF`) in preserved host context would re-fault in a loop or
       triple-fault if `MSR_IA32_MCG_STATUS.RIPV == 0`. Pointing `#MC` and
       synchronous fault vectors in `x86_preserved_idt` to a dedicated preserved
       park stub (which clears `MSR_IA32_MCG_STATUS`, records fault telemetry,
       and parks the core in the exit-check loop) prevents a localized fault on
       one preserved core from resetting the machine during `kexec`.
     - *ARM64 SErrors and Unconditional `VBAR_EL2` Isolation*: On ARM64,
       lower-EL SErrors (`caretaker_el1_error`) already exit guest mode, save
       vCPU state, and stall the vCPU until the incoming kernel re-attaches,
       while EL2 synchronous exceptions and EL2 SErrors vector to
       `arm64_caretaker_handle_invalid()`, which records `ELR_EL2`, `ESR_EL2`,
       and `FAR_EL2` and parks the core in a `wfe()` loop until reclaimed.
       Moving a minimal default preserved vector table into
       `arch/arm64/kernel/preserve_cpu.S` ensures `VBAR_EL2` and `VBAR_EL1` are
       always isolated in `arch_cpu_preserved_park_init()` even when a core is
       preserved without `CONFIG_KVM_CARETAKER`.

### 7.4 Nested Virtualization Support

In RFCv1, preserving a guest VM that itself uses nested virtualization (an L1
guest hypervisor running L2 VMs) is explicitly rejected with `-EOPNOTSUPP`:
- **x86**: `kvm_arch_vcpu_luo_preserve()`, `vmx_caretaker_init()`, and
  `svm_caretaker_init_page()` reject vCPUs currently in L2 guest mode
  (`is_guest_mode(vcpu)`), and `struct kvm_vcpu_arch_ser` does not serialize
  `struct kvm_nested_state`.
- **ARM64**: `arm64_kvm_caretaker_preserve()` rejects vCPUs configured with
  `KVM_ARM_VCPU_HAS_EL2` (`vcpu_has_nv(vcpu)`).

Adding nested virtualization support to OrphanVM divides into two layers (KHO
state preservation and Caretaker runtime execution):

1. **KHO Architectural and MMU State Preservation**:
   - **x86 (`struct kvm_nested_state`)**: KVM already provides internal helpers
     backing `KVM_GET_NESTED_STATE` and `KVM_SET_NESTED_STATE`
     (`kvm_nested_ops.get_state()` and `kvm_nested_ops.set_state()`), which
     serialize the complete nested state (~4-8 KB per vCPU), including guest-mode
     and pending-entry flags (`KVM_STATE_NESTED_GUEST_MODE`,
     `KVM_STATE_NESTED_RUN_PENDING`, `KVM_STATE_NESTED_GIF_SET`), `vmxon_pa` /
     `current_vmptr` (VMX) or `hsave_msr` / `vmcb12_gpa` (SVM), and the cached
     `vmcs12` / `vmcb12` (plus `shadow_vmcs12`). Appending
     `struct kvm_nested_state` to `struct kvm_vcpu_arch_ser` and invoking
     `kvm_nested_ops.get_state()` on `.preserve()` and
     `kvm_nested_ops.set_state()` on `.retrieve()` (prior to restoring segment
     registers and MSRs) preserves the L1/L2 hypervisor state across `kexec`.
   - **ARM64 (`FEAT_NV` / `FEAT_NV2`)**: Virtual EL2 system registers
     (`VNCR_EL2` and trapped EL2 state in `vcpu->arch.ctxt`) are already exposed
     through the `kvm_one_reg` interface and enumerated by
     `kvm_arm_get_sys_reg_indices()` into `state->sysregs[]`.
   - **Nested Shadow TDP / Stage-2 Page Tables**: When a vCPU is executing in L2
     (`is_guest_mode(vcpu)`), hardware two-dimensional paging (`vmcs02.EPTP`,
     `vmcb02.ncr3`, or nested `VTTBR_EL2`) points to a shadow TDP root that
     composes L1's EPT/NPT/Stage-2 mappings with L0's Stage-2 mappings. To keep
     an L2 vCPU running in the Caretaker across `kexec`, `kvm_mmu_preserve_kho()`
     (x86) and `kvm_arch_vm_luo_preserve()` (ARM64) must also walk and preserve
     the active nested shadow TDP page-table pages via `kho_preserve_folio()`.

2. **Caretaker Execution During the `kexec` Window (Run Current Mode, Stall on Nested World Switches)**:
   - Emulating L1 $\leftrightarrow$ L2 world switches (`VMLAUNCH`, `VMRESUME`,
     `VMRUN`, and L2 $\rightarrow$ L1 nested VM-exits) inside
     `.text.cpu_preserved` would require pulling thousands of lines of nested
     hypervisor state machines (`arch/x86/kvm/vmx/nested.c`,
     `arch/x86/kvm/svm/nested.c`) and dynamic shadow EPT page-table construction
     into the Caretaker, conflicting with its zero-allocation, minimal-TCB
     design.
   - Instead, the Caretaker can keep the vCPU executing in whichever mode (L1 or
     L2) was active at `.preserve()` time and stall only if a nested world switch
     is required during the `kexec` window:
     - **vCPU in L1 (`!is_guest_mode(vcpu)`)**: The Caretaker runs `vmcs01` /
       `vmcb01` as usual. If the L1 hypervisor executes a nested virtualization
       instruction (`VMLAUNCH`, `VMRESUME`, `VMRUN`, `VMPTRLD`) during `kexec`,
       the Caretaker treats it as an unhandled architectural exit
       (`ONCORE_EXIT_STALL`) and parks the vCPU until the incoming kernel
       re-attaches and emulates the nested entry.
     - **vCPU in L2 (`is_guest_mode(vcpu)`)**: The Caretaker loads and runs the
       active `vmcs02` (VMX) or `vmcb02` (SVM) with the preserved nested shadow
       TDP root so L2 compute continues across `kexec`. If L2 triggers an exit
       that `vmcs12` / `vmcb12` requires reflecting to L1 (or faults on an
       unmapped shadow EPT entry), the Caretaker stalls (`ONCORE_EXIT_STALL`),
       syncs the active `vmcs02` / `vmcb02` guest state, and lets the incoming
       kernel's full KVM (`nested_vmx_vmexit()` / `nested_svm_vmexit()`) reflect
       the exit to L1 upon `.retrieve()`.

### 7.5 Paravirtual `virtio` Devices and In-Kernel Alternatives (e.g., `virtio-rng`)

While pass-through PCI/VFIO devices can continue DMA and polling I/O in
hardware during the `kexec` window, paravirtual `virtio` devices (`virtio-rng`,
`virtio-blk`, `virtio-net`, `virtio-console`, `virtio-balloon`, `virtio-vsock`)
are emulated in the userspace VMM (or `vhost` workers) and require VMM
involvement to service virtqueue kicks (`ioeventfd`) and inject completions
(`irqfd`).

A common example is **`virtio-rng`**, which is enabled by default in many QEMU
and cloud VMM configurations to supply host entropy to the guest's `hwrng`
subsystem. If a guest touches `virtio-rng` (or any other VMM-emulated `virtio`
device) during the `kexec` window, it can populate virtqueue descriptors in
guest RAM without trapping, but the subsequent MMIO or PIO write to the
`QueueNotify` doorbell triggers a Stage-2/TDP fault or PIO exit. In RFCv1, any
unmapped MMIO/PIO exit stalls the vCPU (`ONCORE_EXIT_STALL`) until the incoming
kernel and userspace VMM re-attach.

Addressing VMM-backed `virtio` devices such as `virtio-rng` during the `kexec`
window involves three mechanisms:

1. **Architectural CPU Alternatives (`RDRAND`/`RDSEED` and `FEAT_RNG` Instead of `virtio-rng`)**:
   - Modern x86 (`RDRAND`, `RDSEED`) and ARM64 (`FEAT_RNG`: `RNDR`, `RNDRRS`)
     processors provide native hardware random number instructions that execute
     inside the guest CPU without causing a VM-exit.
   - Exposing these architectural instructions to the guest (or emulating them
     in a few instructions inside `.text.cpu_preserved` if trapped) allows the
     guest kernel (`arch_get_random_long()` / `crng`) and guest applications to
     obtain hardware entropy across `kexec` without relying on `virtio-rng` or a
     userspace VMM.

2. **Posted `ioeventfd` Doorbell Absorption (Deferred VMM Notification)**:
   - In the `virtio` specification, writing to the `QueueNotify` MMIO/PIO
     register is a posted notification informing the backend that new
     descriptors have been placed in guest RAM.
   - When a guest background thread (such as `hwrng` polling `virtio-rng`, or an
     asynchronous network/storage submission) writes a `QueueNotify` doorbell
     during `kexec`, stalling the entire vCPU on the doorbell instruction is
     unnecessary if the guest does not spin-wait on immediate completion.
   - By exporting the VM's registered `ioeventfd` GPA/PIO doorbell ranges into a
     compact table in the Caretaker control block, the Caretaker can absorb
     `ioeventfd` writes in-place (marking the corresponding `ioeventfd` entry as
     pending in preserved memory and advancing `RIP`/`PC`) so the vCPU continues
     running other guest tasks, and KVM signals all pending `ioeventfd`s once
     the incoming kernel and VMM re-attach.

3. **Lightweight In-Kernel / Caretaker Virtqueue Servicing for Stateless Devices**:
   - For simple, stateless paravirtual devices like `virtio-rng` (where a guest
     without `RDRAND`/`FEAT_RNG` might wait for a buffer to be filled) or
     `virtio-console` transmit, an in-kernel handler or a minimal Caretaker
     virtqueue helper mapped to the device's virtqueue pages in preserved guest
     RAM can consume `avail` ring descriptors, fill `virtio-rng` buffers using
     host hardware RNG instructions (or drain console output), and advance the
     `used` ring index without VMM involvement.

### 7.6 "Kernel Caretaker" for Preserved Userspace Processes (e.g., DPDK, gVisor)

Because the architecture separates physical CPU preservation (`cpu_preserve`)
and on-core workload scheduling (`oncore`) from the KVM-specific
`KVM Caretaker`, the same infrastructure can support a **"Kernel Caretaker"**
(Ring-3 / EL0 Caretaker) that keeps specially tailored host userspace processes
executing across a `kexec` reboot without a virtual machine:

1. **Target Workloads (DPDK, User-Space I/O Pollers, and gVisor)**:
   - High-throughput user-space data planes such as **DPDK** (polling preserved
     VFIO NIC queues out of HugeTLB/memfd memory), SPDK storage engines, or
     real-time control loops spend 100% of their steady-state execution in
     Ring 3 / EL0 without invoking kernel system calls.
   - Similarly, user-space application kernels and sandboxes such as **gVisor**
     (where the Sentry intercepts and services application system calls in user
     space via `systrap` or shared memory over preserved memory mappings) can
     continue running sandboxed workloads across a host kernel `kexec` as long
     as they do not require a host kernel syscall during the brief reboot
     window.

2. **Execution Model Across `kexec` (Preserved Stack/Memory + Stall on Kernel Entrance)**:
   - **Preserved User Address Space**: The process places its code, stacks,
     heap, and DMA/packet buffers in KHO-preserved memory (`memfd` / HugeTLB)
     alongside preserved VFIO device BAR mappings. Its user-space page tables
     (`PGD` on x86, `TTBR0_EL1` on ARM64), combined with the isolated
     `.text.cpu_preserved` / `.data.cpu_preserved` trampoline mappings, are
     preserved across `kexec` via `cpu_preserved_as`.
   - **Ring-3 / EL0 Trampoline**: Rather than entering a guest via
     `VMLAUNCH`/`VMRUN`/`ERET`-to-EL1, the Kernel Caretaker `oncore` callback
     switches to the preserved process's page tables and enters user mode in
     Ring 3 (`SYSRET` / `IRETQ` on x86) or EL0 (`ERET` to EL0 on ARM64), with
     `MSR_LSTAR` / `IDT` (x86) or `VBAR_EL1` (ARM64) pointing at standalone
     entry vectors in `.text.cpu_preserved`.
   - **Stall on Unsupported Kernel Entrance (`syscall` or Fault)**:
     - While the thread executes in user space over pre-mapped preserved memory
       and vDSO timekeeping (`RDTSC` / `CNTVCT_EL0`), it runs at native speed
       across `kexec`.
     - If the thread triggers an unsupported kernel entrance during the `kexec`
       window (such as a host `syscall` (`SYSCALL`/`SVC`), a page fault (`#PF` /
       Data Abort) on an unmapped address, or a synchronous exception), the
       `.text.cpu_preserved` entry stub saves the thread's user register frame
       (`pt_regs`) and FPU state into preserved memory and returns
       `ONCORE_EXIT_STALL` (leaving the user instruction pointer at the trapping
       instruction).
     - Once the incoming kernel boots and re-attaches the preserved process/task
       context via LUO, the new kernel services the pending `syscall` or page
       fault through its standard handlers and resumes the process.
