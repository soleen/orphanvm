# OrphanVM RFCv2 TODO

2. In cpu_preserve_ser we have arch field, can't we add them
   indepdentantly from arm and x86 arch headers like we do for vcpu?

3. I wonder if we should move EVERYTHING that is live update related in
   kvm/ and arches under liveupdate/ instead of under caretaker/ this
   way we can also move the kho, kho_luo etc over there.

4. Imo vmfd should be guarded by KVM_LIVEUPDATE, guest_memfd should use
   it as well, and it should add the KVM capability, does it already do
   so?

6. LUO shold allow to retrieve form the outgoing session to the current
   kernel, for this, we will convert the session into a new type: i.e.
   retrieve inplace (or come up with a new name), the content of such
   session DOES NOT get preserved across reboot, everything is
   unpreserved simultaniously, but FDs are retrieved as requested.
   This will allow to do VMM in-place update, and also a bunch of other
   benefits such as partitioned system. protected system, save from
   crash etc.

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
