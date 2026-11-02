#include "server.h"
#include <string>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

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

std::string localIP(int fd) {
    sockaddr_in local{};
    socklen_t len = sizeof(local);
    getsockname(fd, (sockaddr*)&local, &len);
    char ip [INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &local.sin_addr, ip, sizeof(ip));
    return ip;
}