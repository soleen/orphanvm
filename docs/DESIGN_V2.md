# Orphaned Virtual Machines: Architecture & Design Specification (V2)

**Authors:** Pasha Tatashin, `<add-names>`  
**Contributors:** `<add-names>`  
**Date:** September 2026  
**Status:** Architecture Design Document (V2 Outline)  
**Target Upstream:** Linux Kernel (`mm/`, `kernel/liveupdate/`, `arch/x86/`, `arch/arm64/`, `virt/kvm/`)

---

## 1. Executive Summary & Evolution from V1

### 1.1 The Operational Problem: Live Update Blackout Window

In hyperscale cloud infrastructure, keeping host operating system kernels patched and updated with zero workload interruption is a fundamental operational challenge. Historically, two primary strategies have been employed:

1. **Network Live Migration:**
   - The virtual machine is copied across the network to a secondary host machine running the updated kernel.
   - *Limitations:* Consumes massive network bandwidth, causes severe CPU and cache churn on both source and destination hosts, throttles guest dirtying rates, and relies on available spare compute capacity across the fleet.

2. **Intra-Host Live Update via Kexec Handover (KHO) & Live Update Orchestrator (LUO):**
   - The VM's memory pages, guest file descriptors (`vmfd`, `vcpufd`), and architectural hypervisor state are preserved in-place in physical RAM across a fast `kexec` reboot.
   - *The Remaining Bottleneck:* While memory is preserved in-place, the host operating system and userspace Virtual Machine Monitor (VMM) must still terminate, reboot via kexec, re-enumerate devices, and restart userspace services.
   - *The Blackout Window:* During this 5-to-30-second "Management Gap," all guest vCPUs are paused and serialized into RAM. To the guest workload, time stands still: database transactions stall, latency-sensitive network connections time out, keepalive heartbeats fail, and distributed consensus nodes drop out of quorums.

```
Traditional Live Update (vCPUs Frozen in RAM):
 Host OS (Old)  │══════[Stage KHO]══════│
 Host Kexec     │                       │───────[Reboot Gap: 5-30s]───────│
 Host OS (New)  │                                                         │══════[Adopt vCPUs]══════│
 Guest vCPUs    │───────────────────────│  * * * FROZEN IN RAM * * *      │─────────────────────────│
                                        ^ vCPUs suspended                 ^ vCPUs resumed
                                        <------- Application Outage ------>

OrphanVM Continuous Live Update (On-Core Silicon Preservation):
 Host OS (Old)  │══════[Stage KHO]══════│
 Host Kexec     │                       │───────[Reboot Gap: 5-30s]───────│
 Host OS (New)  │                                                         │══════[Adopt vCPUs]══════│
 Guest vCPUs    │───────────────────────┴─────────────────────────────────┴─────────────────────────│
                                        ^ Handed off to Caretaker         ^ Adopted by New VMM
                                        <---- Zero Application Downtime (30,000+ passes/sec) ------>
```

### 1.2 The OrphanVM Objective: Zero-Blackout Host Live Update

The OrphanVM architecture breaks the historical dependency between a virtual machine's resource ownership and its active execution:

- **Resource Ownership** is decoupled and retained safely by the host kernel's Live Update Orchestrator (LUO) and Kexec Handover (KHO) mechanisms.
- **Silicon Execution** is handed off directly to the physical hardware processor cores hosting the vCPUs.
- **The Caretaker** acts as an ultra-minimal, in-kernel execution engine running directly on the preserved physical cores in host mode (Ring 0 / EL2). It intercepts and resolves trivial VM exits (such as serial console MMIO, `HLT`/`WFI` idle sleep, APIC timer deadlines, and CPUID queries) autonomously while the host kernel reboots underneath it.
- **Zero Blackout:** The guest OS continues executing guest instructions continuously on bare-metal silicon throughout the entire live update transition, achieving a measured **0.000 seconds** of guest downtime.

### 1.3 Architectural Evolution: V1 Prototype vs. V2 In-Kernel Design

The initial prototype of the Caretaker (documented in `docs/DESIGN.md`) explored an external bare-metal payload model. Practical implementation, security audits, and upstream maintainer feedback on LKML motivated a complete architectural redesign for Version 2:

| Architectural Dimension | Version 1 Design (Early Prototype) | Version 2 Design (Current Production Architecture) |
| :--- | :--- | :--- |
| **Execution Placement** | External bare-metal ELF binary compiled outside the kernel tree and loaded into host memory by userspace. | Built directly into the Linux kernel and KVM module (`kernel/liveupdate/caretaker.c`, `virt/kvm/caretaker.c`). |
| **Installation API** | Custom `KVM_SET_CARETAKER` ioctl taking a file descriptor to the ELF binary. | Standard KVM LUO preservation flags (`KVM_VCPU_LUO_FLAG_CARETAKER`) integrated into existing `LIVEUPDATE_SESSION_PRESERVE_FD`. |
| **Security Boundary** | High risk: userspace could attempt to inject arbitrary code into VMX root / EL2 host state; required complex in-kernel ELF parsing and signature checks. | Zero privilege escalation risk: 100% of execution code is native upstream Linux kernel text, signed and verified as part of `vmlinux`. |
| **Address Space Model** | Single global identity map shared across all Caretaker cores. | **Option B (Per-Session Page Tables):** Dedicated PGD per Caretaker session; strict Address Space Isolation (ASI) between tenants. |
| **KHO Serialization** | Monolithic global First-Level Boot (FLB) structure holding all session and core metadata. | **File-Private KHO Data:** Global FLB trimmed to minimal hardware mask; session data serialized into private CPU file descriptors (`cpu_fh_v1`). |
| **Scheduling Engine** | Hardcoded spin-wait polling loop for all non-trivial exits; no local multi-tenancy. | Pluggable round-robin quantum scheduler (`caretaker_sched`) with quantum deadlines and dedicated 1:1 core lock-free fast-paths. |
| **Architecture Backends** | Conceptually x86 VMX-only; AMD and ARM64 deferred. | Full production support across **Intel VMX**, **AMD SVM**, and **ARM64** with unified ops abstraction (`struct kvm_caretaker_ops`). |
| **Memory Management** | Ad-hoc page table setup in architecture code. | Architecture-independent Transition Page Tables (`trans_pgd`) unified across x86 and ARM64. |

---

## 2. Multi-Layer Architecture Overview

The OrphanVM system is architected as a modular, six-layer stack. Each layer possesses strict functional boundaries and clear communication interfaces, enforcing separation of concerns across generic kernel subsystems, architecture memory management, and platform-specific hypervisor hardware.

```
+───────────────────────────────────────────────────────────────────────────────────+
│                              Userspace VMM (NanoVMM)                              │
│   - Session registration (LUO)         - Strategy parsing (e.g. cpu1-2, ram)      │
│   - Memory reservation (HugeTLB)       - Preserved CPU file descriptors (cpu_fh)  │
+───────────────────────────────────────────────────────────────────────────────────+
                                         │  (ioctl / LUO ABI)
+───────────────────────────────────────────────────────────────────────────────────+
│                        Layer 4: KVM Caretaker Abstraction                         │
│   - struct kvm_caretaker_ops (enter, decode_exit, handle_arch_exit, advance_rip)  │
│   - Exit dispatching (Console MMIO, HLT/WFI, MSRs, CPUID, RDTSC)                  │
│   - Attachment signaling & deterministic handshake protocol                       │
+───────────────────────────────────────────────────────────────────────────────────+
                                         │
+───────────────────────────────────────────────────────────────────────────────────+
│                         Layer 3: Caretaker Framework                              │
│   - Multi-session management (struct caretaker_session)                           │
│   - Per-session isolated page tables (sess->pgd_pa, sess->pgd_pages[128])         │
│   - Round-robin quantum scheduler (10ms quantum, dedicated-core fast-path)        │
│   - Atomic lifecycle states (RUNNABLE, RUNNING, CANCELING, DEAD)                  │
+───────────────────────────────────────────────────────────────────────────────────+
                                         │
+───────────────────────────────────────────────────────────────────────────────────+
│                     Layer 2: Physical CPU Preservation Core                       │
│   - Hotplug offline interception (cpuhp_ap_report_dead)                           │
│   - SMP boot shielding (smp_send_stop unicast, cpuhp_bringup_mask skip)           │
│   - Dedicated execution stacks (4KB/8KB) & O(1) stack context retrieval           │
│   - Transition page tables (trans_pgd generic core + arch backends)               │
+───────────────────────────────────────────────────────────────────────────────────+
                                         │
+───────────────────────────────────────────────────────────────────────────────────+
│                      Layer 1: Kexec Handover (KHO) & LUO                          │
│   - Minimal global FLB (cpu_preserved_global_ser: mask, runtime PA/size)          │
│   - File-private CPU serialization (cpu_preserved_file_ser -> session_ser)        │
│   - Folio restoration & non-overlapping scratch memory management                 │
+───────────────────────────────────────────────────────────────────────────────────+
                                         │
+───────────────────────────────────────────────────────────────────────────────────+
│                     Layer 5: Hardware Virtualization Backends                     │
│        Intel VMX (VMCS)   │   AMD SVM (VMCB/HSAVE)   │   ARM64 (Stage-2 / GICv3)  │
│   - Preemption timer      │   - APIC deadline timer  │   - Cross-vCPU SGIs        │
│   - Assembly vmenter.S    │   - Assembly vmenter.S   │   - Assembly vmenter.S     │
+───────────────────────────────────────────────────────────────────────────────────+
```

### 2.1 Architectural Layering & Separation of Concerns

To guarantee that hardware-specific features do not leak into core hypervisor code and to ensure cross-architecture portability, the architecture enforces strict isolation between layers:

1. **Layer 1: KHO & LUO Subsystem (`kernel/liveupdate/`)**
   - *Responsibility:* Preserving physical memory allocations across kexec, managing session lifetimes, and handling file descriptor serialization/deserialization.
   - *Strict Boundary:* Layer 1 has zero knowledge of virtual machines, vCPUs, or hardware execution states. It operates exclusively on generic file descriptors, memory pages, and serialization blobs.

2. **Layer 2: Physical CPU Preservation (`kernel/liveupdate/cpu_preserve.c`, `arch/*/kernel/cpu_preserve.c`)**
   - *Responsibility:* Shielding physical cores from host OS scheduling, intercepting CPU hotplug offline paths, allocating dedicated execution stacks, configuring architecture transition page tables (`trans_pgd`), and maintaining low-power parking loops.
   - *Strict Boundary:* Layer 2 is a generic CPU execution substrate. It knows nothing about KVM, virtualization registers, or VMCS/VMCB structures. It accepts pluggable workload callbacks via `cpu_preserved_attach_workload()`.

3. **Layer 3: Caretaker Framework (`kernel/liveupdate/caretaker.c`)**
   - *Responsibility:* Multi-session coordination, per-session private page table management, quantum deadline enforcement, round-robin time-sliced scheduling, and the single-job dedicated-core fast-path.
   - *Strict Boundary:* Layer 3 knows nothing about vendor virtualization architectures (Intel VMX, AMD SVM, or ARM64). It executes generic `struct caretaker_job` workloads.

4. **Layer 4: KVM Caretaker Abstraction (`virt/kvm/caretaker.c`)**
   - *Responsibility:* Bridging KVM guest abstractions to the Caretaker scheduler. It defines the vendor-neutral `struct kvm_caretaker_ops`, decodes high-level exit reasons, emulates early console MMIO (8250/PL011) and idle states (`HLT`/`WFI`), and orchestrates the atomic attachment handshake.
   - *Strict Boundary:* Layer 4 does not directly execute CPU instructions like `VMLAUNCH`, `VMRUN`, or `ERET`. It delegates hardware interaction to Layer 5 via function pointers.

5. **Layer 5: Hardware Virtualization Engines (`arch/*/kvm/`)**
   - *Responsibility:* Direct physical hardware manipulation: programming VMCS/VMCB host states, configuring preemption timers, restoring host segment selectors, managing hardware GICv3 registers, and executing low-level assembly enter/exit trampolines.
   - *Strict Boundary:* Contained strictly within architecture-specific directories (`arch/x86/kvm/vmx/`, `arch/x86/kvm/svm/`, `arch/arm64/kvm/`).

6. **Layer 6: Userspace Virtual Machine Monitor (`nanovmm/`)**
   - *Responsibility:* VM setup, guest RAM reservation via HugeTLB, guest kernel/initramfs loading, CLI strategy configuration (`-S cpu1-2`), and communicating with LUO via standard ioctls.

---

### 2.2 System Component Interaction Model

The following diagram illustrates the interaction between components during host reboot:

