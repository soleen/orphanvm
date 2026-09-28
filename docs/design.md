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

The [initial design proposal][initial-proposal] (posted without implementation)
proposed loading a standalone, userspace-supplied bare-metal ELF binary into KVM
via a `KVM_SET_CARETAKER` ioctl. In that initial proposal, the Caretaker
permanently interposed on the hardware VM-exit vector (`HOST_RIP`) throughout
the VM's lifetime, acting as a fast-path shim that forwarded exits back to KVM
via physical function pointers when attached, and spinning in a detached loop
during `kexec`.

Following upstream community feedback on that thread, the architecture was
redesigned for the [RFCv1 patch series][rfcv1-series] described in this
document:

1. **Built-in vs. External Caretaker**: Instead of parsing and validating a
   userspace ELF payload, the preserved execution code is proposed to be part of
   the kernel source tree and therefore compiled into the kernel image inside
   dedicated linker sections (`.text.cpu_preserved` and `.data.cpu_preserved`).
2. **Zero Overhead During Normal Operation**: Standard `KVM_RUN` execution is
   untouched while the host is running normally. Hardware VM-exit vectors
   (`HOST_RIP`, `VBAR_EL2`, `HOST_CR3`) are reprogrammed to point to the
   Caretaker only at the moment the live update session enters the preservation
   phase, and are restored to standard KVM handlers upon adoption in the
   incoming kernel or if the live update is canceled.
3. **Layered Subsystem Decomposition**: Rather than embedding physical CPU
   offlining, page-table isolation, and scheduling inside KVM, the current
   design separates the mechanism into generic kernel subsystems (`cpu_preserve`
   and `oncore`) that KVM (`caretaker`) consumes as a client. This keeps core
   CPU lifecycle management in `kernel/liveupdate/` and leaves the door open for
   future non-KVM on-core workloads such as isolated user tasks.
