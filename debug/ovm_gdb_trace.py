#!/usr/bin/env python3
"""
OrphanVM Automated GDB Tracer
Traces kernel exceptions, systemd execution, and caretaker threads across kexec.
"""

import argparse
import os
import subprocess
import sys
import time

def ssh(cmd, port=2222):
    return subprocess.run(
        ["ssh", "-p", str(port), "-o", "StrictHostKeyChecking=no",
         "-o", "UserKnownHostsFile=/dev/null", "-o", "ConnectTimeout=3",
         "-o", "LogLevel=ERROR", "root@localhost", cmd],
        capture_output=True, text=True
    )

def main():
    parser = argparse.ArgumentParser(description="OrphanVM GDB Tracer")
    parser.add_argument("target", nargs="?", default="amd", choices=["amd", "intel", "arm"], help="Target architecture (default: amd)")
    parser.add_argument("--gdb-port", type=int, default=1234, help="GDB stub port (default: 1234)")
    parser.add_argument("--ssh-port", type=int, default=2222, help="Host SSH port (default: 2222)")
    args = parser.parse_args()

    project_root = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
    build_arch = "arm64" if args.target == "arm" else "x86_64"
    vmlinux = os.path.join(project_root, f"output_{build_arch}/linux_host/vmlinux")

    os.system("killall -9 qemu-system-x86_64 qemu-system-aarch64 gdb-multiarch gdb 2>/dev/null")
    time.sleep(1)

    qemu_cmd = [
        "bash", "-c",
        f"export QEMU_EXTRA_OPTS='-no-reboot' && cd {project_root} && source env.sh {args.target} && "
        f"ovm_qemu -C 3 -M 4G -c 2 -m 256M -B -O --nokaslr --gdb --gdb-port {args.gdb_port}"
    ]
    print(f"[1] Starting {args.target.upper()} QEMU with GDB on port {args.gdb_port}...")
    qemu_proc = subprocess.Popen(qemu_cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)

    print(f"[2] Waiting for SSH on port {args.ssh_port}...")
    for _ in range(60):
        res = ssh("echo ready", port=args.ssh_port)
        if res.returncode == 0 and "ready" in res.stdout:
            print("[2] Host SSH is ready!")
            break
        time.sleep(2)
    else:
        print("Error: SSH connection timed out!")
        qemu_proc.kill()
        sys.exit(1)

    print("[3] Suspending guest to orphan on CPU 1 (vcpu,cpu1)...")
    cmd = "FIFO=$(ls /tmp/nvmm_*.fifo 2>/dev/null | head -n 1); echo 'suspend vcpu,cpu1' > $FIFO; sleep 1"
    ssh(cmd, port=args.ssh_port)

    print("[4] Staging kexec kernel...")
    kimg = "Image" if args.target == "arm" else "bzImage"
    kexec_script = f"""
mkdir -p /mnt/boot 2>/dev/null || true
mount -o ro /dev/vdb /mnt/boot 2>/dev/null || true
kexec -u 2>/dev/null || true
sync; echo 3 > /proc/sys/vm/drop_caches 2>/dev/null || true
CMDLINE=$(cat /proc/cmdline 2>/dev/null | sed -E 's/ovm_guest[^ ]*//g' | sed -E 's/panic=[0-9]+/panic=0/g')
CMDLINE=$(echo "$CMDLINE ovm_guest=none nokaslr loglevel=7 ignore_loglevel earlyprintk=ttyS0,115200 panic=0" | tr -s ' ')
kexec -s -l /mnt/boot/{kimg} --command-line="$CMDLINE" || kexec -l /mnt/boot/{kimg} --command-line="$CMDLINE"
umount /mnt/boot 2>/dev/null || true
"""
    ssh(kexec_script, port=args.ssh_port)

    print("[5] Attaching GDB breakpoints...")
    gdb_script = f"""
target remote :{args.gdb_port}
hbreak run_init_process
hbreak panic
hbreak oops_end
hbreak exc_page_fault
hbreak exc_general_protection
continue
"""
    with open("/tmp/gdb_trace.cmd", "w") as f:
        f.write(gdb_script)

    gdb_bin = "gdb-multiarch" if subprocess.run(["which", "gdb-multiarch"], capture_output=True).returncode == 0 else "gdb"
    gdb_proc = subprocess.Popen([gdb_bin, "-nx", "-x", "/tmp/gdb_trace.cmd", vmlinux], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    time.sleep(2)

    print("[6] Triggering kexec live update...")
    exec_cmd = "nohup sh -c 'sleep 0.5; systemctl stop ovm-guest.service 2>/dev/null || true; systemctl disable ovm-guest.service 2>/dev/null || true; systemctl stop systemd-journald.socket systemd-journald.service 2>/dev/null || true; sync; echo 3 > /proc/sys/vm/drop_caches 2>/dev/null || true; mount -o remount,ro / 2>/dev/null || true; sync; sleep 0.2; kexec -e' </dev/null >/dev/null 2>&1 &"
    ssh(exec_cmd, port=args.ssh_port)

    print("[7] Waiting 15s for kexec transition...")
    time.sleep(15)

    print("[8] Collecting GDB state and thread backtraces...")
    gdb_proc.terminate()
    inspect_script = f"""
target remote :{args.gdb_port}
info threads
thread apply all bt 10
thread apply all info registers
quit
"""
    with open("/tmp/gdb_inspect.cmd", "w") as f:
        f.write(inspect_script)

    insp = subprocess.run([gdb_bin, "-batch", "-nx", "-x", "/tmp/gdb_inspect.cmd", vmlinux], capture_output=True, text=True)
    print("\n--- GDB THREAD & REGISTER REPORT ---")
    print(insp.stdout)

    os.system("killall -9 qemu-system-x86_64 qemu-system-aarch64 2>/dev/null")
    qemu_proc.kill()

if __name__ == "__main__":
    main()