```
[ Management Core: CPU 0 ]                     [ Preserved Core: CPU 1 (Caretaker) ]
           │                                                      │
 1. Initiate Live Update                                          │
    (kexec -e triggered)                                          │
           │                                                      │
 2. smp_send_stop()                                               │
    - Unicasts REBOOT_VECTOR to CPU 2..N                          │
    - Explicitly SKIPS preserved CPU 1 ─────────────────────────> │
           │                                                      │
 3. Outgoing Kernel Tears Down                                    │
    - CPU 0 jumps into kexec trampoline                           │
    - Memory controllers preserved                                │ 3. Guest vCPU Execution
           │                                                      │    - Hardware VM-Entry
 4. Incoming Kernel Boots                                         │    - Guest runs instructions
    - Early boot reads minimal FLB                                │    - Trivial exit (HLT/UART)
    - cpuhp_bringup_mask() SKIPS CPU 1 ────────┐                  │    - Caretaker resolves exit
           │                                   │                  │    - Hardware VM-Reentry
 5. Incoming Userspace Starts                  │ (Skipped:        │    - ZERO INTERRUPTION
    - systemd & drivers initialize             │  No INIT/SIPI    │
    - New NanoVMM process launches             │  sent to core)   │
           │                                   │                  │
 6. Reattachment Handshake                     ▼                  │
    - NanoVMM retrieves LUO session                               │
    - KVM sets cb->attachment_state = ATTACHING                   │
    - KVM sends NMI kick to CPU 1 ──────────────────────────────> │ 4. Caretaker Detects Attach
           │                                                      │    - Exits guest slice
           │ <────────────────── Handshake Acknowledged ───────── │    - Saves final vCPU state
           │                                                      │    - Parks core (HLT loop)
 7. Adoption & Resumption                                         │
    - KVM adopts vCPU into incoming VM                            │
    - Physical core unpreserved via cpu_up()                      │
    - Normal host KVM_RUN resumes                                 │
           ▼                                                      ▼
```

---

### 2.3 The Three Execution Realms

During the live update gap, physical processor cores operate across three distinct privilege and architectural realms:

```
                     ┌────────────────────────────────────────────────────────┐
                     │              Realm 1: Host Linux Realm                 │
                     │  - Managed by Linux host scheduler (CPU 0)             │
                     │  - Reboots across kexec (Kernel A -> Kernel B)         │
                     │  - Standard kernel virtual memory (init_mm)            │
                     └────────────────────────────────────────────────────────┘
                                                 │
                                                 │ (Decoupled at Live Update Staging)
                                                 ▼
┌─────────────────────────────────────────────────────────────────────────────────────────────────────┐
│                                 Realm 2: Preserved Silicon Realm                                    │
│  - Dedicated physical cores (CPU 1..N) running in Host Mode (Ring 0 / EL2)                          │
│  - Executes standalone Caretaker runtime text (PAGE_KERNEL_ROX) and data (PAGE_KERNEL)              │
│  - Switched to private per-session PGD (sess->pgd_pa); isolated from host kernel memory             │
│  - Dedicated power-of-two aligned execution stacks with embedded context headers                    │
└─────────────────────────────────────────────────────────────────────────────────────────────────────┘
                                                 │
                                                 │ (Hardware VM-Entry / VM-Exit)
                                                 ▼
┌─────────────────────────────────────────────────────────────────────────────────────────────────────┐
│                                  Realm 3: Guest Workload Realm                                      │
│  - Guest OS and user applications running in Guest Mode (Ring 3 / Ring 0 / EL1 / EL0)               │
│  - Unmodified guest kernel memory mapped via Nested Page Tables (EPT / NPT / Stage-2)               │
│  - Continuous execution uninterrupted across the host kernel reboot gap                             │
└─────────────────────────────────────────────────────────────────────────────────────────────────────┘
```

---

### 2.4 Address Space Separation Model (Option B)

To guarantee that multiple independent tenants running across Caretaker cannot observe or corrupt each other's memory during the management gap, OrphanVM implements **Option B: Per-Session Page Tables**:

```
+─────────────────────────────────────────────────────────────────────────────────────────────────+
│                                Physical Memory (RAM) Layout                                     │
│                                                                                                 │
│  +─────────────────────────+    +─────────────────────────+    +─────────────────────────────+  │
│  │   Preserved Caretaker   │    │    Session 1 Memory     │    │      Session 2 Memory       │  │
│  │   Runtime Buffer        │    │    (Tenant A)           │    │      (Tenant B)             │  │
│  │   - Text (.text) [ROX]  │    │    - Guest RAM          │    │      - Guest RAM            │  │
│  │   - Data (.data) [RW]   │    │    - vCPU 0/1 States    │    │      - vCPU 0 State         │  │
│  │   - Global pcpus array  │    │    - VMCS/VMCB Pages    │    │      - VMCS/VMCB Pages      │  │
│  │   - GDT / IDT stubs     │    │    - Stacks (pCPU 1/2)  │    │      - Stack (pCPU 3)       │  │
│  +─────────────────────────+    +─────────────────────────+    +─────────────────────────────+  │
+─────────────────────────────────────────────────────────────────────────────────────────────────+
                 ▲                                ▲                                ▲
                 │                                │                                │
      Mapped into all sessions          Mapped ONLY into                 Mapped ONLY into
                 │                      Session 1 PGD                    Session 2 PGD
                 │                                │                                │
+─────────────────────────────────+              │                                │
│       Session 1 PGD (CR3)       │ <────────────┘                                │
│   - Maps Shared Runtime (ROX)   │                                               │
│   - Maps Tenant A RAM & Context │                                               │
│   - ZERO Tenant B Mappings      │                                               │
+─────────────────────────────────+                                               │
                                                                                  │
+─────────────────────────────────────────────────────────────────────────────────+
│       Session 2 PGD (CR3)       │ <─────────────────────────────────────────────┘
│   - Maps Shared Runtime (ROX)   │
│   - Maps Tenant B RAM & Context │
│   - ZERO Tenant A Mappings      │
+─────────────────────────────────+
```

Each session owns a private root page table (`sess->pgd_pa`) tracking up to `CARETAKER_MAX_PGD_PAGES` (128) intermediate page table pages. During physical core initialization, the CPU MMU switches directly to `sctx->session_pgd_pa`, ensuring hardware-enforced isolation.

---

## 3. Layer-by-Layer Architectural Specification

### 3.1 Layer 1: Kexec Handover (KHO) & Live Update Orchestrator (LUO)

Layer 1 operates as the memory and state preservation foundation of OrphanVM. It manages the physical preservation of memory folios across kexec reboots, coordinates session lifecycles, and defines the serialization formats passed between the outgoing and incoming kernels.

Crucially, Layer 1 maintains **zero domain knowledge of virtualization or vCPUs**. It treats all workloads as abstract file descriptors (`cpu_fh_v1`), memory folios, and packed serialization byte arrays.

---

#### 3.1.1 Minimal Global First-Level Boot (FLB) Data Layout

Early during incoming kernel initialization—long before userland starts, prior to root filesystem mounting, and before secondary SMP cores are booted—the incoming kernel must determine which physical CPUs must be protected from initialization. OrphanVM achieves this by serializing a strictly minimal global First-Level Boot (FLB) descriptor:

```
+───────────────────────────────────────────────────────────────────────────────────+
|               Global First-Level Boot Descriptor: struct cpu_preserved_global_ser  |
+──────────────────────────────────────┬────────────────────────────────────────────+
| Field Name                           | Architectural Description                  |
+──────────────────────────────────────┼────────────────────────────────────────────+
| cpumask_t cpu_preserved_mask         | Bitmask of physical CPUs preserved on-core |
| u64 text_runtime_pa                  | Physical address of standalone text buffer |
| u64 text_runtime_size                | Byte length of executable text buffer      |
| u64 data_runtime_pa                  | Physical address of standalone data buffer |
| u64 data_runtime_size                | Byte length of writable data buffer        |
| u64 pcpus_runtime_pa                 | Physical address of cpu_preserved_pcpus[]  |
+──────────────────────────────────────┴────────────────────────────────────────────+
```

```
+─────────────────────────────────────────────────────────────────────────────────+
|                         Early Incoming Boot Flow (CPU 0)                        |
|                                                                                 |
| 1. Early Boot Initialization                                                    |
|    kho_init() -> parses Device Tree / ACPI KHO handover tables                  |
|                                                                                 |
| 2. CPU Preservation Early Init                                                  |
|    arch_cpu_preserved_early_init() reads "cpu_flb_v1"                           |
|    Populates global cpu_preserved_mask bitmask                                  |
|                                                                                 |
| 3. Secondary SMP Core Bringup                                                   |
|    cpuhp_bringup_mask() iterates over possible CPUs:                            |
|      - Non-preserved cores (CPU 2..N): Send INIT/SIPI or PSCI CPU_ON -> Boots   |
|      - Preserved cores (CPU 1): cpumask_test_cpu(1, &cpu_preserved_mask)       |
|        -> SKIPS BRINGUP ENTIRELY; CORE REMAINS ACTIVE IN CARETAKER              |
+─────────────────────────────────────────────────────────────────────────────────+
```

**Architectural Rationale for Minimal Global FLB:**
1. **Attack Surface Minimization:** Exposing guest metadata, vCPU registers, or tenant memory references at the early boot stage would introduce vulnerabilities into the incoming kernel's boot path before security modules (SELinux, Lockdown) are initialized.
2. **Deterministic Early Boot:** Parsing complex hierarchical session data during early boot is error-prone. The minimal FLB requires only basic scalar reads and a single bitmask copy.
3. **Complete Tenant Decoupling:** Global boot structures contain no tenant identifiers or VM tokens.

---

#### 3.1.2 Option B: File-Private KHO Data Architecture

Rather than aggregating all preserved core contexts into a monolithic global structure, OrphanVM implements **Option B: Per-File Private Serialization**. Each preserved physical core is represented in userspace by an open sysfs file descriptor (`/sys/devices/system/cpu/cpu<N>/preserve`). When registered with LUO under compatible string `CPU_PRESERVED_LUO_FH_COMPATIBLE` (`"cpu_fh_v1"`), private state is serialized inside `args->serialized_data`:

```
+───────────────────────────────────────────────────────────────────────────────────+
|                  Per-File Serialization: struct cpu_preserved_file_ser            |
+──────────────────────────────────────┬────────────────────────────────────────────+
| Field Name                           | Architectural Description                  |
+──────────────────────────────────────┼────────────────────────────────────────────+
| u32 magic                            | Magic signature: 0x43505546 ("CPUF")       |
| u32 cpu                              | Physical CPU logical identifier            |
| u64 session_ser_pa                   | Physical address of caretaker_session_ser  |
| u64 stack_pa                         | Physical address of dedicated stack        |
| u32 stack_order                      | Allocation order of dedicated stack        |
| u64 pcpu_pa                          | Physical address of struct per-cpu desc    |
+──────────────────────────────────────┴────────────────────────────────────────────+
```

The serialization graph decouples multi-tenant sessions cleanly across kexec:

```
[ Global KHO Table ] ──> struct cpu_preserved_global_ser (cpu_preserved_mask: 0x06)
                                 │
                 ┌───────────────┴───────────────┐
                 ▼                               ▼
      [ LUO File: cpu1 ]              [ LUO File: cpu2 ]
   struct cpu_preserved_file_ser   struct cpu_preserved_file_ser
                 │                               │
                 ▼ (session_ser_pa)              ▼ (session_ser_pa)
  struct caretaker_session_ser    struct caretaker_session_ser
           (Tenant A)                      (Tenant B)
                 │                               │
       ┌─────────┴─────────┐           ┌─────────┴─────────┐
       ▼                   ▼           ▼                   ▼
  sess->pgd_pa        session->rq sess->pgd_pa        session->rq
 (Tenant A PGD)        (vCPU 0)  (Tenant B PGD)        (vCPU 0)
```

**Key Benefits of File-Private Serialization:**
- **Independent Failure Domains:** If Tenant A crashes or userspace cancels Tenant A's LUO session during kexec staging, Tenant A's folios are released via `kho_unpreserve_free()` without mutating or endangering Tenant B.
- **Dynamic Session Ownership:** Preserved CPUs are directly bound to the specific LUO session that acquired them.
- **Granular Retrieval:** The incoming userspace manager retrieves preserved CPU file descriptors individually via standard LUO session ioctls, enabling fine-grained reattachment.

---

#### 3.1.3 KHO Memory Lifecycle & Preservation Invariants

OrphanVM classifies memory into four distinct preservation lifecycles:

```
+───────────────────────────────────────────────────────────────────────────────────+
| Class 1: Standalone Runtime Buffer (Global)                                       |
|   - __cpu_preserved_text (PAGE_KERNEL_ROX) & __cpu_preserved_data (PAGE_KERNEL)   |
|   - Allocated via kho_alloc_preserve() during outgoing kernel boot.               |
|   - Persists across kexec; shared read-only across all tenant sessions.          |
+───────────────────────────────────────────────────────────────────────────────────+
| Class 2: Session-Private Page Table Pages (Per-Session)                           |
|   - Bounded array sess->pgd_pages[128] backing private session PGDs.             |
|   - Immediately preserved via kho_preserve_pages() and D-cache cleaned at alloc.  |
|   - Freed on outgoing cancellation or incoming session termination.               |
+───────────────────────────────────────────────────────────────────────────────────+
| Class 3: Workload Architecture Pages (Per-vCPU)                                   |
|   - Architecture state pages: struct caretaker_x86_page, VMCS/VMCB, GIC state.   |
|   - Preserved directly with the vCPU file descriptor.                             |
+───────────────────────────────────────────────────────────────────────────────────+
| Class 4: Guest Physical RAM (Per-VM)                                              |
|   - Backed by memfd / HugeTLB / guest_memfd.                                      |
|   - Preserved by standard LUO memory management; mapped into session PGD.         |
+───────────────────────────────────────────────────────────────────────────────────+
```