4. **KHO ABI Separation**: In-kernel `vcpufd` preservation via LUO provides a
   serialization format (`include/linux/kho/abi/`) across `kexec`. Generic ABI
   versioning and compatibility across kernel versions is out of scope of this
   design and is addressed as a separate effort (see [`[RFC PATCH 0/3]
   liveupdate: Move to feature flags for LUO and memfd ABI
   compatibility`][luo-abi-rfc] and the LPC session ["Live Update
   Compatibility"][lpc-compat]).

> **Note on Proposal Status:** This document describes the current design
> corresponding to the [RFCv1 patch series][rfcv1-series]. Several aspects, most
> notably how to structure VM-exit handling and share low-level code with KVM
> without duplicating hypervisor logic, as well as open items from the initial
> LKML discussion, remain under active discussion and are covered in
> [Section 7][section-7].

### 1.2 Memory and Device Preservation

This document focuses on **CPU and KVM execution state preservation**
(`vmfd`/Stage-2 MMU, `cpu_preserve`, `oncore`, and `vcpufd`/Caretaker). Within
the broader Live Update project, guest memory and pass-through device
preservation are developed as companion efforts:

- **Guest Memory and Base `vmfd` Preservation**:
  The [`guest_memfd` preservation series][guest-memfd-series]
  (`[PATCH v4 00/11] liveupdate: kvm: Guest_memfd preservation`) introduces
  `guest_memfd` support as well as the base KVM `vmfd` LUO preservation
  infrastructure (`virt/kvm/kvm_luo.c`) that this design expands upon for
  secondary page table and `vcpufd` preservation. Although RFCv1 was based on
  top of that series for proof-of-concept purposes, OrphanVM does not depend on
  `guest_memfd` and can also use standard preserved `memfd` or HugeTLB backing
  memory.
- **Pass-Through Device and DMA Preservation**:
  Full pass-through device preservation across `kexec` is currently under review
  across three companion series:
  - [PCI core support for Live Update][pci-lu-series]
    (`[PATCH v9 00/13] PCI: liveupdate: PCI core support for Live Update`)
  - [Base Live Update support for VFIO/PCI][vfio-lu-series]
    (`[PATCH v5 00/20] vfio/pci: Base Live Update support for VFIO`)
  - [IOMMU live update state preservation][iommu-lu-series]
    (`[PATCH v5 00/18] iommu: Add live update state preservation`)
- **Direct Interrupt Injection Table Preservation**:
  Hardware direct interrupt injection (Intel VT-d Posted Interrupts, AMD AVIC,
  and ARM64 GICv4.1 direct vLPI injection) allows pass-through devices to
  deliver interrupts directly to a running vCPU without host IRQ handlers or
  `irqfd`. For design simplicity, the current PCI, VFIO, and IOMMU live update
  series preserve DMA and BAR MMIO state across `kexec`, but do not preserve
  device interrupts: instead, once a paused VM resumes in the incoming kernel,
  a spurious interrupt is injected so the guest device driver checks for missed
  events or ignores it. With OrphanVM, however, the VM stays alive throughout
  the transition, so relying on a post-reboot spurious interrupt is not an
  option; the full interrupt tables must be preserved so device interrupts
  continue to be delivered during the reboot. Specifically, the IOMMU Interrupt
  Remapping Table Entries (IRTEs) and per-vCPU interrupt delivery tables
  (`pi_desc` on Intel, AVIC backing and physical/logical APIC tables on AMD,
  and GICv4 ITS/vPE tables on ARM64) must remain preserved across KHO and
  pointed at the preserved physical CPU. Preserving these tables still needs to
  be implemented as part of the device preservation effort, and there is no
  patch series for it yet.

---

## 2. Architectural Overview

The OrphanVM design consists of four layers, ordered from bottom to top:

| Layer   | Subsystem       | Kconfig / uAPI             | Key Responsibilities                                              |
| :------ | :-------------- | :------------------------- | :---------------------------------------------------------------- |
| Layer 4 | `kvm/caretaker` | `CONFIG_KVM_CARETAKER`     | Wraps preserved vCPUs as `oncore_job` instances                   |
|         |                 | `KVM_CAP_CARETAKER`        | Cross-kernel state machine (`PAUSED`/`RUNNING`/`STOPPED`)         |
|         |                 |                            | Arch guest entry/exit loop (Intel VMX, AMD SVM, ARM64 VHE)        |
| Layer 3 | `oncore`        | `CONFIG_LIVEUPDATE_ONCORE` | Workload-agnostic session (`oncore_session`) & job (`oncore_job`) |
|         |                 |                            | Time-sliced round-robin FIFO runqueue                             |
|         |                 |                            | Multiplexes $M$ jobs across $N$ preserved physical CPUs           |
| Layer 2 | `cpu_preserve`  | `CONFIG_LIVEUPDATE_CPU`    | `.text.cpu_preserved` & `.data.cpu_preserved`                     |
|         |                 | `/sys/.../cpu<N>/preserve` | CPU hotplug interception & `!cpu_present(cpu)` SMP isolation      |
|         |                 |                            | Isolated page tables (`cpu_preserved_as`) & stack context         |
| Layer 1 | `kvm_luo`       | `KVM_CAP_VCPU_PRESERVE`    | `vmfd` & secondary page table (TDP / Stage-2) LUO preservation    |
|         |                 | `"kvm_vcpu_luo_v1"`        | `vcpufd` architectural state serialization (`kvm_vcpu_arch_ser`)  |

This layering allows incremental upstreaming:

- **Layer 1** is useful on its own without any physical CPU preservation: it
  provides in-kernel `vcpufd` and secondary page table preservation across
  `kexec` (RAM-only suspend/resume without round-tripping vCPU state through
  userspace).
- **Layer 2** preserves physical CPUs across `kexec` in an isolated address
  space, which in some cases speeds up incoming kernel boot by reducing the
  number of CPUs brought online during `smp_init()`, and includes a
  `liveupdate/` selftest (`luo_cpu_preserve`) that verifies the correctness of
  this layer in isolation.
- **Layer 3** provides the generic scheduler for running bounded work on
  preserved physical CPUs, and can also include a selftest with a sample
  in-kernel `oncore_job` to verify its scheduling capabilities independently of
  KVM.
- **Layer 4** connects preserved KVM vCPUs to the `oncore` scheduler when both
  `vcpufd`s and physical CPUs are preserved within the same LUO session.

### 2.1 End-to-End VMM Orchestration Flow

The kernel interfaces are VMM-agnostic and operate through standard
KVM and LUO file descriptor preservation ioctls:

1. **Creation & Capability Discovery**:
   The userspace VMM queries `KVM_CAP_VCPU_PRESERVE` and `KVM_CAP_CARETAKER` via
   `KVM_CHECK_EXTENSION`, and creates `vmfd`, guest backing `memfd` or
   `guest_memfd`, and `vcpufd`s. Normal `KVM_RUN` execution proceeds without any
   Caretaker interposition.
2. **LUO Session Preservation & Handoff to Caretaker**:
   While the VM is running, the VMM preserves its resources into a LUO session
   via `LIVEUPDATE_SESSION_PRESERVE_FD`:
   - Guest memory `memfd` or `guest_memfd` descriptors
   - `vmfd`, preserving KVM metadata and secondary page tables
   - (Optional) Physical CPU descriptors
     (`/sys/devices/system/cpu/cpu<N>/preserve`), preserving them into the LUO
     session, which removes the target physical CPUs from the host scheduler and
     parks them in `cpu_preserved_park_loop()` (in `.text.cpu_preserved` on a
     dedicated preserved stack and isolated page tables) until a workload is
     attached
   - Each `vcpufd`: the VMM pauses the vCPU's `KVM_RUN` thread and preserves
     `vcpufd`, which serializes the ABI-defined architectural state
     (`struct kvm_vcpu_ser` and `struct kvm_vcpu_arch_ser`), initializes its
     Caretaker control block (`struct kvm_caretaker_cb_ser`), and activates the
     `oncore_job` (`oncore_session_activate_job()`), resulting in one of three
     scenarios:
     - **Idle preserved pCPU available**: If a preserved physical CPU in the
       session has nothing scheduled on it (parked in
       `cpu_preserved_park_loop()`), `oncore_session` attaches to that pCPU and
       immediately kicks it to resume guest execution in the Caretaker.
     - **Preserved pCPUs already running vCPU jobs**: If the session's preserved
       physical CPUs are already executing other vCPU jobs, the new `oncore_job`
       is queued on the `oncore_runqueue` and time-sliced across the preserved
       pCPUs alongside the existing jobs.
     - **No preserved pCPUs available**: If no physical CPUs have been preserved
       into the LUO session, the vCPU remains suspended until physical CPUs are
       added (or until retrieved in the incoming kernel).
3. **(Optional) Cancellation**:
   If the live update is canceled (i.e., the LUO session file descriptor is
   closed before `kexec`), LUO unpreserves all resources in the session: each
   vCPU detaches from the Caretaker, restores its updated architectural state
   into the outgoing `struct kvm_vcpu`, and any preserved physical CPUs return
   to the host scheduler so the VMM can resume `KVM_RUN`.
4. **The Kexec Gap**:
   The host performs a `kexec` reboot. Management CPUs reboot into the incoming
   kernel while the preserved physical CPUs (`!cpu_present`) continue executing
   guest vCPUs inside the Caretaker's isolated address space.
5. **Reclamation**:
   Once the incoming kernel boots, the new VMM instance opens the preserved LUO
   session and issues `LIVEUPDATE_SESSION_RETRIEVE_FD` for the
   `memfd`/`guest_memfd`, `vmfd`, and `vcpufd` tokens (preserved physical CPU
   tokens do not need to be retrieved). Retrieving each `vcpufd` detaches the
   vCPU from the Caretaker and restores its updated architectural state into the
   new kernel's `struct kvm_vcpu`. Issuing `LIVEUPDATE_SESSION_FINISH` (or
   closing the session file descriptor) frees the KHO handover structures,
   returns the preserved physical CPUs to the host scheduler, and allows the VMM
   to resume normal `KVM_RUN` threads.

---

## 3. Layer 1: In-RAM vCPU and Secondary Page Table Preservation

Before a vCPU can execute across `kexec` (or even be suspended and resumed in
RAM without physical CPU preservation), both its architectural register state
and the VM's secondary page tables must survive the reboot in KHO-preserved
memory. This layer expands upon the base KVM `vmfd` preservation infrastructure
(`virt/kvm/kvm_luo.c`) introduced by the [`guest_memfd` preservation
series][guest-memfd-series].

### 3.1 In-Kernel `vcpufd` Preservation

In a traditional live update, the userspace VMM must extract all vCPU state via
dozens of `KVM_GET_*` ioctls prior to `kexec`, serialize that state into a file
or memory buffer, and re-issue `KVM_SET_*` ioctls after `kexec`.

In this proposal, `vcpufd` is preserved into a LUO session, governed by the
`KVM_CAP_VCPU_PRESERVE` capability:

- **`vcpufd` Common ABI (`struct kvm_vcpu_ser`)**:
  Defined in `include/linux/kho/abi/kvm.h`, it records the vCPU identifier
  (`vcpu_id`), the LUO token of the parent VM (`vm_token`), and KHO pointers to
  the architectural state buffer (`arch_state`) and Caretaker control block
  (`cb`):

  ```c
  struct kvm_vcpu_ser {
          u32 vcpu_id;
          u64 vm_token;
          DECLARE_KHOSER_PTR(arch_state, struct kvm_vcpu_arch_ser *);
          DECLARE_KHOSER_PTR(cb, struct kvm_caretaker_cb_ser *);
  };
  ```

  A vCPU can be preserved either with or without the Caretaker, determined by
  whether `cb` is populated:
  - **Without Caretaker (`cb == NULL`)**: The vCPU is suspended in RAM across
    `kexec`; `.preserve()` serializes its state into `arch_state`, and
    `.retrieve()` restores it into the new `struct kvm_vcpu` in the incoming
    kernel.
  - **With Caretaker (`cb != NULL`)**: The vCPU is handed off to the Caretaker
    to execute on a preserved physical CPU across `kexec`. `cb` points to the
    Caretaker control block (`struct kvm_caretaker_cb_ser`, detailed in Section
    6.1), which coordinates the cross-kernel execution state machine (`PAUSED`,
    `RUNNING`, `STOPPING`, `STOPPED`) and tracks which physical CPU owns the
    vCPU while `arch_state` serves as the register handoff buffer between the
    outgoing kernel, the Caretaker, and the incoming kernel.

- **`vcpufd` Architectural State (`struct kvm_vcpu_arch_ser`)**:
  In all other KHO ABI structures, existing kernel `struct`s are never reused;
  instead, every structure is built exclusively from fixed-width primitive types
  (`u8`, `u32`, `u64`) or other serialized structures defined under
  `include/linux/kho/abi/`. For `vcpufd` arch-specific preservation, to avoid
  modifying common KVM code or defining a separate, complex serialization
  format, this proposal makes an exception and reuses existing kernel
  structures. Crucially, *all* reused structures are exposed to uAPI. Note while
  uAPI structures are generally stable, they can still evolve over time by
  expanding layouts. Embedding uAPI structures is therefore not a complete ABI
  stability guarantee on its own, but combined with LUO ABI versioning (which,
  as noted in Section 1.2, is developed as an orthogonal effort), any
  incompatible layout change is detectable and will automatically mark a Live
  Update between those kernel versions as incompatible:
  - **x86 (`include/linux/kho/abi/kvm_x86.h`)**:

    ```c
    struct kvm_vcpu_arch_ser {
            struct kvm_regs regs;              /* KVM_GET/SET_REGS */
            struct kvm_sregs sregs;            /* KVM_GET/SET_SREGS */
            struct kvm_mp_state mp_state;      /* KVM_GET/SET_MP_STATE */
            struct kvm_xcrs xcrs;              /* KVM_GET/SET_XCRS */
            struct kvm_lapic_state lapic;      /* KVM_GET/SET_LAPIC */
            struct kvm_xsave xsave;            /* KVM_GET/SET_XSAVE */
            struct kvm_vcpu_events events;     /* KVM_GET/SET_VCPU_EVENTS */
            struct kvm_debugregs debugregs;    /* KVM_GET/SET_DEBUGREGS */
            struct kvm_clock_data clock;       /* KVM_GET/SET_CLOCK */
            DECLARE_KHOSER_PTR(msrs,
                               struct kvm_msrs *);   /* KVM_GET/SET_MSRS */
            DECLARE_KHOSER_PTR(cpuid,
                               struct kvm_cpuid2 *); /* KVM_GET/SET_CPUID2 */
    };
    ```

    `xsave` is 64-byte aligned so the Caretaker can execute hardware `XSAVE64`
    and `XRSTOR64` directly in place, while `msrs` (populated from KVM's
    `msrs_to_save` and `emulated_msrs` lists) and `cpuid` point to the
    variable-length uAPI `struct kvm_msrs` and `struct kvm_cpuid2` tables
    allocated within the same contiguous KHO buffer.

  - **ARM64 (`include/linux/kho/abi/kvm_arm64.h`)**:

    ```c
    struct kvm_arm64_sysregs_ser {
            u32 num_sysregs;
            struct kvm_one_reg sysregs[];      /* KVM_GET/SET_ONE_REG */
    };

    struct kvm_vcpu_arch_ser {
            struct kvm_regs regs;              /* KVM_GET/SET_ONE_REG */
            struct kvm_mp_state mp_state;      /* KVM_GET/SET_MP_STATE */
            struct kvm_vcpu_events events;     /* KVM_GET/SET_VCPU_EVENTS */
            struct kvm_vcpu_init init;         /* KVM_ARM_VCPU_INIT */
            DECLARE_KHOSER_PTR(sysregs, struct kvm_arm64_sysregs_ser *);
    };
    ```

    `sysregs` points to `struct kvm_arm64_sysregs_ser` (placed within the same
    contiguous KHO allocation), which stores each system and VGICv3 CPU
    interface register's `id` and 64-bit value (in `addr`), enumerated via
    `kvm_arm_get_sys_reg_indices()` and read/written using in-kernel accessors
    (`kvm_arm_sys_reg_read()` / `kvm_arm_sys_reg_write()`).

### 3.2 Secondary Page Table Preservation

During the `kexec` blackout window, the host KVM MMU fault handler is offline.
For the guest to continue accessing its memory without triggering faults
(Intel EPT Violations, AMD NPT faults, or ARM64 Stage-2 translation faults),
the existing secondary page tables must remain intact in physical memory across
`kexec`.

Because secondary page tables are a VM-wide resource shared across all vCPUs,
they are preserved once when `vmfd` is preserved. `kvm_luo_preserve()`
serializes the VM type and a KHO pointer to the preserved folio list
(`struct kvm_kho_folios_ser`) into `struct kvm_luo_ser`:

```c
struct kvm_kho_folios_ser {
        u64 nr_folios;
        u64 folios_pa[];
};

struct kvm_luo_ser {
        u64 type;
        DECLARE_KHOSER_PTR(kho_folios, struct kvm_kho_folios_ser *);
};
```

Each architecture walks its secondary page tables during `vmfd` preservation to
populate `folios_pa[]`:

- **x86 (`arch/x86/kvm/mmu/kho.c`)**:
  `kvm_mmu_preserve_kho()` collects all TDP root and non-leaf page-table pages
  (`kvm->arch.tdp_mmu_roots`), active MMU pages (`kvm->arch.active_mmu_pages`),
  and per-vCPU root page tables. Because `kho_preserve_folio()` may allocate
  memory and sleep, preservation runs in two phases: first collecting the
  `struct page *` pointers while holding `kvm->mmu_lock` for write, and then
  allocating `struct kvm_kho_folios_ser` and invoking `kho_preserve_folio()`
  outside `mmu_lock`.
- **ARM64 (`arch/arm64/kvm/kvm_luo.c`)**:
  `kvm_arch_vm_luo_preserve()` preserves the Stage-2 PGD root folio
  (`mmu->pgd_phys`) and walks the guest's Stage-2 page table (`mmu->pgt`) via
  `kvm_pgtable_walk()` (`KVM_PGTABLE_WALK_TABLE_PRE`), preserving every valid
  non-leaf table folio in `struct kvm_kho_folios_ser`.

### 3.3 LUO Lifecycle: Freeze, Cancellation, Retrieve, and Finish

Across a live update, the LUO file handlers for guest memory (`memfd_luo.c`,
`guest_memfd_luo.c`), the VM (`kvm_luo_file_ops`), and vCPUs
(`kvm_vcpu_luo_file_ops`) coordinate four lifecycle transitions:

1. **Pre-`kexec` `.freeze()` Phase (`luo_freeze()`)**:
   When `reboot(LINUX_REBOOT_CMD_KEXEC)` executes, `liveupdate_reboot()` invokes
   `luo_freeze()` before jumping to the incoming kernel:
   - **Guest Memory (`memfd` and `guest_memfd`)**: During
     `LIVEUPDATE_SESSION_PRESERVE_FD`, `memfd_luo_preserve()` freezes the shmem
     inode (`shmem_freeze(inode, true)`) and pins `VM_SHARED` VMA mappings
     (`mm_liveupdate_pin_vmas()`), while `kvm_gmem_luo_preserve()` sets
     `KVM_GMEM_FLAG_PRESERVED` (blocking `fallocate` and punch-hole operations)
     so already-allocated guest RAM can continue to be accessed and dirtied up
     until `kexec`. At `kexec` time, `memfd_luo_freeze()`
     (`memfd_luo_save_folios()`) walks the inode's page cache
     (`filemap_get_folios_contig()`), cleans dirty folios (`folio_mkclean()`),
     calls `kho_preserve_folio()` on each allocated folio, and records their
     PFNs in `struct memfd_luo_folio_ser`. Similarly, `kvm_gmem_luo_freeze()`
     walks the `guest_memfd` page cache, records each folio's PFN and
     preparation state (`KVM_GMEM_SER_PREPARED`), and preserves each folio via
     `kho_preserve_folio()`.
   - **KVM (`vmfd` and `vcpufd`)**: Neither `kvm_luo_file_ops` nor
     `kvm_vcpu_luo_file_ops` defines a `.freeze()` callback because the
     secondary page tables and vCPU architectural/Caretaker state are already
     preserved in KHO memory when `LIVEUPDATE_SESSION_PRESERVE_FD` completes.
2. **Cancellation (`.unpreserve()`) Before `kexec`**:
   If the LUO session file descriptor is closed before `kexec`
   (`luo_session_release()` -> `luo_file_unpreserve_files()`), LUO invokes
   `.unpreserve()` in reverse preservation order:
   - **`vcpufd` (`kvm_vcpu_luo_unpreserve()`)**: Detaches the vCPU from the
     Caretaker (if active), restores its updated architectural state from
     `struct kvm_vcpu_arch_ser` back into the outgoing kernel's existing
     `struct kvm_vcpu`, and frees `ser->arch_state` and `struct kvm_vcpu_ser`
     (`kho_unpreserve_free()`). Because non-architectural host state (such as
     memslots, secondary page tables, and device bindings) remained in place in
     the outgoing kernel, the VMM can immediately resume `KVM_RUN`.
   - **`vmfd` (`kvm_luo_unpreserve()`)**: Unpreserves the secondary page table
     folios in KHO (`kvm_kho_folios_unpreserve()` -> `kho_unpreserve_folio()`,
     leaving the live page tables intact in the outgoing `struct kvm`) and frees
     `struct kvm_luo_ser`.
   - **`memfd` / `guest_memfd` (`memfd_luo_unpreserve()` /
     `kvm_gmem_luo_unpreserve()`)**: Unfreezes the inode
     (`shmem_freeze(inode, false)` or clearing `KVM_GMEM_FLAG_PRESERVED`),
     unpins VMAs, and frees the KHO serialization metadata.
3. **Incoming `.retrieve()` (`LIVEUPDATE_SESSION_RETRIEVE_FD`) and Retrieval
   Order**:
   In the incoming kernel, `luo_retrieve_file()` (used by both
   `LIVEUPDATE_SESSION_RETRIEVE_FD` and `liveupdate_get_file_incoming()`)
   resolves inter-file dependencies lazily:
   - Both `kvm_gmem_luo_retrieve()` and `kvm_vcpu_luo_retrieve()` look up their
     parent VM via `liveupdate_get_file_incoming(args->session, ser->vm_token,
     &vm_file)`. If `vmfd` has not been retrieved yet (`retrieve_status == 0`),
     LUO automatically invokes `kvm_luo_retrieve()` on `vm_token` first and
     caches `luo_file->file`; when userspace later calls
     `LIVEUPDATE_SESSION_RETRIEVE_FD` on `vm_token`, LUO installs a file
     descriptor for that already-created `struct kvm`.
   - In practice, the VMM retrieves `memfd`/`guest_memfd` and `vmfd` first,
     re-establishes non-preserved VM state on `vmfd` (such as `mmap()`ing
     `memfd`, registering memslots via `KVM_SET_USER_MEMORY_REGION2`, and
     re-creating in-kernel IRQchip and device bindings), and then retrieves each
     `vcpufd`:
     - **`memfd` / `guest_memfd` (`memfd_luo_retrieve()` /
       `kvm_gmem_luo_retrieve()`)**: Allocates a new file, calls
       `kho_restore_folio()` on each preserved guest RAM folio, re-inserts the
       folios into the new file's `inode->i_mapping` page cache at their
       original page indices, and frees the KHO folio-list metadata.
     - **`vmfd` (`kvm_luo_retrieve()`)**: Detaches preserved physical CPU
       workloads (`kvm_caretaker_vm_pre_retrieve()`), allocates the new
       `struct kvm` with the preserved `ser->type` (`kvm_create_vm_file()`), and
       invokes `kvm_arch_vm_luo_retrieve()`, while keeping the outgoing kernel's
       KHO-preserved secondary page table folios (`ser->kho_folios`) alive until
       `.finish()`.
     - **`vcpufd` (`kvm_vcpu_luo_retrieve()`)**: Allocates the new
       `struct kvm_vcpu` (`kvm_create_vcpu_file()`), stops Caretaker execution
       (`kvm_caretaker_vcpu_pre_retrieve()`), and restores the updated
       architectural state from `ser->arch_state` (`struct kvm_vcpu_arch_ser`)
       into the new `struct kvm_vcpu` (`kvm_arch_vcpu_luo_retrieve()` and
       `kvm_caretaker_vcpu_retrieve()`).
4. **Incoming `.finish()` (`LIVEUPDATE_SESSION_FINISH` or
   `close(session_fd)`)**:
   When userspace issues `LIVEUPDATE_SESSION_FINISH` (or closes the retrieved
   session file descriptor), `luo_file_finish()` iterates over all session
   entries in reverse order to release KHO handover memory:
   - **`vcpufd` (`kvm_vcpu_luo_finish()`)**: Ensures the Caretaker vCPU has
     stopped (in case `vcpufd` was never retrieved) and frees all KHO-preserved
     vCPU allocations (`ser->arch_state`, `ser->cb`, per-vCPU hardware pages,
     telemetry, and `struct kvm_vcpu_ser`) via `kho_restore_free()`.
   - **`vmfd` (`kvm_luo_finish()`)**: Calls `kvm_kho_folios_finish()`
     (`kho_restore_folio()` + `folio_put()`) to restore and free the outgoing
     kernel's KHO-preserved secondary page table folios (as the incoming
     `struct kvm` builds new secondary page tables against its memslots) and
     frees `struct kvm_luo_ser`.
   - **`memfd` / `guest_memfd` (`memfd_luo_finish()` /
     `kvm_gmem_luo_finish()`)**: No-op if the file was retrieved (since
     `.retrieve()` already moved the folios into the new file's page cache); if
     never retrieved, discards and frees the unretrieved KHO folios.
   - **LUO Session Core**: Drops LUO's internal `struct file *` reference
     (`fput()`) on each retrieved file and destroys the session's KHO block set.

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
- `-ftrivial-auto-var-init=uninitialized` (prevents compiler-synthesized pattern
  initialization calls)
- `-mbranch-protection=none` on ARM64
- Disabled instrumentation (`KCOV_INSTRUMENT_<obj>.o := n`,
  `KCSAN_SANITIZE_<obj>.o := n`, `KASAN_SANITIZE_<obj>.o := n`,
  `UBSAN_SANITIZE_<obj>.o := n`, and
  `CFLAGS_REMOVE_<obj>.o = $(CC_FLAGS_FTRACE)`)
- `objtool` exemptions for `.text.cpu_preserved` from retpoline and return-thunk
  rewriting (`__x86_indirect_thunk_*` / `__x86_return_thunk`), because those
  thunks reside in standard `.text` and are overwritten during `kexec`.

#### Relocating Outside KHO Scratch Memory

A hazard with KHO is that the compiled kernel image itself resides in physical
memory that KHO designates as **KHO Scratch** (memory that the incoming kernel
is permitted to overwrite during early boot decompression and initialization).
Consequently, preserving the physical pages of the outgoing kernel's
`.text.cpu_preserved` section in-place is unsafe.

To solve this, when initializing the preserved runtime buffer
(`cpu_preserved_init_runtime_buffer()`), `cpu_preserve`:

1. Allocates physical pages from the buddy allocator (`alloc_pages()`), which
   are guaranteed to be outside the KHO Scratch regions.
2. Copies the compiled contents of `.text.cpu_preserved` and
   `.data.cpu_preserved` into those newly allocated physical pages.
3. Remaps the outgoing kernel's virtual address ranges
   (`__cpu_preserved_text_start..__cpu_preserved_text_end` and
   `__cpu_preserved_data_start..__cpu_preserved_data_end`) to point to the new
   physical pages (splitting any 2MB/contpte kernel mappings into 4KB PTEs and
   setting `PAGE_KERNEL_ROX` / `PAGE_KERNEL` permissions).
4. Marks those physical pages as preserved in KHO (`kho_preserve_pages()`).

As a result, normal C symbol references and direct calls within
`.text.cpu_preserved` and `.data.cpu_preserved` continue to use their linked
kernel virtual addresses, while backed by safe physical pages that survive
`kexec`.

### 4.2 Lifecycle and `!cpu_present(cpu)` SMP Isolation

Each non-boot physical CPU exposes a sysfs control file:
`/sys/devices/system/cpu/cpu<N>/preserve`.

To preserve a CPU for a live update, userspace opens this file and preserves
the file descriptor into a LUO session via `LIVEUPDATE_SESSION_PRESERVE_FD`
(`"cpu_fh_v1"`, `include/linux/kho/abi/cpu.h`):

```c
struct cpu_preserved_file_ser {
        u32 cpu;
        u64 stack_pa;
        DECLARE_KHOSER_PTR(oncore, struct oncore_session_ser *);
};
```

```
Outgoing Kernel                      kexec                     Incoming Kernel
---------------                      -----                     ---------------
open(/sys/.../cpuN/preserve)
LIVEUPDATE_SESSION_PRESERVE_FD
  -> remove_cpu(cpu)
  -> cpuhp_ap_report_dead()
  -> cpu_preserved_report_dead()
  -> cpu_preserved_park():
       switch SP & CR3/TTBR1
       cpu_preserved_park_loop()
  -> set_cpu_present(cpu, false)
                                 cpu_preserved_park_loop()
                                 runs continuously in    early boot:
                                 isolated address space    cpu_preserved_flb_retrieve()
                                                           set_cpu_present(cpu, false)
                                                         smp_init():
                                                           skips !cpu_present(cpu)
                                                         LIVEUPDATE_SESSION_FINISH:
                                                           cpu_signal_exit(cpu)
                                                           set_cpu_present(cpu, true)
                                                           add_cpu(cpu) -> online
```

1. **Hotplug Interception**:
   During `cpu_preserve_preserve()`, the kernel calls `cpu_preserve(cpu)`, which
   marks the CPU preserved in `cpu_preserved_mask` and calls `remove_cpu(cpu)`.
   Standard Linux CPU hotplug migrates all tasks, timers, and interrupts off the
   core until the CPU reaches the terminal offline hook in
   `cpuhp_ap_report_dead()`. After updating the hotplug sync state to
   `SYNC_STATE_DEAD`, `cpuhp_ap_report_dead()` calls
   `cpu_preserved_report_dead()`, which diverts preserved cores into
   `cpu_preserved_park()` instead of `arch_cpu_idle_dead()` (which would place
   the core in an ACPI/PSCI sleep state).
2. **Isolating via `!cpu_present(cpu)` Across the Kexec Gap**:
   Once `remove_cpu(cpu)` completes and the target core is executing on its
   preserved stack and isolated page tables in `cpu_preserved_park_loop()`, the
   outgoing kernel calls `set_cpu_present(cpu, false)`.
   - Because each preserved core is already offline, removed from
     `cpu_present_mask`, and executing inside its isolated address space from
     the moment `LIVEUPDATE_SESSION_PRESERVE_FD` completes, neither
     `cpu_preserve_file_ops` nor `cpu_preserved_flb_ops` requires a `.freeze()`
     callback at `kexec` time.
   - In the **outgoing kernel**, marking the CPU neither online nor present
     ensures that `reboot` / `kexec` shutdown paths (`smp_send_stop()`,
     `native_stop_other_cpus()`) skip sending `REBOOT_VECTOR` or `STOP` IPIs to
     the preserved core.
   - Across `kexec`, a LUO FLB global structure (`"cpu_flb_v1"`,
     `include/linux/kho/abi/cpu.h`) carries the preserved runtime buffer
     addresses and the bitmap of preserved CPUs (`cpu_preserved_bitmap`):

     ```c
     struct cpu_preserved_global_ser {
             u32 nr_cpu_words;
             u64 text_runtime_pa;
             u64 text_runtime_size;
             u64 data_runtime_pa;
             u64 data_runtime_size;
             DECLARE_KHOSER_PTR(pcpus_runtime, struct cpu_preserved_pcpu_ser *);
             DECLARE_KHOSER_PTR(transition_as, struct cpu_preserved_as_ser *);
             u64 cpu_preserved_bitmap[];
     };
     ```

     During early boot in the **incoming kernel**,
     `cpu_preserved_flb_retrieve()` reads `cpu_preserved_bitmap` and calls
     `set_cpu_present(cpu, false)` *before* `smp_init()` runs. Because
     `smp_init()` only brings up CPUs in `cpu_present_mask`, the incoming kernel
     skips sending `INIT`/`SIPI` (x86) or `CPU_ON` PSCI calls (ARM64) to
     preserved cores without requiring architecture-specific changes in the SMP
     boot path.
3. **Cancellation and Incoming Kernel Reclamation**:
   - **Cancellation (`cpu_preserve_unpreserve()` +
     `cpu_preserved_flb_unpreserve()`)**: If the LUO session file descriptor is
     closed before `kexec`, `cpu_preserve_unpreserve()` calls
     `cpu_unpreserve(cpu)` (which sets `ser->workload` to
     `CPU_PRESERVED_EXITING` via `cpu_signal_exit(cpu)`, sends a wakeup IPI via
     `arch_cpu_preserved_kick()`, waits in `cpu_wait_dead(cpu)` for the core to
     exit `cpu_preserved_park_loop()` and publish `CPU_PRESERVED_DEAD`, frees
     its preserved stack, restores `set_cpu_present(cpu, true)`, and calls
     `add_cpu(cpu)` to return the core to the host scheduler) and removes the
     CPU from `oncore_session` (`oncore_session_remove_cpu()`). Once the last
     preserved CPU is unpreserved, `cpu_preserved_flb_unpreserve()` unpreserves
     the `.text.cpu_preserved` / `.data.cpu_preserved` runtime buffer pages and
     `cpu_preserved_transition_as` page tables.
   - **Incoming Kernel Reclamation (`cpu_preserve_finish()` +
     `cpu_preserved_flb_finish()`)**: Userspace does not need to call
     `LIVEUPDATE_SESSION_RETRIEVE_FD` for preserved physical CPU tokens
     (`"cpu_fh_v1"`). When `LIVEUPDATE_SESSION_FINISH` is issued (or the
     session file descriptor is closed), `cpu_preserve_finish()` automatically
     reconstructs the incoming CPU state (`cpu_preserve_restore_incoming_cpu()`)
     if `.retrieve()` was not called, invokes `cpu_unpreserve(cpu)`
     (`CPU_PRESERVED_EXITING` -> `CPU_PRESERVED_DEAD` ->
     `set_cpu_present(cpu, true)` -> `add_cpu(cpu)`), removes the CPU from
     `oncore_session` (`oncore_session_remove_cpu()`, which destroys the
     session and frees its isolated page tables `cpu_preserved_as` once the last
     CPU is removed), and frees `struct cpu_preserved_file_ser`. Once the last
     preserved CPU finishes, `cpu_preserved_flb_finish()` frees the outgoing
     kernel's preserved `.text.cpu_preserved` / `.data.cpu_preserved` buffer
     pages, `cpu_preserved_transition_as` page tables, and `pcpus_ser` array via
     `kho_restore_free()`.

### 4.3 Isolated Address Space (`struct cpu_preserved_as`) and Stack Context

Before `kexec` overwrites the outgoing kernel's page tables, each preserved CPU
switches its MMU root (`CR3` on x86, `TTBR1_EL1`/`TTBR0_EL2` on ARM64) to an
isolated address space (`struct cpu_preserved_as`).

The isolated page tables map **only**:
- `.text.cpu_preserved` (`PAGE_KERNEL_ROX`)
- `.data.cpu_preserved` (`PAGE_KERNEL`)
- The preserved `pcpus_ser` (`struct cpu_preserved_pcpu_ser`) and `pcpus`
  (`struct cpu_preserved_pcpu`) arrays and per-CPU preserved stacks
- Explicitly mapped workload buffers registered into the session's address space
  via `oncore_session_map_range()` (`cpu_preserved_as_map()`)

No other host kernel memory (neither the kernel linear direct map `PAGE_OFFSET`,
nor `vmalloc`, nor normal `.text`/`.data`) is mapped in `cpu_preserved_as`. Page
tables are constructed using `kernel_ident_mapping_init()` (extended with
`force_pte = true` and `offset = page_va - page_pa` for 4KB page granularity) on
x86 and `trans_pgd_map_range()` on ARM64. All page-table pages allocated for the
isolated address space are recorded in `struct cpu_preserved_as_ser` so the
incoming kernel can adopt (`cpu_preserved_as_adopt()`) and free them
(`cpu_preserved_as_destroy()`) after the CPU returns to normal operation:

```c
struct cpu_preserved_as_ser {
        u32 nr_pgtable_pages;
        u64 pgtable_pages[CPU_PRESERVED_AS_MAX_PGTABLE_PAGES];
};
```

To avoid relying on host per-CPU offset registers (`%gs` on x86 or
`TPIDR_EL1`/`TPIDR_EL2` on ARM64), which may be clobbered by guest execution or
differ across kernels, per-CPU metadata (`struct cpu_preserved_stack_context`,
`include/linux/cpu_preserve.h`) is placed at the base of the power-of-two
aligned preserved stack (4KB on x86, 16KB on ARM64) and located in $O(1)$ time
by masking the stack pointer (`sp & ~(CPU_PRESERVED_STACK_SIZE - 1)`):

```c
struct cpu_preserved_stack_context {
        u64 magic;
        u32 cpu;
        u64 workload_context;
        u64 session_pgd_pa;
};
```

---

## 5. Layer 3: On-Core Execution and Scheduling Framework (`oncore`)

The `oncore` subsystem (`kernel/liveupdate/oncore.c`, `include/linux/oncore.h`,
`CONFIG_LIVEUPDATE_ONCORE`) sits between raw physical CPU preservation and
higher-level workloads such as KVM.

### 5.1 Sessions and Jobs

- **`struct oncore_session`**: Bound 1:1 to a `struct liveupdate_session`. It
  owns the session's `struct cpu_preserved_as` isolated address space, tracks
  the `cpumask` of physical CPUs preserved in that session, and embeds a shared
  `struct oncore_runqueue`.
- **`struct oncore_job`**: Represents an independent unit of work (such as a
  single preserved KVM vCPU) submitted to an `oncore_session` via
  `oncore_session_submit_job()`. Each job registers a callback (`oncore_job_fn`)
  that must reside in `.text.cpu_preserved`:
  - `enum oncore_exit_reason (*run_fn)(void *data, u64 deadline_ticks)`

### 5.2 Time-Sliced Round-Robin Scheduling

Each preserved physical CPU in an `oncore_session` executes
`oncore_sched_cpu_worker()` / `oncore_cpu_schedule_loop()` from inside
`cpu_preserved_park_loop()`:

1. **Hardware Counter Deadlines**:
   Because standard kernel timers, `jiffies`, and softirqs are unavailable,
   `oncore` measures scheduling quantums (`oncore.quantum_ms`, defaulting to
   10ms) directly in hardware counter ticks (`rdtsc()` on x86,
   `__arch_counter_get_cntpct()` on ARM64 via `arch_oncore_read_counter()`) and
   passes an absolute `deadline` tick count to
   `curr->run_fn(curr->data, deadline)`. The workload is responsible for
   returning at or before `deadline` (for example, by arming the VMX preemption
   timer, host LAPIC timer, or ARM64 EL2 physical timer).
2. **1:1 Fast-Path Continuation vs. $M > N$ Multiplexing**:
   - When $M \le N$ (e.g., 4 vCPUs on 4 preserved physical CPUs) and the shared
     runqueue has no waiting jobs (`READ_ONCE(rq->nr_runnable) == 0`), the CPU
     keeps the current job (`curr`) running across consecutive quantums without
     acquiring runqueue locks or re-queuing the job.
   - When $M > N$ (oversubscription), jobs rotate through the FIFO runqueue: at
     the end of a quantum, `curr->run_fn()` serializes its hardware context and
     returns, `oncore_sched_put_prev(rq, curr)` enqueues `curr` at the tail of
     `rq->runnable`, and the next loop iteration dequeues the next runnable job.
   - The dequeue logic (`oncore_sched_pick_next()`) prioritizes: (a) jobs with
     `total_runs == 0` to prevent starvation during initial startup, (b) jobs
     whose `assigned_cpu` matches the current physical CPU to preserve cache and
     VMCS/VMCB locality, and (c) work-stealing from the head of the queue.
3. **Exit Reasons and Low-Power Stall Backoff**:
   - `ONCORE_EXIT_QUANTUM_EXPIRED`: The job ran until its quantum deadline and
     remains runnable.
   - `ONCORE_EXIT_YIELD_IDLE`: The job yielded early on a guest idle instruction
     (`HLT`, `PAUSE`, `WFI`, `WFE`) and remains runnable.
   - `ONCORE_EXIT_STALL`: The job encountered a condition it cannot resolve
     during the `kexec` window (such as an unhandled VM-exit) and yielded early.
     The physical CPU executes a low-power wait
     (`arch_cpu_preserved_park_wait()`, `cpu_relax()` on x86 and `wfe()` on
     ARM64) before re-queuing the job so it does not spin at full speed while
     waiting for the incoming kernel to attach.
   - `ONCORE_EXIT_ATTACH_SIGNALED`: The job has been reclaimed by the incoming
     kernel (or cancelled), transitions to `ONCORE_JOB_DEAD`, and is removed
     from the runqueue.
   - `ONCORE_EXIT_ERROR`: The job encountered an unrecoverable entry failure and
     is dropped from the runqueue.

---

## 6. Layer 4: KVM Caretaker Architecture (`caretaker`)

The KVM Caretaker engine (`virt/kvm/caretaker.c`,
`include/linux/kvm_caretaker.h`, `CONFIG_KVM_CARETAKER`) bridges KVM `vcpufd`
preservation (Layer 1) with the `oncore` scheduler (Layer 3).

When `CONFIG_KVM_CARETAKER` (`KVM_CAP_CARETAKER`) is supported and a `vcpufd` is
preserved into a LUO session that also contains preserved physical CPUs:

1. `kvm_caretaker_vcpu_pre_preserve()` allocates a `struct oncore_job` for
   `kvm_arch_vcpu_caretaker_run()` on the session.
2. `kvm_arch_vcpu_luo_preserve()` serializes the vCPU's architectural state into
   `struct kvm_vcpu_arch_ser`, initializes the architecture Caretaker runtime
   page (`kvm_caretaker_init_common_vcpu()`, setting `cb->state` to
   `KVM_CARETAKER_PAUSED` and populating `ser->cb`), and maps the runtime page
   and `arch_state` buffer into the session's isolated `cpu_preserved_as`.
3. `kvm_caretaker_vcpu_post_preserve()` binds `cb` to the job and calls
   `oncore_session_activate_job()`, queuing the job on the `oncore_runqueue`:
   if an idle preserved physical CPU is parked in `cpu_preserved_park_loop()`,
   `oncore_session` attaches to it and kicks it so the vCPU immediately resumes
   guest execution; if all preserved CPUs in the session are already running
   vCPU jobs, the job is time-sliced across them; and if no physical CPUs are
   available in the LUO session, the vCPU remains suspended until CPUs are
   added.

### 6.1 Cross-Kexec ABI Invariant vs. Private Runtime Pages

A design rule in RFCv1 is that **the incoming kernel must never read or depend
on outgoing-kernel internal structures**. Only structures defined in
`include/linux/kho/abi/` may cross the `kexec` boundary:

- **Cross-Kernel KHO ABI (`include/linux/kho/abi/kvm.h`, `kvm_x86.h`,
  `kvm_arm64.h`)**:
  - `struct kvm_caretaker_cb_ser` coordinates the execution state (`state`,
    `enum kvm_caretaker_state`), the physical CPU ID (`pcpu_id`), `vcpu_id`,
    and the KHO pointer (`telemetry`) to `struct kvm_caretaker_telemetry_ser`:

    ```c
    struct kvm_caretaker_cb_ser {
            u32 state;
            u32 pcpu_id;
            u32 vcpu_id;
            DECLARE_KHOSER_PTR(telemetry, struct kvm_caretaker_telemetry_ser *);
    };
    ```

  - `struct kvm_caretaker_arch_ser`: Contains `struct kvm_caretaker_cb_ser cb`
    at offset 0, plus only the minimal architecture fields required by the
    incoming kernel during hardware state adoption (such as `apic_id`,
    `vmcs_pa`, and `preserved_pages_pa[]` on x86, or `cntvoff_el2`, `hcr_el2`,
    `mdcr_el2`, and VGICv3 CPU interface registers on ARM64).
- **Outgoing-Kernel Private Runtime Page**:
  Each architecture allocates a KHO-preserved runtime context page (`struct
  caretaker_x86_page`, `struct caretaker_vmx_page`, `struct caretaker_svm_page`,
  or `struct caretaker_arm64_page`) that is mapped into the `oncore_session`'s
  isolated address space (`cpu_preserved_as`).
  This page embeds `struct kvm_caretaker_arch_ser abi` at **offset 0**,
  while the rest of the page holds private runtime state (saved host registers,
  standalone GDT/IDT/TSS, exception stacks, scratch variables) that is accessed
  *exclusively* by the outgoing kernel's `.text.cpu_preserved` code. When the
  incoming kernel boots, it resolves `ser->cb` only to
  `struct kvm_caretaker_cb_ser *` (or `struct kvm_caretaker_arch_ser *`) and
  frees the raw page once the vCPU transitions to `KVM_CARETAKER_STOPPED`.

### 6.2 Cross-Kernel State Machine

Handoff between the preserved physical CPU (running the outgoing kernel's
`.text.cpu_preserved` code) and the host kernel reclaiming the vCPU (either the
incoming kernel during `kvm_caretaker_vcpu_pre_retrieve()` or the outgoing
kernel on cancellation during `kvm_caretaker_vcpu_unpreserve()`) is coordinated
via atomic `cmpxchg()` transitions on `cb->state`:

```
                  oncore_job->run_fn() (quantum start)
              +------------------------------------------+
              |                                          v
    +-----------------------+                  +-----------------------+
    | KVM_CARETAKER_PAUSED  |                  | KVM_CARETAKER_RUNNING |
    |          (0)          |                  |          (1)          |
    +-----------------------+                  +-----------------------+
       |                 ^                        |                 |
       |                 | detach_serialize()     |                 |
       |                 +------------------------+                 |
       |                quantum end / yield / stall                 |
       |                                                            |
       | Host kernel attach:                Host kernel attach:     |
       | cmpxchg(PAUSED -> STOPPED)         cmpxchg(RUNNING ->      |
       | (Immediate 0ns reclaim)                    STOPPING)       |
       |                                    + kick(cb->pcpu_id)     |
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
    |          (ser->arch_state is complete; host KVM owns vCPU)       |
    +------------------------------------------------------------------+
```

A key invariant of this state machine is **proactive serialization at quantum
boundaries**:

- Before transitioning from `KVM_CARETAKER_RUNNING` to `KVM_CARETAKER_PAUSED`
  (whether due to `oncore` quantum expiration, `ONCORE_EXIT_YIELD_IDLE`, or
  `ONCORE_EXIT_STALL`), the preserved CPU invokes the architecture's
  `detach_serialize()` routine (`ops->detach_serialize()` on x86,
  `arm64_caretaker_detach_serialize()` on ARM64), flushing live hardware
  registers back into the `struct kvm_vcpu_arch_ser` buffer before publishing
  `KVM_CARETAKER_PAUSED`.
- Therefore, whenever `cb->state == KVM_CARETAKER_PAUSED`, `ser->arch_state` is
  guaranteed to hold the complete, up-to-date architectural state of the vCPU.
- When the reclaiming kernel calls `kvm_caretaker_wait_for_attach()`:
  - **Fast Path (`PAUSED -> STOPPED`)**: If `cb->state` is `PAUSED`, a single
    `cmpxchg(&cb->state, KVM_CARETAKER_PAUSED, KVM_CARETAKER_STOPPED)` claims
    the vCPU immediately without waiting for or kicking any physical CPU. When
    the `oncore` scheduler next invokes the job, it observes `STOPPED` and
    returns `ONCORE_EXIT_ATTACH_SIGNALED`.
  - **Active Path (`RUNNING -> STOPPING -> STOPPED`)**: If `cb->state` is
    `RUNNING`, the reclaiming kernel executes
    `cmpxchg(&cb->state, KVM_CARETAKER_RUNNING, KVM_CARETAKER_STOPPING)` and
    sends a physical IPI (`arch_cpu_preserved_kick(pcpu)`). The IPI forces an
    immediate VM-exit on the preserved CPU; the Caretaker loop observes
    `STOPPING` (`kvm_caretaker_should_exit()`), runs `detach_serialize()`,
    publishes `KVM_CARETAKER_STOPPED`, and returns
    `ONCORE_EXIT_ATTACH_SIGNALED`.

Both live-update cancellation and incoming-kernel reclamation use this state
machine to hand the vCPU back to host KVM:

- **Cancellation in the Outgoing Kernel (`kvm_caretaker_vcpu_unpreserve()`)**:
  When the LUO session file descriptor is closed before `kexec`,
  `kvm_caretaker_vcpu_unpreserve()` invokes
  `kvm_arch_vcpu_luo_pre_retrieve_caretaker()`
  (`kvm_caretaker_wait_for_attach()`) to transition `cb->state` to
  `KVM_CARETAKER_STOPPED`, restores the updated architectural state
  (`struct kvm_vcpu_arch_ser`) and hardware control state back into the outgoing
  kernel's existing `struct kvm_vcpu` (`kvm_arch_vcpu_luo_retrieve()` and
  `kvm_arch_vcpu_luo_attach_caretaker()`), cancels the `oncore_job`
  (`oncore_session_cancel_job()`), and unpreserves and frees the Caretaker
  runtime, hardware, and telemetry pages (`kho_unpreserve_free()`). Because
  non-architectural host state (memslots, secondary page tables, and device
  bindings) never left the outgoing kernel's `struct kvm_vcpu`, the VMM can
  immediately resume `KVM_RUN`.
