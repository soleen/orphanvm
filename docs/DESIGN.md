# Orphaned Virtual Machines: The Caretaker

**Authors:** Pasha Tatashin, `<add-names>`
**Contributors:** `<add-names>`
**Date:** 3/4/2026
**Status:** Draft

## Introduction and Background

The "Live Update" kernel capability provides the foundational ability to
preserve workload state across host transitions. However, the operational
"blackout" window is currently bottlenecked by both kernel and userspace
lifecycles.

### Orphaned VMs

**Definition:** A Virtual Machine actively executing guest instructions on isolated physical hardware completely decoupled from a Host Operating System or a userspace Virtual Machine Monitor (VMM).

Historically, a VM's lifecycle is tied to a userspace VMM process. The Orphaned
VM strategy breaks this dependency by separating resource ownership from active
execution.

Before the old kernel shuts down, it uses the Live Update Orchestrator (LUO) to
maintain ownership of the guest's underlying resources. The LUO preserves the
`vmfd` and `vcpufd` handlers, guest memory, secondary page tables, and other
critical KVM metadata required to successfully restore the VM state in the new
kernel.

However, the active execution of the VM is not managed by the host kernel.
Instead, execution is handed off to a specialized bare-metal component: **The
Caretaker**.

While the VM is "orphaned," it operates entirely outside of both Userspace and
Kernel space. The physical CPU (pCPU) hosting the VM is completely isolated
from the Host OS. The vCPUs continue to execute guest instructions
uninterrupted, and any VM Exits are trapped and handled exclusively by the
Caretaker, all while the host kernel reboots on separate management cores (or
VMM restarts, if the Live Update is for a VMM component only).

## The Caretaker

The Caretaker is a specialized, identity-mapped bare-metal executable attached
to each vCPU. While it is installed during the initial VM setup and remains
permanently enabled as an interpose layer between the Guest and KVM, it is the
key architectural feature required for a VM to be orphaned. By providing an
execution environment independent of the host OS, the Caretaker enables the
guest workload to safely survive the host lifecycle transition.

During normal VM operation, the Caretaker acts as a fast-path shim: it forwards
standard VM Exits down to the backing KVM kernel module and handles certain
exits autonomously. However, during the "Management Gap" (when no host OS is
active and standard KVM handlers are offline), the Caretaker enters a
standalone mode to ensure continuous guest execution without host intervention.

### Constraints

The Caretaker is a bare-metal executable. It does not have a backing kernel, it
makes zero syscalls, and it lacks access to underlying hardware devices or
complex I/O components.

## Hardware Setup & ABI Interface

To interpose the Caretaker between the guest and the host OS, the
initialization sequence must load the bare-metal payload, rewire the physical
CPU's virtualization structures, and establish a syscall-free communication
channel.

### API Design & Caretaker Installation

The Caretaker is installed early in the VM’s lifecycle (e.g., shortly after
`KVM_CREATE_VM`).

To manage this, we introduce a new KVM ioctl (`KVM_SET_CARETAKER`) that
configures the shim on the `vcpufd` (alternatively on `vmfd`). Userspace
provides the Caretaker payload compiled as an ELF binary.

Using an ELF binary allows the Caretaker to be separated into distinct memory
sections:
- **`.text`**: The executable bare-metal instructions. This section is mapped
  as read-execute (RX).
- **`.data` / `.rodata`**: The static data and variables. This includes the
  pre-populated per-vCPU metadata (such as CPUID topology responses) generated
  by the VMM.
- **`.ccb`**: A dedicated section specifically reserved for the Caretaker
  Control Block.

During installation, the userspace VMM opens the ELF file, and passes an fd to
the `KVM_SET_CARETAKER` ioctl.

### Hardware Interposition

During the execution of the `KVM_SET_CARETAKER` ioctl, instead of pointing the
hardware's return path to standard KVM entry points (e.g., `vmx_vmexit` or
`svm_vcpu_run`), KVM reprograms the host-state return area of the CPU's
hardware virtualization control structures (e.g., Intel VMCS, AMD VMCB, or ARM
equivalent) to point directly into the bare-metal Caretaker environment.

Specifically, the following critical host-state fields (using x86 terminology
for illustration) are updated:
- **`HOST_RIP`**: Set to point to the `e_entry` (entry point) of the provided
  ELF `.text` segment. Whenever the guest triggers a VM Exit, the CPU hardware
  will unconditionally jump to this address.
