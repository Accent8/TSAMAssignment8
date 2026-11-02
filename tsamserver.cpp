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
#include "server.h"
#include <ctime>
#include <sstream>

// Handles one readable event on a clients socket: receives available bytes
// then processes every complete frame in the clients buffer
// Returns false if the client should be closed (disconnect or error)
// closing is left to runServer, which owns the fd list
bool handleConnection(Server& s, int fd) {
    Connection& c = s.connections[fd];
    char buff[2048];
    ssize_t n = recv(fd, buff, sizeof(buff), 0); // wont block, poll said its ready
    if (n == 0) {
        log_event("CLOSE", c.ip.c_str(), c.port, c.groupID.c_str(), "disconnected");
        return false;
    }
    if (n < 0) {
        if (errno == EINTR) return true; // interrupted, poll will report and try again
        log_event("CLOSE", c.ip.c_str(), c.port, c.groupID.c_str(), strerror(errno));
        return false;
    }
    
    // TCP is a byte stream, one recv may hold part of a frame or several frames
    // or both, so append to the per client buffer and extract whats complete
    c.buffer.append(buff, n);

    while (true) {
        std::string cmd;
        size_t discarded = 0;
        bool gotFrame = unpackMsg(c.buffer, cmd, &discarded);

        // Checked even when no frame was found, a failed call
        // could have thrown away garbage
        if (discarded > 0) {
            std::string discardMsg = "Discarded malformed bytes: " + std::to_string(discarded);
            log_event("DROP", c.ip.c_str(), c.port, c.groupID.c_str(), discardMsg.c_str());
        }
        if (!gotFrame) break; // rest of the buffer is incomplete frame

        log_event_bytes("RECV", c.ip.c_str(), c.port, c.groupID.c_str(), cmd.c_str(), cmd.length());

        if (c.role == Role::UNKNOWN) {
            if(cmd.rfind("HELO,", 0) == 0) {
                c.role = Role::SERVER;
                log_note("Server added since first msg starts with HELO");
            } else if (c.isLocal) {
                c.role = Role::CLIENT;
                log_note("Client added: local connection without HELO");
            } else {
                log_event("DROP", c.ip.c_str(), c.port, c.groupID.c_str(),
                "first command was not HELO and connection is not local");
                log_event("CLOSE", c.ip.c_str(), c.port, c.groupID.c_str(),
                "unidentified remote connection refused");
            }
        }
        if (c.role == Role::CLIENT) {
            if (!handleClientCommand(s, fd, cmd)) {
                log_event("CLOSE", c.ip.c_str(), c.port, c.groupID.c_str(),
                "send failed");
                return false;
            }
        }
        if (c.role == Role::SERVER) {
            if (!handleServerCommand(s, fd, cmd)) {
                log_event("CLOSE", c.ip.c_str(), c.port, c.groupID.c_str(),
                "send failed");
                return false;
            }
        }
    }
    return true;
}

// The event loop, runs until poll() fails with non recoverable error
void runServer(Server& s, int listenSock) {
    s.fds.push_back({listenSock, POLLIN, 0}); // index 0: listener

    while (true) {
        int ready = poll(s.fds.data(), s.fds.size(), -1); // block until something happens
        if (ready < 0) {
            if (errno == EINTR) {
                continue; // interrupted by a signal, not an error
            } else {
                log_note("Poll failed: %s", strerror(errno));
                break;
            }
        }

        for(size_t i = 0; i < s.fds.size(); i++) {
            // Any events counts, not just POLLIN, and if an event is ignored
            // poll() would return for them again immediately, (busy loop)
            // and recv() then reports the problem
            if (s.fds[i].revents == 0) continue; 

            if (s.fds[i].fd == listenSock) {
                acceptConnection(s, s.fds[i].fd);
            } else {
                int fd = s.fds[i].fd;
                if (!handleConnection(s, fd)) {
                    close(fd);
                    s.connections.erase(fd); // erase too, or a refused fd number inherits old bytes
                    s.fds[i].fd = -1; // mark only: erasing while iterating would skip entries
                }
            }
        }
        // Remove the entries marked above, after the loop finishes running
        auto newFds = std::remove_if(s.fds.begin(), s.fds.end(), [](const pollfd& p) {return p.fd == -1;});
        s.fds.erase(newFds, s.fds.end());
    }
}

int main(int argc, char* argv[]){
    if (argc != 2 && argc != 4) {
        std::cerr << "incorrect amount of args" << std::endl;
        std::cerr << "Usage: " << argv[0] << " <port>" << std::endl;
        std::cerr << "OR" << std::endl;
        std::cerr << argv[0] << "<port> <target ip> <open port on target>" << std::endl;
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
    Server server;
    server.myPort = port;
    if (argc == 4) {
        int peerport;
        if (!parseInt(argv[3], peerport)) {
            return 1;
        }
        connectToPeer(server, argv[2], peerport);
    }

    runServer(server, listenSock);
    close(listenSock);
    return 0;
}