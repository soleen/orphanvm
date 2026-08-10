# OrphanVM Automated AI Agent Testing Instructions

This document provides exact executable command patterns (`bash -c "..."`) and strict timeout rules for an AI agent to execute concurrent compilation, launch host VMs, perform live updates via `ovm_test`, verify continuous guest workload preservation across host reboots, and clean up background processes upon completion.

The automated test runner is available via `ovm_test` (or `make test`), which automatically orchestrates builds, VM execution, kexec live update, and seamless guest workload verification.

---

## 1. Automated Test Suites

### OrphanVM Kexec Live Update Test (`ovm_test` / `make test`)
Run the automated kexec live update test across all supported architectures:

```bash
# Run all platforms in parallel (default)
./scripts/ovm_test

# Or via Makefile
make test

# Options:
./scripts/ovm_test [-t arm,simics,intel,amd] [--no-build] [-l <cancel_loops>] [-k <kexec_loops>]

### OrphanVM Comprehensive Matrix Test (`ovm_test --matrix`)
```bash
./scripts/ovm_test --matrix [-t arm,simics,intel,amd] [--no-build]
```

---

## 2. Timeout & Verification Enforcements

The AI Agent must strictly enforce the following timeouts and verification rules during test execution:

1. **Guest Workload Timeout (30 seconds):**
   - **Initial Boot:** After launching `ovm_qemu` or `ovm_simics`, the agent must detect the guest workload agent within **30 seconds**. If no workload is detected within 30s, **FAIL THE TEST IMMEDIATELY**.
   - **Post-Kexec Resume:** Following host `kexec` reboot, the agent must verify that the guest resumes and advances workload passes within **30 seconds** of host boot. If no progress appears within 30s after host reboot, **FAIL THE TEST IMMEDIATELY**.

2. **Kexec Live Update Timeout (60 seconds):**
   - When executing live updates, the agent must enforce a **60-second execution timeout**.
   - If live update fails to complete kernel staging, session preservation, and host reboot initiation within 60s, **FAIL THE TEST IMMEDIATELY**.

3. **Mandatory Post-Test Task & Process Cleanup:**
   - At the conclusion of testing, the AI Agent **MUST** explicitly kill all background tasks (using `manage_task Action="kill"`) and clean up host QEMU emulator instances:
     ```bash
     killall -9 qemu-system-aarch64 qemu-system-x86_64 || true
     ```

---

## 3. Concurrent Compilation via Background Tasks

Launch two background tasks to compile ARM64 and x86_64 targets simultaneously:

### Task 1: ARM64 Compilation
```bash
bash -c "source env.sh arm && make -j\$(nproc)"
```

### Task 2: x86_64 Compilation
```bash
bash -c "source env.sh amd && make -j\$(nproc)"
```

---

## 4. Intra-Kernel Start & Cancel Live Update Testing (`ovm_test -l <count> -k 0`)

`ovm_test` with `-l <count> -k 0` tests the complete intra-kernel live update start & cancel lifecycle within a single running host VM without host kexec: boots host and guest, isolates guest vCPUs to offlined physical cores (Orphan mode), verifies uninterrupted on-core execution, cancels the preservation state via FIFO resume, and validates continuous monotonic guest execution without state rewind or guest crash across multiple cycles.

```bash
# Run 1 cycle of start & cancel on AMD (default duration)
./scripts/ovm_test -t amd -n -l 1 -k 0

# Run 3 cancel cycles with 2 guest vCPUs and verbose telemetry
./scripts/ovm_test -t amd -n -c 2 -d 2 -l 3 -k 0 -v

# Options:
#   -t, --target <amd|simics|intel|arm>  Target platform (default: amd)
#   -c, --guest-cpus <count>      Number of guest vCPUs (default: 2)
#   -d, --duration <seconds>      Duration per phase in seconds (default: 2)
#   -l, --cancel-loops <count>    Number of start/cancel cycles (default: 1)
#   -k, --kexec-loops <count>     Number of kexec live update loops (default: 1; 0 for cancel only)
#   -n, --no-build                Skip rebuilding components
#   -v, --verbose                 Enable verbose console logging
```

---

## 5. OrphanVM Kexec Live Update & Resume Testing (`ovm_test`)

`ovm_test` exercises the full end-to-end OrphanVM live update lifecycle with on-core vCPU preservation across host kexec and NanoVMM resumption:

1. **Host Boot:** Boots host VM with guest VM running in orphan-capable mode (`-B -O`).
2. **Workload Detection:** Detects guest workload agent loops across all guest vCPUs in guest RAM.
3. **Pre-Orphan Baseline:** Measures pre-orphan baseline guest workload progression rate.
4. **Kernel Staging:** Transfers the new kernel binary into host memory and stages with `kexec -s -l`.
5. **Guest VM Orphan Mode:** Triggers `SIGUSR2` to NanoVMM to isolate and offline physical cores.
6. **Pre-Kexec On-Core Execution:** Verifies guest execution advances on-core while host CPUs are offlined.
7. **Host Kexec Live Update:** Triggers `kexec -e` to reboot into the incoming kernel.
8. **FLB CPU Isolation:** Validates incoming kernel FLB retrieves Caretaker mask and skips secondary bringup.
9. **In-Gap On-Core Execution:** Verifies guest execution continued uninterrupted across the kernel swap.
10. **NanoVMM Resumption:** Launches NanoVMM in the incoming kernel to adopt Caretaker CPUs and restore host cores online.
11. **Continuous Resumed Execution:** Validates guest workload continues incrementing without counter regression.
12. **Telemetry Collection:** Displays Caretaker VM exit telemetry histograms and timing.

```bash
# Run kexec live update and resume test on AMD with 2 vCPUs
./scripts/ovm_test -t amd -c 2

# Run with custom sampling duration (e.g. 5 seconds per phase)
./scripts/ovm_test -t amd -c 2 -d 5

# Options:
#   -t, --target <amd|simics|intel|arm>  Target platform (default: amd)
#   -c, --guest-cpus <count>      Number of guest vCPUs (default: 1)
#   -d, --duration <seconds>      Sampling duration in seconds (default: 2)
#   -n, --no-build                Skip rebuilding components
#   -v, --verbose                 Enable verbose console logging
```
