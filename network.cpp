#include "server.h"
#include "log_event.h"
#include "identity.h"
#include "protocol.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

// Creates a TCP socket for listening on the given port
// Returns the listenening fd, or -1 on failure
int setUpListen(int port) {
    int listenSock = socket(AF_INET, SOCK_STREAM, 0);
    if (listenSock < 0) {
        perror("socket");
        return -1;
    }

    // Allow restarting the server immediately without "Address already in use"
    int opt = 1;
    if (setsockopt(listenSock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("sockopt failed");
        close(listenSock);
        return -1;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY; // accept connections on any local ip
    addr.sin_port = htons(port); // port in network byte order

    if (bind(listenSock, (sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(listenSock);
        return -1;
    }

    if (listen(listenSock, 10) < 0) { // 10 = backlog of pending connections
        perror("listen");
        close(listenSock);
        return -1;
    }
    return listenSock;
}

bool sendTo(Server& s, int fd, const std::string& payload) {
    auto it = s.connections.find(fd);
    if (it == s.connections.end()) {
        log_note("sendTo: no connection with fd %d", fd);
        return false;
    }
    Connection& c = it->second;

    std::string msg = packMsg(payload);
    if (msg.empty()) {
        log_note("Message is too big!");
        return true;
    }
    bool status = sendMsg(fd, msg);
    if (status) {
        log_event_bytes("SEND", c.ip.c_str(), c.port, c.groupID.c_str(),
        payload.c_str(), payload.length());
        return status;
    } else {
        log_note("send to %s:%d failed: %s", c.ip.c_str(), c.port, strerror(errno));
        return status;
    }
}
// Accepts one pending connection, starts polling it and registers it in clients
// a failed accept is logged but does not stop the server.
void acceptConnection(Server& s, int listenSock) {
    sockaddr_in addr{};
    socklen_t addrLen = sizeof(addr);
    int fd = accept(listenSock, (sockaddr*)&addr, &addrLen); // kernel fills in the peer address
    if (fd < 0) {
        log_note("accept() failed: %s", strerror(errno));
        return;
    }

    char ip[INET_ADDRSTRLEN];
    int port = ntohs(addr.sin_port);
    inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip));
    bool local = (ntohl(addr.sin_addr.s_addr) == INADDR_LOOPBACK);

    if (s.myIp.empty() && !local) {
        s.myIp = localIP(fd);
        log_note("my address is: %s", s.myIp.c_str());
    }
    
    // revents starts at 0, so if this push happens during runServers loop
    // the new entry is skipped this round and picked up next poll()
    s.fds.push_back({fd, POLLIN, 0});
    Connection& c = s.connections[fd];
    c.ip = ip;
    c.port = port;
    c.role = Role::UNKNOWN;
    c.isLocal = local;
    c.lastSeen = time(NULL);
    log_event("ACCEPT", ip, port, c.groupID.c_str(), NULL);
}

int connectToPeer(Server& s, std::string ip, int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    
    timeval timeout{};
    timeout.tv_sec = 2;
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    if (fd < 0) {
        log_note("Socket fail: %s", strerror(errno));
        return -1;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, ip.c_str(), & addr.sin_addr) != 1) {
        log_note("invlaid IP: %s", ip.c_str());
        close(fd);
        return -1;
    }
    // TODO: non blocking connect
    if(connect(fd, (sockaddr*)&addr, sizeof(addr)) < 0) {
        log_note("connect failed: %s", strerror(errno));
        close(fd);
        return -1;
    }

    if (s.myIp.empty()) {
        s.myIp = localIP(fd);
        log_note("my address is: %s", s.myIp.c_str());
    }

    s.fds.push_back({fd, POLLIN, 0});
    Connection& c = s.connections[fd];
    c.ip = ip;
    c.port = port;
    c.role = Role::SERVER;
    c.outgoing = true;
    c.listenPort = port;
    log_event("CONN", ip.c_str(), port, NULL, "Successful connection to peer");
    sendTo(s, fd, "HELO," GROUP_ID);
    c.heloSent = true;
    c.lastSeen = time(NULL);
    return fd;
}

void closeConnection(Server& s, int fd, const char* reason) {
    auto it = s.connections.find(fd);
    if (it != s.connections.end()) {
        const Connection& c = it->second;
        log_event("CLOSE", c.ip.c_str(), c.port, c.groupID.c_str(), reason);
        s.connections.erase(it);
    }
    close(fd);
    for (pollfd& p: s.fds) {
        if (p.fd == fd) {
            p.fd = -1;
            break;
        }
    }
}