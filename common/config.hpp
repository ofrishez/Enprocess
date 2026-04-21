#pragma once

#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

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
    std::ifstream f(path);
    if (!f) throw std::runtime_error("Cannot open config: " + path);
    auto j = nlohmann::json::parse(f);

    HostConfig cfg;
    cfg.remote_ip = j.at("remote_ip").get<std::string>();
    for (auto& p : j.at("ports")) {
        PortConfig pc;
        pc.com_port  = p.at("com_port").get<std::string>();
        pc.tcp_port  = p.at("tcp_port").get<int>();
        pc.baud_rate = p.at("baud_rate").get<int>();
        cfg.ports.push_back(pc);
    }
    return cfg;
}
