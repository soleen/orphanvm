# Orphaned Virtual Machines: The Caretaker approach for Live Update

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
   dedicated linker sections (`.cpu_preserved.text` and `.cpu_preserved.data`).
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
> corresponding to the [RFCv1 patch series][rfcv1-series] and subsequent
> refinements. Several aspects, including VM-exit handling boundaries and open
> items from the initial LKML discussion, remain under active discussion and are
> covered in [Section 7][section-7].

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
|         |                 | `KVM_CAP_CARETAKER`        | Cross-kernel state machine (`PAUSED`/`RUNNING`/`STOPPED`/`FAILED`)|
|         |                 |                            | Arch guest entry/exit loop (Intel VMX, AMD SVM, ARM64 VHE)        |
| Layer 3 | `oncore`        | `CONFIG_LIVEUPDATE_ONCORE` | Workload-agnostic session (`oncore_session`) & job (`oncore_job`) |
|         |                 |                            | Time-sliced round-robin FIFO runqueue                             |
|         |                 |                            | Multiplexes $M$ jobs across $N$ preserved physical CPUs           |
| Layer 2 | `cpu_preserve`  | `CONFIG_LIVEUPDATE_CPU`    | `.cpu_preserved.text` & `.cpu_preserved.data`                     |
|         |                 | `/sys/.../cpu<N>/preserve` | CPU hotplug interception & `cpu_is_preserved(cpu)` SMP isolation  |
|         |                 |                            | Isolated page tables (`cpu_preserved_as_ser`) & stack context     |
| Layer 1 | `kvm_luo`       | `KVM_CAP_VCPU_PRESERVE`    | `vmfd` & secondary page table (TDP / Stage-2) LUO preservation    |
|         |                 | `"kvm_vcpu_luo_*_v1"`      | `vcpufd` architectural state serialization (`kvm_vcpu_arch_ser`)  |

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
   - `vmfd`, registering the VM with the LUO session and allocating
     `struct kvm_luo_ser`
   - (Optional) Physical CPU descriptors
     (`/sys/devices/system/cpu/cpu<N>/preserve`), preserving them into the LUO
     session, which removes the target physical CPUs from the host scheduler and
     parks them in `cpu_preserved_park_loop()` (in `.cpu_preserved.text` on a
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
   into the outgoing `struct kvm_vcpu` (if modified by the Caretaker), and any
   preserved physical CPUs return to the host scheduler so the VMM can resume
   `KVM_RUN`.
4. **The Kexec Gap**:
   The host performs a `kexec` reboot. During `liveupdate_reboot()`, LUO invokes
   `.freeze()` (`luo_freeze()`) across all preserved resources, verifying
   cross-FD dependencies (`vm_token` for `guest_memfd` and `vcpufd`) and walking
   and KHO-preserving the VM's quiescent secondary page tables
   (`kvm_luo_freeze()`). Management CPUs then reboot into the incoming kernel
   while the preserved physical CPUs (`cpu_is_preserved(cpu)`) continue
   executing guest vCPUs inside the Caretaker's isolated address space.
5. **Reclamation**:
   Once the incoming kernel boots, the new VMM instance opens the preserved LUO
   session and issues `LIVEUPDATE_SESSION_RETRIEVE_FD` for the
   `memfd`/`guest_memfd`, `vmfd`, and `vcpufd` tokens (preserved physical CPU
   tokens do not need to be retrieved; if retrieved, any active workload on that
   CPU is detached while the CPU stays parked until session finish). Retrieving
   each `vcpufd` detaches the vCPU from the Caretaker and restores its updated
   architectural state into the new kernel's `struct kvm_vcpu`. Issuing
   `LIVEUPDATE_SESSION_FINISH` (or closing the session file descriptor) frees
   the KHO handover structures, returns the preserved physical CPUs to the host
   scheduler, and allows the VMM to resume normal `KVM_RUN` threads.

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
  the Caretaker control block (`cb`) and architectural state buffer
  (`arch_state`):

  ```c
  struct kvm_vcpu_ser {
          u32 vcpu_id;
          u64 vm_token;
          DECLARE_KHOSER_PTR(cb, struct kvm_caretaker_cb_ser *);
          DECLARE_KHOSER_PTR(arch_state, struct kvm_vcpu_arch_ser *);
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
    `RUNNING`, `STOPPING`, `STOPPED`, `FAILED`) and tracks which physical CPU
    owns the vCPU while `arch_state` serves as the register handoff buffer
    between the outgoing kernel, the Caretaker, and the incoming kernel.

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
            struct kvm_sregs2 sregs;           /* KVM_GET/SET_SREGS2 */
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

Once a VM is running in Caretaker mode, secondary page faults are not currently
supported. For the guest to continue accessing its memory without triggering
faults (Intel EPT Violations, AMD NPT faults, or ARM64 Stage-2 translation
faults) that would stall the vCPU until it is retrieved after the reboot, the
existing secondary page tables must be preserved.

Because secondary page tables are a VM-wide resource shared across all vCPUs,
they are associated with `vmfd`. This proposal extends `struct kvm_luo_ser`
(introduced by the `guest_memfd` preservation series, where `type` records the
`KVM_CREATE_VM` machine type argument, such as a regular vs.
protected/confidential VM on x86 and IPA size / protected mode on ARM64) with a
KHO pointer (`kho_folios`) to the preserved secondary page table folio list
(`struct kvm_kho_folios_ser`):

```c
struct kvm_kho_folios_ser {
        u64 nr_folios;
        u64 folios_pa[];
};

struct kvm_luo_ser {
        u64 type;                                  /* KVM_CREATE_VM type */
        DECLARE_KHOSER_PTR(kho_folios, struct kvm_kho_folios_ser *);
};
```

While `struct kvm_luo_ser` is allocated when `vmfd` is preserved into the LUO
session, walking and KHO-preserving the secondary page tables is deferred to the
`.freeze()` phase (`kvm_luo_freeze()`). Because `vmfd` and `vcpufd`s can be
preserved in any order (with `vm_token` dependencies resolved during
`.freeze()`), vCPUs may still be executing in `KVM_RUN` and faulting in or
zapping secondary page tables when `vmfd` `.preserve()` is called. By
`.freeze()` time (`reboot(LINUX_REBOOT_CMD_KEXEC)`), all `vcpufd`s have been
preserved and detached from host `KVM_RUN`, so the secondary page tables are
quiescent.

### 3.3 LUO File Lifecycle (`vmfd` and `vcpufd`)

The `vmfd` (`kvm_luo_file_ops`) and `vcpufd` (`kvm_vcpu_luo_file_ops`) handlers
implement the five `struct liveupdate_file_ops` callbacks:

| Callback        | Trigger                          | `vmfd` (`kvm_luo_*`)                                              | `vcpufd` (`kvm_vcpu_luo_*`)                                                 |
| :-------------- | :------------------------------- | :---------------------------------------------------------------- | :-------------------------------------------------------------------------- |
| `.preserve()`   | `LIVEUPDATE_SESSION_PRESERVE_FD` | Allocates `kvm_luo_ser` and records `KVM_CREATE_VM` `type`        | Completes pending I/O, serializes `kvm_vcpu_arch_ser`, hands off to Caretaker |
| `.freeze()`     | `reboot(LINUX_REBOOT_CMD_KEXEC)` | Walks quiescent secondary page tables and preserves folios in KHO | Resolves parent `ser->vm_token` via `liveupdate_get_token_outgoing()`       |
| `.unpreserve()` | `close(session_fd)` pre-`kexec`  | Unpreserves secondary page tables (if frozen) and frees `ser`     | Detaches from Caretaker, restores state to outgoing `kvm_vcpu`, frees `ser` |
| `.retrieve()`   | `LIVEUPDATE_SESSION_RETRIEVE_FD` | Creates new `struct kvm`; keeps old page tables until `.finish()` | Stops Caretaker vCPU, creates new `kvm_vcpu`, and restores `arch_state`     |
| `.finish()`     | `LIVEUPDATE_SESSION_FINISH`      | Restores and frees outgoing secondary page table folios and `ser` | Stops Caretaker (if unretrieved) and frees all KHO vCPU buffers             |

Two ordering properties govern how these callbacks interact:

1. **Order-Independent Preservation (`.preserve()` -> `.freeze()`)**:
   Userspace can preserve `memfd`/`guest_memfd`, `vmfd`, and `vcpufd`s into a
   LUO session in any order. Both `guest_memfd` and `vcpufd` defer looking up
   their parent `vm_token` (`liveupdate_get_token_outgoing()`) until
   `.freeze()`, when all files have been added to the session and all vCPUs have
   detached from `KVM_RUN` (leaving the VM's secondary page tables quiescent for
   `kvm_luo_freeze()`).
2. **Order-Independent Retrieval (`.retrieve()` -> `.finish()`)**:
   In the incoming kernel, LUO resolves `ser->vm_token` on demand via
   `liveupdate_get_file_incoming()`, automatically retrieving `vmfd` first if
   `guest_memfd` or `vcpufd` is retrieved before `vmfd` (note that if the VM
   uses an in-kernel IRQchip, the VMM still retrieves `vmfd` and creates the
   IRQchip before retrieving `vcpufd`s so per-vCPU LAPIC/VGICv3 state can be
   restored). Meanwhile, the outgoing kernel's KHO-preserved secondary page
   table folios remain alive across `.retrieve()` (so Caretaker vCPUs can
   continue executing until each `vcpufd` is retrieved) and are freed during
   `.finish()`.

---

## 4. Layer 2: Physical CPU Preservation (`cpu_preserve`)

The `cpu_preserve` subsystem (`kernel/liveupdate/cpu_preserve.c`,
`CONFIG_LIVEUPDATE_CPU`) provides the generic mechanism for detaching a physical
CPU from the outgoing Linux kernel, keeping it executing in a self-contained
memory environment across `kexec`, and returning it to the incoming Linux kernel
when the live update completes.

### 4.1 Lifecycle of Preserved Physical CPUs

Each non-boot physical CPU exposes a sysfs control file:
`/sys/devices/system/cpu/cpu<N>/preserve` (and the global mask of preserved CPUs
is readable at `/sys/devices/system/cpu/preserved`). Preserving this file
descriptor into a LUO session transitions the physical core through the
following lifecycle across `kexec`:

```
               LIVEUPDATE_SESSION_PRESERVE_FD
  [ Online ] ---------------------------------> [ Offline ]
      ^                                              |
      | device_online()                              | cpu_preserved_park()
      | cpu_preserved_clear(cpu)                     | cpu_preserved_set(cpu)
      |                                              v
  [ Offline ] <-------------------------------- [ Preserved ]
                LIVEUPDATE_SESSION_FINISH        - survives kexec
                (or optional pre-kexec cancel)   - runs park_loop / jobs
```

1. **Entering Preserved Mode (`Online -> Offline -> Preserved`)**:
   Preserving `/sys/devices/system/cpu/cpu<N>/preserve` records the CPU's
   hardware identifier (`arch_cpu_preserved_hwid(cpu)`), marks the CPU in
   `cpu_preserved_mask` (`cpu_preserved_set(cpu)`), and invokes standard Linux
   CPU hotplug (`device_offline(get_cpu_device(cpu))`) to migrate all tasks,
   timers, and interrupts off the core. At the final offline step
   (`cpuhp_ap_report_dead()` -> `cpu_preserved_report_dead()`), instead of
   entering a platform sleep state (`play_dead()` / ACPI / PSCI), the core
   switches to its preserved stack and isolated page tables and enters
   `cpu_preserved_park_loop()`.
2. **(Optional) Pre-`kexec` Cancellation (`Preserved -> Offline -> Online`)**:
   If the LUO session is closed before `kexec`, `.unpreserve()` aborts the
   preservation and returns the core to the outgoing kernel using the same
   reclamation sequence as step 4.
3. **Surviving `kexec` (`cpu_is_preserved(cpu)`)**:
   Because the core is offline and marked `cpu_is_preserved(cpu)`, the outgoing
   kernel's `kexec` shutdown path skips sending stop IPIs to it (`cpus_stop_mask`
   only includes online CPUs). During early boot of the incoming kernel,
   `cpu_preserve_early_init()` (`early_initcall`) retrieves the global FLB,
   verifies that the incoming kernel's architectural mode matches the outgoing
   kernel's `arch_mode`, resolves each preserved CPU's hardware ID (`ser->hwid`
   via `arch_match_cpu_phys_id()`) to its logical CPU ID in the incoming kernel,
   and sets `cpu_preserved_mask`. When `smp_init()` runs, `_cpu_up()` returns
   `-EBUSY` for any CPU where `cpu_is_preserved(cpu)` is true (and
   `cpuhp_smt_enable()` skips it), preventing SMP bringup from resetting the
   core (`INIT`/`SIPI` on x86, PSCI `CPU_ON` on ARM64).
4. **Returning to the Host (`Preserved -> Offline -> Online`)**:
   On `LIVEUPDATE_SESSION_FINISH` in the incoming kernel (or pre-`kexec`
   cancellation), the host detaches any running workload, sets
   `ser->state = CPU_PRESERVED_EXITING`, kicks the physical CPU to exit
   `cpu_preserved_park_loop()`, waits for `CPU_PRESERVED_DEAD`, clears
   `cpu_preserved_mask`, and brings the CPU back online via `device_online()`
   (which triggers a fresh hardware bringup into the host kernel).

### 4.2 Preserved Sections, Build Model, and Relocation Outside KHO Scratch

All code and static data executed by a preserved CPU during the `kexec` window
are placed in dedicated linker sections (`scripts/cpu_preserved.lds.S`,
`include/asm-generic/vmlinux.lds.h`):

- `.cpu_preserved.text` (`__cpu_preserved_text_start` ..
  `__cpu_preserved_text_end`)
- `.cpu_preserved.rodata` and `.cpu_preserved.ex_table`
  (`__cpu_preserved_data_start` .. `__cpu_preserved_rodata_end`)
- `.cpu_preserved.data` and `.cpu_preserved.bss`
  (`__cpu_preserved_rodata_end` .. `__cpu_preserved_data_end`)

Modeled after `arch/arm64/kvm/hyp/nvhe/`, preserved runtime translation units
(`*.preserved.o`) are built through a dedicated rule in
`scripts/Makefile.cpu_preserve`: each file is compiled with `PRESERVED_CFLAGS`
(including `-D__CPU_PRESERVED_RUNTIME__` and
`-include include/linux/cpu_preserve_compat.h`), partially linked with
`ld -r -T scripts/cpu_preserved.lds --orphan-handling=error` to fold `.text*`,
`.rodata*`, `__ex_table`, `.data*`, and `.bss*` into `.cpu_preserved.*`
sections, and symbol-prefixed with `objcopy --prefix-symbols=__cpu_preserved_`.
Host kernel code references preserved symbols using `cpu_preserved_sym(sym)`:

| Mechanism                                               | Scope        | Purpose                                                                     |
| :------------------------------------------------------ | :----------- | :-------------------------------------------------------------------------- |
| `*.preserved.o` + `scripts/cpu_preserved.lds.S`         | Build / Link | Redirects `.text`, `.rodata`, `__ex_table`, `.data`, `.bss` into `.cpu_preserved.*` |
| `objcopy --prefix-symbols=__cpu_preserved_`             | Build / Link | Namespaces preserved symbols; unresolved calls to host symbols fail linking |
| `cpu_preserve_compat.h` (`#undef CONFIG_BUG`, etc.)     | Compiler TU  | Disables out-of-line `BUG`/`WARN` traps, irqflags tracing, and tracepoints  |
| `-fno-stack-protector`, `-fsanitize=...(none)`          | Compiler TU  | Disables stack-canary (`%gs:0x28` / `sp_el0`), KASAN, KCSAN, UBSAN, KCOV, SCS |
| `-mindirect-branch=keep`, `-mfunction-return=keep`      | Compiler TU  | Emits raw indirect branches/returns on x86 instead of `.text` thunks        |
| `-fno-jump-tables`                                      | Compiler TU  | Prevents `switch` statements from emitting unprefixed jump tables           |
| `-ftrivial-auto-var-init=uninitialized`                 | Compiler TU  | Prevents compiler-synthesized `memset()` calls for stack locals             |
| `$(DISABLE_KSTACK_ERASE)`, `-DDISABLE_BRANCH_PROFILING` | Compiler TU  | Prevents `stackleak_track_stack()` and `ftrace_likely_update()` calls       |
| `-mbranch-protection=none`                              | Compiler TU  | Disables PAC/BTI instructions in preserved ARM64 code                       |
| `objtool` & `modpost` section checks                    | Build / Link | Rejects calls or relocations from `.cpu_preserved.*` to host sections       |

Because the compiled kernel image resides in KHO scratch memory (which `kexec`
specifically uses to load the incoming kernel and early boot allocations so
preserved memory is not overwritten), `.cpu_preserved.text` and
`.cpu_preserved.data` cannot be preserved in place. Instead, similar to how
`kexec` (`relocate_kernel`) and hibernation (`swsusp_arch_resume`) copy their
transition stubs into safe pages mapped via x86 `ident_map` or ARM64
`trans_pgd`, `cpu_preserve` allocates KHO-preserved pages outside KHO scratch
(`cpu_preserved_relocate_runtime()`), copies `.cpu_preserved.text` and
`.cpu_preserved.data` (including `.rodata`, `.ex_table`, and `.bss`) into them,
and maps the kernel virtual addresses of those sections to the new physical
pages in each session's isolated page tables so linked symbol references
continue to work across `kexec`.

### 4.3 Isolated Address Space and Stack Context

Before `kexec` overwrites the outgoing kernel's page tables, each preserved CPU
switches its MMU root (`CR3` on x86, `TTBR1_EL1`/`TTBR0_EL2` on ARM64) to its
session's isolated address space (`struct cpu_preserved_as_ser`), constructed
using `kernel_ident_mapping_init()` on x86 and `trans_pgd_map_range()` on
ARM64.

Each session's isolated page tables map **only**:
- `.cpu_preserved.text` (`PAGE_KERNEL_ROX`)
- `.cpu_preserved.rodata` and `.cpu_preserved.ex_table` (`PAGE_KERNEL_RO`)
- `.cpu_preserved.data` and `.cpu_preserved.bss` (`PAGE_KERNEL`)
- The per-CPU `struct cpu_preserved_ser` descriptors and preserved stacks of
  CPUs belonging to that session
- Explicitly mapped workload buffers registered into the session's address space
  via `oncore_session_map_range()` (`cpu_preserved_as_map()`)

No other host kernel memory (neither the linear direct map `PAGE_OFFSET`, nor
`vmalloc`, nor normal `.text`/`.data`, nor other sessions' stacks or buffers) is
mapped in the session's isolated page tables.

To avoid relying on host per-CPU offset registers (`%gs` on x86 or
`TPIDR_EL1`/`TPIDR_EL2` on ARM64) or global per-CPU arrays, per-CPU execution
metadata (`struct cpu_preserved_stack_context`, `include/linux/cpu_preserve.h`)
is placed on page 0 at the base of the power-of-two aligned preserved stack
allocation (`CPU_PRESERVED_STACK_SIZE` = `2 * THREAD_SIZE`, or `4 * PAGE_SIZE`
on ARM64 when `THREAD_SIZE < 2 * PAGE_SIZE`), followed by an **unmapped hardware
guard page** at `CPU_PRESERVED_STACK_GUARD` (`PAGE_SIZE`) and the usable stack
in `[CPU_PRESERVED_STACK_BASE, CPU_PRESERVED_STACK_SIZE)`. The context is
located in $O(1)$ time by masking the stack pointer
(`sp & ~(CPU_PRESERVED_STACK_SIZE - 1)`):

```c
struct cpu_preserved_stack_context {
        u64 magic;
        u32 cpu;
        u8 oncore_busy;
        u8 oncore_tickless;
        u64 oncore_curr_job;
        u64 workload_context;
        u64 session_pgd_pa;
        struct cpu_preserved_ser *ser;
        void (*entry_fn)(void *data);
        void *running_priv;
#ifdef CONFIG_X86_64
        struct x86_preserved_fault fault;
        struct x86_preserved_cpu x86;
#elif defined(CONFIG_ARM64)
        struct arm64_preserved_fault fault;
#endif
};
```

- `magic`: Validation signature (`CPU_PRESERVED_STACK_MAGIC`, `"CPUPSTAK"`) at
  offset `0`, checked by `cpu_preserved_get_stack_context()` after masking the
  stack pointer to confirm the CPU is executing on a valid preserved stack.
- `cpu`: Logical CPU identifier of the preserved physical CPU.
- `oncore_busy`, `oncore_tickless`, and `oncore_curr_job`: Per-CPU `oncore`
  scheduler state indicating whether a job callback is currently executing,
  whether the CPU is running in tickless 1:1 mode, and the pointer to the
  currently running `struct oncore_job`.
- `workload_context`: Opaque pointer to the owning workload or session context
  (e.g., `struct oncore_session *` passed to `entry_fn`), allowing the core to
  locate its session runqueue without host globals.
- `session_pgd_pa`: Physical address of the session's isolated root page table
  (`as_ser->pgd_pa`), loaded into `CR3` (x86) or `TTBR1_EL1`/`TTBR0_EL2`
  (ARM64) via `arch_cpu_preserved_switch_pgd()`.
- `ser`: Pointer to the CPU's KHO-preserved `struct cpu_preserved_ser`
  descriptor so `cpu_preserved_park_loop()`, `cpu_preserved_should_exit()`, and
  `cpu_preserved_set_dead()` can read and update `ser->state` directly without
  indexing a global array.
- `entry_fn`: Workload callback executed by `cpu_preserved_park_loop()` when
  `ser->state` is `CPU_PRESERVED_WORKLOAD`.
- `running_priv`: Workload-specific pointer for the currently executing job
  (e.g., `struct kvm_caretaker_vcpu *` on ARM64 and x86 fault recovery).
- `fault` and `x86`: Architecture-specific exception diagnostic record
  (`struct x86_preserved_fault` / `struct arm64_preserved_fault`) and per-CPU
  standalone hardware structures on x86 (`struct x86_preserved_cpu`, containing
  the preserved GDT, TSS, and IST stacks for `#DF`, `NMI`, and `#MC`).

> **Note on KHO ABI vs. Isolated Runtime Structures**: Although
> `struct cpu_preserved_stack_context` (along with isolated runtime structures
> such as `struct oncore_session`, `struct oncore_job`, and internal caretaker
> structures) resides in memory that survives `kexec`, it is not part of the KHO
> serialization ABI (`include/linux/kho/abi/`). During the `kexec` window, these
> structures are accessed exclusively by the outgoing kernel's isolated
> `.cpu_preserved.text` and caretaker code running on the preserved CPUs and are
> never interpreted by the incoming kernel (which only frees the underlying
> pages upon reclamation). Only structures that are directly read and
> interpreted by the incoming kernel (via FLB during early boot, or via
> `.retrieve()` and `.finish()` from userspace once the kernel is fully booted)
> must be defined in `include/linux/kho/abi/`.

### 4.4 Preserved Physical CPUs Serialization ABIs

Physical CPU preservation state is handed over across `kexec` using four KHO
ABI structures defined in `include/linux/kho/abi/cpu.h`:

1. **Per-CPU State (`struct cpu_preserved_ser`)**:
   Serialized when `/sys/devices/system/cpu/cpu<N>/preserve` is preserved via
   `LIVEUPDATE_SESSION_PRESERVE_FD`:

   ```c
   struct cpu_preserved_ser {
           u32 cpu;
           u32 state;
           u64 stack_pa;
           DECLARE_KHOSER_PTR(session, struct cpu_preserved_session_ser *);
           u64 hwid;
           DECLARE_KHOSER_PTR(next, struct cpu_preserved_ser *);
   };
   ```

   - `cpu`: Logical CPU identifier of the preserved physical CPU (updated in the
     incoming kernel during early boot by matching `hwid`).
   - `state`: Per-CPU execution state (`CPU_PRESERVED_PARKING`,
     `CPU_PRESERVED_PARKED`, `CPU_PRESERVED_EXITING`, `CPU_PRESERVED_DEAD`,
     `CPU_PRESERVED_FAULTED`, `CPU_PRESERVED_WORKLOAD`,
     `CPU_PRESERVED_DETACHING`), shared between the preserved CPU and the host
     kernel.
   - `stack_pa`: Physical address of the KHO-preserved stack allocation for this
     CPU's isolated execution context (freed during `.finish()`).
   - `session`: KHO pointer to the per-session preservation descriptor
     (`struct cpu_preserved_session_ser`).
   - `hwid`: Hardware CPU identifier (`APIC ID` on x86, `MPIDR_EL1` affinity bits
     on ARM64) used by the incoming kernel to match the physical core even if
     logical CPU numbering changes across `kexec`.
   - `next`: Intrusive KHO pointer linking all preserved CPUs across sessions
     into the global FLB list (`cpu_preserved_global_ser.cpus`).

2. **Per-Session State (`struct cpu_preserved_session_ser`)**:
   Shared by all preserved CPUs belonging to the same LUO session:

   ```c
   struct cpu_preserved_session_ser {
           DECLARE_KHOSER_PTR(as, struct cpu_preserved_as_ser *);
           u64 workload_pa;
   };
   ```

   - `as`: KHO pointer to the isolated address space metadata
     (`struct cpu_preserved_as_ser`) for this session.
   - `workload_pa`: Physical address of the session's workload metadata (for
     `oncore`, the head PA of its `kho_block_set` tracking preserved session and
     job pages so the incoming kernel can free them on session release).

3. **Isolated Address Space Metadata (`struct cpu_preserved_as_ser`)**:
   Referenced via `cpu_preserved_session_ser.as`, this structure records the
   session's isolated page-table root and page-table block chain so the outgoing
   kernel can unpreserve them on cancellation (`cpu_preserved_as_unpreserve()`)
   and the incoming kernel can unmap and free them during `.finish()`
   (`cpu_preserved_as_restore_free()`):

   ```c
   struct cpu_preserved_as_ser {
           u64 pgd_pa;
           u64 nr_pgtable_pages;
           DECLARE_KHOSER_PTR(pg_tables, struct kho_block_header_ser *);
           u64 direct_map_offset;
   };
   ```

   - `pgd_pa`: Physical address of the root page-table page (`CR3` / `TTBR`).
   - `nr_pgtable_pages` and `pg_tables`: Total page-table page count and KHO
     pointer to the `kho_block_set` chain recording all page-table pages
     allocated for the isolated address space without a fixed upper bound.
   - `direct_map_offset`: The outgoing kernel's `PAGE_OFFSET`, allowing the
     incoming kernel (which may boot with a different KASLR direct-map offset)
     to compute the outgoing direct-map virtual addresses when tearing down
     mappings.

4. **Global FLB State (`struct cpu_preserved_global_ser`)**:
   Preserved once across all sessions via a LUO FLB object
   (`"cpu-preserve-global-v1"`) so the incoming kernel can discover preserved
   CPUs during early boot before any session fd is retrieved:

   ```c
   struct cpu_preserved_global_ser {
           u64 text_runtime_pa;
           u64 text_runtime_size;
           u64 data_runtime_pa;
           u64 data_runtime_size;
           DECLARE_KHOSER_PTR(cpus, struct cpu_preserved_ser *);
           u64 arch_mode;
   };
   ```

   - `text_runtime_pa` / `text_runtime_size` and `data_runtime_pa` /
     `data_runtime_size`: Physical address and byte size of the relocated
     `.cpu_preserved.text` and `.cpu_preserved.data` buffers so the incoming
     kernel can free them once all preserved CPUs finish.
   - `cpus`: Head of the singly linked list of preserved CPUs
     (`cpu_preserved_ser.next`), walked by `cpu_preserve_early_init()` during
     early boot.
   - `arch_mode`: Architectural execution mode snapshot
     (`arch_cpu_preserved_mode()`: x2APIC and 5-level paging `LA57` on x86;
     `PAGE_SHIFT`, `VA_BITS`, `PGTABLE_LEVELS`, `LPA2`, `VHE`, and endianness on
     ARM64), verified against the incoming kernel during early boot.

---

## 5. Layer 3: On-Core Scheduler (`oncore`)

The `oncore` subsystem (`kernel/liveupdate/oncore.c`,
`kernel/liveupdate/oncore_runtime.c`, `include/linux/oncore.h`,
`CONFIG_LIVEUPDATE_ONCORE`) sits between raw physical CPU preservation and
higher-level workloads such as KVM.

### 5.1 Sessions and Jobs

- **`struct oncore_session` and `struct oncore_runqueue`**: Bound 1:1 to a
  `struct liveupdate_session` (`kernel/liveupdate/oncore_internal.h`).
  `struct oncore_session` tracks the `cpumask` of physical CPUs preserved in
  that session, embeds a shared `struct oncore_runqueue`, and records all
  KHO-preserved session and job page allocations in a `struct kho_block_set`:

  ```c
  struct oncore_runqueue {
          arch_spinlock_t lock;
          struct list_head runnable;
          u32 nr_runnable;
          u32 nr_active_jobs;
  };

  struct oncore_session {
          struct list_head node;
          char session_name[LIVEUPDATE_SESSION_NAME_LENGTH];
          cpumask_t cpus;
          cpumask_t attached_cpus;
          struct oncore_runqueue rq;
          struct list_head jobs;
          struct mutex lock;
          struct kho_block_set block_set;
          u64 sess_pa;
  };
  ```

  - `cpus` and `attached_cpus`: Bitmasks of physical CPUs preserved in the
    session and those currently attached to `oncore_sched_cpu_worker()`.
  - `rq`: Shared FIFO runqueue protected by a raw `arch_spinlock_t` so
    preserved CPUs can synchronize across `kexec` without host lockdep or
    preemption dependencies.
  - `block_set`: Tracks the physical addresses of the KHO-preserved
    `struct oncore_session` and all `struct oncore_job` pages. Its head
    physical address (`block_set.head_pa`) is stored in
    `cpu_preserved_session_ser.workload_pa` via
    `cpu_preserved_session_set_workload()`. On session completion in the
    incoming kernel (`oncore_session_release()`), `oncore` restores the
    `kho_block_set` and frees all preserved session and job pages without
    interpreting the outgoing kernel's `struct oncore_session` layout.
- **`struct oncore_job`**: Represents an independent unit of work (such as a
  single preserved KVM vCPU) submitted to an `oncore_session` via
  `oncore_session_submit_job()`:

  ```c
  struct oncore_job {
          struct list_head node;
          struct list_head sess_node;
          struct oncore_session *sess;
          oncore_job_fn run_fn;
          void *data;
          enum oncore_job_state state;
          int assigned_cpu;
          u64 job_pa;
  };
  ```

  Each job registers a callback (`oncore_job_fn`) that must reside in
  `.cpu_preserved.text`:
  - `enum oncore_exit_reason (*run_fn)(void *data, u64 deadline_ticks)`

### 5.2 Execution and Time-Sliced Scheduling

Each preserved physical CPU in an `oncore_session` executes
`oncore_sched_cpu_worker()` -> `oncore_cpu_schedule_loop()` from inside
`cpu_preserved_park_loop()`:

1. **Tickless 1:1 Execution vs. $M > N$ Multiplexing**:
   - **Tickless 1:1 Fast Path ($M \le N$)**: When no other jobs are waiting on
     the shared runqueue (`READ_ONCE(rq->nr_runnable) == 0`), `oncore` sets
     `sctx->oncore_tickless = 1` (ordered by `smp_mb()` before re-checking
     `rq->nr_runnable == 0`), passes `deadline = U64_MAX` to
     `curr->run_fn(curr->data, deadline)`, and keeps `curr` assigned across
     consecutive iterations without acquiring runqueue locks. The workload skips
     arming a preemption timer and runs continuously in guest mode, polling
     `oncore_need_resched()` and `cpu_preserved_should_exit()`, until woken by a
     physical IPI (either when a new job is enqueued via
     `oncore_sched_enqueue()` or when the host kernel attaches).
   - **Time-Sliced Multiplexing ($M > N$)**: When other jobs are waiting on the
     runqueue (`READ_ONCE(rq->nr_runnable) > 0`), `oncore` computes a quantum
     deadline (`oncore.quantum_ms`, defaulting to 10ms) in hardware counter
     ticks (`arch_oncore_read_counter()`: TSC on x86, generic counter on ARM64).
     The workload arms a hardware timer (VMX preemption timer, host LAPIC timer,
     or ARM64 EL2 physical timer) to return at `deadline`, serializes its
     context, and rotates through the FIFO runqueue (`rq->runnable`).
   - **Dequeue Priority (`oncore_sched_pick_next()`)**: Prioritizes (a) any job
     in `ONCORE_JOB_CANCELING` state so canceled jobs drain and transition to
     `ONCORE_JOB_DEAD` promptly, and (b) strict FIFO selection from the head of
     `rq->runnable`.
2. **Job Exit Reasons**:
   - `ONCORE_EXIT_QUANTUM_EXPIRED`: Quantum deadline reached; job remains
     runnable.
   - `ONCORE_EXIT_YIELD_IDLE`: Guest executed an idle instruction (`HLT`,
     `PAUSE`, `WFI`, `WFE`); job yields early and remains runnable.
   - `ONCORE_EXIT_STALL`: Unhandled exit during `kexec`; the CPU executes a
     low-power wait (`arch_cpu_preserved_park_wait()`) before re-queuing the job
     to avoid spinning at full speed until the incoming kernel attaches.
   - `ONCORE_EXIT_ATTACH_SIGNALED` / `ONCORE_EXIT_ERROR`: Job was reclaimed by
     the host kernel or hit an entry error; transitions to `ONCORE_JOB_DEAD` and
     is removed from the runqueue.

---

## 6. Layer 4: KVM Caretaker

The KVM Caretaker is an isolated in-kernel agent that runs Orphaned VM vCPUs
and handles their VM-exits while the host kernel is torn down across `kexec`.
Running as an `oncore` workload in `.cpu_preserved.text`, it takes temporary
ownership of the vCPU's hardware virtualization context (`VMCS`, `VMCB`, or EL2
system registers), enters guest mode, services any intercepts or quantum timer
exits that can be resolved without host kernel services, and returns control
to `oncore` upon quantum expiration, guest idle, an unhandled VM-exit
(`ONCORE_EXIT_STALL`), or host kernel reclamation.