- **Reclamation in the Incoming Kernel (`kvm_caretaker_vm_pre_retrieve()`,
  `kvm_caretaker_vcpu_pre_retrieve()`, `kvm_caretaker_vcpu_retrieve()`, and
  `kvm_caretaker_vcpu_finish()`)**:
  1. When `vmfd` is retrieved (`kvm_luo_retrieve()`),
     `kvm_caretaker_vm_pre_retrieve()` calls `cpu_preserved_detach_workload()`
     on preserved physical CPUs (`ser->workload = CPU_PRESERVED_PARKED` + IPI
     kick), causing any running Caretaker vCPU to VM-exit, run
     `detach_serialize()`, publish `KVM_CARETAKER_PAUSED`, and return the
     physical CPU from `oncore_cpu_schedule_loop()` to
     `cpu_preserved_park_loop()`.
  2. When each `vcpufd` is retrieved (`kvm_vcpu_luo_retrieve()`),
     `kvm_caretaker_vcpu_pre_retrieve()` calls `kvm_caretaker_wait_for_attach()`
     to transition `cb->state` to `KVM_CARETAKER_STOPPED` (invalidating CPU data
     caches over `ser->cb` and `ser->arch_state` on ARM64), restores the updated
     architectural state from `ser->arch_state` (`struct kvm_vcpu_arch_ser`)
     into the newly allocated `struct kvm_vcpu`
     (`kvm_arch_vcpu_luo_retrieve()`), and synchronizes hardware control state
     (`kvm_caretaker_vcpu_retrieve()`).
  3. When `LIVEUPDATE_SESSION_FINISH` is issued (`kvm_vcpu_luo_finish()` ->
     `kvm_caretaker_vcpu_finish()`), the kernel ensures `cb->state` has reached
     `KVM_CARETAKER_STOPPED` (in case `vcpufd` was never retrieved), reports and
     frees the KHO telemetry buffer, and frees the Caretaker runtime page and
     per-vCPU preserved hardware pages (`kho_restore_free()`).