**The Non-Overlapping Scratch Memory Invariant:**
When kexec stages the incoming kernel image, it unpacks kernel ELF segments into physical "scratch memory". It is a fundamental invariant that **no KHO-preserved memory folio may overlap with the incoming kernel's scratch range**:
$$\text{PreservedFolio}(i) \cap \text{KexecScratchRange} = \emptyset \quad \forall i$$
The KHO subsystem enforces this invariant during allocation by sourcing preserved folios strictly from reserved memory regions outside the incoming kexec placement window.

---

### 3.2 Layer 2: Physical CPU Preservation Infrastructure (`cpu_preserve`)

Layer 2 provides the low-level physical CPU execution substrate. It isolates physical cores from host OS scheduling, shields them against shutdown and startup IPIs, allocates private execution stacks, and configures transitional MMU page tables.

---

#### 3.2.1 Host Scheduling Decoupling & Hotplug Interception

To safely run an orphaned workload without interference from host OS tasks, timers, or interrupts, the preserved core must be fully decoupled from the Linux kernel scheduler. Rather than inventing an out-of-band CPU offlining path, OrphanVM integrates directly into the Linux CPU hotplug state machine:

```
[ Normal Online CPU ]
         │
         │ userspace registers CPU fd with LUO
         ▼
[ CPU Hotplug Offline Sequence ]
   1. CPUHP_TEARDOWN_CPU: Migrate user tasks and kernel threads away
   2. CPUHP_AP_SCHED_STARTING: Stop scheduler tick on target core
   3. CPUHP_AP_IRQ_AFFINITY: Migrate hardware IRQs and affinity masks to CPU 0
   4. CPUHP_AP_RCU_DYING: Evacuate pending RCU callbacks
   5. cpuhp_ap_report_dead(): Final CPU teardown notification
         │
         ▼
[ Hotplug Intercept: cpu_preserved_report_dead() ]
         │
         ├── Core IS preserved? ──> NO  ──> Standard hardware power-down / mwait
         │
         YES
         │
         ▼
[ Enter Caretaker Execution Realm ]
   - Load self-contained GDT / IDT stubs (arch_cpu_preserved_load_desc)
   - Switch hardware stack pointer to dedicated preserved stack
   - Switch MMU translation to transitional page table (trans_pgd)
   - Enter cpu_preserved_park() / Caretaker workload loop
```

By intercepting hotplug at `cpuhp_ap_report_dead()`, OrphanVM guarantees that:
- The core is cleared from `cpu_online_mask` and `cpu_active_mask`.
- The Linux CFS/RT schedulers will never place a host thread on this core.
- Hardware device interrupts are steered away to non-preserved host CPUs.
- Physical execution never stalls in firmware ACPI/PSCI sleep states.

---

#### 3.2.2 SMP Boot & Shutdown Protection

During a standard kexec reboot, the outgoing kernel halts all secondary cores using IPIs, and the incoming kernel reboots all secondary cores using firmware boot protocols. OrphanVM modifies both paths to protect preserved cores:

```
A. Outgoing Kernel Shutdown: smp_send_stop()
   Normal:   Broadcasts REBOOT_VECTOR / NMI to all secondary CPUs (kills guest).
   OrphanVM: Iterates over online CPUs. If cpu_is_preserved(cpu) == true, the core
             is EXCLUDED from the stop vector unicast. The guest continues running.

B. Incoming Kernel Boot: cpuhp_bringup_mask()
   Normal:   Issues APIC INIT-SIPI (x86) or PSCI CPU_ON (ARM64) to reset cores.
   OrphanVM: Tests cpu_preserved_mask (read from FLB). Preserved cores are SKIPPED.
             No hardware reset signal is sent; Caretaker execution is uninterrupted.
```

---

#### 3.2.3 Dedicated Execution Stacks & $\mathcal{O}(1)$ Stack-Embedded Context

Standard Linux kernel stacks (`thread_info`) belong to host tasks and are destroyed or overwritten during kexec. Preserved cores execute on private, dedicated stacks:

```
+───────────────────────────────────────────────────────────────────────────────────+
|               Preserved CPU Stack Layout (Power-of-Two Aligned)                   |
|               Size: 4KB (x86_64, order 0) / 8KB (ARM64, order 1)                  |
+───────────────────────────────────────────────────────────────────────────────────+
| Higher Memory Addresses                                                           |
|  [stack_va + CPU_PRESERVED_STACK_SIZE]                                            |
|  ───────────────────────────────────────────────────────────────────────────────  |
|  ▲ Headroom: CPU_PRESERVED_STACK_HEADROOM (256 bytes reserved)                    |
|  │ [Initial stack pointer: stack_top]                                             |
|  │                                                                                |
|  │ Caretaker Execution Frames (Call frames, GPR save areas, interrupt stubs)     |
|  │ (Grows downward toward stack base)                                             |
|  ▼                                                                                |
|  ───────────────────────────────────────────────────────────────────────────────  |
|  [Base of Stack: stack_va]                                                        |
|  struct cpu_preserved_stack_context {                                             |
|      u64                        magic;          // 0x435055505354414b ("CPUPSTAK")|
|      int                        cpu;            // Physical CPU ID                |
|      struct caretaker_session   *session;       // Owning Caretaker session       |
|      phys_addr_t                session_pgd_pa; // Session root PGD physical addr |
|      void                       *entry_data;    // Workload private pointer       |
|  };                                                                               |
+───────────────────────────────────────────────────────────────────────────────────+
| Lower Memory Addresses                                                            |
+───────────────────────────────────────────────────────────────────────────────────+
```

**$\mathcal{O}(1)$ Lockless Context Retrieval:**
Standard per-CPU variables (`this_cpu_ptr`) and task descriptors (`current`) rely on `%gs` (x86) or `TPIDR_EL1` (ARM64), which point to host kernel memory invalidated across kexec. OrphanVM retrieves the active CPU context in $\mathcal{O}(1)$ instructions purely by masking the current hardware stack pointer:

```c
static inline struct cpu_preserved_stack_context *
cpu_preserved_get_stack_context(void)
{
    unsigned long sp;
#if defined(CONFIG_X86_64)
    asm volatile("mov %%rsp, %0" : "=r"(sp));
#elif defined(CONFIG_ARM64)
    asm volatile("mov %0, sp" : "=r"(sp));
#endif
    struct cpu_preserved_stack_context *sctx =
        (struct cpu_preserved_stack_context *)(sp & ~(CPU_PRESERVED_STACK_SIZE - 1));

    if (sctx && sctx->magic == CPU_PRESERVED_STACK_MAGIC)
        return sctx;
    return NULL;
}
```

This ensures instantaneous, safe access to the physical CPU ID, session pointer, and root PGD address from any function or interrupt stub without global locks or segment register dependencies.

---

#### 3.2.4 Architecture Transition Page Tables (`trans_pgd`)

When the host kernel kexecs, the primary kernel page tables (`init_mm.pgd` / `swapper_pg_dir`) are torn down and re-initialized. Preserved cores cannot execute with the host PGD active.

OrphanVM leverages and extends the architecture transition page table subsystem (`asm/trans_pgd.h`):

```
+───────────────────────────────────────────────────────────────────────────────────+
|                Transition Page Table (trans_pgd) Mapping Whitelist                |
+───────────────────────────────────┬──────────────┬────────────────────────────────+
| Virtual Address Range             | Protection   | Architectural Contents         |
+───────────────────────────────────┼──────────────┼────────────────────────────────+
| [__cpu_preserved_text_start..end) | ROX          | Caretaker text, vmenter.S, stubs|
| [__cpu_preserved_data_start..end) | RW, NX       | Global state, IDT, GDT, pcpus  |
| [pcpu->stack .. + STACK_SIZE)     | RW, NX       | Dedicated per-core stack       |
| Workload Context Pages            | RW, NX       | struct caretaker_x86_page, VMCS|
| FIX_APIC_BASE (Legacy xAPIC MMIO) | RW, NX, IO   | Local APIC MMIO registers      |
+───────────────────────────────────┴──────────────┴────────────────────────────────+
| FORBIDDEN (Strictly Unmapped):                                                    |
|  - PAGE_OFFSET Direct Physical Linear Map                                         |
|  - Userspace virtual address ranges (0x0000000000000000 - 0x00007FFFFFFFFFFF)     |
|  - Host kernel heap, vmalloc, BPF JIT, and modules areas                          |
+───────────────────────────────────────────────────────────────────────────────────+
```

**Allocator Independence:**
`trans_pgd` constructs page tables using an allocator-independent callback interface (`struct trans_pgd_info`). In OrphanVM, the allocator callback hooks into `kho_alloc_preserve()`, ensuring that intermediate page directory levels (P4D, PUD, PMD, PTE) are immediately preserved across kexec and flushed to the Point of Coherency (PoC).

### 3.3 Layer 3: The Caretaker Execution & Scheduling Framework (`caretaker`)

Layer 3 provides the hardware-agnostic execution engine and scheduling substrate for orphaned workloads across the live update gap. It multiplexes compute resources, enforces multi-tenant memory isolation via per-session page tables, and implements a high-performance quantum scheduler.

Crucially, Layer 3 contains **zero vendor-specific virtualization logic** (no Intel VMX, AMD SVM, or ARM64 instructions). It manages abstract `struct caretaker_job` workloads.

---

#### 3.3.1 Session Architecture & Management (`struct caretaker_session`)

Each independent tenant VM registered with LUO corresponds to a dedicated `struct caretaker_session`:

```
+───────────────────────────────────────────────────────────────────────────────────+
|               Caretaker Session Descriptor: struct caretaker_session               |
+──────────────────────────────────────┬────────────────────────────────────────────+
| Field Name                           | Architectural Description                  |
+──────────────────────────────────────┼────────────────────────────────────────────+
| char name[64]                        | Unique LUO session identifier string       |
| cpumask_t cpus                       | Mask of physical CPUs allocated to session |
| unsigned int cpu_jobs[NR_CPUS]       | Count of assigned jobs per physical CPU    |
| struct mutex lock                    | Mutex protecting session lifecycle state   |
| struct caretaker_runqueue rq         | Unified circular runqueue of runnable jobs |
| struct caretaker_sched_config        | Configured quantum duration and ticks      |
| void *pgd / phys_addr_t pgd_pa       | Root transition page table virtual/physical|
| phys_addr_t pgd_pages[128]           | Array of intermediate page table pages     |
| unsigned int nr_pgd_pages            | Count of allocated page table pages (<=128)|
| struct caretaker_session_ser *ser    | KHO-preserved session metadata descriptor  |
| bool is_incoming                     | True if restored during incoming boot      |
+──────────────────────────────────────┴────────────────────────────────────────────+
```

**Immediate Page Preservation & D-Cache Cleaning Invariant:**
When a session creates its private page table (`arch_caretaker_alloc_session_pgd`), intermediate page directories (P4D, PUD, PMD, PTE) are allocated via `arm64_caretaker_alloc_page()` / `x86_caretaker_alloc_page()`.
- Each allocated page is **immediately preserved via `kho_preserve_pages()` at allocation time**.
- The page is immediately cleaned to the Point of Coherency (PoC) via data cache flushes.
- The physical address is appended to the bounded array `sess->pgd_pages[sess->nr_pgd_pages++]`.

**Architectural Rationale:**
In early prototypes, page table pages were discovered by walking the page table hierarchy during the kexec staging phase. This introduced an $\mathcal{O}(N)$ radix-tree walk with unbounded latency right before reboot. The immediate preservation invariant ensures $\mathcal{O}(1)$ allocation overhead, eliminates late page table traversal, and guarantees deterministic staging times.

---

#### 3.3.2 Multi-Tenant Address Space Isolation (Option B Invariant)

To guarantee that multiple independent tenants running across Caretaker cannot observe or corrupt each other's memory during the management gap, OrphanVM enforces strict Address Space Isolation (ASI):

```
+───────────────────────────────────────────────────────────────────────────────────+
|                   Option B Per-Session Memory Mapping Isolation                   |
+───────────────────────────────────┬──────────────┬────────────────────────────────+
| Virtual Address Range             | Protection   | Visibility / Scope             |
+───────────────────────────────────┼──────────────┼────────────────────────────────+
| Shared Caretaker Text             | ROX          | Mapped across ALL session PGDs |
| Shared Caretaker Data & Descs     | RW, NX       | Mapped across ALL session PGDs |
| Tenant A Private Stacks           | RW, NX       | Mapped ONLY in Session A PGD   |
| Tenant A vCPU Pages (VMCS/VMCB)   | RW, NX       | Mapped ONLY in Session A PGD   |
| Tenant A Guest RAM (HugeTLB)      | RW, NX       | Mapped ONLY in Session A PGD   |
+───────────────────────────────────┴──────────────┴────────────────────────────────+
| Cross-Session Isolation Invariant:                                                |
|   Session_A_Virtual_Space ∩ Session_B_Private_Memory = ∅                          |
| A hardware MMU violation / fault occurs immediately if Tenant A accesses Tenant B |
+───────────────────────────────────────────────────────────────────────────────────+
```