When `CONFIG_KVM_CARETAKER` (`KVM_CAP_CARETAKER`) is enabled and a `vcpufd` is
preserved into a LUO session:

1. **Job Creation**: `kvm_caretaker_vcpu_pre_preserve()` allocates a
   `struct oncore_job` on the session for `kvm_arch_vcpu_caretaker_run()`.
2. **State and Runtime Initialization**: `kvm_arch_vcpu_luo_preserve()`
   serializes architectural state into `struct kvm_vcpu_arch_ser`, initializes
   the Caretaker runtime page in `KVM_CARETAKER_PAUSED` (`ser->cb`), and maps
   both into the session's isolated address space
   (`struct cpu_preserved_as_ser`).
3. **Activation**: `kvm_caretaker_vcpu_post_preserve()` activates the job on the
   `oncore` runqueue (`oncore_session_activate_job()`), waking an idle preserved
   CPU to resume the vCPU immediately (or queuing it until a preserved CPU in
   the session is available).

### 6.1 KVM Caretaker Control Block ABI

For each vCPU running under the Caretaker, `struct kvm_vcpu_ser` (`ser->cb`)
points to a KHO-preserved Caretaker control block defined in
`include/linux/kho/abi/kvm.h` (`kvm_x86.h`, `kvm_arm64.h`):