### 6.3 Architecture Backends in RFCv1

In RFCv1, each supported architecture implements `struct kvm_caretaker_ops`
(`enter_guest`, `decode_exit`, `handle_arch_exit`, `advance_rip`, `arm_timer`,
`disarm_timer`, `pre_run`, and `post_run` in `.text.cpu_preserved`), paired on
x86 with vendor `struct kvm_x86_caretaker_ops` (`init`, `sync_vcpu`, and
`runtime->detach_serialize`):

- **x86 Common (`arch/x86/kvm/caretaker.c`)**:
  Initializes `struct caretaker_x86_page`, builds standalone GDT, TSS, and IDT
  tables (`caretaker_x86_idt`, so any NMI or exception in host mode lands in a
  self-contained handler rather than the torn-down Linux IDT), manages guest FPU
  state via `XRSTOR64` / `XSAVE64` directly against
  `state->xsave.region`, and provides `kvm_x86_caretaker_arm_timer()` to program
  the host local APIC timer (`MSR_IA32_TSC_DEADLINE` or `APIC_TMICT`).
- **Intel VMX (`arch/x86/kvm/vmx/caretaker.c`, `caretaker_vmenter.S`)**:
  Preserves `vmcs01` (`vmcs`, `msr_bitmap`, `pml_pg`, `ve_info`) across `kexec`,
  reprograms `HOST_CR3` to the isolated `cpu_preserved_as` CR3 and `HOST_RIP` to
  `vmx_caretaker_exit_handler`, and uses the hardware VMX preemption timer to
  enforce `oncore` quantum deadlines. Upon adoption in the incoming kernel
  (`vmx_caretaker_sync_vcpu()`), because `struct loaded_vmcs` is internal to
  each kernel build, the incoming kernel allocates a new `loaded_vmcs`,
  temporarily loads the preserved `abi->vmcs_pa` via `vmptrld`, copies the
  guest-visible VMCS fields (`vmx_caretaker_guest_fields[]`) into the new VMCS,
  and applies `ser->arch_state`.
