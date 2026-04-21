// Linux remote agent: TCP server <-> real serial port
// One listener thread per configured port.
// Accepts one host connection at a time; opens the serial device when
// connected and closes it when the connection drops, ready for reconnect.

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <termios.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "common/config.hpp"

// ---------------------------------------------------------------------------
// Serial helpers
// ---------------------------------------------------------------------------

static speed_t baud_to_speed(int rate) {
    switch (rate) {
        case 9600:    return B9600;
        case 19200:   return B19200;
        case 38400:   return B38400;
        case 57600:   return B57600;
        case 115200:  return B115200;
        case 230400:  return B230400;
        case 460800:  return B460800;
        case 921600:  return B921600;
        case 1000000: return B1000000;
        case 2000000: return B2000000;
        default: throw std::runtime_error("Unsupported baud rate: " + std::to_string(rate));
    }
}

static int open_serial(const std::string& device, int baud_rate) {
    int fd = open(device.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        std::cerr << "[ERROR] Cannot open " << device << ": " << strerror(errno) << "\n";
        return -1;
    }

    termios tty = {};
    tcgetattr(fd, &tty);

    speed_t speed = baud_to_speed(baud_rate);
    cfsetispeed(&tty, speed);
    cfsetospeed(&tty, speed);

    // 8N1, raw mode, no flow control
    tty.c_cflag  = (tty.c_cflag & ~CSIZE) | CS8;
    tty.c_cflag &= ~(PARENB | CSTOPB | CRTSCTS);
    tty.c_cflag |=  CLOCAL | CREAD;
    tty.c_iflag  = IGNBRK;
    tty.c_oflag  = 0;
    tty.c_lflag  = 0;
    tty.c_cc[VMIN]  = 0;
    tty.c_cc[VTIME] = 0;

    tcsetattr(fd, TCSANOW, &tty);
    tcflush(fd, TCIOFLUSH);

    return fd;
}

// ---------------------------------------------------------------------------
// TCP helpers
// ---------------------------------------------------------------------------

static int make_listen_socket(const std::string& ip, int port) {
    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) return -1;

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(static_cast<uint16_t>(port));
    inet_pton(AF_INET, ip.c_str(), &addr.sin_addr);

    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0 ||
        listen(fd, 1) < 0) {
        std::cerr << "[ERROR] bind/listen on port " << port << ": " << strerror(errno) << "\n";
        close(fd);
        return -1;
    }
    return fd;
}

// ---------------------------------------------------------------------------
// Bidirectional bridge: TCP socket <-> serial fd
// Runs until either side closes or errors.
// ---------------------------------------------------------------------------

static void bridge(int tcp_fd, int serial_fd, const std::string& label) {
    constexpr int BUF = 65536;
    char buf[BUF];

    while (true) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(tcp_fd,    &fds);
        FD_SET(serial_fd, &fds);
        int nfds = std::max(tcp_fd, serial_fd) + 1;

        if (select(nfds, &fds, nullptr, nullptr, nullptr) < 0) {
            if (errno == EINTR) continue;
            break;
        }

        // serial -> TCP
        if (FD_ISSET(serial_fd, &fds)) {
            ssize_t n = read(serial_fd, buf, BUF);
            if (n <= 0) break;
            ssize_t sent = 0;
            while (sent < n) {
                ssize_t w = send(tcp_fd, buf + sent, n - sent, MSG_NOSIGNAL);
                if (w <= 0) goto done;
                sent += w;
            }
        }

        // TCP -> serial
        if (FD_ISSET(tcp_fd, &fds)) {
            ssize_t n = recv(tcp_fd, buf, BUF, 0);
            if (n <= 0) break;
            ssize_t written = 0;
            while (written < n) {
                ssize_t w = write(serial_fd, buf + written, n - written);
                if (w <= 0) goto done;
                written += w;
            }
        }
    }

done:
    std::cout << "[" << label << "] Connection closed\n";
}

// ---------------------------------------------------------------------------
// Per-port listener: waits for a host to connect, bridges it to the
// serial device, then loops back to wait for the next connection.
// ---------------------------------------------------------------------------

static void port_listener(RemotePortConfig cfg, std::string listen_ip,
                           std::atomic<bool>& shutdown) {
    std::string label = cfg.serial_device + " <-> TCP:" + std::to_string(cfg.tcp_port);

    int server_fd = make_listen_socket(listen_ip, cfg.tcp_port);
    if (server_fd < 0) {
        std::cerr << "[SKIP] " << label << "\n";
        return;
    }

    // Non-blocking accept so we can poll the shutdown flag
    fcntl(server_fd, F_SETFL, O_NONBLOCK);

    std::cout << "[OK] Listening  " << label << " @ " << cfg.baud_rate << " baud\n";

    while (!shutdown) {
        // Poll for incoming connection with a short timeout
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(server_fd, &rfds);
        timeval tv = {0, 50'000};   // 50 ms
        if (select(server_fd + 1, &rfds, nullptr, nullptr, &tv) <= 0)
            continue;

        int client_fd = accept(server_fd, nullptr, nullptr);
        if (client_fd < 0) continue;

        int flag = 1;
        setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
        std::cout << "[" << label << "] Host connected\n";

        int serial_fd = open_serial(cfg.serial_device, cfg.baud_rate);
        if (serial_fd < 0) {
            close(client_fd);
            continue;
        }

        bridge(client_fd, serial_fd, label);

        close(serial_fd);
        close(client_fd);
    }

    close(server_fd);
    std::cout << "[" << label << "] Listener stopped\n";
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(int argc, char* argv[]) {
    // Ignore SIGPIPE so broken TCP writes return EPIPE instead of crashing
    signal(SIGPIPE, SIG_IGN);

    const std::string config_path = (argc > 1) ? argv[1] : "remote_config.json";

    RemoteConfig cfg;
    try {
        cfg = load_remote_config(config_path);
    } catch (const std::exception& e) {
        std::cerr << "[ERROR] " << e.what() << "\n";
        return 1;
    }

    std::atomic<bool> shutdown{false};
    std::vector<std::thread> threads;

    for (auto& pc : cfg.ports)
        threads.emplace_back(port_listener, pc, cfg.listen_ip, std::ref(shutdown));

    if (threads.empty()) {
        std::cerr << "[ERROR] No ports configured.\n";
        return 1;
    }

    std::cout << "[INFO] Running. Press Enter to stop.\n";
    std::cin.get();

    shutdown = true;
    for (auto& t : threads)
        t.join();

    return 0;
}