1. **Common Control Block (`struct kvm_caretaker_cb_ser`)**:
   Coordinates the cross-kernel execution state (`state`,
   `enum kvm_caretaker_state`), the assigned physical CPU ID (`pcpu_id`),
   `vcpu_id`, and the KHO pointer (`telemetry`) to
   `struct kvm_caretaker_telemetry_ser`:

   ```c
   struct kvm_caretaker_cb_ser {
           u32 state;
           u32 pcpu_id;
           u32 vcpu_id;
           DECLARE_KHOSER_PTR(telemetry, struct kvm_caretaker_telemetry_ser *);
   };
   ```

2. **Architecture Control Block (`struct kvm_caretaker_arch_ser`)**:
   Embeds `struct kvm_caretaker_cb_ser cb` at offset 0, followed by the minimal
   architecture fields needed by the incoming kernel during hardware state
   adoption and page cleanup:

   - **x86 (`include/linux/kho/abi/kvm_x86.h`)**:

     ```c
     struct kvm_caretaker_arch_ser {
             struct kvm_caretaker_cb_ser cb;
             u32 nr_preserved_pages;
             u64 pi_desc_pa;
             u64 preserved_pages_pa[KVM_X86_CARETAKER_MAX_PAGES];
     };
     ```

   - **ARM64 (`include/linux/kho/abi/kvm_arm64.h`)**:

     ```c
     struct kvm_caretaker_arch_ser {
             struct kvm_caretaker_cb_ser cb;
             u32 vgic_initialized;
             u32 cflags;
             u64 cntvoff_el2;
             u64 hcr_el2;
             u64 mdcr_el2;
             u32 used_lrs;
             u32 vgic_hcr;
             u32 vgic_vmcr;
             u32 vmid;
             u32 vgic_ap0r[4];
             u32 vgic_ap1r[4];
             u64 vgic_lr[16];
     };
     ```

