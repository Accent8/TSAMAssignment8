#include "protocol.h"
#include <string>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <cerrno>
#include <cstring>
#include <ctime>

std::string packMsg(const std::string& payload) {
    std::string frame;
    size_t totalLength = payload.size() + FRAME_OVERHEAD;
    if (totalLength > MAX_FRAME) {
        return frame; // to large: empty string = fail
    }
    // Safe narrowing, totalength <= MAX_FRAME fits in 16 bits
    uint16_t netLen = htons(static_cast<uint16_t>(totalLength));

    frame += SOH;
    frame.append(reinterpret_cast<const char*>(&netLen), 2); // raw bytes of length
    frame += STX;
    frame += payload;
    frame += ETX;
    return frame;
}

bool sendMsg(int fd, const std::string& data) {
    // send() may transmit fewer bytes than requested so keep going
    // until everythin is out
    size_t sent = 0;
    while (sent < data.size()) {
        ssize_t n = send(fd, (data.data() + sent), (data.size() - sent), 0);
        if (n < 0) {
            if (errno == EINTR) {
                continue; // interrupted by a signal before sending, retry
            } else {
                return false; // real error, errno is kept
            }
        } else {
            sent += n;
        }
    }
    return true;
}

bool unpackMsg(std::string& buf, std::string& out, size_t* discarded) {
    // Each pass looks at the frame at the front of the buf
    // Every path either removes at least one byte from the front and continues
    // or returns, so the loop always terminates
    while (true) {
        if (buf.length() < HEADER_SIZE) {
            return false; // not a full header yet, wait for more data
        }
        if (buf[0] != SOH) {
            // out of sync, skip ahead to the next possible frame start
            size_t pos = buf.find(SOH);
            if (pos != std::string::npos) {
                if (discarded) *discarded += pos; // skipped garbage before next SOH
                buf.erase(0, pos);
                continue;
            } else {
                // No SOH anywhere, the whole buffer is garbage
                // Clearing it also stops misbehaving clients from growing the buffer
                if (discarded) *discarded += buf.size(); // No SOH at all, everything deleted
                buf.clear();
                return false;
            }
        }

        
        uint16_t netLen;
        std::memcpy(&netLen, buf.data() + 1, 2);
        uint16_t len = ntohs(netLen);

        // Validates the header before waiting for the body, so a bogus length
        // cannot make us wait, and we ignore the following frame
        if (len < FRAME_OVERHEAD || len > MAX_FRAME || buf[3] != STX) {
            if (discarded) *discarded += 1;
            buf.erase(0, 1); // Drop the first byte, next pass resyncs
            continue;
        }
        if (buf.size() < len) {
            return false; // header is fine, body still incomplete so we wait
        }
        if (buf[len - 1] != ETX) {
            // Always discard from the front since deleting bytes in the middle
            // can turn a garbage frame into a valid one
            if (discarded) *discarded += 1;
            buf.erase(0, 1);
            continue;
        }

        // Positions, not searching since length bytes can contain any value
        // including 0x02, so find(STX) is unreliable
        out = buf.substr(HEADER_SIZE, len - FRAME_OVERHEAD);
        buf.erase(0, len);
        return true;
    }
}

std::string timestamp(){
    time_t curr= time(nullptr);
    char buf[32];
    strftime(buf, sizeof buf, "%d.%m.%Y - %H:%M:%S", localtime(&curr));
    return buf;
}
