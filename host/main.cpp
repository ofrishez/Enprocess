// Windows bridge: virtual COM port <-> TCP socket
// Each configured port runs two threads: serial->TCP and TCP->serial.
// Uses WaitCommEvent(EV_RXCHAR) for minimal read latency on the serial side.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>

#include <atomic>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "common/config.hpp"

#ifdef _MSC_VER
#pragma comment(lib, "ws2_32.lib")
#endif

// ---------------------------------------------------------------------------
// Serial helpers
// ---------------------------------------------------------------------------

static HANDLE open_com(const std::string& name, int baud_rate) {
    std::string path = "\\\\.\\" + name;
    HANDLE h = CreateFileA(path.c_str(),
                           GENERIC_READ | GENERIC_WRITE,
                           0, nullptr,
                           OPEN_EXISTING,
                           FILE_FLAG_OVERLAPPED,
                           nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        std::cerr << "[ERROR] Cannot open " << name
                  << " (err=" << GetLastError() << ")\n";
        return h;
    }

    DCB dcb = {};
    dcb.DCBlength = sizeof(DCB);
    GetCommState(h, &dcb);
    dcb.BaudRate = static_cast<DWORD>(baud_rate);
    dcb.ByteSize = 8;
    dcb.Parity   = NOPARITY;
    dcb.StopBits = ONESTOPBIT;
    dcb.fBinary  = TRUE;
    SetCommState(h, &dcb);

    // Queues large enough to not drop bytes at high baud rates
    SetupComm(h, 65536, 65536);

    return h;
}

// ---------------------------------------------------------------------------
// TCP helpers
// ---------------------------------------------------------------------------

static SOCKET make_tcp_socket() {
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return s;
    int flag = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY,
               reinterpret_cast<char*>(&flag), sizeof(flag));
    return s;
}

static SOCKET connect_tcp(const std::string& ip, int port) {
    SOCKET s = make_tcp_socket();
    if (s == INVALID_SOCKET) return s;

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(static_cast<u_short>(port));
    inet_pton(AF_INET, ip.c_str(), &addr.sin_addr);

    if (connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        std::cerr << "[ERROR] TCP connect to " << ip << ":" << port
                  << " failed (err=" << WSAGetLastError() << ")\n";
        closesocket(s);
        return INVALID_SOCKET;
    }
    return s;
}

// ---------------------------------------------------------------------------
// Per-port bridge state
// ---------------------------------------------------------------------------

struct Bridge {
    std::string      label;     // e.g. "COM5:5000"
    HANDLE           hCom  = INVALID_HANDLE_VALUE;
    SOCKET           sock  = INVALID_SOCKET;
    std::atomic<bool> running{true};
};

// ---------------------------------------------------------------------------
// serial -> TCP thread
// Uses WaitCommEvent(EV_RXCHAR) so the thread wakes the instant bytes arrive.
// ---------------------------------------------------------------------------

static void serial_to_tcp(Bridge* b) {
    // ReadFile blocks until at least one byte arrives or 200 ms passes.
    // This works on both real and virtual (com0com) ports unlike WaitCommEvent.
    COMMTIMEOUTS ct = {};
    ct.ReadIntervalTimeout        = MAXDWORD;
    ct.ReadTotalTimeoutMultiplier = MAXDWORD;
    ct.ReadTotalTimeoutConstant   = 200;
    SetCommTimeouts(b->hCom, &ct);

    constexpr DWORD BUF = 65536;
    auto buf = std::make_unique<char[]>(BUF);

    OVERLAPPED ov = {};
    ov.hEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);

    while (b->running) {
        DWORD got = 0;
        ResetEvent(ov.hEvent);

        if (!ReadFile(b->hCom, buf.get(), BUF, &got, &ov)) {
            if (GetLastError() != ERROR_IO_PENDING) {
                b->running = false;
                break;
            }
            WaitForSingleObject(ov.hEvent, INFINITE);
            if (!GetOverlappedResult(b->hCom, &ov, &got, FALSE)) {
                b->running = false;
                break;
            }
        }

        if (got == 0) continue;

        // Forward to TCP — flush entire chunk atomically
        DWORD sent = 0;
        while (sent < got && b->running) {
            int n = send(b->sock, buf.get() + sent,
                         static_cast<int>(got - sent), 0);
            if (n <= 0) { b->running = false; break; }
            sent += static_cast<DWORD>(n);
        }
    }

    CloseHandle(ov.hEvent);
    std::cout << "[" << b->label << "] serial->tcp thread exiting\n";
}

// ---------------------------------------------------------------------------
// TCP -> serial thread
// Blocks on recv(); writes to COM with overlapped I/O.
// ---------------------------------------------------------------------------

static void tcp_to_serial(Bridge* b) {
    constexpr int BUF = 65536;
    auto buf = std::make_unique<char[]>(BUF);

    while (b->running) {
        int n = recv(b->sock, buf.get(), BUF, 0);
        if (n <= 0) {
            b->running = false;
            break;
        }

        OVERLAPPED ov = {};
        ov.hEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);

        DWORD written = 0;
        if (!WriteFile(b->hCom, buf.get(), static_cast<DWORD>(n),
                       &written, &ov)) {
            if (GetLastError() == ERROR_IO_PENDING)
                GetOverlappedResult(b->hCom, &ov, &written, TRUE);
        }
        CloseHandle(ov.hEvent);
    }

    std::cout << "[" << b->label << "] tcp->serial thread exiting\n";
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(int argc, char* argv[]) {
    const std::string config_path = (argc > 1) ? argv[1] : "host_config.yaml";

    HostConfig cfg;
    try {
        cfg = load_host_config(config_path);
    } catch (const std::exception& e) {
        std::cerr << "[ERROR] " << e.what() << "\n";
        return 1;
    }

    // Init Winsock
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        std::cerr << "[ERROR] WSAStartup failed\n";
        return 1;
    }

    std::vector<std::shared_ptr<Bridge>> bridges;
    std::vector<std::thread> threads;

    for (auto& pc : cfg.ports) {
        auto b = std::make_shared<Bridge>();
        b->label = pc.com_port + ":" + std::to_string(pc.tcp_port);

        b->hCom = open_com(pc.com_port, pc.baud_rate);
        if (b->hCom == INVALID_HANDLE_VALUE) {
            std::cerr << "[SKIP] " << b->label << "\n";
            continue;
        }

        b->sock = connect_tcp(cfg.remote_ip, pc.tcp_port);
        if (b->sock == INVALID_SOCKET) {
            CloseHandle(b->hCom);
            std::cerr << "[SKIP] " << b->label << "\n";
            continue;
        }

        std::cout << "[OK] Bridge active: " << b->label
                  << " @ " << pc.baud_rate << " baud\n";

        Bridge* raw = b.get();
        threads.emplace_back(serial_to_tcp, raw);
        threads.emplace_back(tcp_to_serial, raw);
        bridges.push_back(b);
    }

    if (bridges.empty()) {
        std::cerr << "[ERROR] No bridges started.\n";
        WSACleanup();
        return 1;
    }

    std::cout << "[INFO] Running. Press Ctrl+C to stop.\n";

    for (auto& t : threads)
        t.join();

    for (auto& b : bridges) {
        CloseHandle(b->hCom);
        closesocket(b->sock);
    }

    WSACleanup();
    return 0;
}
