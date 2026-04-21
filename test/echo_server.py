"""
Simulates the remote device: a TCP server that echoes every byte it receives.
Reads port numbers from the bridge config file.

Usage:
    python echo_server.py [config_path]   (default: ../host_config.json)
"""

import json
import socket
import sys
import threading
from pathlib import Path


def handle_client(conn: socket.socket, addr, tcp_port: int) -> None:
    tag = f"[:{tcp_port}]"
    print(f"{tag} Connected from {addr[0]}:{addr[1]}")
    try:
        while True:
            data = conn.recv(65536)
            if not data:
                break
            conn.sendall(data)
            print(f"{tag} Echo {len(data):>5} bytes: {data[:40]!r}{'...' if len(data) > 40 else ''}")
    except OSError:
        pass
    finally:
        conn.close()
        print(f"{tag} Disconnected")


def listen(tcp_port: int) -> None:
    tag = f"[:{tcp_port}]"
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as srv:
        srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        srv.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        srv.bind(("0.0.0.0", tcp_port))
        srv.listen(1)
        print(f"{tag} Listening")
        while True:
            conn, addr = srv.accept()
            conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            threading.Thread(target=handle_client, args=(conn, addr, tcp_port), daemon=True).start()


def main() -> None:
    config_path = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).parent.parent / "host_config.json"
    config = json.loads(config_path.read_text())
    ports = [p["tcp_port"] for p in config["ports"]]

    print(f"Echo server starting on ports {ports}  (Ctrl+C to stop)")
    threads = [threading.Thread(target=listen, args=(p,), daemon=True) for p in ports]
    for t in threads:
        t.start()

    try:
        for t in threads:
            t.join()
    except KeyboardInterrupt:
        print("\nStopped.")


if __name__ == "__main__":
    main()
