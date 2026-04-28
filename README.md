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

**Windows host:**

- [MSYS2](https://www.msys2.org) with ucrt64 toolchain
- [com0com](https://sourceforge.net/projects/com0com/) — creates virtual COM port pairs
- Python 3 + `pip install pyyaml pyserial` — for tests

**Linux remote (Jetson):**

- GCC, CMake, `libyaml-cpp-dev`

---

## Windows — Build

### 1. Install MSYS2 toolchain

Open the **ucrt64** shell (`C:\msys64\ucrt64.exe`) and run:

```bash
pacman -S mingw-w64-ucrt-x86_64-gcc \
          mingw-w64-ucrt-x86_64-cmake \
          mingw-w64-ucrt-x86_64-ninja \
          mingw-w64-ucrt-x86_64-yaml-cpp
```

### 2. Build

Open a ucrt64 shell in the project directory (paste into PowerShell or `Win+R`):

```bat
C:\msys64\msys2_shell.cmd -ucrt64 -here -where "C:\path\to\Enprocess"
```

Then inside that shell:

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Output: `build/serial_bridge.exe`

---

## Windows — com0com Setup

com0com creates **pairs** of virtual COM ports — one end for your app, the other for the bridge.

Run the setup script as **Administrator** to create the pairs automatically from your config:

```bat
python test/setup_com_ports.py test_host_config.yaml
```

To remove them:

```bat
python test/setup_com_ports.py --remove test_host_config.yaml
```

Or create pairs manually from an admin command prompt:

```bat
cd "C:\Program Files (x86)\com0com"
setupc install PortName=COM10 PortName=COM11
setupc install PortName=COM12 PortName=COM13
setupc install PortName=COM14 PortName=COM15
```

Verify in **Device Manager → Ports (COM & LPT)** after setup.

---

## Windows — Configure

Edit `host_config.yaml`:

```yaml
remote_ip: 192.168.1.100
ports:
  - com_port: COM11
    tcp_port: 5000
    baud_rate: 115200
  - com_port: COM13
    tcp_port: 5001
    baud_rate: 9600
  - com_port: COM15
    tcp_port: 5002
    baud_rate: 57600
```

| Field       | Description                                    |
|-------------|------------------------------------------------|
| `remote_ip` | IP address of the remote device (TCP server)   |
| `com_port`  | Bridge-side port of the com0com pair           |
| `tcp_port`  | TCP port the remote device is listening on     |
| `baud_rate` | Baud rate to configure on the virtual COM port |

---

## Windows — Run

```bash
build/serial_bridge.exe host_config.yaml
```

The bridge connects to the remote device on each configured TCP port and starts forwarding traffic. If a port fails to open or connect it is skipped; the rest continue.

---

## Testing

The test suite has two phases that run automatically:

**Phase 1 — TCP echo** (no com0com needed)  
Connects directly to the echo server, verifies data is echoed correctly.

**Phase 2 — Full loop** (requires com0com + COM ports set up)  
Starts `serial_bridge.exe`, opens the app-side COM ports, and verifies data travels the complete path:

```text
COM10 → com0com → COM11 → bridge → TCP → echo server → TCP → bridge → COM11 → com0com → COM10
```

### Run tests

```bash
# Terminal 1: start the fake remote device
python test/echo_server.py test_host_config.yaml

# Terminal 2: run all tests
python test/test_bridge.py test_host_config.yaml
```

### Expected output

```text
Phase 1 — TCP echo  (server=127.0.0.1)

  PASS  TCP:5000  79 bytes echoed correctly
  PASS  TCP:5001  79 bytes echoed correctly
  PASS  TCP:5002  79 bytes echoed correctly

Phase 2 — Full loop  (COM -> com0com -> bridge -> TCP -> echo -> back)

  PASS  COM10 -> COM11 -> TCP:5000  75 bytes round-tripped
  PASS  COM12 -> COM13 -> TCP:5001  75 bytes round-tripped
  PASS  COM14 -> COM15 -> TCP:5002  75 bytes round-tripped

Latency — TCP  (200 iterations per port)

  TCP:5000  min=0.04ms  avg=0.09ms  max=0.60ms
  TCP:5001  min=0.04ms  avg=0.07ms  max=0.37ms
  TCP:5002  min=0.04ms  avg=0.08ms  max=0.38ms

All tests passed.
```

Phase 2 is skipped automatically if com0com is not installed.

---

## Remote agent (Linux / Jetson)

The remote agent runs on the target Linux device. It listens on TCP ports and bridges each incoming connection to a real serial port.

### Build

```bash
sudo apt install libyaml-cpp-dev   # Ubuntu / Jetson
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Output: `build/serial_bridge_remote`

### Configure

Edit `remote_config.yaml`:

```yaml
listen_ip: 0.0.0.0
ports:
  - serial_device: /dev/ttyUSB0
    tcp_port: 5000
    baud_rate: 115200
  - serial_device: /dev/ttyUSB1
    tcp_port: 5001
    baud_rate: 9600
  - serial_device: /dev/ttyUSB2
    tcp_port: 5002
    baud_rate: 57600
```

| Field           | Description                                    |
|-----------------|------------------------------------------------|
| `listen_ip`     | Interface to bind on (`0.0.0.0` = all)         |
| `serial_device` | Real serial port on the Linux device           |
| `tcp_port`      | TCP port to listen on (must match host config) |
| `baud_rate`     | Baud rate to configure on the serial port      |

Jetson hardware UARTs: `/dev/ttyTHS0`, `/dev/ttyTHS1`, etc.  
USB-serial adapters: `/dev/ttyUSB0`, `/dev/ttyUSB1`, etc.

### Run

```bash
./build/serial_bridge_remote remote_config.yaml
```

When the Windows bridge connects the agent opens the mapped serial device, bridges traffic bidirectionally, then waits for the next connection.

---

## Project structure

```text
├── host/
│   └── main.cpp              # Windows bridge (virtual COM <-> TCP client)
├── remote/
│   └── main.cpp              # Linux agent (TCP server <-> real serial port)
├── common/
│   └── config.hpp            # YAML config parsing for both sides
├── test/
│   ├── echo_server.py        # Fake remote device (TCP echo server)
│   ├── test_bridge.py        # Two-phase test: TCP echo + full COM loop
│   └── setup_com_ports.py   # Creates/removes com0com pairs from config
├── host_config.yaml          # Windows bridge config
├── remote_config.yaml        # Linux agent config
├── test_host_config.yaml     # Test config (localhost, includes app COM ports)
└── CMakeLists.txt
```
