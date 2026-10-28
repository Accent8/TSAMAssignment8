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

// SENDMSG,from,to,message
// only the first two commas separate the fields, message may contain a comma
// empty messages are also allowed
static std::string handleSendMsg(const std::string& args, Mailbox& mailbox) {
    size_t c1 = args.find(','); // end of from
    if (c1 == std::string::npos) {
        return "ERROR, missing fields";
    }
    size_t c2 = args.find(',', c1 + 1); // end of to
    if (c2 == std::string::npos) {
        return "ERROR, missing fields";
    }

    // cut the message up
    std::string sender = args.substr(0, c1); // from the start to first comma
    std::string receiver = args.substr(c1 + 1, c2 - c1 - 1); // between the commas
    std::string text = args.substr(c2 + 1); // everything else

    // validate
    if (sender.empty() || receiver.empty()) {
        return "ERROR, empty field";
    }

    // store message if complete
    mailbox[receiver].push_back({sender, text});
    return "OK";
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
    else if (name == "SENDMSG") return handleSendMsg(args, mailbox);
    else if (name == "GETMSG") return handleGetMsg(args, mailbox); 
    else return "ERROR, unknown command";
}
