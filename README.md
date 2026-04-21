# Serial Bridge

Tunnels virtual COM ports on a Windows PC over TCP to a remote device (Jetson, STM32, etc.).  
The remote device acts as a **TCP server**; this bridge connects to it as a client.

```text
[Your App] → [COM10 (virtual)] ──┐
[Your App] → [COM12 (virtual)] ──┤ com0com pairs
[Your App] → [COM14 (virtual)] ──┘
                                   serial_bridge.exe
                                         │ TCP
                               ┌─────────┴─────────┐
                          port 5000            port 5001 ...
                               │
                         Remote Device
                     (TCP server, e.g. Jetson)
```

---

## Requirements

- Windows 10/11
- [MSYS2](https://www.msys2.org) with ucrt64 toolchain
- [com0com](https://sourceforge.net/projects/com0com/) — creates virtual COM port pairs

### Install MSYS2 toolchain

Open the **ucrt64** shell (`C:\msys64\ucrt64.exe`) and run:

```bash
pacman -S mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja
```

---

## Build

Open the ucrt64 shell in the project directory — paste this into PowerShell or `Win+R`:

```bat
C:\msys64\msys2_shell.cmd -ucrt64 -here -where "C:\path\to\Enprocess"
```

Then build:

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Output: `build/serial_bridge.exe`

---

## com0com Setup

com0com creates **pairs** of virtual COM ports — one end for your app, the other for the bridge.

Run the setup script as Administrator to create the pairs from your config automatically:

```bat
python test/setup_com_ports.py test_host_config.json
```

Or create them manually from an admin command prompt:

```bat
cd "C:\Program Files (x86)\com0com"
setupc install PortName=COM10 PortName=COM11
setupc install PortName=COM12 PortName=COM13
setupc install PortName=COM14 PortName=COM15
```

To remove pairs:

```bat
python test/setup_com_ports.py --remove test_host_config.json
```

Verify in **Device Manager → Ports (COM & LPT)** after setup.

---

## Configuration

Edit `host_config.json` before running:

```json
{
    "remote_ip": "192.168.1.100",
    "ports": [
        { "com_port": "COM11", "tcp_port": 5000, "baud_rate": 115200 },
        { "com_port": "COM13", "tcp_port": 5001, "baud_rate": 9600   },
        { "com_port": "COM15", "tcp_port": 5002, "baud_rate": 57600  }
    ]
}
```

| Field       | Description                                    |
|-------------|------------------------------------------------|
| `remote_ip` | IP address of the remote device (TCP server)   |
| `com_port`  | Bridge-side port of the com0com pair           |
| `tcp_port`  | TCP port the remote device is listening on     |
| `baud_rate` | Baud rate to configure on the virtual COM port |

Add or remove entries in `ports` to change the number of channels.

---

## Run

```bash
build/serial_bridge.exe host_config.json
```

The bridge connects to the remote device on each configured TCP port and starts forwarding traffic. If a connection fails it skips that port and continues with the rest.

---

## Testing

Tests the TCP interface between the bridge and the remote device — no com0com required.  
`echo_server.py` simulates the remote device; `test_bridge.py` connects to it and verifies data is echoed correctly, then measures round-trip latency.

### Run tests

```bash
# Terminal 1: start the fake remote device (echo server)
python test/echo_server.py test_host_config.json

# Terminal 2: run the tests
python test/test_bridge.py test_host_config.json
```

`test_host_config.json` is the same as `host_config.json` but with `remote_ip` set to `127.0.0.1`.

### Expected output

```text
Echo test  (server=127.0.0.1)

  PASS  TCP:5000  79 bytes echoed correctly
  PASS  TCP:5001  79 bytes echoed correctly
  PASS  TCP:5002  79 bytes echoed correctly

Latency test  (200 iterations per port)

  TCP:5000  min=0.03ms  avg=0.08ms  max=0.37ms
  TCP:5001  min=0.04ms  avg=0.08ms  max=0.30ms
  TCP:5002  min=0.04ms  avg=0.08ms  max=0.47ms

All tests passed.
```

---

## Remote agent (Linux / Jetson)

The remote agent runs on the target Linux device. It acts as a TCP server and bridges each incoming connection to a real serial port.

### Build (on the Linux device)

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Output: `build/serial_bridge_remote`

### Configure

Edit `remote_config.json`:

```json
{
    "listen_ip": "0.0.0.0",
    "ports": [
        { "serial_device": "/dev/ttyUSB0", "tcp_port": 5000, "baud_rate": 115200 },
        { "serial_device": "/dev/ttyUSB1", "tcp_port": 5001, "baud_rate": 9600   },
        { "serial_device": "/dev/ttyUSB2", "tcp_port": 5002, "baud_rate": 57600  }
    ]
}
```

| Field           | Description                                     |
|-----------------|-------------------------------------------------|
| `listen_ip`     | Interface to bind on (`0.0.0.0` = all)          |
| `serial_device` | Real serial port on the Linux device            |
| `tcp_port`      | TCP port to listen on (must match host config)  |
| `baud_rate`     | Baud rate to configure on the serial port       |

Jetson hardware UARTs are typically `/dev/ttyTHS0`, `/dev/ttyTHS1`, etc.  
USB-serial adapters appear as `/dev/ttyUSB0`, `/dev/ttyUSB1`, etc.

### Run (remote)

```bash
./build/serial_bridge_remote remote_config.json
```

The agent listens on each configured port. When the Windows bridge connects it opens the mapped serial device, bridges traffic bidirectionally, then closes the device and waits for the next connection.

---

## Project structure

```text
├── host/
│   └── main.cpp              # Windows bridge (virtual COM <-> TCP client)
├── remote/
│   └── main.cpp              # Linux agent (TCP server <-> real serial port)
├── common/
│   └── config.hpp            # JSON config parsing for both sides
├── test/
│   ├── echo_server.py        # Fake remote device (TCP echo server)
│   ├── test_bridge.py        # Automated test suite
│   └── setup_com_ports.py   # Creates/removes com0com pairs from config
├── host_config.json          # Windows bridge config
├── remote_config.json        # Linux agent config
├── test_host_config.json     # Test config (localhost)
└── CMakeLists.txt
```
