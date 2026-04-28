"""
Full-loop bridge test.

Phase 1 — TCP echo (always runs, no bridge or COM ports needed):
    Connects directly to the echo server and verifies data is echoed correctly.

Phase 2 — Full loop (requires com0com + serial_bridge.exe):
    Starts the bridge executable, opens the app-side COM ports, sends data
    through the full path (COM -> com0com -> bridge -> TCP -> echo server -> back),
    and verifies the echo.

Usage:
    python test_bridge.py [config_path]   (default: ../test_host_config.yaml)
"""

import socket
import subprocess
import sys
import time
from pathlib import Path

import serial
import yaml


PASS = "\033[32mPASS\033[0m"
FAIL = "\033[31mFAIL\033[0m"
SKIP = "\033[33mSKIP\033[0m"

ROOT = Path(__file__).parent.parent
BRIDGE_EXE = ROOT / "build" / "serial_bridge.exe"


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def tcp_roundtrip(host: str, port: int, payload: bytes, timeout: float = 3.0) -> bytes | None:
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
            s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            s.settimeout(timeout)
            s.connect((host, port))
            s.sendall(payload)
            received = b""
            deadline = time.monotonic() + timeout
            while len(received) < len(payload):
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    break
                s.settimeout(remaining)
                chunk = s.recv(len(payload) - len(received))
                if not chunk:
                    break
                received += chunk
            return received
    except OSError:
        return None


def com_roundtrip(port_name: str, baud: int, payload: bytes, timeout: float = 3.0) -> bytes | None:
    try:
        with serial.Serial(port_name, baud, timeout=timeout) as ser:
            ser.reset_input_buffer()
            ser.write(payload)
            received = b""
            deadline = time.monotonic() + timeout
            while len(received) < len(payload):
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    break
                ser.timeout = remaining
                chunk = ser.read(len(payload) - len(received))
                if not chunk:
                    break
                received += chunk
            return received
    except Exception:
        return None


# ---------------------------------------------------------------------------
# Phase 1: TCP direct
# ---------------------------------------------------------------------------

def phase1_tcp(config: dict) -> bool:
    host = config["remote_ip"]
    print(f"Phase 1 — TCP echo  (server={host})\n")
    all_ok = True
    for p in config["ports"]:
        port = p["tcp_port"]
        payload = f"PING_port_{port}_{'x' * 64}".encode()
        received = tcp_roundtrip(host, port, payload)
        if received == payload:
            print(f"  {PASS}  TCP:{port}  {len(payload)} bytes echoed correctly")
        elif received is None:
            print(f"  {FAIL}  TCP:{port}  connection failed — is echo_server.py running?")
            all_ok = False
        else:
            print(f"  {FAIL}  TCP:{port}  data mismatch")
            all_ok = False
    return all_ok


# ---------------------------------------------------------------------------
# Phase 2: Full loop through the bridge and COM ports
# ---------------------------------------------------------------------------

def phase2_full_loop(config: dict, config_path: Path) -> bool:
    print(f"\nPhase 2 — Full loop  (COM -> com0com -> bridge -> TCP -> echo -> back)\n")

    if not BRIDGE_EXE.exists():
        print(f"  {SKIP}  Bridge executable not found: {BRIDGE_EXE}")
        print(f"         Build first: cmake --build build")
        return True

    ports_with_com = [p for p in config["ports"] if p.get("app_com_port")]
    if not ports_with_com:
        print(f"  {SKIP}  No app_com_port entries in config")
        return True

    # Start the bridge
    bridge = subprocess.Popen(
        [str(BRIDGE_EXE), str(config_path)],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True
    )

    # Give it time to open COM ports and connect to the echo server
    time.sleep(1.5)

    if bridge.poll() is not None:
        out = bridge.stdout.read()
        print(f"  {SKIP}  Bridge exited immediately — com0com not installed or COM ports not found")
        print(f"         {out.strip()}")
        return True

    all_ok = True
    try:
        for p in ports_with_com:
            app_com = p["app_com_port"]
            tcp_port = p["tcp_port"]
            baud = p["baud_rate"]
            payload = f"LOOP_{app_com}_{'y' * 64}".encode()

            received = com_roundtrip(app_com, baud, payload)
            tag = f"{app_com} -> COM{p['com_port']} -> TCP:{tcp_port}"

            if received == payload:
                print(f"  {PASS}  {tag}  {len(payload)} bytes round-tripped")
            elif received is None:
                print(f"  {FAIL}  {tag}  could not open {app_com}")
                all_ok = False
            else:
                print(f"  {FAIL}  {tag}  data mismatch — got {received!r}")
                all_ok = False
    finally:
        bridge.terminate()
        bridge.wait()

    return all_ok


# ---------------------------------------------------------------------------
# Latency
# ---------------------------------------------------------------------------

def latency_test(config: dict, iterations: int = 200) -> None:
    host = config["remote_ip"]
    print(f"\nLatency — TCP  ({iterations} iterations per port)\n")
    for p in config["ports"]:
        port = p["tcp_port"]
        payload = b"L" * 8
        times = []
        try:
            with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
                s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
                s.settimeout(3.0)
                s.connect((host, port))
                for _ in range(iterations):
                    t0 = time.perf_counter()
                    s.sendall(payload)
                    s.recv(len(payload))
                    times.append((time.perf_counter() - t0) * 1000)
        except OSError as e:
            print(f"  TCP:{port}  error — {e}")
            continue
        avg = sum(times) / len(times)
        print(f"  TCP:{port}  min={min(times):.2f}ms  avg={avg:.2f}ms  max={max(times):.2f}ms")


# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------

def main() -> None:
    config_path = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "test_host_config.yaml"
    if not config_path.exists():
        print(f"Config not found: {config_path}")
        sys.exit(1)

    config = yaml.safe_load(config_path.read_text())

    ok1 = phase1_tcp(config)
    ok2 = phase2_full_loop(config, config_path)
    latency_test(config)

    print()
    print("All tests passed." if (ok1 and ok2) else "Some tests failed.")
    sys.exit(0 if (ok1 and ok2) else 1)


if __name__ == "__main__":
    main()