- **`HOST_RSP`**: Programmed to point to a specialized, pre-allocated bare-
  metal stack dedicated strictly to this vCPU's Caretaker. This allows it to
  execute completely independently of the host kernel's normal thread stacks.
- **`HOST_SSP` and `HOST_INTR_SSP_TABLE`**: KVM pre-allocates a Shadow Stack
  for the Caretaker. These VMCS fields are programmed during initialization to
  ensure that `RET` and `IRET` instructions executed by the Caretaker do not
  trigger fatal `#CP` faults when the host kernel is detached.
- **`HOST_GS_BASE`**: Programmed to point to CCB/Shared Metadata for this vCPU.
- **`HOST_CR3`**: Configured to point to the Caretaker’s isolated, identity-
  mapped page tables, ensuring memory fetch safety when the host kernel’s page
  tables are torn down.

**Note on Optimization vs. Security:** Constantly switching the page table (CR3) on every VM Exit can be expensive due to TLB flushing. To optimize performance, the Caretaker can share the host kernel's page tables while the kernel is still around, and dynamically replace `HOST_CR3` with the dedicated, isolated page tables only when the vCPU is orphaned (during the detachment phase). On the other hand, maintaining a permanently isolated CR3 for the Caretaker adds a strong security boundary, achieving hardware-enforced separation similar to KVM Address Space Isolation (ASI).

### KVM-Caretaker ABI

The Caretaker requires a defined ABI to communicate with the host KVM
subsystem. This ABI is implemented via the shared, identity-mapped `.ccb`
section of the ELF payload, acting as the Caretaker Control Block (CCB).

The CCB acts as the source of truth for the Caretaker's execution loop and
contains three primary elements:
- **Attachment State Flag**: An atomic variable indicating the current
  relationship with the host KVM subsystem (e.g., `KVM_ATTACHED` or
  `KVM_DETACHED`).
- **KVM Routing Pointers**: The physical function pointers that the Caretaker
  uses to safely jump into the host KVM's standard VM Exit handlers when
  operating in normal mode.
- **Shared Configuration Metadata**: A physical pointer to dedicated memory
  pages used by the kernel to share dynamic vCPU configuration data with the
  Caretaker. Because every guest is configured differently, KVM populates these
  pages with the specific parameters negotiated during VM initialization (such
  as CPUID feature masks, APIC routing, and timer states). These pages also
  include a pre-allocated Telemetry Buffer for the Caretaker to log VM Exits
  and spin-wait durations. These dedicated pages are explicitly preserved
  across the host reboot via KHO, ensuring the Caretaker maintains continuous
  access to the exact context required to accurately emulate trivial exits
  during the gap.

## Caretaker VM Exit Flow

Upon vmexit, the Caretaker's execution flow follows a routing hierarchy:

### The Fast Path

The Caretaker first evaluates the VM Exit reason. If the exit belongs to a
category that the Caretaker is programmed to resolve natively, it handles it
internally. For example, profiling of guests has identified the following exit
categories for potential local resolution:
- **Guest Idle Exits (e.g., HLT)**: When the guest OS goes idle, it triggers
  idle exits. The Caretaker intercepts these and halts the physical core until
  the next guest-bound interrupt fires, preserving host power.
- **Timer and APIC Exits**: Even an idle guest frequently writes to interrupt
  controllers and system registers to configure internal timers. The Caretaker
  handles these trivial writes directly, acknowledging the timer updates.
- **Exits like CPUID or safe system register accesses**: Resolved by reading
  pre-computed responses from the shared metadata pages to accurately service
  the guest dynamically.

### Host Routing

If the VM Exit requires KVM/VMM intervention (e.g., a Page Fault or emulated
device I/O), the Caretaker cannot resolve it locally. It must check the CCB
attachment state flag to determine where to route the exit:
- **If `KVM_ATTACHED`**: The host kernel is actively managing the system. The
  Caretaker acts as a fast-path trampoline, setting up the standard host
  registers and transferring execution to the KVM Routing Pointers.
- **If `KVM_DETACHED`**: The host kernel is offline for Live Update. The
  Caretaker places the specific vCPU into a safe "spin-wait" polling loop,
  continuously checking the CCB flag change.

## Preservation of the vCPU

For this orchestration to work across a host OS replacement, the file
descriptor associated with the vCPU must outlive the userspace process that
created it, leveraging the LUO:

- **Creation**: The VMM initially creates the `vcpufd` via the standard
  `KVM_CREATE_VCPU` ioctl on the VM-wide `kvmfd`. Immediately after creation,
  the VMM issues the `KVM_SET_CARETAKER` ioctl on the newly created `vcpufd`.
  This installs the Caretaker as an interpose layer, allocating the CCB and
  rewiring the hardware virtualization control structures (e.g., `HOST_RIP`)
  for this specific vCPU. A dedicated userspace thread then takes ownership of
  this file descriptor to drive the `KVM_RUN` loop.
- **LUO Registration & Isolation**: In preparation for Pauseless Update, the
  VMM acquires a session from the userspace luo-agent and registers the
  `vcpufd` using `LIVEUPDATE_SESSION_PRESERVE_FD`. KVM's LUO `.preserve()`
  handler is invoked. Prior to this step, the VMM must pause emulated devices;
  the guest relies on VFIO pass-through for primary I/O to survive the gap
  without triggering VMM-bound exits. Crucially, the `.preserve()` phase is the
  moment when the pCPU is isolated from the host OS. The kernel finalizes the
  state, offlines the core (from the OS perspective), transitions the vCPU
  fully into the Caretaker, and preserves the required KVM data to KHO.
- **The Gap**: When the VMM process exits prior to the kexec transition, open
  file descriptors are normally destroyed. However, because the `vcpufd` is
  registered with LUO, the kernel holds a reference to the underlying `struct
  file` to ensure it survives the reboot. While the VMM has exited, the
  isolated pCPU appears completely offline to the host OS. During the boot of
  the Next Kernel, the core `smp_init()` routine parses the LUO FLB data and
  explicitly skips initializing these preserved pCPUs. This shields the
  dedicated cores from reset signals, allowing the guest workload to continue
  uninterrupted.
- **Reclamation**: The Next Kernel boots and LUO deserializes the session. When
  the new VMM process spawns, it retrieves the preserved session and issues
  `LIVEUPDATE_SESSION_RETRIEVE_FD` using its token. LUO invokes KVM's
  `.retrieve()` callback to map the preserved `vcpufd` back into the new VMM's
  file descriptor table. As part of this retrieval process, the host formally
  brings the isolated pCPU back online, and the new VMM userspace thread is
  attached back to the active VM thread running on the vCPU. Finally, KVM
  populates the new KVM Routing Pointers in the CCB and atomically flips the
  Host State Flag back to `KVM_ATTACHED`. This breaks the Caretaker's spin-wait
  loop (if it is in this state), allowing standard KVM operation to resume.

## Gap Observability and Telemetry

During the host transition standard host-level observability tools (e.g.,
ftrace, perf, eBPF) are completely offline. To ensure production readiness,
site reliability, and precise latency accounting, the Caretaker must act as a
standalone micro-profiler while the host is detached.

The architecture includes a pre-allocated, identity-mapped Telemetry Buffer,
passed to the Caretaker via the Shared Configuration Metadata during
installation. During the gap, the Caretaker writes to this memory region to
record execution data:

- **Exit Counters**: The Caretaker increments hardware-specific counters for
  every VM Exit reason it encounters. This provides a precise tally of natively
  handled events (e.g., `HLT`, `CPUID`, or APIC writes) that occurred while the
  host was offline.
- **Spin-Wait Profiling**: If a complex exit forces the Caretaker to block, it
  reads the hardware time stamp counter (e.g., `RDTSC` on x86, `CNTVCT_EL0` on
  ARM64) immediately before entering the spin-wait loop. It reads the counter
  again the exact moment the CCB flag flips back to `KVM_ATTACHED`. The
  Caretaker records the specific exit reason that caused the block and the
  total accumulated stall time.
- **Post-Gap Ingestion**: Upon successful reattachment, the newly booted host
  KVM subsystem parses this Telemetry Buffer. The recorded data is exported via
  debugfs (e.g., `/sys/kernel/debug/kvm/`).

## Challenges During Gap

While the Caretaker manages standard VM Exits, completely offlining a physical
CPU introduces critical edge cases regarding asynchronous hardware interrupts.
Below are the primary architectural challenges and their proposed mitigations.

### VFIO Interrupt Routing

- **The Problem**: The design relies on VFIO pass-through for primary I/O to
  survive the gap. However, when a physical device (e.g., an NVMe drive)
  completes a read, it sends a hardware interrupt (MSI/MSI-X) back to the CPU.
  By default, external hardware interrupts cause a VM Exit. Because the
  Caretaker natively lacks Linux IRQ routing tables, it has no immediate
  mechanism to inject that specific device interrupt into the guest's virtual
  APIC or GIC. The guest would stall indefinitely waiting for the I/O
  completion.