- **AMD SVM (`arch/x86/kvm/svm/caretaker.c`, `caretaker_vmenter.S`)**:
  Copies `vmcb01` into the preserved `caretaker_svm_page->vmcb` and allocates a
  dedicated preserved `hsave_area` (`MSR_VM_HSAVE_PA`). Uses the host LAPIC
  timer combined with `INTERCEPT_INTR` to preempt guest execution at the
  `oncore` quantum deadline, and synchronizes dirty VMCB save-area fields
  (`rip`, `rsp`, `rax`, `rflags`, control registers, segment descriptors, and
  syscall/sysenter MSRs) back into `ser->arch_state` in
  `svm_caretaker_detach_serialize()`.
- **ARM64 VHE (`arch/arm64/kvm/caretaker.c`, `caretaker_vmenter.S`)**:
  Installs a standalone EL2 vector table (`caretaker_hyp_vector` in `VBAR_EL2`),
  context-switches EL1 system registers, FP/SIMD state, Pointer Authentication
  keys, Stage-2 MMU registers (`VTCR_EL2`, `VTTBR_EL2`), VGICv3 CPU interface
  registers (`ICH_LR<n>_EL2`, `ICH_AP*R<n>_EL2`, `ICH_VMCR_EL2`, `ICH_HCR_EL2`),
  and the virtual timer (`CNTV_CTL_EL0`, `CNTV_CVAL_EL0`). Enforces `oncore`
  quantums via the EL2 physical timer (`CNTHP_CVAL_EL2` / `CNTHP_CTL_EL2`) and
  writes dirty hardware state back into `cap->abi` and the `kvm_one_reg` entries
  of `cap->arch_state` in `arm64_caretaker_detach_serialize()`.