### 6.2 Orphaned VM State Machine

Handoff between the preserved physical CPU (running the outgoing kernel's
`.cpu_preserved.text` code) and the host kernel reclaiming the vCPU (either the
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
       |                 ^                        |        |        |
       |                 | detach_serialize()     |        |        |
       |                 +------------------------+        |        |
       |                quantum end / yield / stall        |        |
       |                                                   |        |
       | Host kernel attach:       Host kernel attach:     |        |
       | cmpxchg(PAUSED ->         cmpxchg(RUNNING ->      |        |
       |         STOPPED)                  STOPPING)       |   Entry error
       | (Immediate 0ns reclaim)   + kick(cb->pcpu_id)     |   or host fault
       |                                                   v        |
       |                              +-----------------------+     |
       |                              | KVM_CARETAKER_STOPPING|     |
       |                              |          (2)          |     |
       |                              +-----------------------+     |
       |                                                   |        |
       |                             Caretaker VM-exits,   |        |
       |                             detach_serialize(),   |        |
       |                             sets STOPPED          |        |
       v                                                   v        v
    +---------------------------------------------------------+  +--------+
    |                 KVM_CARETAKER_STOPPED (3)               |  | FAILED |
    |      (ser->arch_state is complete; host KVM owns vCPU)  |  |  (4)   |
    +---------------------------------------------------------+  +--------+
```

Whenever the Caretaker pauses a vCPU (upon quantum expiration, guest idle, or an
unhandled exit), it flushes live hardware registers into `ser->arch_state`
(`detach_serialize()`) before setting `cb->state = KVM_CARETAKER_PAUSED`. As a
result, `ser->arch_state` in RAM is always up to date whenever `cb->state` is
`PAUSED`:

- **Reclaiming a `PAUSED` vCPU (`PAUSED -> STOPPED`)**:
  `kvm_caretaker_wait_for_attach()` claims the vCPU immediately with a single
  `cmpxchg(&cb->state, KVM_CARETAKER_PAUSED, KVM_CARETAKER_STOPPED)` without
  kicking or waiting for any physical CPU.
- **Reclaiming a `RUNNING` vCPU (`RUNNING -> STOPPING -> STOPPED`)**:
  `kvm_caretaker_wait_for_attach()` sets
  `cmpxchg(&cb->state, KVM_CARETAKER_RUNNING, KVM_CARETAKER_STOPPING)` and sends
  a physical IPI (`arch_cpu_preserved_kick(pcpu)`). The IPI forces a VM-exit on
  the preserved CPU, which flushes its hardware state via `detach_serialize()`,
  sets `cb->state = KVM_CARETAKER_STOPPED`, and exits to `oncore`.
- **Handling Unrecoverable Faults (`RUNNING -> FAILED`)**:
  If a vCPU hits an unrecoverable hardware entry error (`ONCORE_EXIT_ERROR`) or
  triggers an unexpected host-mode exception while running in the Caretaker, the
  preserved fault handler transitions `cb->state` to `KVM_CARETAKER_FAILED (4)`.
  Upon reclamation, `kvm_caretaker_wait_for_attach()` detects `FAILED` (or
  quarantines a timed-out vCPU in `kvm_caretaker_quarantined_vcpus`), returns
  `-EIO`, and marks the VM bugged via `kvm_vm_bugged()`.

Both live-update cancellation and incoming-kernel reclamation use this handshake
to return the vCPU to host KVM:

- **Outgoing Cancellation (`kvm_caretaker_vcpu_unpreserve()`)**:
  If the LUO session is closed before `kexec`, the outgoing kernel transitions
  `cb->state` to `KVM_CARETAKER_STOPPED`, restores the updated `ser->arch_state`
  and hardware control state back into the existing `struct kvm_vcpu`, cancels
  the `oncore` job, and frees the Caretaker pages so the VMM can immediately
  resume `KVM_RUN`.
- **Incoming Reclamation (`kvm_vcpu_luo_retrieve()`, `kvm_vcpu_luo_finish()`)**:
  In the new kernel, retrieving each `vcpufd` transitions `cb->state` to
  `KVM_CARETAKER_STOPPED` and restores `ser->arch_state` and hardware control
  state into the new `struct kvm_vcpu`, and `.finish()` stops any unretrieved
  Caretaker vCPUs and frees the preserved Caretaker and telemetry pages.

### 6.3 KVM Caretaker Arch Backends

The outermost Caretaker execution loop is `kvm_caretaker_vcpu_run()` (called
from the `oncore` job callback `kvm_arch_vcpu_caretaker_run()`), which invokes
`ops->pre_run()`, arms the quantum timer (`ops->arm_timer()`), and repeatedly
invokes `ops->vcpu_run(cvcpu, &reason)` until the quantum expires, the guest
yields or stalls, or the host kernel attaches, before calling
`ops->disarm_timer()` and `ops->post_run()`. Each architecture implements
`struct kvm_caretaker_ops` in `.cpu_preserved.text` and shares KVM's low-level
world-switch assembly (`__vmx_vcpu_run`, `__svm_vcpu_run`, `__guest_enter`) and
inline register/MSR/CPUID/sysreg helpers (`arch/x86/kvm/switch.h`,
`arch/x86/kvm/vmx/switch.h`, `arch/x86/kvm/svm/switch.h`, and
`arch/arm64/kvm/hyp/include/hyp/switch.h`) by embedding a `struct kvm_vcpu`
inside the Caretaker's preserved runtime page:

- **x86 Common**:
  Sets up standalone per-CPU GDT, TSS, and IST stacks (`struct x86_preserved_cpu`)
  and `x86_preserved_idt` so host-mode exceptions and interrupts land in
  self-contained handlers, preserves the virtual-APIC (`apic->regs`) page, and
  saves/restores guest FPU (`XRSTOR64` / `XSAVE64` on `state->xsave.region`),
  `XCR0`, `PKRU`, and `DEBUGCTL` in place.
- **Intel VMX**:
  Embeds `struct vcpu_vmx` in `struct caretaker_vmx_page` with a dedicated
  `vmcs01` copied from the host VMCS, preserves Posted Interrupt / IPIv tables
  (`pid_table`, `pi_desc_pa`), reprograms `HOST_CR3`, `HOST_RIP`, `HOST_GS_BASE`,
  `HOST_GDTR_BASE`, `HOST_IDTR_BASE`, and `HOST_TR_BASE` to the Caretaker's
  isolated context, and uses the VMX preemption timer for quantum deadlines. On
  incoming adoption, it coalesces any pending Posted Interrupt bits from the
  preserved `pi_desc_pa` into the new vCPU's LAPIC `IRR` and restores
  `ser->arch_state`.
- **AMD SVM**:
  Embeds `struct vcpu_svm` in `struct caretaker_svm_page` with a preserved
  `vmcb01`, MSR/IOPM permission bitmaps (`msrpm`, `iopm`), and a per-CPU `hsave`
  page (`MSR_VM_HSAVE_PA`), uses the host LAPIC timer with `INTERCEPT_INTR` for
  quantum deadlines, and flushes dirty VMCB save-area fields (and any pending
  `V_IRQ` into LAPIC `IRR`) back into `ser->arch_state`.
- **ARM64 VHE**:
  Embeds `struct kvm_vcpu` in `struct caretaker_arm64_page`, installs standalone
  EL1 (`arm64_preserved_vectors`) and EL2 (`caretaker_hyp_vector`) vector tables
  (`VBAR_EL1`, `VBAR_EL2`), pins the VM's Stage-2 `vmid` across `kexec`,
  context-switches EL1 system registers, FP/SIMD/SVE, Stage-2 MMU (`VTCR_EL2`,
  `VTTBR_EL2`), VGICv3 CPU interface registers, and the virtual timer via shared
  `hyp/switch.h` helpers, and uses the EL2 physical timer (`CNTHP_CVAL_EL2`) for
  quantum deadlines.

### 6.4 Implemented VM-Exits

While the initial upstream landing should not implement any guest VM-exit
emulation (stalling on any synchronous VM-exit until the incoming kernel
reclaims the vCPU, as discussed in Section 7.1), the current implementation
includes a small set of fastpath VM-exit handlers shared with KVM (`switch.h`)
for proof-of-concept testing.

#### VM-Exit Handling Call Stack

```
cpu_preserved_park_loop()                        [Layer 2: CPU Park Loop]
  -> oncore_sched_cpu_worker()
    -> oncore_cpu_schedule_loop()                [Layer 3: Quantum Loop]
      -> kvm_arch_vcpu_caretaker_run()           [Layer 4: Job Callback]
        -> kvm_caretaker_vcpu_run()              [Layer 4: VM-Exit Loop]
          -> ops->pre_run() / ops->arm_timer()
          -> while (!kvm_caretaker_should_exit()):
               ops->vcpu_run()
                 -> __vmx_vcpu_run() / __svm_vcpu_run() / __guest_enter()
                 -> vmx/svm/caretaker_exit_handlers[]
          -> ops->disarm_timer() / ops->post_run()
```

#### Per-Platform VM-Exit Table

| Exit Type       | Intel VMX               | AMD SVM               | ARM64 VHE            | Caretaker Action            |
| :-------------- | :---------------------- | :-------------------- | :------------------- | :-------------------------- |
| `PREEMPT_TIMER` | `PREEMPTION_TIMER`, IRQ | `INTR`, `NMI`, `SMI`  | `ARM_EXCEPTION_IRQ`  | Exit quantum (`PAUSED`)     |
| `IDLE`          | `HLT`, `PAUSE`          | `HLT`, `PAUSE`        | `WFx`, `DABT`/`IABT` | `cpu_relax()`, yield idle   |
| `CPUID`         | `CPUID`                 | `CPUID`               | N/A                  | Guest `cpuid` table lookup  |
| `RDTSC`         | `RDTSC`                 | `RDTSC`               | N/A                  | Scale host `TSC`, continue  |
| `MSR`           | `MSR_READ`, `MSR_WRITE` | `MSR`                 | N/A                  | Emulate arch/x2APIC MSRs    |
| `INSN_STEP`     | N/A                     | `INVD`, `WBINVD`      | N/A                  | Emulate as NOP, advance `RIP` |
| `SYSREG` / `IPI`| Silicon (`IPIv`)        | Stalls                | `SYS64` (`ICC_SGI*`) | Inject SGI / emulate sysreg |
| `UNHANDLED`     | `EPT_VIOLATION`, etc.   | `NPF`, `VMMCALL`      | Other `ESR_EL2` traps| Leave `PC`, return `STALL`  |

### 6.5 Gap Telemetry and Observability

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

While the [RFCv1 patch series][rfcv1-series] and subsequent refinements
demonstrate end-to-end continuous vCPU execution across `kexec` on Intel VMX,
AMD SVM, and ARM64 VHE, several architectural questions—both from the [initial
proposal discussion][initial-proposal] and from prototyping—merit further
discussion with upstream maintainers.

### 7.1 VM-Exit Handling Strategy and Sharing Code with KVM

A central design question is how the Caretaker should enter the guest and handle
VM-exits without duplicating KVM into a second in-kernel hypervisor.

In the [initial LKML discussion][initial-proposal], Paolo Bonzini noted that the
Caretaker is the non-preemptible inner part of `vcpu_enter_guest()`, and that
`vmx_exit_handlers_fastpath()` / `svm_exit_handlers_fastpath()` already provide
a blueprint for exits that can be handled with interrupts disabled.

#### Zero-VM-Exit Baseline

All four layers and the Intel VMX, AMD SVM, and ARM64 VHE backends can be
upstreamed without implementing any guest VM-exit emulation beyond the hardware
preemption timer / host kick exit. While in the Caretaker, vCPUs continue
executing in guest mode (including timers and interrupts handled in silicon),
stall (`ONCORE_EXIT_STALL`) on the first synchronous VM-exit without advancing
`RIP`/`PC`, and resume execution of the faulting instruction in standard KVM
once adopted by the incoming kernel.

#### Why Calling Unmodified KVM Entry Paths Directly Is Problematic

Directly calling `vcpu_enter_guest()`, `vmx/svm_vcpu_run()`, or unadapted
`vmx/svm_exit_handlers_fastpath()` from the Caretaker is problematic because the
Caretaker executes in an isolated address space while the rest of the host
kernel is torn down:

1. **Isolated Code Section (`.cpu_preserved.text`)**:
   All Caretaker code must reside in `.cpu_preserved.text` and compile without
   stack protectors, jump tables, ftrace, sanitizers, or return thunks. If a
   shared KVM function makes even a single transitive call to an unannotated
   helper (`WARN_ON_ONCE()`, tracepoints, `static_branch_unlikely()`, or
   out-of-line library routines), the CPU jumps into unmapped kernel text during
   `kexec` and faults.
2. **Unmapped Host Heap Structures**:
   KVM's full entry/exit paths chase pointers into `vcpu->kvm`, `mmu`, `apic`,
   and `memslots`. Those heap structures live in the host linear map, which is
   unmapped in the session's isolated address space unless explicitly preserved
   and mapped.
3. **No Linux Task or Per-CPU Runtime**:
   Standard KVM code relies on `current`, host per-CPU variables (`%gs` on x86,
   `TPIDR_EL1` on ARM64), preemption tracking, and RCU, none of which exist on
   an offlined (`cpu_is_preserved(cpu)`) CPU running on a standalone preserved
   stack across `kexec`.
4. **Cross-Kernel ABI Boundary**:
   Internal structs (`struct kvm_vcpu`, `struct vcpu_vmx`, `struct vcpu_svm`)
   can change layout between kernels, so the handoff to the incoming kernel must
   serialize into a fixed KHO ABI (`struct kvm_vcpu_arch_ser`).

#### KVM Caretaker VM-Exits Code Sharing (`switch.h` and `*.preserved.o`)

To avoid re-implementing hypervisor logic or duplicating code, the kernel shares
guest entry/exit and fastpath VM-exit handling code between normal KVM and the
Caretaker through a unified build and data-structure model across all
`.cpu_preserved.text` code (`cpu_preserve`, `oncore`, and the Caretaker):

1. **Shared Switch Headers and Embedded `struct kvm_vcpu`**:
   KVM's low-level world-switch assembly (`vmx/vmenter.S`, `svm/vmenter.S`,
   ARM64 `hyp/nvhe/switch.c` and `caretaker/entry.S`) and inline register,
   VMCS/VMCB, MSR, CPUID, and sysreg helpers are factored into shared headers
   (`arch/x86/kvm/switch.h`, `arch/x86/kvm/vmx/switch.h`,
   `arch/x86/kvm/svm/switch.h`, and `arch/arm64/kvm/hyp/include/hyp/switch.h`).
   By embedding a self-contained `struct vcpu_vmx`, `struct vcpu_svm`, or
   `struct kvm_vcpu` inside the Caretaker's preserved runtime page
   (`struct caretaker_vmx_page`, `struct caretaker_svm_page`,
   `struct caretaker_arm64_page`), both normal `KVM_RUN` and the Caretaker
   runtime execute the same world-switch assembly and inline exit helpers.
2. **Partial Linking and Symbol Prefixing (`*.preserved.o` Build Pattern)**:
   Following the `arch/arm64/kvm/hyp/nvhe/` build model
   (`scripts/Makefile.cpu_preserve` and `scripts/cpu_preserved.lds.S`),
   preserved `.c` and `.S` files (including shared KVM world-switch files and
   `lib/cpu_preserve_string.c` for `memcpy`, `memset`, `memmove`, and `memcmp`)
   are compiled with `PRESERVED_CFLAGS` (`-D__CPU_PRESERVED_RUNTIME__`,
   `-include include/linux/cpu_preserve_compat.h`), partially linked (`ld -r`)
   with `scripts/cpu_preserved.lds` to place `.text`, `.rodata`, `__ex_table`,
   `.data`, and `.bss` into `.cpu_preserved.*` sections, and symbol-prefixed via
   `objcopy --prefix-symbols=__cpu_preserved_`. This ensures that even
   compiler-emitted implicit calls to `memcpy` or `memset` resolve to
   `__cpu_preserved_memcpy` inside `.cpu_preserved.text`.
3. **Link-Time and `objtool` Isolation Verification**:
   Because `objcopy --prefix-symbols=__cpu_preserved_` also renames *undefined*
   symbol references inside each `*.preserved.o` object, any accidental call
   from preserved code to an unpreserved kernel symbol (such as `printk` or an
   unshared KVM helper) becomes an undefined reference to
   `__cpu_preserved_printk` and fails the `vmlinux` link immediately,
   complemented by `objtool` and `modpost` section checks.

### 7.2 Bare-Metal Platform Considerations

1. **CPU Enumeration, Hardware IDs, and Architectural Mode Verification**:
   To harden against logical CPU renumbering or incompatible kernel
   configurations across `kexec`, `cpu_preserve` records each preserved CPU's
   hardware identifier (`ser->hwid` via `arch_cpu_preserved_hwid()`: `APIC ID`
   on x86, `MPIDR_EL1` affinity bits on ARM64) and the outgoing kernel's
   architectural execution mode (`global_ser->arch_mode` via
   `arch_cpu_preserved_mode()`). During early boot of the incoming kernel
   (`cpu_preserve_early_init()`), `cpu_preserve` verifies `arch_mode`
   compatibility and matches each preserved CPU's `ser->hwid` against the
   incoming kernel's present CPUs via `arch_match_cpu_phys_id()`, updating
   `ser->cpu` if logical CPU numbering changed.
2. **Platform Events (SMIs, MCEs, and SErrors)**:
   Preserved CPUs avoid deep C-states (`PAUSE` / `WFE`) and install standalone
   exception vectors (`x86_preserved_idt` with dedicated per-CPU IST stacks for
   `#DF`, `NMI`, and `#MC` on x86; `arm64_preserved_vectors` at `VBAR_EL1` and
   `caretaker_hyp_vector` at `VBAR_EL2` on ARM64):
   - **SMIs (x86)**: Handled transparently by firmware in SMRAM; on AMD SVM,
     `SVM_EXIT_SMI` is handled in `svm_exit_handlers` by exiting the current
     quantum cleanly rather than stalling the vCPU.
   - **MCEs and Host Exceptions**: On x86, `x86_preserved_handle_exception()`
     handles recoverable `#MC` events in place (clearing `MCi_STATUS` and
     `MCG_STATUS.MCIP` when `RIPV` and `UC == 0` allow continuation); for fatal
     or synchronous host faults (`#DF`, `#GP`, `#PF`), it records fault
     diagnostics in `sctx->fault`, invokes the workload's `abort_fn`
     (`kvm_x86_caretaker_fault_abort()` to mark the vCPU
     `KVM_CARETAKER_FAILED`), sets `ser->state = CPU_PRESERVED_FAULTED`, and
     parks the CPU in `cpu_preserved_park_wait_exit()`. On ARM64,
     `arm64_preserved_vectors` (`VBAR_EL1`) is installed unconditionally in
     `arch_cpu_preserved_park_init()`, and `caretaker_hyp_vector` (`VBAR_EL2`)
     catches unexpected EL2 exceptions and SErrors via
     `arm64_caretaker_handle_invalid()`, recording fault diagnostics and parking
     the CPU safely until host reclamation.

