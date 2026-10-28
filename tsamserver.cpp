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
#include <ctime>
#include <sstream>

// Per connection state
enum class Role { UNKNOWN, CLIENT, SERVER};
struct Connection {
    std::string buffer, ip, groupID; 
    int port;
    int listenPort = -1; // where peer accepts connections (for SERVERS)
    Role role = Role::UNKNOWN;
    bool outgoing = false; // did we dail them?
    bool heloSent = false; // so we never send HELO twice
    int waiting = -1;
};

struct KnownServer {
    std::string ip;
    int port;
    std::string heardFrom; // who told us
    time_t lastSeen;
};

struct Server {
    std::vector<pollfd> fds;
    std::map<int, Connection> connections;
    std::map<std::string, KnownServer> known;
    Mailbox mailbox;
    int myPort;
};

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

std::string localIP(int fd) {
    sockaddr_in local{};
    socklen_t len = sizeof(local);
    getsockname(fd, (sockaddr*)&local, &len);
    char ip [INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &local.sin_addr, ip, sizeof(ip));
    return ip;
}

std::string buildServersList(const Server& s, int fd) {
    std::string ip = localIP(fd);
    std::string list;
    list.append(std::string(GROUP_ID) + ',' + ip + "," + std::to_string(s.myPort) + ';');
    for (const auto& [cfd, c] : s.connections) {
        if (c.role != Role::SERVER) {
            continue;
        }
        if (c.groupID.empty()) {
            continue;
        }
        if (c.listenPort == -1) {
            continue;
        }
        list.append(c.groupID + ',' + c.ip + ',' + 
        std::to_string(c.listenPort) + ';');
    }
    return list;
}

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
    std::string msg = packMsg(payload);
    if (msg.empty()) {
        log_note("Message is too big!");
        return true;
    }
    bool status = sendMsg(fd, msg);
    if (status) {
        log_event_bytes("SEND", s.connections[fd].ip.c_str(), s.connections[fd].port, 
            s.connections[fd].groupID.c_str(), payload.c_str(), payload.length());
        return status;
    }
    return status;
}