When a physical core switches between sessions or enters a session workload, the MMU reloads the root page table pointer (`CR3` on x86, `TTBR1_EL1` on ARM64) with `sess->pgd_pa`, ensuring hardware-enforced tenant boundaries.

---

#### 3.3.3 Round-Robin Quantum Scheduler & Dedicated-Core Fast-Path

Caretaker provides a deterministic, lightweight scheduler designed for predictable latency:

```
+───────────────────────────────────────────────────────────────────────────────────+
|             Caretaker Schedulable Job: struct caretaker_job                       |
+──────────────────────────────────────┬────────────────────────────────────────────+
| Field Name                           | Architectural Description                  |
+──────────────────────────────────────┼────────────────────────────────────────────+
| struct list_head node                | Runqueue linkage node                      |
| struct caretaker_session *session    | Owning session pointer                     |
| char name[64]                        | Workload identifier (e.g. "vcpu0")         |
| enum caretaker_job_state state       | NEW, RUNNABLE, RUNNING, CANCELING, DEAD    |
| caretaker_job_fn run_fn              | Workload execution dispatch callback       |
| void *data                           | Opaque workload context (vCPU descriptor)  |
| int preferred_cpu / assigned_cpu     | Physical core affinity binding             |
| u64 total_runs / total_runtime_ns    | Cumulative execution runtime accounting    |
| u64 preemptions                      | Count of quantum expiration preemption events|
+──────────────────────────────────────┴────────────────────────────────────────────+
```

```
+───────────────────────────────────────────────────────────────────────────────────+
|                    Caretaker Quantum Scheduling Architecture                      |
|                                                                                   |
|  Configured Quantum: 10ms (caretaker.quantum_ms=<N>)                             |
|  Hardware Counter Ticks: quantum_ticks = (quantum_ms * counter_frequency) / 1000  |
|                                                                                   |
|           ┌─────────────────────────────────────────────────────────┐             |
|           ▼                                                         │             |
|  [ Session Runqueue: rq.runnable ]                                  │             |
|    Head -> [ Job 0: vCPU 0 ] -> [ Job 1: vCPU 1 ] -> ... ───────────┘             |
|                 │                                                                 |
|                 │ caretaker_rq_lock() & pop_head()                                |
|                 ▼                                                                 |
|  [ Physical CPU Core Execution ]                                                  |
|    - Set hardware preemption timer to: current_ticks + quantum_ticks              |
|    - Execute job->run_fn(job->data, deadline_ticks)                               |
|    - Hardware VM-Entry into Guest Execution Slice                                 |
|                 │                                                                 |
|                 ├── Preemption Timer Expires (CARETAKER_EXIT_QUANTUM_EXPIRED)     |
|                 ▼                                                                 |
|  [ Re-enqueue & Context Switch ]                                                  |
|    - Re-insert Job 0 at tail of rq.runnable                                       |
|    - Dequeue Job 1 and dispatch to physical core                                  |
+───────────────────────────────────────────────────────────────────────────────────+
```

**The Dedicated-Core Lock-Free Fast-Path:**
In high-performance virtualization environments, vCPUs are pinned 1:1 to physical cores ($M = N$). For this dominant operational mode, the Caretaker scheduler provides an ultra-low overhead fast-path:

```c
/* Fast-Path Check in caretaker_session_run_slice(): */
if (rq->nr_runnable == 0 && current_job->state == CARETAKER_JOB_RUNNING) {
    /*
     * DEDICATED CORE FAST-PATH:
     * No other jobs compete for this core.
     * Bypass spinlock acquisition, list removal, and list re-insertion!
     */
    deadline_ticks = arch_caretaker_read_counter() + sched_config->quantum_ticks;
    reason = current_job->run_fn(current_job->data, deadline_ticks);
    continue;
}
```

This fast-path eliminates spinlock overhead, atomic bus contention, and memory cache bouncing, delivering **near-bare-metal vCPU execution throughput** during the host reboot gap.

---

#### 3.3.4 Safe Handshake & Cancellation Protocol

To guarantee clean recovery if a live update is aborted or if a tenant session is terminated, Caretaker implements an atomic job state progression:

```
[ CARETAKER_JOB_NEW ]
         │
         │ caretaker_session_submit_job()
         ▼
[ CARETAKER_JOB_RUNNABLE ]
         │
         │ Pop from runqueue by physical core
         ▼
[ CARETAKER_JOB_RUNNING ] <──────┐ (Fast-path / quantum refresh)
         │                       │
         ├── Cancellation?       │
         │   caretaker_session_cancel_job()
         ▼
[ CARETAKER_JOB_CANCELING ]
   - atomic_set(&job->state, CARETAKER_JOB_CANCELING)
   - arch_cpu_preserved_kick(job->assigned_cpu) via NMI / IPI
   - Poll for job to notice cancellation (timeout: CARETAKER_CANCEL_TIMEOUT_US = 20s)
   - Step sleep: 100µs; re-kick every 5ms
         │
         ▼ (Core acknowledges and exits slice)
[ CARETAKER_JOB_DEAD ]
   - Core parks in cpu_preserved_park()
   - Session structures safe to unpreserve / free
```

---

### 3.4 Layer 4: KVM Caretaker Abstraction Layer (`virt/kvm/caretaker`)

Layer 4 bridges the hypervisor-agnostic Caretaker scheduler (Layer 3) with the architecture virtualization backends (Layer 5). It provides vendor-neutral VM exit classification, services trivial in-gap exits, and coordinates the reattachment handshake with the incoming host kernel.

---

#### 3.4.1 Virtualization Operations Vector (`struct kvm_caretaker_ops`)

To decouple generic exit handling logic from architecture-specific control registers and instructions, Layer 4 defines an operations vector table:

```c
struct kvm_caretaker_ops {
    int  (*enter_guest)(void *vcpu_data);
    void (*decode_exit)(void *vcpu_data, struct kvm_caretaker_exit *exit);
    bool (*handle_arch_exit)(void *vcpu_data, struct kvm_caretaker_exit *exit);
    void (*advance_rip)(void *vcpu_data, u64 next_rip);
    void (*arm_timer)(void *vcpu_data, u64 deadline_ticks);
    void (*disarm_timer)(void *vcpu_data);
    void (*pre_run)(void *vcpu_data);
    void (*post_run)(void *vcpu_data);
    void (*sync_vcpu)(struct kvm_vcpu *vcpu, void *vcpu_data);
};
```

```
+───────────────────────────────────────────────────────────────────────────────────+
|               Responsibilities of kvm_caretaker_ops Callbacks                     |
+──────────────────────────┬────────────────────────────────────────────────────────+
| Callback                 | Architectural Operation                                |
+──────────────────────────┼────────────────────────────────────────────────────────+
| enter_guest()            | Invokes low-level assembly world-switch (vmenter.S)    |
| decode_exit()            | Translates raw hardware exit codes into normalized exit|
| handle_arch_exit()       | Processes architecture-specific exits (e.g. ARM64 SGI) |
| advance_rip()            | Advances guest instruction pointer past completed insn |
| arm_timer()              | Programs hardware preemption timer or APIC deadline    |
| disarm_timer()           | Disarms hardware timer prior to context switch         |
| pre_run() / post_run()   | Host register save/restore and FPU/SIMD state isolation|
| sync_vcpu()              | Copies preserved registers back into host kvm_vcpu     |
+──────────────────────────┴────────────────────────────────────────────────────────+
```

---

#### 3.4.2 Normalized Exit Dispatching & Emulation Engine

During the live update gap, userspace VMMs (QEMU/NanoVMM) and host Linux kernel services are unavailable. Caretaker must autonomously resolve all VM exits that occur:

```
+───────────────────────────────────────────────────────────────────────────────────+
|                 Normalized VM Exit Types (enum kvm_caretaker_exit_type)           |
+──────────────────────────────────┬────────────────────────────────────────────────+
| Exit Type                        | In-Gap Caretaker Emulation Strategy            |
+──────────────────────────────────┼────────────────────────────────────────────────+
| KVM_CARETAKER_EXIT_IDLE          | HLT / PAUSE / WFI: Arm timer, execute low-power|
|                                  | wait (cpu_relax / wfe) until interrupt/deadline|
+──────────────────────────────────┼────────────────────────────────────────────────+
| KVM_CARETAKER_EXIT_CONSOLE       | 8250 / PL011 UART MMIO: Absorb early boot      |
|                                  | printk writes, maintain valid FIFO/LSR status  |
+──────────────────────────────────┼────────────────────────────────────────────────+
| KVM_CARETAKER_EXIT_PREEMPT_TIMER | Time slice expired: Yield slice to next vCPU   |
+──────────────────────────────────┼────────────────────────────────────────────────+
| KVM_CARETAKER_EXIT_CROSS_VCPU    | APIC ICR / GICv3 SGI: Route cross-vCPU kicks   |
+──────────────────────────────────┼────────────────────────────────────────────────+
| KVM_CARETAKER_EXIT_CPUID         | Service CPUID queries using cached capabilities|
+──────────────────────────────────┼────────────────────────────────────────────────+
| KVM_CARETAKER_EXIT_RDTSC         | Read physical TSC counter with zero exit drift |
+──────────────────────────────────┼────────────────────────────────────────────────+
| KVM_CARETAKER_EXIT_INSN_STEP     | INVD, WBINVD: Step instruction pointer past op |
+──────────────────────────────────┴────────────────────────────────────────────────+
```

**Detailed Emulation Mechanics:**
1. **Early Console Emulation (8250 & PL011 UART):**
   Guest kernels frequently output progress messages via early serial printk. If unhandled, MMIO exits cause VM hangs. Layer 4 emulates:
   - **8250 UART (x86 I/O ports `0x3f8`–`0x3ff`):** Emulates Line Status Register (`LSR`) returning `UART_LSR_TEMT | UART_LSR_THRE` (Transmitter Empty), Line Control (`LCR`), and Divisor Latch (`DLL`/`DLM`). Characters written to Transmit Holding Register (`THR`) are absorbed or written to debug buffers.
   - **PL011 UART (ARM64 MMIO `0x09000000`):** Emulates Data Register (`UARTDR`) and Flag Register (`UARTFR`) asserting `TXFE` (Transmit FIFO Empty) and clearing `BUSY`.
2. **Guest Idle Exits (`HLT` / `WFI`):**
   When the guest operating system has no runnable threads, it executes `HLT` (x86) or `WFI` (ARM64). Caretaker:
   - Advances guest `RIP` / `PC`.
   - Computes remaining quantum ticks until the next scheduled event.
   - Enters architecture low-power wait (`cpu_relax()` / `wfe()`), preventing 100% host core burn while idling.

---

#### 3.4.3 Hypervisor Detachment & Reattachment Handshake Protocol

Reattaching an orphaned, actively running vCPU into the incoming kernel's KVM subsystem requires a lockless, cross-kernel handshake orchestrated via the Caretaker Control Block (`struct caretaker_cb`):

```
[ Outgoing Kernel KVM ]                           [ Caretaker Core ]
          │                                               │
 1. Set cb->attachment_state = CARETAKER_KVM_DETACHED     │
 2. Jump to kexec boot ─────────────────────────────────> │ (Autonomous Caretaker Run)
                                                          │ - Enters guest
                                                          │ - Handles exits
                                                          │ - Loops on-core
[ Incoming Kernel KVM ]                                   │
          │                                               │
 3. Userspace VMM calls KVM_SET_USER_MEMORY_REGION        │
 4. KVM attaches to preserved vCPU:                       │
    - Set cb->attachment_state = CARETAKER_KVM_ATTACHING  │
    - arch_cpu_preserved_kick(target_cpu) via NMI/IPI ──> │ 5. Caretaker Detects Attach:
          │                                               │    - Detects ATTACHING state
          │                                               │    - Calls ops->sync_vcpu()
          │                                               │    - Saves guest GPRs/MSRs
          │                                               │    - Sets cb->attachment_state
          │                                               │      = CARETAKER_KVM_ATTACHED
          │                                               │    - Exits Caretaker loop
          │                                               │    - Drops into cpu_preserved_park()
          │ <────────────── Handshake Complete ───────────│
 6. Incoming KVM confirms ATTACHED state                  │
 7. Bring core online via cpu_up()                        │
 8. Normal KVM_RUN takes ownership of vCPU ───────────────┘
```

