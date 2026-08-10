# OrphanVM Debugging Tools and Utilities

This directory contains standalone debugging utilities for diagnosing host kernel boot, kexec live update transitions, Caretaker vCPU round-robin scheduling, and hardware virtualization states.

---

## Tools Overview

### 1. `ovm_gdb_trace.py`
Automated GDB tracer that attaches to the host QEMU instance, sets hardware breakpoints on key kernel symbols (such as `exc_page_fault`, `run_init_process`, `panic`, and `do_double_fault`), triggers kexec live update, and dumps per-CPU backtraces and register states.

**Usage:**
```bash
python3 debug/ovm_gdb_trace.py [amd|intel|arm]
```

**Features:**
- Automatic QEMU launch with `--gdb` and `--nokaslr`.
- Breakpoint tracing on entry points, exception handlers, and task switches.
- Per-thread backtrace and control register (`CR0`, `CR2`, `CR3`, `CR4`, `EFER`) inspection.

---

### 2. `ovm_kexec_direct.py`
Direct standalone test harness that boots the host VM, launches NanoVMM, suspends the guest into asymmetric or symmetric orphan mode, performs kexec live update, and executes post-kexec health diagnostics.

**Usage:**
```bash
python3 debug/ovm_kexec_direct.py [amd|intel|arm]
```

**Features:**
- Step-by-step logging with timestamps.
- Verifies FLB CPU isolation (`/sys/devices/system/cpu/online`).
- Inspects `/sys/devices/system/cpu/preserved`.
- Verifies guest workload loop counters before and after kexec.

---

### 3. `ovm_qemu_debug.py`
Low-level QEMU exception and interrupt tracer using QEMU's `-d int,cpu_reset,guest_errors` logging facility with `-no-reboot` protection to catch triple faults, invalid MSR access, and hardware reset sources.

**Usage:**
```bash
python3 debug/ovm_qemu_debug.py [amd|intel|arm]
```

**Features:**
- Captures QEMU internal exception logs to `/tmp/qemu_debug_<port>.log`.
- Identifies exact faulting instruction pointers (`RIP`/`PC`) and exception vectors.
