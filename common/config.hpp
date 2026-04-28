#pragma once

#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>
#include <yaml-cpp/yaml.h>

// ---------------------------------------------------------------------------
// Host (Windows) config
// ---------------------------------------------------------------------------

struct PortConfig {
    std::string com_port;   // bridge end of the com0com pair, e.g. "COM11"
    int         tcp_port  = 0;
    int         baud_rate = 115200;
};

struct HostConfig {
    std::string             remote_ip;
    std::vector<PortConfig> ports;
};

inline HostConfig load_host_config(const std::string& path) {
    YAML::Node doc = YAML::LoadFile(path);

    HostConfig cfg;
    cfg.remote_ip = doc["remote_ip"].as<std::string>();
    for (auto p : doc["ports"]) {
        PortConfig pc;
        pc.com_port  = p["com_port"].as<std::string>();
        pc.tcp_port  = p["tcp_port"].as<int>();
        pc.baud_rate = p["baud_rate"].as<int>();
        cfg.ports.push_back(pc);
    }
    return cfg;
}

// ---------------------------------------------------------------------------
// Remote (Linux) config
// ---------------------------------------------------------------------------

struct RemotePortConfig {
    std::string serial_device;  // e.g. "/dev/ttyUSB0"
    int         tcp_port  = 0;
    int         baud_rate = 115200;
};

struct RemoteConfig {
    std::string                   listen_ip = "0.0.0.0";
    std::vector<RemotePortConfig> ports;
};

inline RemoteConfig load_remote_config(const std::string& path) {
    YAML::Node doc = YAML::LoadFile(path);

    RemoteConfig cfg;
    if (doc["listen_ip"])
        cfg.listen_ip = doc["listen_ip"].as<std::string>();
    for (auto p : doc["ports"]) {
        RemotePortConfig pc;
        pc.serial_device = p["serial_device"].as<std::string>();
        pc.tcp_port      = p["tcp_port"].as<int>();
        pc.baud_rate     = p["baud_rate"].as<int>();
        cfg.ports.push_back(pc);
    }
    return cfg;
}
