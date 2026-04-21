"""
Creates or removes com0com virtual COM port pairs defined in the config.

Usage:
    python setup_com_ports.py [--remove] [config_path]

    --remove      Remove all pairs listed in the config instead of creating them
    config_path   Path to config JSON (default: ../test_host_config.json)

Must be run as Administrator.
"""

import ctypes
import json
import subprocess
import sys
from pathlib import Path


SETUPC_CANDIDATES = [
    r"C:\Program Files (x86)\com0com\setupc.exe",
    r"C:\Program Files\com0com\setupc.exe",
]


def find_setupc() -> Path | None:
    for p in SETUPC_CANDIDATES:
        if Path(p).exists():
            return Path(p)
    return None


def is_admin() -> bool:
    try:
        return ctypes.windll.shell32.IsUserAnAdmin() != 0
    except Exception:
        return False


def run(setupc: Path, *args: str) -> tuple[int, str]:
    result = subprocess.run(
        [str(setupc), *args],
        capture_output=True, text=True,
        cwd=setupc.parent
    )
    output = (result.stdout + result.stderr).strip()
    return result.returncode, output


def create_pairs(setupc: Path, ports: list[dict]) -> None:
    for p in ports:
        app = p.get("app_com_port")
        bridge = p.get("com_port")
        if not app or not bridge:
            print(f"  SKIP  missing app_com_port or com_port in entry: {p}")
            continue
        print(f"  Creating pair  {app} <-> {bridge} ...", end=" ", flush=True)
        code, out = run(setupc, "install", f"PortName={app}", f"PortName={bridge}")
        print("OK" if code == 0 else f"FAILED\n    {out}")


def remove_pairs(setupc: Path, ports: list[dict]) -> None:
    # List current pairs and match by port name
    _, listing = run(setupc, "list")
    print("Current pairs:\n" + (listing or "  (none)") + "\n")

    for p in ports:
        app = p.get("app_com_port")
        bridge = p.get("com_port")
        if not app or not bridge:
            continue
        # Find the pair index that contains either port name
        pair_index = None
        for line in listing.splitlines():
            if app in line or bridge in line:
                # Line format: "       0 CNCA0 PortName=COM3"
                parts = line.split()
                if parts and parts[0].isdigit():
                    pair_index = parts[0]
                    break
        if pair_index is None:
            print(f"  SKIP  {app} <-> {bridge}  (not found in com0com)")
            continue
        print(f"  Removing pair {pair_index}  ({app} <-> {bridge}) ...", end=" ", flush=True)
        code, out = run(setupc, "remove", pair_index)
        print("OK" if code == 0 else f"FAILED\n    {out}")


def main() -> None:
    args = sys.argv[1:]
    remove = "--remove" in args
    args = [a for a in args if a != "--remove"]

    config_path = Path(args[0]) if args else Path(__file__).parent.parent / "test_host_config.json"
    if not config_path.exists():
        print(f"Config not found: {config_path}")
        sys.exit(1)

    if not is_admin():
        print("This script must be run as Administrator (com0com requires it).")
        sys.exit(1)

    setupc = find_setupc()
    if not setupc:
        print("com0com not found. Install it from https://sourceforge.net/projects/com0com/")
        sys.exit(1)

    config = json.loads(config_path.read_text())
    ports = config["ports"]

    if remove:
        print(f"Removing {len(ports)} COM pair(s)...\n")
        remove_pairs(setupc, ports)
    else:
        print(f"Creating {len(ports)} COM pair(s)...\n")
        create_pairs(setupc, ports)

    print("\nDone. Verify in Device Manager -> Ports (COM & LPT).")


if __name__ == "__main__":
    main()