### 6.4 Gap Telemetry and Observability

Because standard host tracing (`ftrace`, `perf`, `bpf`) cannot run on isolated
CPUs during `kexec`, `struct kvm_caretaker_telemetry_ser` in the KHO ABI
(`include/linux/kho/abi/kvm.h`, `CONFIG_KVM_CARETAKER_DEBUG`) records per-vCPU
activity during the gap:

```c
struct kvm_caretaker_telemetry_ser {
        u64 total_runs;
        u64 total_exits;
        u64 stall_count;
        u64 last_exit_reason;
        u64 last_exit_rip;
        u64 stall_exit_reason;
        u64 stall_exit_rip;
};
```

When the incoming kernel adopts the vCPU, it copies this telemetry from the KHO
ABI struct (`kvm_caretaker_telemetry_report()`) and exposes it under
`/sys/kernel/debug/kvm/<pid>-<fd>/vcpu<N>/caretaker_telemetry`.

---

## 7. Open Design Challenges and Topics for Further Discussion

While the [RFCv1 patch series][rfcv1-series] demonstrates end-to-end continuous
vCPU execution across `kexec` on Intel VMX, AMD SVM, and ARM64 VHE, it is an
initial proof-of-concept. Several architectural questions, both from the
[initial proposal discussion][initial-proposal] and from prototyping RFCv1,
still need to be addressed or discussed further with upstream maintainers.