**Attachment Timeout & Resilience:**
The incoming kernel polls for handshake completion with a 2.0-second timeout (`KVM_CARETAKER_ATTACH_TIMEOUT_US = 2000000`), sending periodic NMI/IPI kicks every 5 milliseconds. If the guest is in a tight non-root instruction loop, the next hardware exit or preemption timer event guarantees instantaneous exit and handshake completion.

### 3.5 Layer 5: Platform-Specific Virtualization Engines

Layer 5 contains the hardware-specific virtualization engines and low-level assembly trampolines. It translates high-level Caretaker scheduling requests into concrete processor instructions (`VMLAUNCH`, `VMRUN`, `ERET`), programs hardware control structures, and enforces architectural invariants.

---

#### 3.5.1 Intel VMX Engine (`arch/x86/kvm/vmx/`)

The Intel VMX backend implements autonomous non-root guest execution using hardware VMCS controls and standalone assembly trampolines.

```
+───────────────────────────────────────────────────────────────────────────────────+
|               Intel VMX Preserved Page: struct caretaker_vmx_page                 |
+──────────────────────────────────────┬────────────────────────────────────────────+
| Field Name                           | Architectural Description                  |
+──────────────────────────────────────┼────────────────────────────────────────────+
| struct caretaker_x86_page common     | Shared x86 Caretaker descriptor            |
|   - struct caretaker_cb cb           | Atomic attachment state control block      |
|   - u64 vmcs_pa                      | Physical address of guest VMCS folio       |
|   - struct desc_struct gdt[GDT_ENTRIES] Preserved standalone GDT                  |
|   - gate_desc idt[256]               | Preserved standalone IDT                   |
|   - struct x86_hw_tss tss            | Preserved Task State Segment (TSS)         |
|   - u8 stack[PAGE_SIZE]              | Dedicated execution stack for VMX loops    |
|   - u64 host_cr3                     | Session root PGD physical address          |
|   - u64 kernel_gs_base               | Guest MSR_KERNEL_GS_BASE value             |
+──────────────────────────────────────┴────────────────────────────────────────────+
```

**Host VMCS Configuration for Standalone Execution:**
During host live update, the outgoing Linux kernel tears down its host segment registers, GDT, and IDT. To prevent hardware VM-exit failures (`VMX_EXIT_REASON_FAILED_VMENTRY`), the VMX engine reconfigures the VMCS host-state area:
- `HOST_CR3` is programmed directly to `cvp->common.host_cr3` (`session_pgd_pa`).
- `HOST_RIP` is programmed to the assembly entry point in `caretaker_vmenter.S` (`vmx_caretaker_exit_handler`).
- `HOST_RSP` is pointed to the dedicated stack (`&cvp->common.stack[PAGE_SIZE - 256]`).
- `HOST_CS_SELECTOR` (`__KERNEL_CS`), `HOST_DS_SELECTOR`, `HOST_ES_SELECTOR`, `HOST_FS_SELECTOR`, `HOST_GS_SELECTOR`, and `HOST_TR_SELECTOR` are wired to selectors defined within the standalone GDT.
- `HOST_GDTR_BASE`, `HOST_IDTR_BASE`, and `HOST_TR_BASE` point to `cvp->common.gdt`, `cvp->common.idt`, and `cvp->common.tss`.

**Hardware Preemption Timer Time-Slicing:**
To enforce quantum deadlines without software timer interrupts, the Intel engine programs the hardware VMX Preemption Timer:
- Set bit 6 (`PIN_BASED_VMX_PREEMPTION_TIMER`) in `PIN_BASED_VM_EXEC_CONTROL`.
- The timer value is calculated using the hardware multiplier reported in `MSR_IA32_VMX_MISC[4:0]`:
  $$\text{TimerValue} = \frac{\text{DeadlineTicks} - \text{CurrentTSC}}{2^{\text{VMX\_MISC\_SHIFT}}}$$
- The processor hardware automatically decrements the preemption timer in non-root operation at the designated rate, triggering exit code `EXIT_REASON_PREEMPTION_TIMER` (0x34) when it expires.
- Before executing `VMCLEAR` or context switching, the timer is safely disarmed via `vmx_caretaker_disarm_timer()`.

**Assembly World-Switch Trampoline (`caretaker_vmenter.S`):**
- Saves host callee-saved registers (`%rbx`, `%rbp`, `%r12`–`%r15`).
- Preserves host `%cr2` across guest execution.
- Restores guest General Purpose Registers (`RAX` through `R15`) from `cvp->common`.
- Executes `VMLAUNCH` (or `VMRESUME` if previously launched).
- On VM-exit, saves all guest GPRs, saves `EXIT_QUALIFICATION`, `VM_EXIT_REASON`, and `GUEST_RIP`, and restores host registers.
- **Architectural Corner Case:** `EFER_LMA` is gated on the `CS.L` (Long Mode) attribute to cleanly support 16-bit real-mode AP trampolines as well as 64-bit kernels without triggering VMCS consistency checks.

---

#### 3.5.2 AMD SVM Engine (`arch/x86/kvm/svm/`)

The AMD SVM engine executes guest workloads using the Virtual Machine Control Block (VMCB) and host save area (`HSAVE`).

```
+───────────────────────────────────────────────────────────────────────────────────+
|               AMD SVM Preserved Page: struct caretaker_svm_page                   |
+──────────────────────────────────────┬────────────────────────────────────────────+
| Field Name                           | Architectural Description                  |
+──────────────────────────────────────┼────────────────────────────────────────────+
| struct caretaker_x86_page common     | Shared x86 Caretaker descriptor            |
| struct vmcb vmcb                     | Preserved VMCB copy for standalone run     |
| u8 hsave_area[PAGE_SIZE]             | Standalone Host Save Area (MSR_VM_HSAVE_PA)|
+──────────────────────────────────────┴────────────────────────────────────────────+
```

**Host VMCB Configuration:**
- `hsave_area` is mapped into the session PGD and its physical address is programmed into `MSR_VM_HSAVE_PA`.
- VMCB Intercepts are explicitly configured for standalone execution:
  - **Intercepted:** `INTERCEPT_HLT`, `INTERCEPT_CPUID`, `INTERCEPT_MSR`, `INTERCEPT_NMI`, `INTERCEPT_INIT`.
  - **Passed-Through (Cleared):** `INTERCEPT_PAUSE`, `INTERCEPT_RDTSC`, `INTERCEPT_VMMCALL`, `INTERCEPT_MONITOR`, `INTERCEPT_MWAIT`, `INTERCEPT_INTR`.
- Clean bits (`control.clean = 0`) are cleared and `tlb_ctl` is set to `TLB_CONTROL_FLUSH_ALL_ASID` to ensure clean TLB state on the preserved physical core.

**Time-Slicing via Local APIC Deadline Timers:**
Because AMD SVM does not feature an integrated in-VMCB hardware preemption timer, Caretaker programs the local APIC timer:
- In x2APIC mode, programs `MSR_IA32_TSC_DEADLINE` with the target TSC quantum deadline.
- In legacy xAPIC mode, programs `APIC_TMICT` via the preserved APIC fixmap window (`FIX_APIC_BASE`).
- When the deadline expires, the local APIC fires an interrupt, causing the CPU to exit `VMRUN` with `SVM_EXIT_INTR`.

**Assembly World-Switch Trampoline (`caretaker_vmenter.S`):**
- Wraps execution in Global Interrupt manipulation: `clgi` (Clear Global Interrupt Flag) is executed before restoring guest registers, and `stgi` (Set Global Interrupt Flag) is executed after saving exit state.
- Executes `VMRUN` using `csp->common.vmcb_pa`.
- Decodes AMD exit codes: `SVM_EXIT_IOIO` (UART 8250 emulation), `SVM_EXIT_HLT`, `SVM_EXIT_CPUID`, `SVM_EXIT_MSR`, and `SVM_EXIT_INVD`.

---

#### 3.5.3 ARM64 Engine (`arch/arm64/kvm/`)

The ARM64 engine manages guest execution at Exception Level 2 (EL2) across Stage-2 MMU translation, system registers, and GICv3 virtual interrupt interfaces.

```
+───────────────────────────────────────────────────────────────────────────────────+
|               ARM64 Preserved Context: struct caretaker_arm64_context              |
+──────────────────────────────────────┬────────────────────────────────────────────+
| Field Name                           | Architectural Description                  |
+──────────────────────────────────────┼────────────────────────────────────────────+
| struct kvm_cpu_context ctxt          | Guest EL1/EL0 system registers & GPRs      |
| struct kvm_vcpu_fault_info fault     | ESR_EL2, FAR_EL2, HPFAR_EL2, DISR_EL1      |
| u64 vttbr                            | Stage-2 Translation Table Base Register    |
| u64 hcr_el2                          | Hypervisor Configuration Register          |
| struct vgic_v3_cpu_if vgic_v3        | List Registers (LRs), ICH_VMCR, ICH_HCR    |
+──────────────────────────────────────┴────────────────────────────────────────────+
```

**Stage-2 MMU & Hyp Vector Integration:**
- The Stage-2 page table tree is preserved during staging via `kvm_pgtable_walk()` using `preserve_stage2_visitor()`, which preserves every intermediate table page with `kho_preserve_pages()`.
- System registers are loaded and saved using `arm64_caretaker_load_sysregs()` and `arm64_caretaker_save_sysregs()`:
  - Memory Management: `SCTLR_EL1`, `CPACR_EL1`, `TTBR0_EL1`, `TTBR1_EL1`, `TCR_EL1`, `MAIR_EL1`.
  - Exception State: `VBAR_EL1`, `ESR_EL1`, `FAR_EL1`, `ELR_EL1`, `SPSR_EL1`.
  - Thread Descriptors: `TPIDR_EL1`, `TPIDR_EL0`, `TPIDRRO_EL0`, `SP_EL1`.

**GICv3 Virtual CPU Interface & SGI Emulation:**
- Virtual interrupt injection state is preserved by saving and restoring the GICv3 List Registers (`ICH_LR<n>_EL2`), `ICH_HCR_EL2`, and `ICH_VMCR_EL2`.
- **Cross-vCPU SGI Trapping:** When the guest issues `MSR ICC_SGI1R_EL1, Xt` to signal another vCPU, Caretaker traps the write, extracts the target MPIDR and INTID, resolves the physical core via `arch_cpu_preserved_mpidr_to_cpu()`, and wakes the target core using `gicv3_caretaker_kick_cpu()`.
- **Redistributor Wakeup:** Interacts directly with `GICR_WAKER` to ensure the core redistributor is awake and not in power-down mode.

**Time-Slicing via EL2 Generic Timer:**
- Programs the EL2 Physical Timer (`CNTHP_TVAL_EL2` / `CNTHP_CTL_EL2`).
- When the virtual counter `CNTVCT_EL0` reaches the deadline, the timer raises an EL2 interrupt, breaking the guest out of execution into Caretaker.

---

#### 3.5.4 Multi-Architecture Virtualization Comparison

```
+──────────────────────────┬──────────────────────┬──────────────────────┬──────────────────────+
| Feature                  | Intel VMX (x86_64)   | AMD SVM (x86_64)     | ARM64 (aarch64)      |
+──────────────────────────┼──────────────────────┼──────────────────────┼──────────────────────+
| Virtualization Structure | VMCS (vmcs01.vmcs)   | VMCB (vmcb01.ptr)    | struct kvm_cpu_context|
| World-Switch Instruction | VMLAUNCH / VMRESUME  | VMRUN                | ERET (from EL2)      |
| Second-Level Paging      | EPT (Extended Pages) | NPT (Nested Pages)   | Stage-2 MMU (VTTBR)  |
| Hardware Root PA Field   | VMCS EPT Pointer     | VMCB cr3 (ncr3)      | VTTBR_EL2 (VMID+PA)  |
| Quantum Preemption       | VMX Preemption Timer | APIC TSC-Deadline    | EL2 Physical Timer   |
| Hardware Timer Type      | In-VMCS Hardware Cnt | Local APIC Register  | CNTHP_TVAL_EL2 Reg   |
| Host Page Table Register | HOST_CR3 in VMCS     | Host CR3 in VMCB     | TTBR1_EL1 register   |
| Cross-vCPU Kick          | APIC ICR NMI         | APIC ICR NMI         | GICv3 SGI / SEV      |
| Serial Console Emulation | 8250 UART (Port 3F8) | 8250 UART (Port 3F8) | PL011 UART (MMIO)    |
| Low-Power Idle Exits     | HLT / PAUSE exits    | HLT / PAUSE exits    | WFI / WFE exits      |
| Assembly Trampoline File | vmx/caretaker_vmenter| svm/caretaker_vmenter| kvm/caretaker_vmenter|
+──────────────────────────┴──────────────────────┴──────────────────────┴──────────────────────+
```

---

### 3.6 Layer 6: Userspace VMM & Testing Framework

Layer 6 encompasses the userspace virtualization management applications and the automated verification suite.

---

#### 3.6.1 NanoVMM (`nanovmm/`)

NanoVMM is a lightweight, zero-dependency Virtual Machine Monitor engineered specifically for live update validation, minimal footprint overhead, and precise CPU preservation control.

