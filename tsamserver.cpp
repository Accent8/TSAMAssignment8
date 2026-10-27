#include <iostream>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <cerrno>
#include <poll.h>
#include <vector>
#include <algorithm>
#include <map>
#include <cstring>
#include <arpa/inet.h>
#include "protocol.h"
#include <csignal>
#include "commands.h"
#include "log_event.h"
#include "identity.h"

// Per connection state
struct Client {
    std::string buffer; // bytes recevied but not a complete frame yet
    std::string ip;
    int port;
    std::string groupID;
};

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

// Accepts one pending connection, starts polling it and registers it in clients
// a failed accept is logged but does not stop the server.
void acceptClient(int listenSock, std::vector<pollfd>& fds, std::map<int, Client>& clients) {
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
    std::string name = std::string(ip) + ":" + std::to_string(ntohs(addr.sin_port))
                    + " (fd " + std::to_string(fd) + ")";

    // revents starts at 0, so if this push happens during runServers loop
    // the new entry is skipped this round and picked up next poll()
    fds.push_back({fd, POLLIN, 0});
    clients[fd].ip = ip;
    clients[fd].port = port;
    log_event("ACCEPT", ip, port, NULL, NULL);
}

// Handles one readable event on a clients socket: receives available bytes
// then processes every complete frame in the clients buffer
// Returns false if the client should be closed (disconnect or error)
// closing is left to runServer, which owns the fd list
bool handleClient(int clientSock, Client& client, Mailbox& mailbox) {
    char buff[2048];
    ssize_t n = recv(clientSock, buff, sizeof(buff), 0); // wont block, poll said its ready
    if (n == 0) {
        log_event("CLOSE", client.ip.c_str(), client.port, NULL, "disconnected");
        return false;
    }
    if (n < 0) {
        if (errno == EINTR) return true; // interrupted, poll will report and try again
        log_event("CLOSE", client.ip.c_str(), client.port, NULL, strerror(errno));
        return false;
    }
    
    // TCP is a byte stream, one recv may hold part of a frame or several frames
    // or both, so append to the per client buffer and extract whats complete
    client.buffer.append(buff, n);

    while (true) {
        std::string cmd;
        size_t discarded = 0;
        bool gotFrame = unpackMsg(client.buffer, cmd, &discarded);

        // Checked even when no frame was found, a failed call
        // could have thrown away garbage
        if (discarded > 0) {
            std::string discardMsg = "Discarded malformed bytes: " + std::to_string(discarded);
            log_event("DROP", client.ip.c_str(), client.port, NULL, discardMsg.c_str());
        }
        if (!gotFrame) break; // rest of the buffer is incomplete frame

        log_event_bytes("RECV", client.ip.c_str(), client.port, NULL, cmd.c_str(), cmd.length());

        std::string response = handleCommand(cmd, mailbox);
        std::string frame = packMsg(response);
        if (frame.empty()) { // response too large to fit ito 1 frame
            log_note("Error, response too large");
            response = "ERROR, response too large";
            frame = packMsg(response);
        }

        if (!sendMsg(clientSock, frame)) {
            log_event("CLOSE", client.ip.c_str(), client.port, NULL, "sendMsg failed");
            return false;
        }
        log_event_bytes("SEND", client.ip.c_str(), client.port, NULL, response.c_str(), response.length());
    }
    return true;
}

// The event loop, runs until poll() fails with non recoverable error
void runServer(int listenSock) {
    std::vector<pollfd> fds; // what poll() watches, index 0 is the listener
    std::map<int, Client> clients; // per connection state, keyed by fd
    Mailbox mailbox; // lives here so messages outlive any single client
    fds.push_back({listenSock, POLLIN, 0}); // index 0: listener

    while (true) {
        int ready = poll(fds.data(), fds.size(), -1); // block until something happens
        if (ready < 0) {
            if (errno == EINTR) {
                continue; // interrupted by a signal, not an error
            } else {
                log_note("Poll failed: %s", strerror(errno));
                break;
            }
        }

        for(size_t i = 0; i < fds.size(); i++) {
            // Any events counts, not just POLLIN, and if an event is ignored
            // poll() would return for them again immediately, (busy loop)
            // and recv() then reports the problem
            if (fds[i].revents == 0) continue; 

            if (fds[i].fd == listenSock) {
                acceptClient(listenSock, fds, clients);
            } else {
                int fd = fds[i].fd;
                if (!handleClient(fd, clients[fd], mailbox)) {
                    close(fd);
                    clients.erase(fd); // erase too, or a refused fd number inherits old bytes
                    fds[i].fd = -1; // mark only: erasing while iterating would skip entries
                }
            }
        }
        // Remove the entries marked above, after the loop finishes running
        auto newFds = std::remove_if(fds.begin(), fds.end(), [](const pollfd& p) {return p.fd == -1;});
        fds.erase(newFds, fds.end());
    }

}

int main(int argc, char* argv[]){
    if (argc != 2) {
        std::cerr << "incorrect amount of args" << std::endl;
        std::cerr << "Usage: " << argv[0] << " <port>" << std::endl;
        return 1;
    }
    int port;
    try {
        port = std::stoi(argv[1]);
    } catch (const std::exception&) { // invalid_argument or out_of_range
        std::cerr << "invalid port: " << argv[1] << std::endl;
        return 1;
    }
    if (port < 1 || port > 65535) {
        std::cerr << "port must be between 1 and 65535" << std::endl;
        return 1;
    }

    // Without this, send() to a client that has disconnected raises SIGPIPE
    // which kills the whole server, ignored send() just fails with EPIPE
    signal(SIGPIPE, SIG_IGN);

    int listenSock = setUpListen(port);
    if (listenSock < 0) {
        std::string listenFail = "Failed to listen on " + std::to_string(port);
        log_note("%s", listenFail.c_str());
        return 1;
    }
    std::string msg = "listening on port " + std::to_string(port);
    log_event("START", NULL, 0, GROUP_ID, msg.c_str());

    runServer(listenSock);
    close(listenSock);
    return 0;
}