- **Proposed Solution**: The deployment may utilize hardware-accelerated
  interrupt routing, such as Posted Interrupts (Intel PI), Advanced Virtual
  Interrupt Controller (AMD AVIC), or GICv4 Direct Virtual Interrupt Injection
  (ARM64). This hardware feature allows the IOMMU/SMMU to route physical device
  interrupts directly into the guest’s virtual interrupt controller without
  causing a VM Exit. Alternatively, KVM could share its IRQ routing tables with
  the Caretaker via the Shared Configuration Metadata during setup. This would
  allow the Caretaker to intercept the hardware interrupt and manually inject
  the virtual interrupt into the guest in software.

### Guest-to-Guest IPIs

- **The Problem**: If the guest OS attempts to wake up a sleeping thread, one
  orphaned vCPU will send an Inter-Processor Interrupt (IPI) to another
  orphaned vCPU. In standard virtualization without hardware assistance,
  writing to the APIC ICR (or sending an ARM SGI) causes a VM Exit so the host
  KVM can emulate the message delivery. During the gap, KVM is unavailable to
  route this message.
- **Proposed Solution**: The architecture may leverage hardware virtualized
  interrupts (Intel APICv, AMD AVIC, or ARM GICv4.1 virtual SGIs). This allows
  the hardware silicon to handle IPI delivery between the isolated pCPUs
  natively, eliminating the VM Exit. Alternatively, the Caretaker can be
  programmed to emulate the IPI delivery. By utilizing the shared memory
  metadata, the Caretaker can determine the target vCPU and directly update its
  pending interrupt state.

### Stray Hardware Interrupts

- **The Problem**: What happens if a Non-Maskable Interrupt (NMI), a hardware
  timer tick, or a Machine Check Exception / System Error (MCE / ARM SError)
  arrives while the CPU is actively executing Caretaker code in `KVM_DETACHED`
  mode?
- **Proposed Solution**: To safely handle these asynchronous events, the
  Caretaker payload should establish and load its own minimal, self-contained
  Interrupt Descriptor Table (IDT on x86) or Exception Vector Table (via
  `VBAR_EL2` on ARM64) during initialization. This ensures that if a stray
  hardware event or NMI arrives while executing Caretaker instructions in host
  mode, the CPU can catch the fault and park the core in a known state rather
  than triggering a catastrophic host reboot. - On x86, when transitioning into
  the gap, KVM explicitly programs `HOST_IDTR` and `HOST_GDTR` to these self-
  contained tables. If an NMI or stray hardware event arrives, the CPU catches
  the fault using the Caretaker's native handlers, parking the core or logging
  the event rather than attempting host recovery. KVM also programs
  `HOST_INTR_SSP_TABLE` within the Caretaker's isolated environment so that if
  the exception handlers execute an `IRET`, the hardware's CET shadow stack
  unroll succeeds without triggering a `#CP` exception.

### Timekeeping Drift

- **The Problem**: While the guest is orphaned, it continues executing and
  reading time via the physical CPU's hardware Time Stamp Counter (TSC or ARM
  generic timer). Because the host kernel is offline, software-based
  paravirtualized clocks (such as `kvmclock`) are no longer being updated by
  host background threads. Furthermore, when the Next Kernel boots, its
  calculation of host `CLOCK_MONOTONIC` or wall-time might drift from the old
  kernel. If the new KVM subsystem resets the VM's TSC offsets or updates the
  PV clock structures with standard initialization values upon adoption, the
  guest could experience a time jump.
- **Proposed Solution**: During the gap, the guest relies entirely on the
  physical CPU's Invariant TSC (which the hardware automatically offsets
  natively via the VMCS/VMCB) for continuous timekeeping. To ensure safe
  reattachment, KVM must serialize the exact state of the guest's PV clocks,
  TSC offsets, and the old kernel's base reference times into KHO memory during
  the LUO `.preserve()` phase. Upon adoption, the new KVM subsystem must
  synchronize its internal tracking with this preserved data, ensuring that any
  subsequent updates to the guest's PV clock memory guarantee a strictly
  monotonic and smooth time progression.

### Nested Page Table Updates

- **The Problem**: As the guest executes, it may attempt to access memory that
  has not yet been mapped by the hypervisor, or it may interact with MMIO
  regions. Normally, this triggers an EPT Violation (Intel) or NPT Page Fault
  (AMD), prompting KVM to allocate host pages and update the secondary page
  tables. How are these updates handled when the host KVM subsystem is offline
  during the gap?