void parseServersList(Server& s, int fd, const std::string& args) {
    Connection& c = s.connections[fd];

    std::stringstream entries(args);
    std::string entry;
    while (std::getline(entries, entry, ';')) { // One "ID,IP,PORT" at a time
        if (entry.empty()) continue;
        std::vector<std::string> fields;
        std::stringstream parts(entry);
        std::string field;
        while (std::getline(parts, field, ',')) {
            fields.push_back(field);
        }
        if (fields.size() != 3) {
            log_note("Incorrect amount of fields, got %zu expected 3. Skipping...", fields.size());
            continue;
        }
        int port;
        if (!parseInt(fields[2], port)) {
            log_note("Bad port in SERVERS entry from %s: %s", c.groupID.c_str(), entry.c_str());
            continue;
        }

        if (port < 1 || port > 65535) {
            log_note("Port number out of range from %s: %s! skipping...", c.groupID.c_str(), entry.c_str());
            continue;
        }
        if (fields[0] == GROUP_ID) { // is it us?
            continue;
        }
        if (fields[0] == c.groupID) { // is it the sender?
            c.listenPort = port;
            s.known[fields[0]] = {fields[1], port, c.groupID, time(NULL)};
            log_note("%s listens on port %d", c.groupID.c_str(), port);
            continue;
        }
        if (s.known.count(fields[0]) == 0 && s.known.size() >= 200) {
            log_note("known-servers list full, ignoring %s", fields[0].c_str());
            continue;
        }
        log_note("%s says %s is at %s:%d", c.groupID.c_str(), fields[0].c_str(),
            fields[1].c_str(), port);
        s.known[fields[0]] = {fields[1], port, c.groupID, time(NULL)};
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
    
    // revents starts at 0, so if this push happens during runServers loop
    // the new entry is skipped this round and picked up next poll()
    s.fds.push_back({fd, POLLIN, 0});
    Connection& c = s.connections[fd];
    c.ip = ip;
    c.port = port;
    c.role = Role::UNKNOWN;
    log_event("ACCEPT", ip, port, c.groupID.c_str(), NULL);
}

int connectToPeer(Server& s, std::string ip, int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
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
    return fd;
}

bool handleClientSendMsg(Server&s, int fd, std::string cmd) {
    size_t c1 = cmd.find(','); // end of from
    if (c1 == std::string::npos) {
        return sendTo(s, fd, "ERROR, missing fields");
    }
    size_t c2 = cmd.find(',', c1 + 1); // end of to
    if (c2 == std::string::npos) {
        return sendTo(s, fd, "ERROR, missing fields");
    }
    // cut the message up
    // from always ignored, use GROUP_ID instead
    std::string from = cmd.substr(0, c1); // from the start to first comma
    std::string to = cmd.substr(c1 + 1, c2 - c1 - 1); // between the commas
    std::string text = cmd.substr(c2 + 1); // everything else

    if (to.empty()) {
        return sendTo(s, fd, "ERROR, empty field");
    }

    std::string out = "SENDMSG," GROUP_ID "," + to + "," + 
    std::to_string(time(NULL)) + "," + text;
    
    int peerFD = -1;
    for (const auto& [cfd, c] : s.connections) {
        if (c.role == Role::SERVER && c.groupID == to) {
            peerFD = cfd;
            break;
        }
    }
        
    if (peerFD != -1) {
        if (!sendTo(s, peerFD, out)) {
            log_note("Sending to %s failed", to.c_str());
        } 
    } else {
        // TODO: fix missing timestamp
        s.mailbox[to].push_back({GROUP_ID, text});
        log_note("no direct connection to %s, holding message", to.c_str());
    }

    return sendTo(s, fd, "OK");
}

bool handleClientCommand(Server& s, int fd, std::string& cmd) {
    if (cmd.rfind("SENDMSG,", 0) == 0) {
        return handleClientSendMsg(s, fd, cmd.substr(8)); // 8 = strlen("SENDMSG,")
    }
    std::string response = handleCommand(cmd, s.mailbox);
    if (response.size() + FRAME_OVERHEAD > MAX_FRAME) {
        log_note("ERROR, response too large");
        response = "ERROR, response too large";
    }
    return sendTo(s, fd, response);
}

bool handleServerCommand(Server& s, int fd, std::string& cmd) {
    Connection& c = s.connections[fd];
    size_t comma = cmd.find(',');
    std::string name = cmd.substr(0, comma);
    std::string args = (comma == std::string::npos) ? "" : cmd.substr(comma + 1);
    if (name == "HELO") {
        if (args.empty()) {
            log_event("DROP", c.ip.c_str(), 
            c.port, c.groupID.c_str(), "HELO without group id");
            return true;
        }
        if (c.groupID.empty()) {
            c.groupID = args;
            log_note("%s:%d identifies as %s", c.ip.c_str(),
             c.port, args.c_str());
        } else {
            log_note("%s:%d already has a groupID set: %s", c.ip.c_str(),
             c.port, c.groupID.c_str());
        }

        if (!c.heloSent) {
            if (sendTo(s, fd, "HELO," GROUP_ID)) {
                c.heloSent = true;
            } else {
                return false;
            }
        }

        bool status = sendTo(s, fd, "SERVERS," + buildServersList(s, fd));
        if (status == false) {
            return false;
        }
       return true;
    }
    if (name == "SERVERS") {
        log_note("SERVERS from %s: %s", c.groupID.c_str(), args.c_str());
        parseServersList(s, fd, args);
        return true;
    }
    if (name == "STATUSREQ") {
        return sendTo(s, fd, "STATUSRESP," + handleStatusReq(s.mailbox));
    }
    if (name == "KEEPALIVE") {
        int n;
        if (!parseInt(args, n)) {
            log_event("DROP", c.ip.c_str(), c.port, c.groupID.c_str(), "Cannot convert N to int KEEPALIVE,N");
            return true;
        }
        if (n < 0) {
            log_event("DROP", c.ip.c_str(), c.port, c.groupID.c_str(), "bad KEEPALIVE count: " + n);
        }
        c.waiting = n;
        log_note("%s holds %d messages for us", c.groupID.c_str(), c.waiting);
        if (c.waiting > 0) {
            return sendTo(s, fd, "GETMSGS," GROUP_ID);
        }
        return true;
    }

    log_event("DROP", c.ip.c_str(), c.port, c.groupID.c_str(), ("Unhandled command: " + name).c_str());
    return true;
}

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
            } else {
                c.role = Role::CLIENT;
                log_note("Client added since first msg does not start with HELO");
            }
        }
        if (c.role == Role::CLIENT) {
            if (!handleClientCommand(s, fd, cmd)) return false;
        }
        if (c.role == Role::SERVER) {
            if (!handleServerCommand(s, fd, cmd)) return false;
        }
    }
    return true;
}

// The event loop, runs until poll() fails with non recoverable error
void runServer(Server& s, int listenSock, int port) {
    s.myPort = port;
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
    if (argc == 4) {
        int peerport;
        if (!parseInt(argv[3], peerport)) {
            std::cout << "bad port blah" << std::endl;
            return 1;
        }
        connectToPeer(server, argv[2], peerport);
    }

    runServer(server, listenSock, port);
    close(listenSock);
    return 0;
}