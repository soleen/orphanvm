# Review Instructions

When asked to perform a code review using this specification, execute a maintainer-level technical review of `nanovmm` (or the targeted code scope) applying kernel design and code quality criteria.

---

## 1. Quality & Parameter Evaluation Criteria

Analyze the codebase across the following key technical dimensions:

1. **Simplicity & Code Reduction**:
   - Eliminating over-engineered state machines, unnecessary wrapper layers, and bloat.
   - Preferring straightforward, linear logic and minimal line counts to express invariants.
   - Removing redundant helper functions or deduplicating similar logic blocks.

2. **Proper Design Layering**:
   - Verifying clean 3-tier architecture layering:
     - **Core VMM Engine (`nvmm.c`)**: POSIX threads, CLI, terminal raw mode, LUO preservation loop.
     - **Architecture Interface Contract (`nvmm.h`)**: Pure function hooks & abstract structs.
     - **Architecture Backends (`x86_64.c`, `arm64.c`)**: Boot protocols, hardware devices, page tables.
   - Guaranteeing **ZERO preprocessor `#ifdef` directives in `.c` files** (architecture separation enforced purely through build target selection in Makefile).

3. **Multi-Platform Hardware Architecture Support**:
   - **x86_64 Backend (AMD & Intel)**:
     - Vendor-neutral KVM setup supporting both AMD (SVM / NPT) and Intel (VMX / EPT) hosts.
     - CPUID leaf configuration and MSR save/restore loops (`KVM_GET_MSR_INDEX_LIST` / `get_one_msr` / `set_one_msr`).
     - E820 memory map with MMIO hole, 4-level page table identity mapping, and 8250 PIO UART (`0x3F8`).
   - **ARM64 Backend**:
     - ARM GICv3 interrupt controller initialization, redistributor/CPU sysreg state preservation.
     - Self-contained binary Flattened Device Tree (FDT) generator with buffer overflow checks.
     - AMBA PL011 MMIO UART (`0x09000000`) and dynamic vCPU register list enumeration (`KVM_GET_REG_LIST`).

4. **Concurrency & Thread Safety**:
   - Pthread mutex/condvar synchronization and atomic state transitions.
   - Signal handler races (`SIGUSR1`, `SIGUSR2`, `SIGSTOP`, `SIGCONT`) vs syscall execution.
   - KVM `immediate_exit` flag timing relative to `ioctl(KVM_RUN)`.
   - Deterministic vCPU pause barriers with zero window for missed wakeup/pause events.

5. **Memory Management & Page Tables**:
   - 4-level page table construction (PML4, PDP, PD, PTE) and identity mapping limits.
   - 2MB vs 1GB hugepage mapping correctness and CPUID feature probing (`pdpe1gb`).
   - `mmap()` flags (`MAP_SHARED`), alignment, error handling, and `memfd_create()` lifecycles.

6. **Live Update (`/dev/liveupdate`) & State Preservation**:
   - Session creation, descriptor preservation (`LIVEUPDATE_SESSION_PRESERVE_FD`), and token binding.
   - Deferred session completion (`LIVEUPDATE_SESSION_FINISH`) to guarantee rollback safety until guest execution is verified.
   - Host `kexec` abort handling (e.g. process resumption after `SIGSTOP`).
   - Atomic state struct serialization, magic/version validation, and endianness/alignment safety.

7. **ABI & Syscall Safety**:
   - KVM ioctl structure layout matching and error check rigor.
   - System call error handling, return value validation, and error code propagation.

8. **Performance & I/O Hygiene**:
   - Elimination of single-byte syscall chatter (e.g. stack-buffered `read()` on `STDIN_FILENO`).
   - Serial ring buffer capacity, overflow diagnostics, and interrupt line (`COM1_IRQ` / `PL011`) pulsing rate.

9. **Commit Granularity & Architecture Isolation**:
   - Strictly separating commits by architecture: Core KVM/Caretaker infrastructure vs. `x86_64` vs. `arm64` vs. AMD SVM vs. Intel VMX.
   - Ensuring no experimental hardware loops or speculative code are bundled into core infrastructure or telemetry patches.
   - Verifying all commit lines $\le 72$ chars with mandatory `Signed-off-by` and zero AI metadata tags.

10. **Caretaker Telemetry & CCB Invariants**:
    - Ensuring all detached/gap VM exit handling is instrumented with `struct kvm_ccb_telemetry` and `kvm_ccb_record_exit()`.
    - Verifying serialization layout stability in KHO ABI (`include/linux/kho/abi/kvm.h`).
    - Guaranteeing atomic transition invariants between `KVM_ATTACHED` and `KVM_DETACHED`.

11. **Linus's "Good Taste" Architectural Principles**:
    - Eliminating special cases by designing clean, unified data structures and helpers.
    - Eliminating magic numeric literals by introducing named macros.
    - Minimizing unnecessary global state and duplicate logic paths.

---

## 2. Strict Output Formatting Rules

When generating the review output, follow these formatting constraints without exception:

1. **No Email Headers or Filler**:
   - Do **NOT** include mailing list headers (`From: Linus...`, `To: ...`, `Subject: ...`).
   - Do **NOT** write introductory conversational text, general summaries, or closing signatures.

2. **Output Format**:
   - Output **ONLY** a prioritized list of concrete **Code Change Requests**.
   - Format each change request strictly as follows:

### [CR-X] <Concise Title of Code Change Request>
- **Component / File**: `<filepath>:<lines>`
- **Category**: `[Simplicity | Layering | Arch-x86 | Arch-ARM64 | Concurrency | Memory | Live-Update | ABI | Performance | Good-Taste]`
- **Severity**: `[Critical | High | Medium | Low]`
- **Mechanics & Problem**:
  <Clear technical explanation of why the code fails, the race condition window, or the structural defect.>
- **Required Code Change**:
  <Exact code change, diff block, or refactored helper function to resolve the issue.>

---
