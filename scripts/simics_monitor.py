import threading
import time
import socket
import json
import conf
import simics

def monitor_loop():
    last_steps = []
    time.sleep(1)
    while True:
        try:
            num_cores = len(conf.qsp.mb.cpu0.core)
            current_steps = [conf.qsp.mb.cpu0.core[i][0].steps for i in range(num_cores)]
            if len(last_steps) != num_cores:
                last_steps = current_steps
            deltas = [current_steps[i] - last_steps[i] for i in range(num_cores)]
            last_steps = current_steps
            
            rips = [hex(conf.qsp.mb.cpu0.core[i][0].rip) for i in range(num_cores)]
            smm = [conf.qsp.mb.cpu0.core[i][0].in_smm for i in range(num_cores)]
            core_strs = [f"C{i}: +{deltas[i]:,} ({rips[i]}, smm={smm[i]})" for i in range(num_cores)]
            print(f"[SIMICS MONITOR] {' | '.join(core_strs)}", flush=True)
        except Exception as e:
            print(f"[SIMICS MONITOR] err: {e}", flush=True)
        time.sleep(2)

def get_cpu_status(idx):
    c = conf.qsp.mb.cpu0.core[idx][0]
    return {
        "core": idx,
        "rip": hex(c.rip),
        "steps": c.steps,
        "in_smm": getattr(c, "in_smm", -1),
        "block_smi": getattr(c, "block_smi", -1),
        "smi_count": getattr(c, "smi_count", -1),
        "smm_base": hex(getattr(c, "smm_base", 0)),
        "cr0": hex(getattr(c, "cr0", 0)),
        "cr3": hex(getattr(c, "cr3", 0)),
        "cr4": hex(getattr(c, "cr4", 0)),
    }

def do_smi(cpu_idx):
    c = conf.qsp.mb.cpu0.core[cpu_idx][0]
    c.ports.SMI.signal.signal_raise()
    c.ports.SMI.signal.signal_lower()
    return f"SMI pulsed on CPU {cpu_idx}"

def cmd_server():
    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try:
        server.bind(("127.0.0.1", 4001))
        server.listen(5)
        print("[SIMICS MONITOR] Command server listening on port 4001", flush=True)
    except Exception as e:
        print(f"[SIMICS MONITOR] Failed to bind command server: {e}", flush=True)
        return

    while True:
        client, _ = server.accept()
        threading.Thread(target=handle_client, args=(client,), daemon=True).start()

def handle_client(client):
    try:
        rfile = client.makefile("r", encoding="utf-8")
        wfile = client.makefile("w", encoding="utf-8")
        for line in rfile:
            line = line.strip()
            if not line:
                continue
            if line == "quit":
                break
            parts = line.split(maxsplit=1)
            cmd = parts[0]
            arg = parts[1] if len(parts) > 1 else ""

            if cmd == "status":
                num_cores = len(conf.qsp.mb.cpu0.core)
                st = [get_cpu_status(i) for i in range(num_cores)]
                wfile.write(json.dumps(st) + "\n")
                wfile.flush()
            elif cmd == "smi":
                target = arg.strip()
                if target == "all":
                    res = []
                    for i in range(len(conf.qsp.mb.cpu0.core)):
                        res.append(do_smi(i))
                    wfile.write(json.dumps(res) + "\n")
                else:
                    idx = int(target)
                    res = do_smi(idx)
                    wfile.write(json.dumps(res) + "\n")
                wfile.flush()
            elif cmd == "eval":
                try:
                    out = eval(arg)
                    wfile.write(json.dumps(str(out)) + "\n")
                except Exception as ex:
                    wfile.write(json.dumps(f"ERR: {ex}") + "\n")
                wfile.flush()
            else:
                wfile.write(json.dumps(f"UNKNOWN CMD: {cmd}") + "\n")
                wfile.flush()
    except Exception as e:
        pass
    finally:
        client.close()

t1 = threading.Thread(target=monitor_loop, daemon=True)
t1.start()

t2 = threading.Thread(target=cmd_server, daemon=True)
t2.start()

print("[SIMICS MONITOR] Monitor and control threads started.", flush=True)
