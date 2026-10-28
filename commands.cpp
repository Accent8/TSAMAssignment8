#include "commands.h"

// STATUSREQ: "user1:count1,user2:count2,..." or "EMPTY"
// format was given in assignment: no spaces, no trailing comma
std::string handleStatusReq(const Mailbox& mailbox) {
    if (mailbox.empty()) {
        return "EMPTY";
    }
    std::string result;
    for (const auto& [user, queue] : mailbox) {
        if (!queue.empty()) {
            if (!result.empty()) {
                result += ",";   // separator only between entries
            }
            result += user + ":" + std::to_string(queue.size());
        }
    }
    return result;
}

// GETMSG,user
// returns the oldest message, then it gets deleted
// or none if there are no messages
static std::string handleGetMsg(const std::string& user, Mailbox& mailbox) {
    if (user.empty()) {
        return "ERROR, missing user";
    }

    // find() instead of [] so we dont create a new empty entry
    auto entry = mailbox.find(user);
    if (entry == mailbox.end()) {
        return "NONE";
    }
    std::deque<Message>& msgQueue = entry->second;
    Message m = msgQueue.front();
    msgQueue.pop_front();

    if(msgQueue.empty()) {
        mailbox.erase(entry);
    }

    return m.from + "," + m.text;
}

std::string handleCommand(const std::string& cmd, Mailbox& mailbox) {
    // splitt of the command name, args is the only thing we want after the first comma
    size_t comma = cmd.find(',');
    std::string name = cmd.substr(0, comma); // whole command if there is no comma
    std::string args = (comma == std::string::npos) ? "" : cmd.substr(comma + 1);

    if (name == "STATUSREQ") return handleStatusReq(mailbox); // extra args are ignored
    else if (name == "GETMSG") return handleGetMsg(args, mailbox); 
    else return "ERROR, unknown command";
}
