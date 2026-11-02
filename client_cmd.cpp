#include "server.h"
#include "log_event.h"
#include "identity.h"
#include "protocol.h"
#include <string>

bool handleClientSendMsg(Server& s, int fd, const std::string& args) {
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
    
    if (out.size() > MAX_CMD) {
        log_note("Message too big!");
        return sendTo(s, fd, "ERROR, message too big!");
    }

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
    for (const auto& [cfd, c] : s.connections) {
        if (c.ip == ip && c.listenPort == port) {
            std::string who = c.groupID.empty() ? c.ip + ":" + // Incase the groupID is empty
            std::to_string(c.listenPort) : c.groupID;          // we use IP and Port instead
            return sendTo(s, fd, "ERROR, already connected to " + who);
        }
    }
    if (countPeers(s) >= MAX_PEERS) {
        return sendTo(s, fd, "ERROR, already connected to max nr of servers");
    }
    int peerFd = connectToPeer(s, ip, port);
    if (peerFd == -1) {
        return sendTo(s, fd, "ERROR, connect fail");
    }
    return sendTo(s, fd, "OK");
}

bool handleClientCommand(Server& s, int fd, const std::string& cmd) {
    if (cmd.rfind("SENDMSG,", 0) == 0) {
        return handleClientSendMsg(s, fd, cmd.substr(8)); // 8 = strlen("SENDMSG,")
    }
    if (cmd.rfind("CONNECT,", 0) == 0) {
        return handleClientConnect(s, fd, cmd.substr(8)); // 8 = strlen("CONNECT",)
    }
    std::string response = handleCommand(cmd, s.mailbox);
    if (response.size() > MAX_CMD) {
        log_note("ERROR, response too large");
        response = "ERROR, response too large";
    }
    return sendTo(s, fd, response);
}