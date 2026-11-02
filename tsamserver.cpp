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
    std::string myIp;
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

std::string buildServersList(const Server& s) {
    std::string list;
    list.append(std::string(GROUP_ID) + ',' + s.myIp + "," + std::to_string(s.myPort) + ';');
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

        auto it = s.known.find(fields[0]);
        if (it == s.known.end()) {
            log_note("%s says %s is at %s:%d", c.groupID.c_str(), fields[0].c_str(),
            fields[1].c_str(), port);
        } else if (it->second.ip != fields[1] || it->second.port != port) {
            log_note("%s says %s moved from %s:%d to %s:%d", c.groupID.c_str(), fields[0].c_str(),
             it->second.ip.c_str(), it->second.port, fields[1].c_str(), port);
        }
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

    if (s.myIp.empty()) {
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
    return fd;
}

int findPeer(const Server& s, const std::string& groupId) {
    for (const auto& [cfd, c] : s.connections) {
        if (c.role == Role::SERVER && c.groupID == groupId) {
            return cfd;
        }
    }
    return -1;
}


// Rebuilds the SENDMSG command from a stored message
std::string toSendMsg(const Message& m) {
    return "SENDMSG," + m.from + "," + m.to + "," + std::to_string(m.created)
    + "," + m.text;
}

bool deliverHeld(Server&s, int fd, const std::string& group) {
    auto entry = s.mailbox.find(group); // retrive the messages for group
    if (entry == s.mailbox.end()) {
        log_note("nothing held for %s", group.c_str());
        return true;
    }

    std::deque<Message>& queue = entry->second;
    int sent = 0;
    while (!queue.empty()) {
        if (sendTo(s, fd, toSendMsg(queue.front()))) {
            queue.pop_front();
            sent++;
        } else {
            return false;
        }
    }
    s.mailbox.erase(entry);
    log_note("delivered %d held messages to %s", sent, group.c_str());
    return true;
}

bool handleClientSendMsg(Server&s, int fd, const std::string& args) {
    size_t c1 = args.find(','); // end of from
    if (c1 == std::string::npos) {
        return sendTo(s, fd, "ERROR, missing fields");
    }
    size_t c2 = args.find(',', c1 + 1); // end of to
    if (c2 == std::string::npos) {
        return sendTo(s, fd, "ERROR, missing fields");
    }
    // cut the message up
    // from always ignored, use GROUP_ID instead
    std::string from = args.substr(0, c1); // from the start to first comma
    std::string to = args.substr(c1 + 1, c2 - c1 - 1); // between the commas
    std::string text = args.substr(c2 + 1); // everything else

    if (to.empty()) {
        return sendTo(s, fd, "ERROR, empty field");
    }
    time_t now = time(NULL);
    std::string out = "SENDMSG," GROUP_ID "," + to + "," + 
    std::to_string(now) + "," + text;
    
    int peerFD = findPeer(s, to);

    if (peerFD != -1) {
        if (sendTo(s, peerFD, out)) {
            return sendTo(s, fd, "OK");
        } 
        log_note("sending to %s failed, holding instead", to.c_str());
    } 
    s.mailbox[to].push_back({GROUP_ID, to, now, text});
    log_note("no direct connection to %s, holding message", to.c_str());
    return sendTo(s, fd, "OK");
}

bool handleClientConnect(Server&s, int fd, const std::string& args) {
    size_t c1 = args.find(","); 
    if (c1 == std::string::npos) {
        return sendTo(s, fd, "ERROR, missing fields");
    }
    std::string ip = args.substr(0, c1);
    std::string strPort = args.substr(c1 + 1);
    int port;
    if (!parseInt(strPort, port)) {
        return sendTo(s, fd, "ERROR, bad port");
    }
    if (port < 1 || port > 65535) {
        return sendTo(s, fd, "ERROR, bad port");
    }
    if (port == s.myPort && (ip == s.myIp)) {
        return sendTo(s, fd, "ERROR, cannot connect to myself");
    }
    int connectionCount = 0;
    for (const auto& [cfd, c] : s.connections) {
        if (c.ip == ip && c.listenPort == port) {
            std::string who = c.groupID.empty() ? c.ip + ":" + // Incase the groupID is empty
            std::to_string(c.listenPort) : c.groupID;          // we use IP and Port instead
            return sendTo(s, fd, "ERROR, already connected to " + who);
        }
        if (c.role == Role::SERVER) {
            connectionCount++;
        }
    }
    if (connectionCount >= 8) {
        return sendTo(s, fd, "ERROR, already connected to max nr of servers");
    }
    int peerFd = connectToPeer(s, ip, port);
    if (peerFd == -1) {
        return sendTo(s, fd, "ERROR, connect fail");
    }
    return sendTo(s, fd, "OK");
}

bool handleServerSendMsg(Server&s, int fd, const std::string& args) {
    Connection& c = s.connections[fd];
    size_t c1 = args.find(','); // end of from
    if (c1 == std::string::npos) {
        log_event("DROP", c.ip.c_str(), c.port, c.groupID.c_str(), "Missing fields sendMSG");
        return true;
    }
    size_t c2 = args.find(',', c1 + 1); // end of to
    if (c2 == std::string::npos) {
        log_event("DROP", c.ip.c_str(), c.port, c.groupID.c_str(), "Missing fields sendMSG");
        return true;
    }
    size_t c3 = args.find(',', c2 + 1); // end of timestamp
    if (c3 == std::string::npos) {
        log_event("DROP", c.ip.c_str(), c.port, c.groupID.c_str(), "Missing fields sendMSG");
        return true;
    }

    std::string from = args.substr(0, c1); // sender
    std::string to = args.substr(c1 + 1, c2 - c1 - 1); // receiver
    std::string ts = args.substr(c2 + 1, c3 - c2 - 1); // timestamp
    std::string text = args.substr(c3 + 1); // text

    if (from.empty() || to.empty()) {
        log_event("DROP", c.ip.c_str(), c.port, c.groupID.c_str(), "Sender or receiver empty, dropping msg...");
        return true;
    }
    long created;
    if (!parseLong(ts, created)) {
        log_event("DROP", c.ip.c_str(), c.port, c.groupID.c_str(), "Invalid timestamp, dropping msg...");
        return true;
    }

    if (to == GROUP_ID) {
        s.mailbox[to].push_back({from, to, created, text});
        log_note("message for us from: %s", from.c_str());
        return true;
    }
    
    int peerFD = findPeer(s, to);
    if (peerFD != -1) {
        if (sendTo(s, peerFD, "SENDMSG," + args)) {
            log_note("forwarded message from %s to %s", from.c_str(), to.c_str());
            return true;
        }
        log_note("forwarding to %s failed, holding instead", to.c_str());
    }

    s.mailbox[to].push_back({from, to, created, text});
    log_note("holding message from %s for %s", from.c_str(), to.c_str());
    return true;
}

// GETMSGS,<group> send every message we hold for group as SENDMSG
// commands on this connection. only if the connection identified itself
// as the group may it collect messages
bool handleServerGetMsgs(Server&s, int fd, const std::string& group) {
    Connection& c = s.connections[fd];
    if (group.empty()) {
        log_event("DROP", c.ip.c_str(), c.port, c.groupID.c_str(), "GETMSGS group empty dropping...");
        return true;
    }
    if (group != c.groupID) {
        log_note("%s is collecting mail for: %s", c.groupID.c_str(), group.c_str());
    }
    return deliverHeld(s, fd, group);
}

bool handleServerStatusResp(Server& s, int fd,  const std::string& args) {
    Connection& c = s.connections[fd];
    if (args == "EMPTY") {
        return true;
    }

    std::stringstream entries(args);
    std::string entry;
    while (std::getline(entries, entry, ',')) {
        size_t colon = entry.find(':');
        if (colon == std::string::npos || colon == 0) {
            log_note("bad entry from %s: %s", c.groupID.c_str(), entry.c_str());
            continue;
        }
        std::string name = entry.substr(0, colon);
        std::string countStr = entry.substr(colon + 1);
        int count;
        if (!parseInt(countStr, count)) {
            log_note("invalid count from %s: %s", c.groupID.c_str(), entry.c_str());
            continue;
        }
        if (count == 0) continue;
        if (count <= 0) {
            log_note("bad STATUSRESP entry from %s: %s", c.groupID.c_str(), entry.c_str());
            continue;
        }
        bool forUs = (name == GROUP_ID);
        bool forNeighbour = (name != c.groupID && findPeer(s, name) != -1);
        if (!forUs && !forNeighbour) {
            continue;
        }
        if (!sendTo(s, fd, "GETMSGS," + name)) {
            return false; // connection dead
        }
        log_note("%s holds %d messages for %s, fetching them", c.groupID.c_str(), count, name.c_str());
    }
    return true;
}

bool handleClientCommand(Server& s, int fd, std::string& cmd) {
    if (cmd.rfind("SENDMSG,", 0) == 0) {
        return handleClientSendMsg(s, fd, cmd.substr(8)); // 8 = strlen("SENDMSG,")
    }
    if (cmd.rfind("CONNECT,", 0) == 0) {
        return handleClientConnect(s, fd, cmd.substr(8)); // 8 = strlen("Connect",)
    }
    std::string response = handleCommand(cmd, s.mailbox);
    if (response.size() + FRAME_OVERHEAD > MAX_FRAME + FRAME_OVERHEAD) {
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

        if (!sendTo(s, fd, "SERVERS," + buildServersList(s))) {
            return false;
        }
        return deliverHeld(s, fd, c.groupID);
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
            log_event("DROP", c.ip.c_str(), c.port, c.groupID.c_str(), ("bad KEEPALIVE count: " + args).c_str());
            return true;
        }
        c.waiting = n;
        log_note("%s holds %d messages for us", c.groupID.c_str(), c.waiting);
        if (c.waiting > 0) {
            return sendTo(s, fd, "GETMSGS," GROUP_ID);
        }
        return true;
    }
    if (name == "SENDMSG") {
        return handleServerSendMsg(s, fd, args);
    }
    if (name == "GETMSGS") {
        return handleServerGetMsgs(s, fd, args);
    }
    if (name == "STATUSRESP") {
        return handleServerStatusResp(s, fd, args);
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