# Assignment 8: BOT-NET

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
It writes its log to `server.log` in the current directory (appends to the file)

## Operating system

Developed on macOS and tested on Linux (TSAM Server).

## Protocol

Every command and every response is sent as one frame:

```
<SOH><length><STX><payload><ETX>
```

`SOH` = 0x01, `STX` = 0x02, `ETX` = 0x03. `length` is a 16-bit unsigned integer in
network byte order giving the size of the whole frame, including the 5 framing bytes.
Frames of up to 5005 bytes are accepted.
Self reported IPs beats hearsay IPs.
Localhost only client.

We also followed the protocol decision that the class agreed apon:
Self first in SERVERS
Anyone may collect mail with GETMSGS, and gets logged

## Commands and responses

Fields are separated by commas. Errors are always returned as `ERROR, <reason>`.

### `SENDMSG,<GROUP_ID>,<to>,<message>`

Stores `<message>` for user `<to>`, sent by `<GROUP_ID>`. Only the first two commas separate
fields, so the message itself may contain commas. An empty message is allowed.
`<from>` gets ignored, server sends as GROUP_ID

| Situation | Response |
|---|---|
| Message stored | `OK` |
| Fewer than three fields | `ERROR, missing fields` |
| Empty `<to>` | `ERROR, empty field` |

### `GETMSG,<username>`

Returns the oldest stored message for `<username>` and deletes it (first in, first out).

| Situation | Response |
|---|---|
| A message is stored | `<from>,<message>` (split on the first comma only) |
| No messages for this user | `NONE` |
| Empty username | `ERROR, missing user` |

### `STATUSREQ`

Returns `<group1>:<count1>,<group2>:<count2>,...` for every group who currently has at
least one stored message (no spaces), or exactly `EMPTY` if no messages are stored.
A group disappears from the list as soon as their last message has been fetched.
Any arguments after `STATUSREQ` are ignored.

### `CONNECT,<IP>,<PORT>`

Connects to the server at `<IP>` to `<PORT>`
Replies `OK` with a successfull new connection
`Already connected` if we are already connected to that peer
We can have a maximum of 8 peers, and we cannot connect to ourselves



### Anything else

Unknown commands from client, including an empty command, get `ERROR, unknown command`.
Unkown commands from servers get DROPped with no reply


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
- a header with a missing `STX` or a length outside 5..5005 has its `SOH` dropped;
- a frame whose last byte is not `ETX` has its `SOH` dropped;
- if the buffer contains no `SOH` at all, it is cleared.

The number of discarded bytes is logged. Malformed input never closes the connection
or crashes the server, and the per client buffer cannot grow without bound.

**Disconnecting clients.** When a client disconnects (cleanly or through an error),
its socket is closed and its buffer is freed. Stored messages are kept, so a message
can be fetched after its sender has disconnected. `SIGPIPE` is ignored, so writing to a
client that has just disconnected, just exits instead of terminating the server.

**Message storage.** Messages are kept in memory only and are lost when the server stops.

**Oversized responses.** If a response would not fit into one 5005-byte frame, the
server sends `ERROR, response too large` instead.

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

## Server to Server protocol

### `HELO`
Each time we connect to a server we send `HELO` then we wait for a `HELO` back, if we get one we send `SERVERS,` back
Then if we have any held mail for this group we deliver it

### `SERVERS`
We put ourselves first in this list so other server know where to connect to us. 
Then follows a list of servers we have identified and their listen ports.
Once we get SERVERS back we parse the list and build our connection book with those servers, skipping any invalid entries and update the entires we know.
Self reporting form servers beats hearsay

### `KEEPALIVE,n`
We send this to connected peers every 60 seconds with the count of how many messages we hold for them.
If we get n > 0 response we send `GETMSGS` to that peer

### `SENDMSG` 
We send this to recipient, could be us, another peer or someone we dont know
if this is someone we dont know we hold onto that message.
and we leave the timestamp unchanged.

### `GETMSGS`
We hand over everything we hold for that group, and log who collected it.

### `STATUSREQ/STATUSRESP` 
On `STATUSRESP` we send `GETMSGS` for ourselves and for groups we are connected too.
On `STATUSREQ` we list all groups we hold messages for.

## Points claimed:

### 1b
```bash
Line 1879 - 2026-10-28T14:08:10Z ACCEPT 81.15.111.84:64482 - - 
Line 1880 - 2026-10-28T14:08:10Z RECV 81.15.111.84:64482 - HELO,G_snillar
Line 1881 - 2026-10-28T14:08:10Z note Server added since first msg starts with HELO
Line 1882 - 2026-10-28T14:08:10Z note Adding groupID: G_snillar to client: 81.15.111.84:64482
Line 1883 - 2026-10-28T14:08:10Z SEND 81.15.111.84:64482 G_snillar HELO,G_banana
Line 1884 - 2026-10-28T14:08:10Z SEND 81.15.111.84:64482 G_snillar SERVERS,G_banana,130.208.246.98,4088;I_instr1,130.208.246.98,5001;
Line 1885 - 2026-10-28T14:08:10Z RECV 81.15.111.84:64482 G_snillar SERVERS,I_instr1,130.208.246.98,5001;G_banana,130.208.246.98,4088;
Line 1886 - 2026-10-28T14:08:10Z note SERVERS from G_snillar: I_instr1,130.208.246.98,5001;G_banana,130.208.246.98,4088;
```
### 1c
```bash
First Group
Line 13774 - 2026-11-02T14:35:27Z RECV 130.208.246.98:44392 G_kiwi SENDMSG,G_kiwi,G_banana,1793630127,hellooooo
Line 13829 - 2026-11-02T14:37:13Z SEND 127.0.0.1:42552 - G_kiwi,hellooooo
Second Group
Line 15104 - 2026-11-02T17:29:37Z RECV 130.208.246.98:4044 G_G-Monster SENDMSG,G_G-Monster,G_banana,1793640531,Have a Monster Energy drink Today and Every DAY, or 7 Monsters if possible
Line 15115 - 2026-11-02T17:30:19Z SEND 127.0.0.1:37268 - G_G-Monster,Have a Monster Energy drink Today and Every DAY, or 7 Monsters if possible
```
### 1d
```bash
First Group
Line 15116 - 2026-11-02T17:30:33Z RECV 127.0.0.1:37268 - SENDMSG,G_banana,G_G-Monster,I love monster
Line 15117 - 2026-11-02T17:30:33Z SEND 130.208.246.98:4044 G_G-Monster SENDMSG,G_banana,G_G-Monster,1793640633,I love monster
Second Group
Line 13840 - 2026-11-02T14:37:26Z RECV 127.0.0.1:42552 - SENDMSG,G_banana,G_kiwi,Wasssuppppp
Line 13841 - 2026-11-02T14:37:26Z SEND 130.208.246.98:44392 G_kiwi SENDMSG,G_banana,G_kiwi,1793630246,Wasssuppppp
```
### 2g
```bash
Line 13612 - 2026-11-02T14:28:15Z RECV 130.208.246.98:5010 I_dispatch SENDMSG,I_dispatch,G_banana,1793629695,Credited: dispatch 0 (Roll call). Fragment: Löng ekkasog haustfiðlanna . You now hold 1.
```

## Authorship statement
Hreimur wrote all code.
Daniela and Tumi gave input into the structure of the code.
cppreference.com, man pages and stackoverflow.com were used to help with code writing.
Claude was used to explain code concepts, and to review our code and point out bugs.
All code and this README was written by us.