```
+───────────────────────────────────────────────────────────────────────────────────+
|                      NanoVMM Preservation Strategies                              |
+──────────────────────────┬──────────────┬─────────────────────────────────────────+
| Strategy                 | CLI Flag     | Execution Behavior Across Live Update   |
+──────────────────────────┼──────────────┼─────────────────────────────────────────+
| Symmetric On-Core        | -S cpu1-2    | All guest vCPUs pinned 1:1 to preserved |
|                          |              | physical cores. Uninterrupted execution.|
+──────────────────────────┼──────────────┼─────────────────────────────────────────+
| Asymmetric On-Core       | -S cpu1      | Selected vCPUs run on-core; unpreserved |
|                          |              | vCPUs pause and resume after kexec.     |
+──────────────────────────┼──────────────┼─────────────────────────────────────────+
| Memory-Only Fallback     | -S           | All vCPUs pause. Guest RAM preserved    |
| (RAM Preservation Only)  |              | via LUO memfd; re-executed in next OS.  |
+──────────────────────────┴──────────────┴─────────────────────────────────────────+
```

**Operational Workflow:**
1. **Boot:** Allocates guest memory using HugeTLB pages (`MAP_HUGETLB`), loads the guest kernel and initramfs into guest physical RAM, and sets up vCPU registers.
2. **LUO Session Registration:** Opens `/dev/liveupdate` and creates a named session (e.g. `nanovmm_<pid>`).
3. **CPU Preservation Acquisition:** For each preserved core specified via `-S`, opens `/sys/devices/system/cpu/cpu<N>/preserve` and binds the descriptor into the LUO session.
4. **Live Update Handover:** Upon receiving `SIGUSR1` or live update trigger, calls `ioctl(luo_fd, LIVEUPDATE_SESSION_PRESERVE)`.
5. **Incoming Reattachment:** The newly booted NanoVMM process opens the LUO session via `ioctl(luo_fd, LIVEUPDATE_SESSION_RETRIEVE)`, retrieves preserved file descriptors, maps guest memory, reattaches to running vCPUs, and resumes management.

---

#### 3.6.2 Unified Test Framework & 8-Configuration Matrix (`scripts/ovm_test`)

To validate that physical CPU preservation and Caretaker on-core execution function reliably across all topologies and architectures, the project includes `scripts/ovm_test`:

```
+───────────────────────────────────────────────────────────────────────────────────+
|               OrphanVM 8-Configuration Test Matrix (scripts/ovm_test)             |
+────┬───────────┬──────────────┬──────────────┬────────────────────────────────────+
| ID | Host CPUs | Guest vCPUs  | Preservation | Test Classification                |
+────┼───────────┼──────────────┼──────────────┼────────────────────────────────────+
| 1  | 2 Cores   | 2 vCPUs      | -S cpu1      | Symmetric On-Core (2-CPU Topology) |
| 2  | 2 Cores   | 2 vCPUs      | -S cpu1      | Asymmetric Sub-core Execution      |
| 3  | 2 Cores   | 2 vCPUs      | -S (mem only)| Memory-Only RAM Fallback Baseline  |
| 4  | 4 Cores   | 4 vCPUs      | -S cpu1-3    | Symmetric On-Core (4-CPU Topology) |
| 5  | 4 Cores   | 2 vCPUs      | -S cpu1      | Asymmetric 2-on-4 Core Allocation  |
| 6  | 4 Cores   | 4 vCPUs      | -S (mem only)| Memory-Only RAM Fallback Baseline  |
| 7  | 8 Cores   | 8 vCPUs      | -S cpu1-7    | Symmetric Multi-Core Scaling (8-CPU|
| 8  | 4 Cores   | 2 vCPUs      | Loop Cancel  | Intra-Kernel Staging / Abort Stress|
+────┴───────────┴──────────────┴──────────────┴────────────────────────────────────+
```

**Automated Verification Checks:**
- **Uninterrupted Guest Progress:** Monitors monotonic guest clock ticks (`jiffies` / `rdtsc`) and serial console heartbeats, asserting that the guest never experienced blackout or stall during the host kexec gap.
- **CPU Isolation Verification:** Inspects incoming host kernel logs to ensure preserved cores were excluded from secondary boot bringup (`cpuhp_bringup_mask`).
- **Clean Handshake Verification:** Asserts that `attachment_state` transitions through `DETACHED` $\to$ `ATTACHING` $\to$ `ATTACHED` without timing out.
- **Cross-Platform Verification:** The test runner executes uniformly across Intel VMX (`-t intel`), AMD SVM (`-t amd`), and ARM64 (`-t arm`).

---

## 4. Key Architectural Decisions & Rationale

Building a production-grade live update hypervisor that maintains active physical CPU execution across host kernel reboots requires making foundational trade-offs across security, performance, maintainability, and Linux kernel community standards. This section documents the key architectural choices made in OrphanVM and their underlying rationale.

---

### 4.1 In-Kernel Caretaker vs. Standalone Bare-Metal ELF

A fundamental early architectural question was whether the Caretaker execution loop should be implemented as an **external bare-metal ELF binary** (loaded into physical RAM and jumped to during kexec) or as an **in-kernel module integrated directly into Linux and KVM**.

```
+─────────────────────────────────┬──────────────────────────────────┬──────────────────────────────────+
| Architectural Criteria          | Standalone External ELF Binary   | In-Kernel Caretaker Subsystem    |
+─────────────────────────────────┼──────────────────────────────────┼──────────────────────────────────+
| Privilege & Security Model      | High Risk: Userspace supplies an | Minimal Risk: Executable code is |
|                                 | unverified Ring 0 / EL2 binary   | part of the signed host kernel   |
|                                 | to execute on bare silicon.      | image; subject to Lockdown/IMA.  |
+─────────────────────────────────┼──────────────────────────────────┼──────────────────────────────────+
| ABI Stability & Drift           | Fragile: External binary must    | Stable: Direct C struct sharing  |
|                                 | track internal VMCS/VMCB offsets | within the same kernel build; no |
|                                 | across different kernel versions.| ABI translation layer needed.    |
+─────────────────────────────────┼──────────────────────────────────┼──────────────────────────────────+
| Memory & Hardware Management    | Complex: Must implement its own  | Simple: Reuses Linux kernel MMU, |
|                                 | page tables, APIC/GIC drivers,   | KVM VMCS/VMCB setup routines, and|
|                                 | and early UART drivers from raw. | transition page table subsystem. |
+─────────────────────────────────┼──────────────────────────────────┼──────────────────────────────────+
| Kernel Upstreaming Feasibility  | Low: Upstream maintainers reject | High: Supported by KVM and x86   |
|                                 | external micro-kernels running   | maintainers (Paolo Bonzini, Alex |
|                                 | outside the Linux codebase.      | Graf) as native KVM feature.     |
+─────────────────────────────────┴──────────────────────────────────┴──────────────────────────────────+
```

**Architectural Rationale:**
1. **Security & Signature Integrity:** In enterprise environments enforcing UEFI Secure Boot and Linux Kernel Lockdown, running arbitrary code at Ring 0 / EL2 without cryptographic signature validation is prohibited. By embedding Caretaker within the kernel (`.text.cpu_preserved`), all execution code is verified at kernel boot time using existing kernel image verification pipelines.
2. **Zero ABI Maintenance Burden:** Hardware virtualization structures (Intel VMCS, AMD VMCB, ARM64 Hyp context) change frequently across processor generations and kernel revisions. An external binary would suffer from constant ABI bit-rot. In-kernel compilation ensures that `struct kvm_vcpu`, `vmx_ops`, and `svm_ops` remain strictly synchronized at compile time.
3. **Upstream Alignment:** In-kernel integration follows the precedent set by KHO and Live Update Orchestrator (LUO), treating physical CPU preservation as a native Linux capability.

---

### 4.2 Per-Session Page Tables (Option B) vs. Global Shared PGD

Earlier drafts considered **Option A: A Single Global Shared PGD** containing all Caretaker code, data, and memory mappings for all concurrent virtual machines. OrphanVM rejected Option A in favor of **Option B: Per-Session Private Page Tables**.

```
+───────────────────────────────────────────────────────────────────────────────────+
|                  Comparison: Global PGD (Option A) vs Option B                     |
+──────────────────────────────────┬───────────────────────┬────────────────────────+
| Dimension                        | Option A (Global PGD) | Option B (Per-Session) |
+──────────────────────────────────┼───────────────────────┼────────────────────────+
| Multi-Tenant Memory Isolation    | NONE (Shared VAS)     | HARDWARE ENFORCED (ASI)|
| Side-Channel Vulnerability (L1TF)| High Risk             | Mitigated via CR3/TTBR |
| Session Teardown Independence    | Complex (Global TLB)  | Clean (Per-session PGD)|
| Bounded Intermediate Memory      | Unbounded Radix Walk  | Bounded: 128 Pages Max |
| Radical Lifecycle Decoupling     | Partial               | Complete               |
+──────────────────────────────────┴───────────────────────┴────────────────────────+
```

**Architectural Rationale:**
1. **Hardware-Enforced Address Space Isolation (ASI):** In cloud environments, multiple untrusted VMs run on the same physical server. Under Option A, Tenant A and Tenant B share a single host virtual address space during the live update gap. A speculative execution vulnerability (e.g. Spectre, Meltdown, or L1 Terminal Fault) or an unhandled exception in Caretaker could allow Tenant A to read Tenant B's guest memory. Under Option B, each session has a separate root PGD (`sess->pgd_pa`). The MMU hardware strictly prevents Tenant A from addressing Tenant B's memory.
2. **Independent Failure Domains & Teardown:** If userspace cancels Session 1 or if Session 1's vCPU crashes, Session 1's page table pages (`sess->pgd_pages[]`) can be freed immediately via `kho_unpreserve_free()` without acquiring locks across Session 2, and without issuing cross-core shootdown IPIs that would interrupt Session 2's execution.
3. **Elimination of Pre-Kexec Radix Tree Walks:** By bounding intermediate page table pages per session (`CARETAKER_MAX_PGD_PAGES = 128` = 512KB maximum) and immediately preserving each page upon allocation (`arm64_caretaker_alloc_page()`), OrphanVM eliminates the need to traverse and preserve page tables at live update trigger time. Staging latency is reduced to $\mathcal{O}(1)$.

---

### 4.3 Transition Page Tables (`trans_pgd`) Architecture Unification

Historically, ARM64 included an internal page table cloning mechanism called `trans_pgd` used exclusively for kernel hibernation and kexec transitions. Meanwhile, x86 maintained several ad-hoc, separate page table copy mechanisms scattered across architecture code.

**The Decision:**
OrphanVM extracted and generalized `trans_pgd` into a cross-architecture memory management subsystem (`asm/trans_pgd.h`, `arch/x86/mm/trans_pgd.c`, `arch/arm64/mm/trans_pgd.c`).

**Architectural Rationale:**
1. **Allocator Independence:** Standard Linux page table routines (`pgd_alloc()`, `pud_alloc()`) depend heavily on the kernel buddy allocator (`alloc_pages()`) and slab subsystem (`kmalloc()`). During live update staging and kexec handover, the page allocator cannot be invoked dynamically without risking lock deadlocks. `trans_pgd` abstracts allocation via `struct trans_pgd_info`, allowing callers to supply pre-allocated or KHO-preserved pages.
2. **Linear Range Mapping (`trans_pgd_map_range`):** Both architectures need to map specific physical contiguous chunks (Caretaker text, Caretaker data, stacks, APIC fixmaps) into transitional address spaces. Unifying `trans_pgd_map_range()` eliminated hundreds of lines of duplicate arch-specific page table building logic.
3. **Robust Cache Coherency:** `trans_pgd` includes built-in data cache maintenance (`dcache_clean_poc`), ensuring that page table translations constructed by CPU 0 are instantly coherent and visible to secondary preserved cores without memory barrier bugs.

---

### 4.4 Stack-Embedded Context Retrieval vs. Global Per-CPU Arrays

In normal Linux kernel operation, per-CPU data is accessed using architecture-specific segment register offsets: `%gs:this_cpu_off` on x86, or `TPIDR_EL1` on ARM64. During kexec handover, the incoming kernel wipes out and relocates the host per-CPU data segment while preserved cores are actively running in Caretaker.

```
+───────────────────────────────────────────────────────────────────────────────────+
|               Evaluation of Per-Core Context Retrieval Strategies                 |
+──────────────────────────────────┬───────────────────────┬────────────────────────+
| Strategy                         | Latency / Complexity  | Resilience Across Kexec|
+──────────────────────────────────┼───────────────────────┼────────────────────────+
| Host Per-CPU Pointers (%gs/TPIDR)| Fast (1 cycle)        | FATAL: Overwritten by  |
|                                  |                       | incoming kernel boot   |
+──────────────────────────────────┼───────────────────────┼────────────────────────+
| Global Array Indexed by APIC ID  | Slow: Requires APIC ID| Fragile: APIC IDs can  |
|                                  | register MMIO reads   | be sparse or > NR_CPUS |
+──────────────────────────────────┼───────────────────────┼────────────────────────+
| Stack-Embedded Context (Selected)| Instantaneous O(1):   | 100% Resilient: Stack  |
|                                  | sp & ~(STACK_SIZE - 1)| is dedicated & private |
+──────────────────────────────────┴───────────────────────┴────────────────────────+
```

