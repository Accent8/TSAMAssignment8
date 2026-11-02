#pragma once
#include <string>
#include <map>
#include <vector>
#include <ctime>
#include <poll.h>
#include "commands.h"

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
    bool isLocal = false;
    time_t lastKeepalive = 0;
    time_t lastSeen = 0;
};

struct KnownServer {
    std::string ip;
    int port;
    std::string heardFrom; // who told us
    time_t lastSeen;
    bool selfReported;
    time_t lastSeen = 0;
    time_t lastAttempt = 0;
};

struct Server {
    std::vector<pollfd> fds;
    std::map<int, Connection> connections;
    std::map<std::string, KnownServer> known;
    Mailbox mailbox;
    int myPort = 0;
    std::string myIp;
};

// helpers.cpp
bool parseInt(const std::string& text, int& out);
bool parseLong(const std::string& text, long& out);
std::string localIP(int fd);
bool isUsableAddress(const std::string& ip);

// network.cpp
int setUpListen(int port);
bool sendTo(Server& s, int fd, const std::string& payload);
void acceptConnection(Server&s, int listenSock);
int connectToPeer(Server& s, std::string ip, int port);
void closeConnection(Server& s, int fd, const char* reason);

// peers.cpp
std::string buildServersList(const Server& s);
void parseServersList(Server& s, int fd, const std::string& args);
int findPeer(const Server&s, const std::string& groupId);
void tick(Server& s);

// server_cmd.cpp
std::string toSendMsg(const Message& m);
bool deliverHeld(Server& s, int fd, const std::string& group);
bool handleServerSendMsg(Server& s, int fd, const std::string& args);
bool handleServerGetMsgs(Server& s, int fd, const std::string& group);
bool handleServerStatusResp(Server& s, int fd, const std::string& args);
bool handleServerCommand(Server& s, int fd, const std::string& cmd);

// client_cmd.cpp
bool handleClientSendMsg(Server& s, int fd, const std::string& args);
bool handleClientConnect(Server& s, int fd, const std::string& args);
bool handleClientCommand(Server& s, int fd, const std::string& cmd);