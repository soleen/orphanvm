# OrphanVM RFCv2 TODO

## 1. Decouple Physical CPU Preservation (`cpu_preserve`) from `oncore`

Currently, `cpu_preserve_preserve()` (`kernel/liveupdate/cpu_preserve.c`)
immediately calls `oncore_session_add_cpu()`, which calls
`cpu_preserved_attach_workload(cpu, oncore_sched_cpu_worker, sess)` and moves
the CPU from `CPU_PRESERVED_PARKED` (`cpu_preserved_park_loop()`) into
`CPU_PRESERVED_WORKLOAD` (`oncore_cpu_schedule_loop()`) even before any
`oncore_job` or `vcpufd` is preserved. In addition,
`cpu_preserve_can_preserve()` gates `/sys/devices/system/cpu/cpu<N>/preserve` on
`IS_ENABLED(CONFIG_LIVEUPDATE_ONCORE)`.

- Remove `IS_ENABLED(CONFIG_LIVEUPDATE_ONCORE)` from
  `cpu_preserve_can_preserve()` so Layer 2 (`CONFIG_LIVEUPDATE_CPU`) can
  preserve physical CPUs via LUO when `CONFIG_LIVEUPDATE_ONCORE=n`.
- Keep preserved physical CPUs parked in `cpu_preserved_park_loop()`
  (`CPU_PRESERVED_PARKED`) when `/sys/devices/system/cpu/cpu<N>/preserve` is
  preserved into a LUO session; do not touch or attach `oncore_sched_cpu_worker`
  until an `oncore_job` (such as a preserved `vcpufd`) is actually activated in
  the session.

## 2. Order-Independent `vcpufd` and Physical CPU Preservation

Currently, `oncore_session_submit_job()` (`kernel/liveupdate/oncore.c`) returns
`NULL` if no physical CPUs have been added to the LUO session yet
(`!sess || cpumask_empty(oncore_session_cpus(sess))`), which causes
`kvm_caretaker_vcpu_pre_preserve()` (`virt/kvm/caretaker.c`) to fall back to
RAM-only preservation and requires physical CPUs to be preserved before
`vcpufd`s.

- Allow `vcpufd`s (and generic `oncore_job`s) to be preserved into a LUO session
  before physical CPUs are added:
  - Create or look up the `oncore_session` in `oncore_session_submit_job()` via
    `oncore_get_or_create_session(s)` even when `oncore_session_cpus(sess)` is
    empty.
  - Initialize the vCPU's Caretaker control block (`KVM_CARETAKER_PAUSED`) and
    enqueue the `oncore_job` on the session's `oncore_runqueue` so the vCPU
    remains suspended until physical CPUs are available.
  - When physical CPUs are subsequently preserved into the LUO session via
    `oncore_session_add_cpu()`, attach `oncore_sched_cpu_worker` and kick the
    newly added CPUs so queued `oncore_job`s immediately start executing.
  - If no physical CPUs are ever added to the session before `kexec`, the
    preserved `vcpufd`s remain suspended in RAM across `kexec` and are restored
    normally from `struct kvm_vcpu_arch_ser` during
    `LIVEUPDATE_SESSION_RETRIEVE_FD`.

## 3. Layer 3 (`oncore`) Standalone Selftest

- Add a selftest under `tools/testing/selftests/liveupdate/` with a sample
  in-kernel `oncore_job` to verify Layer 3 (`CONFIG_LIVEUPDATE_ONCORE`)
  scheduling capabilities across `kexec` independently of KVM.

## 4. VMM-Only Live Update Using LUO + OrphanVM

Investigate how to live-update the userspace VMM process without performing a
host `kexec` reboot while keeping guest vCPUs executing in the Caretaker:

- Determine whether the old VMM needs to pass its existing `vmfd`, `vcpufd`, and
  `memfd`/`guest_memfd` descriptors (or the LUO session descriptor) to the new
  VMM process, or whether LUO can allow retrieving preserved file descriptors
  back on cancel / same-kernel session retrieval.
- Note that upstream KVM binds `struct kvm` to the creating process's
  `struct mm_struct` (`kvm->mm != current->mm` returns `-EIO` in
  `kvm_vm_ioctl()` and `kvm_vcpu_ioctl()`), so a new VMM process with a new
  `mm_struct` either needs fresh `vmfd`/`vcpufd` instances created via LUO
  retrieve (`LIVEUPDATE_SESSION_RETRIEVE_FD`) or explicit `kvm->mm` rebinding.

## 5. Remove Redundant `flags` from `struct kvm_vcpu_ser`

