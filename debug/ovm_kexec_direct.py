#!/usr/bin/env python3
"""
OrphanVM Direct Kexec Diagnostics Harness
Manually drives a host VM through guest preservation, kexec live update, and state verification.
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
    parser = argparse.ArgumentParser(description="OrphanVM Direct Kexec Diagnostics")
    parser.add_argument("target", nargs="?", default="amd", choices=["amd", "intel", "arm"], help="Target architecture (default: amd)")
    parser.add_argument("--strategy", default="vcpu,cpu1", help="Preservation strategy (default: vcpu,cpu1)")
    parser.add_argument("--ssh-port", type=int, default=2222, help="Host SSH port (default: 2222)")
    args = parser.parse_args()

    project_root = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
    os.system("killall -9 qemu-system-x86_64 qemu-system-aarch64 2>/dev/null")
    time.sleep(1)

    qemu_cmd = f"bash -c 'cd {project_root} && source env.sh {args.target} && ovm_qemu -C 3 -M 4G -c 2 -m 256M -B -O --nokaslr'"
    print(f"[1] Starting {args.target.upper()} Host VM...")
    qemu_proc = subprocess.Popen(qemu_cmd, shell=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)

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

    print(f"[3] Preserving guest VM with strategy '{args.strategy}'...")
    ssh(f"FIFO=$(ls /tmp/nvmm_*.fifo 2>/dev/null | head -n 1); echo 'suspend {args.strategy}' > $FIFO", port=args.ssh_port)
    time.sleep(1)

    online_before = ssh("cat /sys/devices/system/cpu/online", port=args.ssh_port).stdout.strip()
    print(f"[3] Host online CPUs before kexec: {online_before}")

    print("[4] Loading kexec kernel...")
    kimg = "Image" if args.target == "arm" else "bzImage"
    kexec_script = f"""
mkdir -p /mnt/boot 2>/dev/null || true
mount -o ro /dev/vdb /mnt/boot 2>/dev/null || true
kexec -u 2>/dev/null || true
sync; echo 3 > /proc/sys/vm/drop_caches 2>/dev/null || true
CMDLINE=$(cat /proc/cmdline 2>/dev/null | sed -E 's/ovm_guest[^ ]*//g' | sed -E 's/panic=[0-9]+/panic=0/g')
CMDLINE=$(echo "$CMDLINE ovm_guest=none earlyprintk=ttyS0,115200 panic=0 systemd.crash_reboot=no systemd.log_target=console systemd.journald.forward_to_console=1" | tr -s ' ')
kexec -s -l /mnt/boot/{kimg} --command-line="$CMDLINE" || kexec -l /mnt/boot/{kimg} --command-line="$CMDLINE"
umount /mnt/boot 2>/dev/null || true
"""
    ssh(kexec_script, port=args.ssh_port)

    pre_boot_id = ssh("cat /proc/sys/kernel/random/boot_id", port=args.ssh_port).stdout.strip()
    print(f"[4] Pre-kexec Boot ID: {pre_boot_id}")

    print("[5] Triggering kexec live update...")
    trigger_cmd = "nohup sh -c 'sleep 0.5; systemctl stop ovm-guest.service 2>/dev/null || true; systemctl disable ovm-guest.service 2>/dev/null || true; systemctl stop systemd-journald.socket systemd-journald.service 2>/dev/null || true; sync; echo 3 > /proc/sys/vm/drop_caches 2>/dev/null || true; mount -o remount,ro / 2>/dev/null || true; sync; sleep 0.2; kexec -e' </dev/null >/dev/null 2>&1 &"
    ssh(trigger_cmd, port=args.ssh_port)

    print("[6] Waiting for incoming kernel to boot...")
    time.sleep(15)
    post_boot_id = ""
    for _ in range(40):
        res = ssh("cat /proc/sys/kernel/random/boot_id", port=args.ssh_port)
        if res.returncode == 0 and res.stdout.strip():
            post_boot_id = res.stdout.strip()
            if post_boot_id != pre_boot_id:
                print(f"[6] Incoming kernel reached! Post-kexec Boot ID: {post_boot_id}")
                break
        time.sleep(1)
    else:
        print("Error: Incoming kernel SSH timed out!")
        qemu_proc.kill()
        sys.exit(1)

    print("\n==================== INCOMING KERNEL DIAGNOSTICS ====================")
    print(">>> /proc/cmdline:")
    print(ssh("cat /proc/cmdline", port=args.ssh_port).stdout)

    print(">>> Host Online CPUs (/sys/devices/system/cpu/online):")
    print(ssh("cat /sys/devices/system/cpu/online", port=args.ssh_port).stdout)

    print(">>> Preserved CPUs (/sys/devices/system/cpu/preserved):")
    print(ssh("cat /sys/devices/system/cpu/preserved 2>/dev/null || echo 'Sysfs preserved CPUs not available'", port=args.ssh_port).stdout)

    print(">>> NanoVMM PID Status:")
    print(ssh("pidof nvmm || echo 'NanoVMM is not running (as expected in incoming gap)'", port=args.ssh_port).stdout)

    os.system("killall -9 qemu-system-x86_64 qemu-system-aarch64 2>/dev/null")
    qemu_proc.kill()

if __name__ == "__main__":
    main()