**Architectural Rationale:**
1. **$\mathcal{O}(1)$ Lockless Access:** By allocating dedicated execution stacks with natural power-of-two alignment (`4KB` on x86, `8KB` on ARM64) and embedding `struct cpu_preserved_stack_context` at the stack base, any Caretaker routine or interrupt handler can find its owning CPU ID, session pointer, and root PGD in a single CPU instruction:
   ```c
   sctx = (struct cpu_preserved_stack_context *)(sp & ~(CPU_PRESERVED_STACK_SIZE - 1));
   ```
2. **Zero Hardware Register Dependencies:** Does not require dedicating or preserving a segment register or system register (`%gs`, `%fs`, `TPIDR_EL1`, `TPIDR_EL2`), avoiding conflicts with guest register states.
3. **Immunity to Memory Relocation:** Because the context lives inside the core's private stack folio, changes to global kernel memory descriptors, page tables, or SMP structures in the host OS have zero impact on the preserved core.

---

### 4.5 Dedicated-Core Fast-Path in Caretaker Scheduler

Cloud virtualization hosts two distinct classes of virtual machine deployments:
1. **Overcommitted Multi-Tenant Instances:** Where $M \text{ vCPUs} > N \text{ physical cores}$, requiring time-sliced round-robin scheduling.
2. **Dedicated Pinned Workloads:** Where $M = N$, and each guest vCPU is pinned 1:1 to a dedicated physical CPU core.

**The Decision:**
Implement a dedicated-core lock-free bypass in `caretaker_session_run_slice()`:

```c
if (rq->nr_runnable == 0 && current_job->state == CARETAKER_JOB_RUNNING) {
    /* Bypass runqueue spinlocks, list removals, and re-enqueueing */
    deadline_ticks = arch_caretaker_read_counter() + sched_config->quantum_ticks;
    reason = current_job->run_fn(current_job->data, deadline_ticks);
    continue;
}
```

**Architectural Rationale:**
- In a naive round-robin implementation, every VM-exit (e.g. UART early printk or preemption timer tick) requires acquiring the runqueue atomic spinlock (`atomic_cmpxchg`), updating queue pointers, and releasing the lock.
- On multi-socket NUMA systems with tens of cores, runqueue lock contention degrades guest throughput.
- The dedicated-core fast-path detects when no other jobs are waiting. It **completely bypasses all atomic operations and linked-list operations**, refreshing the deadline timer and immediately re-entering the guest.
- This design achieves near-zero virtualization overhead, matching the performance of standard bare-metal KVM.

---

## 5. End-to-End Lifecycle & State Machine

The end-to-end execution lifecycle of an OrphanVM workload progresses through four well-defined architectural phases. Across these phases, hardware resources transition through strict state machines to guarantee continuous guest progress, zero memory loss, and complete failure resilience.

---

### 5.1 The Four Execution Phases

```
+───────────────────────────────────────────────────────────────────────────────────+
|                          Phase 1: Normal VM Execution                             |
|  - Userspace VMM (NanoVMM/QEMU) owns VM and vCPU file descriptors.                |
|  - Host KVM executes guest slices via standard KVM_RUN ioctl.                     |
|  - Caretaker Control Block attachment_state == CARETAKER_KVM_ATTACHED.            |
|  - Host Linux scheduler CFS/RT manages CPU allocations; MMU uses init_mm.         |
+───────────────────────────────────────────────────────────────────────────────────+
                                          │
                                          │ Trigger Live Update (SIGUSR1 / LUO Preserve)
                                          ▼
+───────────────────────────────────────────────────────────────────────────────────+
|                     Phase 2: Live Update Staging & CPU Shielding                  |
|  1. Userspace opens /dev/liveupdate and creates a named LUO session.              |
|  2. Preserves guest RAM folios and Stage-2 / EPT page tables via KHO.             |
|  3. Userspace opens /sys/devices/system/cpu/cpu<N>/preserve for target pCPUs.     |
|  4. CPU hotplug offlines target cores; cpuhp_ap_report_dead() intercepts cores.  |
|  5. Cores switch to dedicated stacks and private session PGD (sess->pgd_pa).      |
|  6. Caretaker registers jobs; attachment_state transitions to DETACHED.           |
|  7. KHO writes minimal FLB header (cpu_preserved_global_ser).                     |
+───────────────────────────────────────────────────────────────────────────────────+
                                          │
                                          │ kexec -e (Host Reboot Execution)
                                          ▼
+───────────────────────────────────────────────────────────────────────────────────+
|               Phase 3: Host Kexec Transition (The Management Gap)                 |
|  - Outgoing kernel tears down on CPU 0; smp_send_stop() SKIPS preserved cores.   |
|  - CPU 0 jumps into incoming kernel entry point.                                  |
|  - Preserved cores run autonomously in Caretaker:                                 |
|      * Hardware VM-Entry into guest vCPU.                                         |
|      * Trivial exits (8250/PL011 console, HLT/WFI idle, timers) handled on-core.  |
|      * Zero execution pause or blackout experienced by guest OS.                  |
|  - Incoming kernel boots on CPU 0:                                                |
|      * kho_init() extracts cpu_preserved_mask from "cpu_flb_v1".                  |
|      * cpuhp_bringup_mask() SKIPS preserved cores; no INIT/SIPI reset sent.       |
|  - Incoming userland boots; systemd services start up.                            |
+───────────────────────────────────────────────────────────────────────────────────+
                                          │
                                          │ New VMM Launches & Retrieves Session
                                          ▼
+───────────────────────────────────────────────────────────────────────────────────+
|                     Phase 4: Incoming Adoption & Handover                         |
|  1. Incoming VMM opens LUO session via LIVEUPDATE_SESSION_RETRIEVE_FD.            |
|  2. VMM re-maps preserved guest RAM into its address space.                       |
|  3. Host KVM driver sets attachment_state = CARETAKER_KVM_ATTACHING.              |
|  4. KVM sends APIC NMI / GICv3 SGI kick to preserved core.                        |
|  5. Caretaker detects attach signal, executes ops->sync_vcpu(), and saves state.  |
|  6. Caretaker sets attachment_state = CARETAKER_KVM_ATTACHED and enters park loop.|
|  7. Host kernel onlines core via cpu_up() and resumes normal KVM_RUN execution.  |
+───────────────────────────────────────────────────────────────────────────────────+
```

---

### 5.2 Physical CPU Preservation State Machine

Each physical CPU core designated for preservation traverses the following architectural state machine:

```
                  ┌───────────────────────────────┐
                  │            ONLINE             │ (Normal Host Scheduling)
                  └───────────────────────────────┘
                                  │
                                  │ Userspace preserves CPU fd via LUO
                                  ▼
                  ┌───────────────────────────────┐
                  │       HOTPLUG_OFFLINING       │ (Tasks & IRQs evacuated)
                  └───────────────────────────────┘
                                  │
                                  │ cpuhp_ap_report_dead() intercept
                                  ▼
                  ┌───────────────────────────────┐
                  │       PRESERVED_PARKED        │ (Dedicated stack loaded,
                  └───────────────────────────────┘  GDT/IDT loaded, loops in park)
                                  │
                                  │ Caretaker submits & activates job
                                  ▼
                  ┌───────────────────────────────┐
                  │     CARETAKER_JOB_RUNNING     │ <───┐ (Fast-path quantum
                  └───────────────────────────────┘     │  refresh / slice re-entry)
                     │                         │        │
      Quantum Expiry │                         │ Attach │
      (Multi-job)    ▼                         │ Signal │
        [ Context Switch / Next Job ]          ▼        │
                     │                 ┌───────────────┴───────────────┐
                     └──────────────── │     ATTACHING_SYNC_STATE      │
                                       └───────────────────────────────┘
                                                       │
                                                       │ ops->sync_vcpu() executed;
                                                       │ cb->attachment_state = ATTACHED
                                                       ▼
                                       ┌───────────────────────────────┐
                                       │       PRESERVED_PARKED        │
                                       └───────────────────────────────┘
                                                       │
                                                       │ Incoming kernel calls cpu_up()
                                                       ▼
                                       ┌───────────────────────────────┐
                                       │            OFFLINE            │ (Exits park loop)
                                       └───────────────────────────────┘
                                                       │
                                                       │ Secondary SMP bringup
                                                       ▼
                                       ┌───────────────────────────────┐
                                       │            ONLINE             │ (Rejoined host OS)
                                       └───────────────────────────────┘
```

```
+───────────────────────────────────────────────────────────────────────────────────+
|                  Physical CPU State Transition Reference Table                    |
+───────────────────────┬─────────────────────────────┬─────────────────────────────+
| Current State         | Event / Trigger             | Next State & Action Taken   |
+───────────────────────┼─────────────────────────────┼─────────────────────────────+
| ONLINE                | LUO preserve CPU fd         | HOTPLUG_OFFLINING: Evacuate |
|                       |                             | host tasks, stop sched tick |
+───────────────────────┼─────────────────────────────┼─────────────────────────────+
| HOTPLUG_OFFLINING     | cpuhp_ap_report_dead()      | PRESERVED_PARKED: Load stack|
|                       |                             | switch PGD, enter park loop |
+───────────────────────┼─────────────────────────────┼─────────────────────────────+
| PRESERVED_PARKED      | Job activation              | CARETAKER_JOB_RUNNING: Run  |
|                       |                             | guest slice via vmenter.S   |
+───────────────────────┼─────────────────────────────┼─────────────────────────────+
| CARETAKER_JOB_RUNNING | Preemption timer expires    | CARETAKER_JOB_RUNNING: Fast |
|                       | (Single job on core)        | path refresh; continue run  |
+───────────────────────┼─────────────────────────────┼─────────────────────────────+
| CARETAKER_JOB_RUNNING | Preemption timer expires    | CARETAKER_JOB_RUNNABLE: Pop |
|                       | (Multiple jobs on core)     | next job; context switch    |
+───────────────────────┼─────────────────────────────┼─────────────────────────────+
| CARETAKER_JOB_RUNNING | KVM signals ATTACHING       | ATTACHING_SYNC_STATE: Save  |
|                       | (Incoming kernel handshake) | guest registers to vCPU page|
+───────────────────────┼─────────────────────────────┼─────────────────────────────+
| ATTACHING_SYNC_STATE  | Sync complete               | PRESERVED_PARKED: Drop into |
|                       |                             | architecture wait (cpu_relax|
+───────────────────────┼─────────────────────────────┼─────────────────────────────+
| PRESERVED_PARKED      | Incoming VMM unpreserves CPU| OFFLINE: Exit park loop;    |
|                       |                             | ready for cpu_up() bringup  |
+───────────────────────┼─────────────────────────────┼─────────────────────────────+
| OFFLINE               | cpu_up(target_cpu)          | ONLINE: Re-enter Linux host |
|                       |                             | scheduler and join SMP pool |
+───────────────────────┴─────────────────────────────┴─────────────────────────────+
```

---

### 5.3 Abort & Cancellation State Machine

If a live update transition is aborted by the operator or fails due to staging errors, OrphanVM guarantees safe and deterministic rollback:

```
[ Active Staging / Caretaker Execution ]
                   │
                   │ ioctl(luo_fd, LIVEUPDATE_SESSION_CANCEL) or Abort Triggered
                   ▼
[ Step 1: Signal Job Cancellation ]
   - Caretaker sets job->state = CARETAKER_JOB_CANCELING.
   - arch_cpu_preserved_kick() unicasts NMI / SGI to each preserved core.
   - Wait up to CARETAKER_CANCEL_TIMEOUT_US (20 seconds) for cores to exit slices.
                   │
                   ▼
[ Step 2: Transition Cores to Parked State ]
   - Cores complete current VM-exit or preemption event.
   - Cores transition to CARETAKER_JOB_DEAD and park in cpu_preserved_park().
                   │
                   ▼
[ Step 3: Unwind Page Tables & Folio Reservations ]
   - MMU on each core reloads original host PGD (init_mm.pgd).
   - Session page table pages released via kho_unpreserve_free().
   - Workload folios unpreserved; return to standard buddy allocator management.
                   │
                   ▼
[ Step 4: Re-online Physical Cores ]
   - Host kernel invokes cpu_up() for each unpreserved core.
   - Cores exit parking loop, execute arch_cpu_preserved_park_finish(), and rejoin
     the host Linux scheduler.
   - Full host SMP topology restored with zero orphaned memory or lost hardware.
```

---

## 6. Challenges, Limitations & Bare-Metal Readiness

While the OrphanVM prototype is thoroughly verified on QEMU and Simics virtualized simulation platforms across Intel VMX, AMD SVM, and ARM64 architectures, deploying physical CPU preservation on real bare-metal server hardware introduces hardware-level synchronization, power management, firmware, and bus challenges. This section details these hardware challenges and provides concrete mitigation architectures.

