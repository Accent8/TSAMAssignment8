#include "server.h"
#include "log_event.h"
#include "identity.h"
#include <string>
#include <sstream>

// Rebuilds the SENDMSG command from a stored message
std::string toSendMsg(const Message& m) {
    return "SENDMSG," + m.from + "," + m.to + "," + std::to_string(m.created)
    + "," + m.text;
}

bool deliverHeld(Server& s, int fd, const std::string& group) {
    auto entry = s.mailbox.find(group); // retrive the messages for group
    if (entry == s.mailbox.end()) {
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

bool handleServerSendMsg(Server& s, int fd, const std::string& args) {
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
// commands on this connection.
bool handleServerGetMsgs(Server& s, int fd, const std::string& group) {
    Connection& c = s.connections[fd];
    if (group.empty()) {
        log_event("DROP", c.ip.c_str(), c.port, c.groupID.c_str(), "GETMSGS group empty dropping...");
        return true;
    }
    if (group != c.groupID) {
        log_note("%s is collecting mail for: %s", c.groupID.c_str(), group.c_str());
    }
    if (s.mailbox.count(group) == 0) {
        log_note("nothing held for %s", group.c_str());
        return true;
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
        if (count < 0) {
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

bool handleServerCommand(Server& s, int fd, const std::string& cmd) {
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
            log_note("%s:%d claimed %s, but already has a groupID set: %s", c.ip.c_str(),
             c.port, args.c_str(), c.groupID.c_str());
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