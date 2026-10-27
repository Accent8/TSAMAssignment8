#pragma once

#include <string>
#include <cstddef>

constexpr char SOH = 0x01; // start of header
constexpr char STX = 0x02; // start of text
constexpr char ETX = 0x03; // end of text
constexpr size_t MAX_FRAME = 5000; // largest frame we accept or send
constexpr size_t HEADER_SIZE = 4; // SOH + 2 length bytes + STX
constexpr size_t FRAME_OVERHEAD = 5; // header + ETX

// packs a payload to include the header given in the assignment
// returns an empty string if the payload is to large
std::string packMsg(const std::string& payload);

// Sends all of data on socket fd, looping over partial sends and retrying on EINTR
// Returns false on any other error except EINTR
bool sendMsg(int fd, const std::string& data);

// Extracts one complete frame from the front of buff into out
// Returns false if more data is needed. malformed bytes are discarded
// if discarded, the number of discarded bytes is added to it
bool unpackMsg(std::string& buf, std::string& out, size_t* discarded = nullptr);

// Current local time as "DD.MM.YYYY - HH:MM:SS" 
std::string timestamp();