### 7.3 Nested Virtualization Support

The current implementation rejects nested virtualization (`is_guest_mode(vcpu)`
on x86, `vcpu_has_nv(vcpu)` on ARM64) with `-EOPNOTSUPP`. Supporting it requires
two additions:

1. **KHO State and Shadow TDP Preservation**:
   Serialize nested hypervisor state in `struct kvm_vcpu_arch_ser`
   (`struct kvm_nested_state` via `kvm_nested_ops.get_state()`/`set_state()` on
   x86, and virtual EL2 system registers via `state->sysregs[]` on ARM64) and
   preserve active nested shadow TDP / Stage-2 page tables (`vmcs02.EPTP`,
   `vmcb02.ncr3`, nested `VTTBR_EL2`) alongside the VM's primary TDP mappings.
2. **Run Current Mode, Stall on Nested World Switches**:
   Rather than pulling KVM's nested state machine into `.cpu_preserved.text`,
   the Caretaker keeps the vCPU executing in whichever mode (L1 `vmcs01`/`vmcb01`
   or L2 `vmcs02`/`vmcb02`) was active at `.preserve()` time. Any L1 -> L2 entry
   (`VMLAUNCH`, `VMRESUME`, `VMRUN`) or L2 exit that must be reflected to L1
   stalls (`ONCORE_EXIT_STALL`) until the incoming kernel adopts the vCPU and
   emulates the nested transition in standard KVM.

