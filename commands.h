#pragma once

#include <string>
#include <map>
#include <deque>

// One stored message, recipient becomes the key of the mailbox entry
// this messages lives in
struct Message {
    std::string from;
    std::string to;
    time_t created; // from the SENDMSG, passed on unchanged
    std::string text;
};

// username -> that users messages with the oldest first
// user is never in the map if their queue is empty
using Mailbox = std::map<std::string, std::deque<Message>>;
// Parses one command, then updates the mailbox, returns the response payload to send back
// supported commands: SENDMSG,GETMSG,STATUSREQ
// errors are returned: "ERROR, reason"
std::string handleCommand(const std::string& cmd, Mailbox& mailbox);
std::string handleStatusReq(const Mailbox& mailbox);