- **Proposed Solution**: During the "Management Gap," there are absolutely no
  updates made to the NPT/EPT. The existing secondary page tables are fully
  preserved in memory via LUO kvmfd preservation prior to detachment, allowing
  the guest to seamlessly access all previously mapped memory. If the guest
  triggers a new page fault (requiring an NPT/EPT update) during the gap, the
  Caretaker simply categorizes it as a Blocking Exit.

### Compromised Caretaker

- **The Problem**: The Caretaker runs in Host Mode. If left unprotected, this
  could allow a lightly privileged userspace process (e.g., QEMU or crosvm) to
  inject arbitrary executable code directly into the CPU's most privileged
  hardware state (VMX Root / Ring 0 / EL2).
- **Proposed Solution**: To mitigate this risk, the `KVM_SET_CARETAKER` ioctl
  may adopt the security model used by the `kexec_file_load()` syscall. Rather
  than trusting userspace to pass physical addresses, the kernel must take full
  ownership of payload validation:
  - **In-Kernel ELF Parsing**: Instead of
  passing raw segment addresses (which is vulnerable to manipulation), the VMM
  passes a file descriptor for the Caretaker binary. The host KVM subsystem
  then performs the ELF parsing entirely in-kernel. This guarantees that the
  kernel controls exactly where the `.text`, `.data`, and `.ccb` sections are
  mapped, preventing userspace from tricking the kernel into overwriting
  sensitive host memory.
  - **Signature Verification & Secure Boot**: If the
  host is running with Secure Boot or Kernel Lockdown enabled, KVM mandates
  that the Caretaker ELF binary be cryptographically signed. The kernel
  verifies the signature against the system's trusted keyring (e.g.,
  `.builtin_trusted_keys`) before loading it. An unsigned or modified payload
  is outright rejected.
  - **IMA (Integrity Measurement Architecture)
  Integration**: The loading of the Caretaker is hooked directly into the
  kernel's IMA subsystem. The binary is measured (hashed and extended into the
  hardware TPM) for remote attestation and appraised against local security
  policies before execution is permitted.

### Caretaker Update

- **The Problem**: Given that caretaker is permanently installed during VM
  setup, how does it get updated on long running VMs?
- **Proposed Solution**: To update the Caretaker, we can do the update only
  when vCPUs are not isolated (i.e. not during live update gap):
  - **vCPU
  Quiescence**: The userspace VMM issues the `KVM_SET_CARETAKER` ioctl. KVM
  then sends `kvm_vcpu_kick` to force the target vCPU to exit the guest and
  return to the host kernel.
  - KVM parses the new ELF. It allocates fresh
  memory pages for the new `.text` and `.data` segments.
  - The kernel populates
  the new `.ccb` with the existing VM context.
  - After the new environment is
  fully staged, KVM updates the physical CPU's virtualization control
  structures.
  - When the vCPU thread resumes and re-enters the guest, the next
  VM Exit will trigger the hardware to jump into the new Caretaker payload. The
  old memory segments are then safely freed.

---

## 8. Maintainer Alignment & Implementation Status

Following architectural reviews on LKML (`docs/thread.txt`), the design was
streamlined to align with maintainer consensus (Paolo Bonzini, Alexander Graf,
David Woodhouse):

1. **In-Kernel Placement**: Abandoned userspace ELF loading in favor of an
   in-kernel Caretaker implementation built directly into KVM (`caretaker_svm.c`,
   `caretaker_svm.h`, and `vmenter.S`).
2. **KHO & LUO Handover**: Kexec Handover (KHO) serializes hardware state
   (VMCB/VMCS/HSAVE/page tables) and hands over the isolated `caretaker_page`
   to the incoming kernel.
3. **Fastpath VM-Exit Reuse**: Attached execution utilizes standard KVM fastpaths
   (`svm_exit_handlers_fastpath`), while detached mode handles trivial I/O and
   advances RIP locally.
4. **SMP Boot Protection**: Secondary boot (`smpboot.c`, `cpu.c`) skips
   `INIT/SIPI` reset sequences for active Caretaker CPUs during incoming kernel
   bringup.
5. **Implementation Milestones**:
   - **AMD x86_64**: Phase 6.6 complete and verified (0.000s guest downtime gap).
   - **Intel x86_64**: Phase 7.1 in progress.
   - **ARM64**: Phase 7.2 scheduled next.