### 7.4 Paravirtual Devices

While pass-through PCI/VFIO devices continue DMA in hardware during `kexec`,
paravirtual `virtio` devices (`virtio-rng`, `virtio-net`, `virtio-blk`,
`virtio-console`) normally trap into the userspace VMM on `QueueNotify` doorbell
writes (`ioeventfd`), stalling the vCPU (`ONCORE_EXIT_STALL`) until the incoming
VMM re-attaches. Four approaches can avoid or service these exits during the
gap:

1. **Architectural CPU Instructions (e.g., `virtio-rng`)**:
   Exposing native hardware random instructions (`RDRAND`/`RDSEED` on x86,
   `FEAT_RNG` on ARM64) lets the guest obtain entropy directly in guest mode
   without relying on `virtio-rng` or a userspace VMM.
2. **Posted `ioeventfd` Doorbell Absorption**:
   Because `QueueNotify` writes are posted notifications that descriptors are
   ready in guest RAM, the Caretaker can match registered `ioeventfd` GPA/PIO
   ranges, mark the `ioeventfd` pending in preserved memory, and advance
   `RIP`/`PC` so the vCPU keeps running other guest tasks; KVM then signals all
   pending `ioeventfd`s upon VMM re-attachment.
3. **Minimal In-Caretaker Servicing for Stateless Queues**:
   For simple stateless devices (`virtio-rng` or `virtio-console` TX), a small
   Caretaker helper mapped to the virtqueue ring pages in preserved guest RAM
   can consume `avail` descriptors, fill or drain the buffer, and advance the
   `used` index in place.