---

### 6.1 Bare-Metal Hardware Challenges (Beyond Virtualized Testbeds)

```
+───────────────────────────────────────────────────────────────────────────────────+
|               Bare-Metal Hardware Physical Risks & Mitigation Matrix              |
+──────────────────────────┬─────────────────────────────┬──────────────────────────+
| Hardware Phenomenon      | Bare-Metal Failure Mode     | Architectural Mitigation |
+──────────────────────────┼─────────────────────────────┼──────────────────────────+
| Non-Contiguous APIC IDs  | NMI/IPI kicks hit wrong core| Serialize ACPI MADT map  |
|                          | or drop; attach handshake   | in cpu_preserved_global; |
|                          | times out.                  | no apicid = cpu fallback |
+──────────────────────────┼─────────────────────────────┼──────────────────────────+
| System Management Mode   | Firmware broadcast SMI fails| Maintain SMI responsive  |
| (SMM / SMI Rendezvous)   | cross-core rendezvous timeout; loops (cpu_relax/PAUSE);|
|                          | triggers hardware CATERR.   | check firmware tolerance |
+──────────────────────────┼─────────────────────────────┼──────────────────────────+
| Physical IOMMU & DMA     | In-flight PCIe DMA during   | IOMMU context handover via|
| (VFIO / SR-IOV Devices)  | kexec hits reset IOMMU root;| KHO, or device DMA queue |
|                          | triggers AER / PCI MCEs.    | quiescence before kexec. |
+──────────────────────────┼─────────────────────────────┼──────────────────────────+
| Dynamic ACPI/DT B次元    | Arbitrary MMIO redistributor| Dynamic platform parser; |
| Addressing (ARM64 GIC)   | & UART addresses fail if    | trans_pgd_map_range for  |
|                          | hardcoded to QEMU defaults. | actual physical addresses|
+──────────────────────────┼─────────────────────────────┼──────────────────────────+
| Package C-State Power    | Core sleep promotes into    | Assert PM QoS latency;   |
| Gating (PCU / C6 / C7)   | Package C6/C7, gating L3    | clamp package C-states to|
|                          | cache & memory interconnect.| C1/C1E during staging.   |
+──────────────────────────┼─────────────────────────────┼──────────────────────────+
| MTRRs & Cache Attributes | Outgoing kernel destroys    | Snapshot and restore     |
| (Memory Type Registers)  | MTRR state; guest runs in   | physical MTRRs in        |
|                          | uncached (UC) slow memory.  | caretaker_x86_page.      |
+──────────────────────────┴─────────────────────────────┴──────────────────────────+
```

---

#### 6.1.1 Non-Contiguous Physical APIC IDs & ACPI MADT Parsing

On multi-socket NUMA servers with Simultaneous Multithreading (SMT), physical APIC IDs are rarely sequential or identical to logical Linux CPU IDs. For instance, on dual-socket AMD EPYC or Intel Xeon systems:
- CPU 0 might have APIC ID `0x0000` (Socket 0, Core 0, Thread 0).
- CPU 1 might have APIC ID `0x0020` (Socket 1, Core 0, Thread 0).
- CPU 2 might have APIC ID `0x0001` (Socket 0, Core 0, Thread 1).

**The Risk:**
If architecture kick routines (`arch_cpu_preserved_kick`) fall back to `apicid = cpu` when the kernel table `cpuid_to_apicid` is uninitialized during early boot, NMI or IPI kicks will be dispatched to the wrong physical silicon or non-existent APIC targets. The incoming kernel will fail to signal the preserved core, causing reattachment timeouts.

**Mitigation Architecture:**
1. During outgoing live update staging, capture the complete `cpuid_to_apicid[NR_CPUS]` mapping directly from the kernel ACPI MADT parser.
2. Serialize this mapping into the First-Level Boot structure (`struct cpu_preserved_global_ser`).
3. In incoming early boot (`arch_cpu_preserved_early_init`), initialize the APIC ID translation table immediately upon reading the FLB, ensuring that all subsequent physical APIC ICR writes target the precise hardware cores.

---

#### 6.1.2 System Management Mode (SMM) & SMI Rendezvous Deadlocks

On enterprise x86 platforms, server firmware (BIOS/UEFI) executes out-of-band in System Management Mode (SMM / Ring -2). Firmware uses periodic broadcast System Management Interrupts (SMIs) for critical hardware management:
- Thermal monitoring and active fan speed control.
- Memory controller ECC correctable error threshold logging.
- Voltage regulator telemetry and chassis intrusion detection.

**The Risk:**
Server firmware implementations enforce a **synchronous SMI rendezvous**: when an SMI is asserted, all physical CPU cores across all sockets must halt current execution, save processor state into SMRAM, and enter SMM within a bounded hardware timeout (typically 2 to 5 milliseconds).
If a preserved CPU core is executing in VMX non-root operation or AMD SVM non-root operation with SMIs masked or blocked, or executing in a tight uninterruptible assembly loop:
- The core fails to enter SMM before the rendezvous timer expires.
- The platform chipset detects an unresponsive processor.
- The motherboard hardware watchdog triggers an unmaskable Catastrophic Error (**CATERR**) or forces an instant hardware platform reboot.

**Mitigation Architecture:**
1. **Unblocked SMI Intercepts:** Ensure VMCS execution controls do not block SMIs. In Intel VMX, bit 2 of Pin-Based VM-Execution Controls (`NMI_EXITING`) must not inadvertently interfere with SMI dispatch.
2. **Pipeline Relaxation in Wait Loops:** In all Caretaker idle, parking, and spin loops (`arch_cpu_preserved_park_wait`), execute `cpu_relax()` (`PAUSE` on x86, `wfe` on ARM64) with memory barriers. This instruction pipeline relaxation allows hardware to acknowledge pending external SMIs and transition the core into SMRAM cleanly.

---

#### 6.1.3 Physical IOMMU & DMA Passthrough Quiescence

In enterprise clouds, high-performance database and networking VMs use direct device assignment (VFIO / PCIe SR-IOV) to achieve near-wireline networking (100GbE / 200GbE SmartNICs) and GPU acceleration.

**The Risk:**
Unlike emulated virtio devices that can be paused in software, assigned PCIe physical devices continue executing Direct Memory Access (DMA) transactions autonomously into host physical RAM.
When the host Linux kernel executes `kexec -e`:
1. The incoming kernel reboots and initializes the platform IOMMU driver (Intel VT-d `DMAR` / AMD-Vi `IVRS`).
2. Standard IOMMU initialization resets hardware translation units, clears root table pointers, and invalidates IOMMU Context Caching and IOTLB.
3. In-flight DMA packets issued by the physical NIC hit an unmapped or uninitialized IOMMU translation unit.
4. The PCIe Root Complex receives an unmapped DMA transaction, signaling an **Advanced Error Reporting (AER) Uncorrectable Error** or asserting an NMI / Machine Check Exception (MCE), immediately crashing the entire host server.

**Mitigation Architecture:**
Two distinct architectural approaches resolve this bare-metal challenge:
- **Approach A (Hardware IOMMU Handover via KHO):** Extend KHO to preserve IOMMU page table roots, device context tables, and interrupt remapping tables across kexec. The incoming kernel detects preserved IOMMU contexts and skips hardware reset, maintaining active DMA translation uninterrupted across host reboot.
- **Approach B (Software DMA Quiescence Protocol):** Prior to initiating live update staging, the host hypervisor issues a paravirtualized quiescence signal to the guest kernel (or uses PCIe Function Level Reset / FLR on endpoint queues). The guest driver pauses ring buffer transmission for the duration of the 100ms kexec gap, resuming queue processing after reattachment.

---

#### 6.1.4 Dynamic ACPI/Device-Tree Topologies on ARM64

In virtualized QEMU `virt` environments, the GICv3 distributor is placed at `0x08000000`, the redistributor region at `0x080a0000`, and the PL011 UART at `0x09000000`.

**The Risk:**
On bare-metal ARM64 server platforms (such as Neoverse V2, AWS Graviton, or Neoverse N2 platforms), the GICv3 redistributor regions are placed at arbitrary, non-standard 64-bit physical addresses defined exclusively in ACPI MADT (`GICR` structures) or Device Tree (`interrupt-controller` nodes). Hardcoding memory addresses results in immediate bus faults and hypervisor panics.

**Mitigation Architecture:**
1. During LUO session setup, query the active platform irqchip driver (`gic_data.dist_phys_base` and `gic_data.redist_regions`).
2. Store the physical redistributor base addresses, stride distances, and discovery mechanisms in `struct caretaker_arm64_context`.
3. Use `trans_pgd_map_range()` to map these dynamically resolved physical addresses into the session's private `trans_pgd` at staging time, ensuring full portability across any physical ARM64 motherboard.

---

#### 6.1.5 Package C-State Power Gating & Memory Interconnect Throttling

Modern server processors feature aggressive autonomous Power Control Units (PCUs) that manage package-level C-states (e.g. Package C6/C7).

**The Risk:**
When multiple cores enter idle states (`HLT` / `MWAIT` / `WFI`), the PCU can promote individual core sleep into a deep package power-down. In deep package sleep:
- The shared Last-Level Cache (L3 / LLC) may be partially power-gated or flushed.
- Multi-socket interconnect links (Intel Ultra Path Interconnect - UPI, or AMD Infinity Fabric) transition into low-power link states (e.g. L1 sleep).
- Memory controllers enter self-refresh modes.
If preserved CPU 1 is executing guest instructions while CPU 0 is rebooting, a package power-state transition can induce multi-millisecond memory latency spikes or throttle interconnect bandwidth, violating guest SLA guarantees.

**Mitigation Architecture:**
1. During live update staging, the Caretaker subsystem asserts a Power Management Quality of Service constraint:
   ```c
   cpu_latency_qos_add_request(&caretaker_pm_qos, 0);
   ```
2. On x86 platforms, Caretaker clamps maximum package C-states by configuring power management MSRs (e.g. `MSR_PKG_CST_CONFIG_CONTROL`) to restrict sleep states to C1/C1E, keeping memory controllers, UPI links, and L3 caches at full operating power throughout the reboot gap.

---

### 6.2 Software Limitations & Future Roadmap

To achieve full enterprise deployment, the following software enhancements represent the immediate project roadmap:

```
+───────────────────────────────────────────────────────────────────────────────────+
|                          OrphanVM Future Engineering Roadmap                      |
+──────────────────────────────────┬────────────────────────────────────────────────+
| Roadmap Feature                  | Description & Engineering Objective            |
+──────────────────────────────────┼────────────────────────────────────────────────+
| Dynamic Page Table Expansion     | Remove the 128-page limit (CARETAKER_MAX_PGD)  |
|                                  | via on-demand KHO folio chunk allocation.      |
+──────────────────────────────────┼────────────────────────────────────────────────+
| In-Gap Virtual APIC IPI Routing  | Emulate x86 guest x2APIC ICR writes on-core;   |
|                                  | route virtual IPIs to physical ICR NMIs.       |
+──────────────────────────────────┼────────────────────────────────────────────────+
| In-Memory Console Ring Buffer    | Capture all guest serial console printk writes |
|                                  | during the gap into a 64KB preserved circular  |
|                                  | buffer for post-boot debug inspection.         |
+──────────────────────────────────┼────────────────────────────────────────────────+
| Live Caretaker Engine Upgrades   | Support upgrading Caretaker execution text     |
|                                  | itself across consecutive kernel releases      |
|                                  | while guest vCPUs remain actively executing.   |
+──────────────────────────────────┴────────────────────────────────────────────────+
```

1. **Dynamic Page Table Expansion:**
   Currently, session page tables allocate up to `CARETAKER_MAX_PGD_PAGES = 128` pages (512KB). While sufficient for VMs with standard memory configurations, multi-terabyte guest instances with heavily fragmented physical memory pages may require deeper intermediate page tables. Future revisions will allocate transitional page table folios dynamically as linked page-blocks.
2. **In-Gap Cross-vCPU Virtual IPI Routing:**
   In the current prototype, cross-vCPU IPIs on ARM64 are routed via GICv3 SGIs, whereas on x86, guest APIC ICR writes during the gap are absorbed or wait for preemption slices. The next milestone will add complete virtual x2APIC ICR emulation on x86, mapping guest target vCPU IDs directly to physical APIC IDs and issuing physical ICR NMIs across cores during the gap.
3. **In-Memory Guest Console Ring Buffer:**
   Early console printk output written by the guest kernel during the live update gap is currently absorbed by Layer 4 to prevent I/O blocking. Implementing a 64KB KHO-preserved circular ring buffer in `struct caretaker_cb` will allow the incoming VMM to dump the complete in-gap guest console output into host system logs upon reattachment.
4. **Live Caretaker Code Updates:**
   The ultimate evolution of physical CPU preservation is supporting seamless updates to the Caretaker runtime itself. By versioning the entry trampolines and executing a transient CPU pause-and-remap sequence, consecutive kernel versions can upgrade the Caretaker execution engine on-the-fly without stopping guest workloads.