`struct kvm_vcpu_ser` (`include/linux/kho/abi/kvm.h`) currently defines a
`u32 flags` field whose only flag is `KVM_VCPU_LUO_FLAG_CARETAKER`, which is
redundant with `vcpu->caretaker.job` during `.preserve()` and
`ser->cb.phys != 0` (`KHOSER_LOAD_PTR(ser->cb) != NULL`) across
`.post_preserve()`, `.unpreserve()`, `.retrieve()`, and `.finish()`:

- Remove `enum kvm_vcpu_luo_flags` (`KVM_VCPU_LUO_FLAG_CARETAKER`) and replace
  `u32 flags` in `struct kvm_vcpu_ser` with `u32 reserved` (must be zero).
- Check `vcpu->caretaker.job != NULL` in `kvm_arch_vcpu_luo_preserve()` and
  `KHOSER_LOAD_PTR(ser->cb) != NULL` (or `ser->cb.phys != 0`) in
  `virt/kvm/caretaker.c`, `virt/kvm/caretaker_debug.c`, `arch/x86/kvm/`, and
  `arch/arm64/kvm/`.

## 6. Separate Variable-Length Arrays in `struct kvm_vcpu_arch_ser`

Currently, x86 `struct kvm_vcpu_arch_ser` (`include/linux/kho/abi/kvm_x86.h`)
packs `msrs[num_msrs]` and `cpuid_nent` `struct kvm_cpuid_entry2` entries
back-to-back at the end of `struct kvm_vcpu_arch_ser` via untyped pointer
arithmetic (`(void *)&state->msrs[state->num_msrs]`), and ARM64
`struct kvm_vcpu_arch_ser` (`include/linux/kho/abi/kvm_arm64.h`) embeds
`u32 num_sysregs` + `struct kvm_one_reg sysregs[]` inline:

- On **x86** (`include/linux/kho/abi/kvm_x86.h`), replace `num_msrs`,
  `cpuid_nent`, and `msrs[]` in `struct kvm_vcpu_arch_ser` with typed KHO
  pointers to the existing uAPI variable-length container structures
  `DECLARE_KHOSER_PTR(msrs, struct kvm_msrs *)` and
  `DECLARE_KHOSER_PTR(cpuid, struct kvm_cpuid2 *)`.
- On **ARM64** (`include/linux/kho/abi/kvm_arm64.h`), define
  `struct kvm_arm64_sysregs_ser` (`u32 num_sysregs; u32 reserved;
  struct kvm_one_reg sysregs[];`) and reference it from
  `struct kvm_vcpu_arch_ser` via
  `DECLARE_KHOSER_PTR(sysregs, struct kvm_arm64_sysregs_ser *)`.
- Because `kho_alloc_preserve()` preserves memory at page granularity, compute
  the full combined size up front and place `struct kvm_vcpu_arch_ser` together
  with `struct kvm_msrs` and `struct kvm_cpuid2` (on x86) or
  `struct kvm_arm64_sysregs_ser` (on ARM64) within a single contiguous
  `kho_alloc_preserve(size)` allocation so there is zero extra page overhead.

## 7. Defer Secondary Page Tables and `vm_token` Lookup to `.freeze()`

Currently, secondary page tables (`struct kvm_kho_folios_ser`) are walked and
passed to `kho_preserve_folio()` during `LIVEUPDATE_SESSION_PRESERVE_FD(vmfd)`
(`kvm_luo_preserve()` -> `kvm_arch_vm_luo_preserve()`), and
`kvm_vcpu_luo_preserve()` calls `liveupdate_get_token_outgoing()` during
`.preserve()`:

- Calling `liveupdate_get_token_outgoing()` in `kvm_vcpu_luo_preserve()` forces
  `vmfd` to be preserved before `vcpufd`s (unlike `guest_memfd`, which defers
  `liveupdate_get_token_outgoing()` to `kvm_gmem_luo_freeze()`).
- Preserving secondary page tables in `kvm_luo_preserve()` runs while vCPUs may
  still be executing in `KVM_RUN`, allowing new TDP / Stage-2 page-table pages
  to be faulted in (which are then missed by KHO preservation) or existing
  page-table pages to be zapped and freed back to the buddy allocator while
  still marked preserved in KHO.
- On x86 (`arch/x86/kvm/mmu/kho.c`), `kvm_mmu_collect_all()` collects raw
  `struct page *` pointers without deduplicating (e.g., when
  `vcpu->arch.mmu == &vcpu->arch.guest_mmu`), which can record duplicate PFNs in
  `folios_pa[]` and trigger `WARN_ON_ONCE(info.magic != KHO_PAGE_MAGIC)` on the
  second `kho_restore_free()` call during `kvm_kho_folios_finish()`.
