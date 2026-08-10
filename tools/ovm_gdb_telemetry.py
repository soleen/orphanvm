#!/usr/bin/env python3
"""
OrphanVM GDB Telemetry and Step Debugging Tool
==============================================

Connects to QEMU or Simics GDB stubs to inspect physical CPU states,
host hypervisor context, and guest vCPU progress.

Usage Instructions:
-------------------
1. Batch / Automated Telemetry Dump:
   Run non-interactively against a running QEMU or Simics GDB stub:

     gdb-multiarch -batch -nx output_arm64/linux_host/vmlinux \\
         -ex "target remote :1234" \\
         -x tools/ovm_gdb_telemetry.py \\
         -ex "thread apply all bt 15" \\
         -ex "detach" -ex "quit"

2. Interactive GDB Debugging:
   Start GDB and source the script:

     gdb-multiarch output_arm64/linux_host/vmlinux
     (gdb) target remote :1234
     (gdb) source tools/ovm_gdb_telemetry.py

   Registered Custom Commands:
     - 'ovm-telemetry': Inspect all CPU threads, identify execution
       modes (Host Hypervisor, Guest Stage 2, Guest Workload, EL0/EL1),
       print registers, decoded instructions, ARM64 sysregs, and
       host backtraces.
     - 'ovm-step [N]': Step N instructions (default: 1) on the current
       CPU thread and report register & disassembly changes.

3. Test Suite Integration:
   Pass '-g' or '--gdb' when executing OrphanVM test scripts:

     ./scripts/ovm_test -t arm -g

   On hang detection or resumption failure, GDB will automatically
   invoke this script and capture diagnostics in the test logs.
"""

import gdb
import sys

def analyze_cpu_state():
    print("=" * 80)
    print(" OrphanVM Multi-Core GDB Telemetry & Execution State Analysis")
    print("=" * 80)

    try:
        threads = gdb.selected_inferior().threads()
    except Exception as e:
        print(f"Error accessing threads: {e}")
        return

    for t in threads:
        t.switch()
        tid = t.num
        try:
            pc_val = int(gdb.parse_and_eval("$pc"))
            sp_val = int(gdb.parse_and_eval("$sp"))
        except Exception:
            print(f"[CPU {tid}] Unable to read registers")
            continue

        mode = "UNKNOWN"
        if pc_val >= 0xffff000000000000:
            mode = "HOST KERNEL / HYPERVISOR"
        elif pc_val >= 0x40000000 and pc_val < 0x80000000:
            mode = "GUEST PHYSICAL / STAGE 2"
        elif pc_val < 0x10000000:
            mode = "GUEST USER WORKLOAD (ovm_agent)"
        else:
            mode = "GUEST VIRTUAL / EL0/EL1"

        print(f"\n--- [CPU Thread #{tid}] Mode: {mode} ---")
        print(f"  PC: 0x{pc_val:016x}   SP: 0x{sp_val:016x}")

        try:
            disas = gdb.execute("x/i $pc", to_string=True).strip()
            print(f"  Instruction: {disas}")
        except Exception:
            pass

        # Architecture-specific system registers (ARM64)
        sysregs = ["hcr_el2", "cptr_el2", "cpacr_el1", "sctlr_el1", "vbar_el1", "tpidr_el1", "vttbr_el2"]
        reg_strs = []
        for r in sysregs:
            try:
                v = int(gdb.parse_and_eval(f"${r}"))
                reg_strs.append(f"{r}=0x{v:x}")
            except Exception:
                pass
        if reg_strs:
            print(f"  Sysregs: {', '.join(reg_strs)}")

        if mode == "HOST KERNEL / HYPERVISOR":
            try:
                bt = gdb.execute("bt 6", to_string=True).strip()
                print("  Host Backtrace:")
                for line in bt.splitlines()[:6]:
                    print(f"    {line}")
            except Exception:
                pass

    print("\n" + "=" * 80)

class OrphanVMTelemetryCmd(gdb.Command):
    """Inspect all CPU threads and OrphanVM hypervisor / guest state."""
    def __init__(self):
        super(OrphanVMTelemetryCmd, self).__init__("ovm-telemetry", gdb.COMMAND_USER)

    def invoke(self, arg, from_tty):
        analyze_cpu_state()

class OrphanVMStepDebugCmd(gdb.Command):
    """Step N instructions on a specific thread and report register state changes."""
    def __init__(self):
        super(OrphanVMStepDebugCmd, self).__init__("ovm-step", gdb.COMMAND_USER)

    def invoke(self, arg, from_tty):
        args = arg.split()
        steps = int(args[0]) if len(args) > 0 else 1
        for i in range(steps):
            gdb.execute("stepi")
            pc = int(gdb.parse_and_eval("$pc"))
            disas = gdb.execute("x/i $pc", to_string=True).strip()
            print(f"[Step {i+1}/{steps}] PC=0x{pc:016x}: {disas}")

OrphanVMTelemetryCmd()
OrphanVMStepDebugCmd()
analyze_cpu_state()
