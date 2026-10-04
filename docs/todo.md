# OrphanVM RFCv2 TODO

## Post-RFCv2

### 1. VMM-Only Live Update Using LUO + OrphanVM

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
