"""
Tests the TCP interface between the Windows bridge and the remote device.

Connects directly to the echo server (which simulates the remote device),
sends data, and verifies it comes back correctly.

Usage:
    python test_bridge.py [config_path]   (default: ../host_config.json)
"""

import json
import socket
import sys
import time
from pathlib import Path


PASS = "\033[32mPASS\033[0m"
FAIL = "\033[31mFAIL\033[0m"


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


def test_echo(config: dict) -> bool:
    host = config["remote_ip"]
    print(f"Echo test  (server={host})\n")
    all_ok = True
    for p in config["ports"]:
        port = p["tcp_port"]
        payload = f"TEST_port_{port}_{'x' * 64}".encode()
        received = tcp_roundtrip(host, port, payload)
        if received == payload:
            print(f"  {PASS}  TCP:{port}  {len(payload)} bytes echoed correctly")
        elif received is None:
            print(f"  {FAIL}  TCP:{port}  connection failed — is echo_server.py running?")
            all_ok = False
        else:
            print(f"  {FAIL}  TCP:{port}  data mismatch — sent {payload!r}, got {received!r}")
            all_ok = False
    return all_ok


def latency_test(config: dict, iterations: int = 200) -> None:
    host = config["remote_ip"]
    print(f"\nLatency test  ({iterations} iterations per port)\n")
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


def main() -> None:
    config_path = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).parent.parent / "host_config.json"
    if not config_path.exists():
        print(f"Config not found: {config_path}")
        sys.exit(1)

    config = json.loads(config_path.read_text())
    config.setdefault("remote_ip", "127.0.0.1")

    ok = test_echo(config)
    latency_test(config)

    print()
    print("All tests passed." if ok else "Some tests failed.")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