### 7.1 VM-Exit Handling Strategy and Sharing Code with KVM

A central open design question is how the Caretaker should enter the guest and
handle VM-exits without duplicating KVM into a second in-kernel hypervisor.

In the [initial LKML discussion][initial-proposal], Paolo Bonzini noted that the
Caretaker is the non-preemptible inner part of `vcpu_enter_guest()`, and that
`vmx_exit_handlers_fastpath()` / `svm_exit_handlers_fastpath()` already provide
a blueprint for exits that can be handled with interrupts disabled.

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
  preemption), the Caretaker does not attempt to decode or emulate the
  instruction; it serializes state, returns `ONCORE_EXIT_STALL`, and waits in a
  low-power loop until the incoming kernel finishes booting and reclaims the
  vCPU, where standard KVM handles the pending exit normally.

Even with a zero-exit or minimal-exit baseline, however, the Caretaker still
requires world-switch assembly (`caretaker_vmenter.S`), guest/host register
context switching, and hardware control structure (`VMCS` / `VMCB` / EL2 sysreg)
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
   Annotating existing functions across `arch/x86/kvm/` or `arch/arm64/kvm/`
   with `__cpu_preserved_text` is fragile: if a shared KVM function makes even
   one transitive call to an unannotated helper (`WARN_ON_ONCE()`, `printk()`,
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
   Standard KVM code assumes a valid Linux task context (`current`), host
   per-CPU variables (`%gs` on x86, `TPIDR_EL1` on ARM64), preemption tracking,
   lockdep, and RCU. On a preserved CPU that is offline and `!cpu_present(cpu)`,
   running on a standalone 4KB/16KB stack across `kexec`, none of those
   facilities exist.
4. **Cross-Kernel Struct Layout Drift**:
   Internal kernel structures (`struct kvm_vcpu`, `struct vcpu_vmx`,
   `struct vcpu_svm`) are not an ABI and can change size or field offsets
   between the outgoing and incoming kernels. While the outgoing kernel's
   `.text.cpu_preserved` only executes until `KVM_CARETAKER_STOPPED`, the
   handoff boundary to the incoming kernel must still translate into a fixed KHO
   ABI (`struct kvm_vcpu_arch_ser`).

#### Candidate Approaches for Code Sharing

We must converge with upstream maintainers on how to balance code reuse against
isolation safety. The primary options under consideration are:

- **Option 1: Strict Zero/Minimal Exit Policy + Hardware Virtualization
  Offload**
  - Keep the Caretaker restricted to world-switch entry/exit and quantum
    preemption, and rely on hardware virtualization features (Intel APICv /
    Posted Interrupts / IPI virtualization, AMD AVIC, ARM64 GICv4.1 direct
    vLPI/vSGI injection, and direct timer passthrough) to keep guests running
    without VM-exits. Any guest action that forces a software VM-exit stalls
    (`ONCORE_EXIT_STALL`) until the incoming kernel re-attaches.
  - *Trade-off*: Eliminates exit-handler duplication and keeps the preserved
    attack surface minimal, though each architecture still needs a small
    world-switch assembly stub (or shared macro) and workloads without hardware
    interrupt/timer offload will stall upon their first timer or IPI exit.
- **Option 2: Refactoring Low-Level World-Switch Macros and Stateless
  Primitives**
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
    save/restore logic without pulling `struct kvm_vcpu` or host heap
    dependencies into the isolated address space, at the cost of refactoring
    KVM's low-level entry/exit assembly.
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
    similar split-compilation boundary into `arch/x86/kvm/` requires
    refactoring.
- **Option 4: Mapping Outgoing `struct kvm_vcpu` into `cpu_preserved_as` During
  the Gap**
  - Because the code running in `.text.cpu_preserved` during the `kexec` window
    belongs to the *outgoing* kernel image, its struct layout matches the
    outgoing kernel's `struct kvm_vcpu`. If `struct kvm_vcpu` (and its immediate
    sub-structures) are preserved in KHO and mapped into `cpu_preserved_as`,
    outgoing `.text.cpu_preserved` code can operate on `struct kvm_vcpu *`
    during the gap and serialize into the versioned KHO ABI
    (`struct kvm_vcpu_arch_ser`) only when transitioning to
    `KVM_CARETAKER_STOPPED` for the incoming kernel.
  - *Trade-off*: Avoids inventing parallel per-arch context structs for the
    runtime loop, but still requires auditing every shared function to ensure it
    never chases unmapped pointers (`vcpu->kvm`, `memslots`), touches `current`
    or per-CPU variables, or invokes unpreserved kernel helpers.

