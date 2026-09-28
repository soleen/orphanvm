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