- On ARM64 (`arch/arm64/kvm/kvm_luo.c`), `kvm_arch_vm_luo_preserve()` walks
  `mmu->pgt` without holding `kvm->mmu_lock` and calls `kho_preserve_folio()`
  (which may sleep) directly inside `stage2_kho_visitor()`.
- **Fix for RFCv2**:
  - Move `liveupdate_get_token_outgoing()` from `kvm_vcpu_luo_preserve()` to
    `kvm_vcpu_luo_freeze()` (matching `kvm_gmem_luo_freeze()`), allowing `vmfd`
    and `vcpufd`s to be preserved in any order and verifying the dependency at
    `.freeze()` time.
  - Move secondary page table walking and `kho_preserve_folio()` from
    `kvm_luo_preserve()` to `kvm_luo_freeze()` (`kvm_arch_vm_luo_freeze()`). At
    `.freeze()` time, all `vcpufd`s are already preserved and detached from
    `KVM_RUN`, so secondary page tables are quiescent and live-update
    cancellation before `kexec` avoids unnecessary page-table walks.
  - Deduplicate collected page-table folios and use the two-phase collect (under
    `mmu_lock`) + `kho_preserve_folio()` (outside `mmu_lock`) pattern on both
    x86 and ARM64.

## 8. Clean Up `__cpu_preserved_text` Attributes and `Makefile` Flags

Currently, `kernel/liveupdate/Makefile`, `arch/x86/kvm/Makefile`, and
`arch/arm64/kvm/Makefile` duplicate `-fno-stack-protector` and per-file
`KASAN`/`KCSAN`/`UBSAN`/`KCOV`/`FTRACE` disables across mixed `.c` files while
missing several kernel hardening flags used by `arch/arm64/kvm/hyp/nvhe` and
`arch/x86/purgatory`:

- Define `__cpu_preserved_text` in `include/linux/cpu_preserve.h` using
  `__noinstr_section(".text.cpu_preserved")`, `__no_stack_protector`, and
  `__noscs` so per-function attributes handle `notrace`, KASAN, KCSAN, KMSAN,
  KCOV, GCOV, stack protector, and Shadow Call Stack without stripping
  instrumentation from normal host kernel functions in the same file.
- Remove redundant `-fno-stack-protector` from the `Makefile` rules and add
  `$(DISABLE_KSTACK_ERASE)` (to prevent `CONFIG_GCC_PLUGIN_STACKLEAK` from
  inserting `stackleak_track_stack()` calls), `-DDISABLE_BRANCH_PROFILING` (to
  prevent `CONFIG_TRACE_BRANCH_PROFILING` from emitting `ftrace_likely_update()`
  calls), `GCOV_PROFILE_<obj>.o := n`, and `KMSAN_SANITIZE_<obj>.o := n`
  alongside `-fno-jump-tables` and `-ftrivial-auto-var-init=uninitialized`.

---

## Already Implemented Since RFCv1

1. **Map Session Buffers into Per-Session `cpu_preserved_as`**:
   - `oncore_session_map_range()` (`kernel/liveupdate/oncore.c`) maps buffers
     into `sess->as` via `cpu_preserved_as_map()` when `sess && sess->as` is
     valid instead of falling back to the global `cpu_preserved_map_range()`.
   - `kvm_arch_vcpu_caretaker_preserve()` (`arch/x86/kvm/caretaker.c`) maps
     `struct kvm_vcpu_arch_ser *state` into the session's isolated address space
     via `oncore_session_map_buffer(sess, state, size)`.
2. **Preserve `kvmclock` and Refresh `MSR_IA32_TSC` Across `kexec` on x86**:
   - Added `struct kvm_clock_data clock` to `struct kvm_vcpu_arch_ser`
     (`include/linux/kho/abi/kvm_x86.h`) and exposed in-kernel `get_kvmclock()`
     and `kvm_set_clock()` helpers (`arch/x86/kvm/x86.c`).
   - `kvm_arch_vcpu_luo_preserve()` and `kvm_arch_vcpu_luo_retrieve()`
     (`arch/x86/kvm/kvm_luo.c`) save and restore `kvmclock` state and restore
     `MSR_IA32_TSC` and `MSR_IA32_TSC_ADJUST` across Caretaker retrieval.
   - `vmx_caretaker_post_exit()` (`arch/x86/kvm/vmx/caretaker.c`) and
     `svm_caretaker_detach_serialize()` (`arch/x86/kvm/svm/caretaker.c`) refresh
     `MSR_IA32_TSC` in `state->msrs[]` (`rdtsc() + tsc_offset`) at exit time so
     `kvm_synchronize_tsc()` on retrieve computes the preserved `tsc_offset`
     instead of a stale pre-`kexec` timestamp.