4. **Forwarding Virtqueues or VM-Exits to Another VM or Accelerator Card**:
   For stateful I/O (`virtio-net`, `virtio-blk`) or complex device exits, the
   Caretaker (or guest virtqueues directly) can forward the request over shared
   preserved memory or PCIe to an external endpoint that remains alive across
   `kexec`, such as a dedicated service/driver VM running on its own preserved
   CPUs or an off-host SmartNIC/DPU accelerator card.

### 7.5 Kernel Caretaker

Because CPU preservation (`cpu_preserve`) and on-core scheduling (`oncore`) are
independent of KVM, the same infrastructure can support a **Kernel Caretaker**
that keeps userspace threads running across a host `kexec` reboot.

1. **Target Workloads**:
   - **Userspace Data Planes (DPDK, SPDK)**: Threads that poll preserved VFIO
     NIC or storage queues in `memfd`/HugeTLB memory and run in user mode
     without making kernel system calls.
   - **Userspace Sandboxes (gVisor)**: Application kernels where the Sentry
     services guest system calls in user space (`systrap` or shared memory) and
     can keep running across `kexec` as long as it does not make a host kernel
     syscall during the reboot window.
2. **How It Works**:
   - **Preserved User Page Tables**: The process's user-space mappings
     (`memfd`/HugeTLB code, stack, heap, and VFIO BARs) are mapped alongside the
     `.cpu_preserved.text` trampoline inside the session's isolated address
     space (`struct cpu_preserved_as_ser`).
   - **User-Mode Entry and Vectors**: The `oncore` job switches to the preserved
     user page tables and enters user mode, with syscall and exception vectors
     pointing to `.cpu_preserved.text` stubs.
   - **Stall on Syscall or Page Fault**: As long as the thread stays in user
     mode over pre-mapped memory and vDSO clocks (`RDTSC` / `CNTVCT_EL0`), it
     runs across `kexec` without interruption. If it issues a host `syscall` or
     faults on an unmapped page, the `.cpu_preserved.text` stub saves its user
     registers (`pt_regs`) and FPU state and stalls (`ONCORE_EXIT_STALL`) with
     the instruction pointer left on the trapping instruction; once the incoming
     kernel adopts the task via LUO, it handles the pending syscall or fault
     normally.

[initial-proposal]: https://lore.kernel.org/all/afEwWZksU0Fw61oT@plex
[rfcv1-series]: https://lore.kernel.org/all/20260920193650.3373435-1-pasha.tatashin@soleen.com
[luo-abi-rfc]: https://lore.kernel.org/kexec/20260903023452.721732-1-loganodell@google.com
[lpc-compat]: https://lpc.events/event/20/contributions/2613
[section-7]: #7-open-design-challenges-and-topics-for-further-discussion
[guest-memfd-series]: https://lore.kernel.org/all/20260728121138.1103610-1-tarunsahu@google.com/
[pci-lu-series]: https://lore.kernel.org/all/20260918200640.887030-1-dmatlack@google.com/
[vfio-lu-series]: https://lore.kernel.org/all/20260714151505.3466855-1-vipinsh@google.com/
[iommu-lu-series]: https://lore.kernel.org/all/20260921004834.2601285-1-skhawaja@google.com/#t
