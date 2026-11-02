#include "server.h"
#include <string>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

// Validates an int from a string, used for port numbers
bool parseInt(const std::string& text, int& out) {
    try {
        size_t used = 0;
        out = std::stoi(text, &used);
        if (used != text.size()) {
            return false;
        } 
    } catch (const std::exception&) {
        return false;
    }
    return true;
}

// Validates longs from string, used for timestamps
bool parseLong(const std::string& text, long& out) {
    try {
        size_t used = 0;
        out = std::stoll(text, &used);
        if (used != text.size()) {
            return false;
        } 
    } catch (const std::exception&) {
        return false;
    }
    return true;
}

// Returns our local IP addr
std::string localIP(int fd) {
    sockaddr_in local{};
    socklen_t len = sizeof(local);
    getsockname(fd, (sockaddr*)&local, &len);
    char ip [INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &local.sin_addr, ip, sizeof(ip));
    return ip;
}

// True if ip is an IPv4 address another machine could reach us at
// rejects ips like "0.0.0.0" and "127.x.x.x"
bool isUsableAddress(const std::string& ip) {
    in_addr a{};
    if (inet_pton(AF_INET, ip.c_str(), &a) != 1) {
        return false;
    }
    uint32_t h = ntohl(a.s_addr);
    if (h == INADDR_ANY) {
        return false;
    }
    if ((h >> 24) == IN_LOOPBACKNET) { // first byte 127 = loopback range
        return false;
    }
    return true;
}