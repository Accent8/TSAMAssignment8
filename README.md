# Assignment 6: Client-Server Messaging

A simple mail server `tsamserver` and client `tsamclient` communicating over TCP
using the framed protocol from the assignment.

## Building

```bash
make
```

This builds both executables, `tsamserver` and `tsamclient`. `make clean` removes them.
Requires `g++` with C++17 support, no third party libraries are used.

## Running

```bash
./tsamserver <port>                # e.g. ./tsamserver 4044
./tsamclient <server-ip> <port>    # e.g. ./tsamclient 127.0.0.1 4044
```

The server takes no input from the terminal; all communication happens over TCP.
It writes its log to `server.log` in the current directory (overwritten on every start)
and mirrors each log line to stdout.

## Operating system

Developed on macOS and tested on Linux (WSL).

## Protocol

Every command and every response is sent as one frame:

```
<SOH><length><STX><payload><ETX>
```

`SOH` = 0x01, `STX` = 0x02, `ETX` = 0x03. `length` is a 16-bit unsigned integer in
network byte order giving the size of the whole frame, including the 5 framing bytes.
Frames of up to 5000 bytes are accepted.

## Commands and responses

Fields are separated by commas. Errors are always returned as `ERROR, <reason>`.

### `SENDMSG,<from>,<to>,<message>`

Stores `<message>` for user `<to>`, sent by `<from>`. Only the first two commas separate
fields, so the message itself may contain commas. An empty message is allowed.

| Situation | Response |
|---|---|
| Message stored | `OK` |
| Fewer than three fields | `ERROR, missing fields` |
| Empty `<from>` or `<to>` | `ERROR, empty field` |

### `GETMSG,<username>`

Returns the oldest stored message for `<username>` and deletes it (first in, first out).

| Situation | Response |
|---|---|
| A message is stored | `<from>,<message>` (split on the first comma only) |
| No messages for this user | `NONE` |
| Empty username | `ERROR, missing user` |

### `STATUSREQ`

Returns `<user1>:<count1>,<user2>:<count2>,...` for every user who currently has at
least one stored message (no spaces), or exactly `EMPTY` if no messages are stored.
A user disappears from the list as soon as their last message has been fetched.
Any arguments after `STATUSREQ` are ignored.

### Anything else

Unknown commands, including an empty command, get `ERROR, unknown command`.

## Server behaviour

**Multiple clients.** The server uses a single `poll()` loop over the listening socket
and all client sockets. It blocks while idle (no busy-waiting) and serves all connected
clients without one client blocking another.

**Partial and combined frames.** TCP is a byte stream, so each client has its own
receive buffer. Bytes are appended as they arrive and a command is only processed once
its complete frame has been received. Several frames arriving together are all
processed, and a frame split across several `recv()` calls is reassembled.

**Malformed input.** Malformed data is discarded and the server resyncs on the next `SOH` byte, so a valid frame following garbage is still processed. Specifically:

- bytes before an `SOH` are skipped;
- a header with a missing `STX` or a length outside 5..5000 has its `SOH` dropped;
- a frame whose last byte is not `ETX` has its `SOH` dropped;
- if the buffer contains no `SOH` at all, it is cleared.

The number of discarded bytes is logged. Malformed input never closes the connection
or crashes the server, and the per client buffer cannot grow without bound.

**Disconnecting clients.** When a client disconnects (cleanly or through an error),
its socket is closed and its buffer is freed. Stored messages are kept, so a message
can be fetched after its sender has disconnected. `SIGPIPE` is ignored, so writing to a
client that has just disconnected, just exits instead of terminating the server.

**Message storage.** Messages are kept in memory only and are lost when the server stops.

**Oversized responses.** If a response would not fit into one 5000-byte frame, the
server sends `ERROR, response too large` instead.

## Server log

Every line is timestamped (`DD.MM.YYYY - HH:MM:SS`) and flushed immediately. The log
records server start, every client connect and disconnect (with IP and port), every
received command (`RECV`), every sent response (`SEND`), discarded malformed bytes,
and errors such as failed sends. Example:

```
12.10.2026 - 19:01:50  Server listening on port 4044
12.10.2026 - 19:01:51  Client 127.0.0.1:50538 (fd 5) connected
12.10.2026 - 19:01:51  RECV from 127.0.0.1:50538 (fd 5): SENDMSG,alice,bob,hi
12.10.2026 - 19:01:51  SEND to 127.0.0.1:50538 (fd 5): OK
12.10.2026 - 19:01:51  Client 127.0.0.1:50538 (fd 5) disconnected
```

## Client

The client connects to the server given on the command line (getaddrinfo() resolves the address and each returned address is tried until one connects), then reads commands from stdin, one command per line:

`SENDMSG,<from>,<to>,<message>`
`GETMSG,<username>`
`STATUSREQ`

Each line is sent to the server unchanged, packed into one frame, so the server decides whether a command is valid. Empty lines are ignored, and a line too long to fit into one frame is not sent (the client prints a message instead).

Output. Every command sent and every response received is printed with a timestamp in the same format as the server log:

[12.10.2026 - 20:58:40] sent: SENDMSG,hreimur,dani,hi
[12.10.2026 - 20:58:40] responce: OK

No busy-waiting. The client uses poll() on stdin and the server socket at the same time, so it blocks until the user types something or the server sends data, and responses are shown as soon as they arrive.

Partial and combined frames. Like the server, the client keeps a buffer of received bytes and only prints a response once its complete frame has arrived. It uses the same framing code (protocol.cpp), so malformed data from the server is discarded the same way. Input from stdin is also buffered until a complete line has been read.

Exiting. The client exits when stdin is closed (Ctrl+D, or end of piped input; an unfinished last line is still sent first), when the server closes the connection (server closed the connection is printed) or on a socket error. SIGPIPE is ignored, so a server that disappears while the client is sending does not kill the client.

The client does not interpret the responses: GETMSG replies `(<from>,<message>)`, NONE and ERROR, `<reason>` are printed exactly as the server sent them.