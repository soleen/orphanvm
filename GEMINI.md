# OrphanVM Project

This directory contains the OrphanVM project, which provides live update
with CPU preservation support.

## Components
- **buildroot**: Buildroot system for generating the project's root
  filesystem and toolchains.
- **overlays/buildroot_guest**: Overlay files for the Buildroot system.
- **docs**: Documentation directory, including detailed design
  specifications (e.g., `DESIGN.md`).
- **linux**: Linux kernel source code.
- **nanovmm**: Nano VMM source code.

## Key Files
- **env.sh**: Sourced bash environment script that prepends `output_<arch>/bin`
  to `$PATH` (defaults to `x86_64` on x86 hosts or `arm64` on arm64 hosts;
  supports optional argument: `source env.sh [arm64|x86_64]`).
- **scripts/ovm_guest**: Bash script that automatically launches NanoVMM
  using the compiled Linux kernel and initramfs image. Installed to
  `output_<arch>/bin/ovm_guest`.
- **scripts/ovm_guest_launcher**: Script executed at host VM startup to
  auto-launch `ovm_guest`, `ovm_guest_qemu`, or no guest VM based on kernel
  cmdline. Installed to `output_<arch>/bin/ovm_guest_launcher`.
- **scripts/ovm_console**: Auto-reconnecting serial console script for
  Simics that silently waits for port 4000 to open without terminal
  spamming. Installed to `output_<arch>/bin/ovm_console`.
- **scripts/ovm_ssh**: Helper script to SSH into the host VM on the
  configured target port. Installed to `output_<arch>/bin/ovm_ssh`.
- **scripts/ovm_scp**: Helper script to copy files to/from the host VM
  over SSH. Installed to `output_<arch>/bin/ovm_scp`.
- **scripts/ovm_kexec**: Helper script to stage and trigger host live
  kernel update over SSH. Installed to `output_<arch>/bin/ovm_kexec`.
- **scripts/ovm_test**: Comprehensive test suite performing intra-kernel
  live update start & cancel loops and host kexec live update with on-core
  vCPU preservation, verifying incoming FLB CPU isolation and uninterrupted
  guest progress. Supports `--matrix` for full 8-configuration execution.
  Installed to `output_<arch>/bin/ovm_test`.
- **scripts/ovm_demo**: Stage-ready conference demonstration script
  running LLaMA 1B on-CPU inference across 4 vCPUs while streaming tokens
  in real-time directly from host physical memory throughout host live
  kernel updates. Installed to `output_<arch>/bin/ovm_demo`.
- **configs/\<arch\>/linux_host_config_fragment**: Configuration fragment for
  the host Linux kernel build, merged with default config.
- **configs/\<arch\>/linux_guest_config_fragment**: Configuration fragment for
  the guest Linux kernel build.
- **configs/\<arch\>/buildroot_config_fragment**: Configuration fragment for
  the Buildroot build.
- **Makefile**: Coordinates building `linux`, `buildroot`, and `nanovmm`,
  and automatically fetches git submodules if not already initialized.
- **README.md**: General project overview and user guide.
- **.gitmodules**: Git submodules tracking configuration.

## Git Commit Guidelines
- **Line Length**: Every line in git commit messages (subject and body) MUST
  be **72 characters or fewer**.
- **Signed-off-by**: ALWAYS sign off commits (e.g. `git commit -s`) with
  `Signed-off-by: Pasha Tatashin <pasha.tatashin@soleen.com>`.
- **No AI Tags**: NEVER add AI tags or metadata (e.g., `TAG=`, `CONV=`,
  `ORIGINAL_AUTHOR=`) to git commit messages.
- **Formatting**: Use standard Linux kernel commit message style with an
  imperative subject line and concise bulleted summary.
- **Architecture Separation**: Keep commits strictly separated by
  architecture (core KVM/Caretaker infrastructure, `x86_64`, `arm64`,
  AMD SVM, Intel VMX). Never bundle arch-specific code into core commits.
- **Review Standards**: Code reviews must strictly follow the
  maintainer criteria defined in `docs/REVIEW_PROMPT.md`.
