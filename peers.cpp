#include "server.h"
#include "identity.h"
#include "log_event.h"
#include <vector>
#include <string>
#include <sstream>

constexpr int PEER_TIMEOUT = 300;
constexpr int KEEPALIVE_INTERVAL = 60;

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
            s.known[fields[0]] = {c.ip, port, c.groupID, time(NULL), true};
            log_note("%s listens on port %d", c.groupID.c_str(), port);
            continue;
        }

        if (!isUsableAddress(fields[1])) {
            log_note("%s gave an unusable address for %s: %s", c.groupID.c_str(),
                    fields[0].c_str(), entry.c_str());
            continue;
        }
        auto it = s.known.find(fields[0]);
        if (it != s.known.end() && it->second.selfReported) {
            continue; // server told us itself, third party cant override that
        }

        if (s.known.count(fields[0]) == 0 && s.known.size() >= 200) {
            log_note("known-servers list full, ignoring %s", fields[0].c_str());
            continue;
        }

        if (it == s.known.end()) {
            log_note("%s says %s is at %s:%d", c.groupID.c_str(), fields[0].c_str(),
            fields[1].c_str(), port);
        } else if (it->second.ip != fields[1] || it->second.port != port) {
            log_note("%s says %s moved from %s:%d to %s:%d", c.groupID.c_str(), fields[0].c_str(),
             it->second.ip.c_str(), it->second.port, fields[1].c_str(), port);
        }
        s.known[fields[0]] = {fields[1], port, c.groupID, time(NULL), false};
    }
}

int findPeer(const Server& s, const std::string& groupId) {
    for (const auto& [cfd, c] : s.connections) {
        if (c.role == Role::SERVER && c.groupID == groupId) {
            return cfd;
        }
    }
    return -1;
}

static void sendKeepalives(Server& s, time_t now) {
    for (auto& [cfd, c] : s.connections) {
        if (c.role != Role::SERVER || c.groupID.empty()) continue;
        if (now - c.lastKeepalive < KEEPALIVE_INTERVAL) continue;

        size_t n = 0;
        auto it = s.mailbox.find(c.groupID);
        if (it != s.mailbox.end()) {
            n = it->second.size();
        }

        c.lastKeepalive = now;
        sendTo(s, cfd, "KEEPALIVE," + std::to_string(n));
    }
}

static void dropSilentPeers(Server& s, time_t now) {
    std::vector<int> silent;
    for (const auto& [cfd, c] : s.connections) {
        if (c.role != Role::SERVER) continue;
        if (now - c.lastSeen < PEER_TIMEOUT) continue;
        silent.push_back(cfd);
    }
    for (int fd : silent) {
        closeConnection(s, fd, "no traffic for 5 minutes");
    }
}

void tick(Server& s) {
    time_t now = time(NULL);
    sendKeepalives(s, now);
    dropSilentPeers(s, now);
    // maintainPeers(s, now);
}