### 7.2 Unresolved Items from the Initial LKML Discussion

Several points raised during the [initial proposal discussion][initial-proposal]
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
3. **`HLT` / Idle Exit Handling and `oncore` $M > N$ Multiplexing vs. 1:1
   Pinning**:
   In the initial discussion, it was suggested that initial support should
   assume a 1:1 mapping of active vCPUs to isolated physical CPUs, and that
   `HLT` exits during the gap should either be skipped or have their hardware
   intercepts disabled (`HLT`/`PAUSE`/`MONITOR`/`MWAIT`, e.g. via
   `KVM_CAP_X86_DISABLE_EXITS`). RFCv1 instead introduced the `oncore` scheduler
   to support $M > N$ oversubscription and kept `HLT`/`WFI` intercepts enabled
   so an idle vCPU yields its quantum to other runnable vCPUs on the same pCPU.
   Whether $M > N$ multiplexing during the brief `kexec` window justifies the
   complexity of `oncore`, versus enforcing 1:1 vCPU-to-pCPU pinning (and
   leaving any excess vCPUs suspended in RAM across `kexec`) with idle
   intercepts disabled, should be decided with maintainers.
4. **Moving APIC Emulation into Standard KVM Fastpaths**:
   For multi-vCPU guests without hardware IPI virtualization (Intel APICv/IPIv,
   AMD AVIC), moving a subset of APIC emulation (such as `APIC_ICR` / IPI
   delivery) into `vmx_exit_handlers_fastpath()` and
   `svm_exit_handlers_fastpath()` was proposed as a standalone improvement that
   benefits both normal KVM and the Caretaker. In RFCv1, ARM64 emulates
   `ICC_SGI1R_EL1` in the Caretaker, whereas x86 currently treats x2APIC
   `APIC_ICR` writes as a blocking exit (`ONCORE_EXIT_STALL`).
5. **Decoupling Asynchronous vCPU Execution from `kexec` (`KVM_RUN_ASYNC` / VMM
   Upgrade)**:
   Paolo Bonzini and David Woodhouse discussed decoupling detached vCPU
   execution from `kexec` so that a userspace VMM can detach, restart or upgrade
   itself, and re-attach to running vCPUs without a kernel reboot, potentially
   staged as: (a) `ioctl(KVM_RUN_ASYNC)` running the vCPU in a
   `struct vhost_task` kernel thread, (b) `vmfd`/`vcpufd` handoff to a new `mm`,
   (c) ASI during normal `KVM_RUN` (enabled via a module parameter), and (d)
   `kexec` serialization on top. RFCv1 supports intra-kernel start/cancel loops
   through LUO session preservation and retrieval, but does not implement
   `KVM_RUN_ASYNC` (`vhost_task`) or always-on ASI during normal `KVM_RUN`.
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
     using `cpu_relax()` (`PAUSE`) on x86 and `wfe()` on ARM64 in
     `arch_cpu_preserved_park_wait()`, and requesting maximum autonomous
     performance (`MSR_HWP_REQUEST`, `MSR_IA32_ENERGY_PERF_BIAS`,
     `MSR_AMD_CPPC_REQ`) before entering the preserved runtime. Both
     architectures switch to standalone descriptor and vector tables in
     `.text.cpu_preserved` / `.data.cpu_preserved` (`x86_preserved_idt` and
     `caretaker_x86_idt` on x86, `caretaker_hyp_vector` in `VBAR_EL2` and
     `VBAR_EL1` on ARM64) so an asynchronous interrupt or exception during
     `kexec` never vectors into torn-down host kernel text.
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
       `x86_preserved_idt` and `caretaker_x86_idt` return immediately via
       `iretq` (`x86_preserved_iret_stub` / `x86_preserved_iret_err_stub`).
       While `NMI` (vector 2) must `iretq` because x86 uses NMI IPIs to wake
       preserved cores, a Machine Check Exception (`#MC`, vector 18) or
       synchronous fault (`#DF`, `#GP`, `#PF`) in preserved host context would
       re-fault in a loop or triple-fault if `MSR_IA32_MCG_STATUS.RIPV == 0`.
       Pointing `#MC` and synchronous fault vectors in `x86_preserved_idt` and
       `caretaker_x86_idt` to a dedicated preserved park stub (which clears
       `MSR_IA32_MCG_STATUS`, records fault telemetry, and parks the core in the
       exit-check loop) prevents a localized fault on one preserved core from
       resetting the machine during `kexec`.
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
     serialize the complete nested state (~4-8 KB per vCPU), including
     guest-mode and pending-entry flags (`KVM_STATE_NESTED_GUEST_MODE`,
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
     an L2 vCPU running in the Caretaker across `kexec`,
     `kvm_mmu_preserve_kho()` (x86) and `kvm_arch_vm_luo_preserve()` (ARM64)
     must also walk and preserve the active nested shadow TDP page-table pages
     via `kho_preserve_folio()`.

2. **Caretaker Execution During the `kexec` Window (Run Current Mode, Stall on
   Nested World Switches)**:
   - Emulating L1 $\leftrightarrow$ L2 world switches (`VMLAUNCH`, `VMRESUME`,
     `VMRUN`, and L2 $\rightarrow$ L1 nested VM-exits) inside
     `.text.cpu_preserved` would require pulling thousands of lines of nested
     hypervisor state machines (`arch/x86/kvm/vmx/nested.c`,
     `arch/x86/kvm/svm/nested.c`) and dynamic shadow EPT page-table construction
     into the Caretaker, conflicting with its zero-allocation, minimal-TCB
     design.
   - Instead, the Caretaker can keep the vCPU executing in whichever mode (L1 or
     L2) was active at `.preserve()` time and stall only if a nested world
     switch is required during the `kexec` window:
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

### 7.5 Paravirtual `virtio` Devices and In-Kernel Alternatives (`virtio-rng`)

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

1. **Architectural CPU Alternatives (`RDRAND`/`RDSEED` and `FEAT_RNG` Instead of
   `virtio-rng`)**:
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

3. **Lightweight In-Kernel / Caretaker Virtqueue Servicing for Stateless
   Devices**:
   - For simple, stateless paravirtual devices like `virtio-rng` (where a guest
     without `RDRAND`/`FEAT_RNG` might wait for a buffer to be filled) or
     `virtio-console` transmit, an in-kernel handler or a minimal Caretaker
     virtqueue helper mapped to the device's virtqueue pages in preserved guest
     RAM can consume `avail` ring descriptors, fill `virtio-rng` buffers using
     host hardware RNG instructions (or drain console output), and advance the
     `used` ring index without VMM involvement.

### 7.6 "Kernel Caretaker" for Preserved Userspace Processes (DPDK, gVisor)

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

2. **Execution Model Across `kexec` (Preserved Stack/Memory + Stall on Kernel
   Entrance)**:
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

[initial-proposal]: https://lore.kernel.org/all/afEwWZksU0Fw61oT@plex
[rfcv1-series]: https://lore.kernel.org/all/20260920193650.3373435-1-pasha.tatashin@soleen.com
[luo-abi-rfc]: https://lore.kernel.org/kexec/20260903023452.721732-1-loganodell@google.com
[lpc-compat]: https://lpc.events/event/20/contributions/2613
[section-7]: #7-open-design-challenges-and-topics-for-further-discussion
[guest-memfd-series]: https://lore.kernel.org/all/20260728121138.1103610-1-tarunsahu@google.com/
[pci-lu-series]: https://lore.kernel.org/all/20260918200640.887030-1-dmatlack@google.com/
[vfio-lu-series]: https://lore.kernel.org/all/20260714151505.3466855-1-vipinsh@google.com/
[iommu-lu-series]: https://lore.kernel.org/all/20260921004834.2601285-1-skhawaja@google.com/#t
