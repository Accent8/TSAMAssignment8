#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <csignal>
#include <string>
 
#include <unistd.h>
#include <poll.h>
#include <netdb.h>
#include <sys/types.h>
#include <sys/socket.h>

#include "protocol.h"

//connects to the server
int connecttoServer(const char* ip, const char* port){
    //describe what kind of address we want
    struct addrinfo hints, *svr, *p; //hint= what you are asking for, svr(server) points to the results
    memset(&hints, 0, sizeof(hints));
    hints.ai_family= AF_INET; //ipv4
    hints.ai_socktype= SOCK_STREAM; //tcp

    int result= getaddrinfo(ip, port, &hints, &svr);
    if (result != 0){
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(result));
        return -1;
    }

    //getaddrinfo can ret many addresses, try all until conects
    int sock= -1;
    for(p= svr; p != nullptr; p= p->ai_next){
        sock= socket(p-> ai_family, p-> ai_socktype, p-> ai_protocol); //socket(svr->ai_family, svr->ai_socktype, svr->ai_protocol) ef thad a ad vera haegt ad breita hints
        if (sock < 0) continue;
        if (connect(sock, p-> ai_addr, p->ai_addrlen)== 0) break; //connected
        close(sock);
        sock= -1;
    }

   freeaddrinfo(svr);
   if (sock <0){
    perror("could not connect to server");
   }
   return sock;
}

//packs a command line in a frame and sends to server
bool sendCommand(int sock, std::string line){
    //if (!line.empty() && line.back()== '\r') line.pop_back(); //Remove a trailing '\r' (e.g. input pasted from Windows)
    if (line.empty()) return true;

    std::string frame= packMsg(line);
    if (frame.empty()){
        printf("command too long, not sent\n");
        return true;
    }

    if (!sendMsg(sock, frame)){
        perror("failed sending to server");
        return false;
    }

    printf("[%s] sent: %s\n", timestamp().c_str(), line.c_str()); //std::cout << "[" << timestamp() << "] SENT: " << line << std::endl;
    fflush(stdout); //makes sure the line is written out right away
    return true;

}

//reads what the user typed and sends every complete line
bool userInput(int sock, std::string &inputBuffer){
    char buffer[1024];
    ssize_t bytes_read= read(STDIN_FILENO, buffer, sizeof(buffer));
    if (bytes_read<0){
        if(errno== EINTR) return true;
        perror("reading from stdin failed");
        return false;
    }
    else if(bytes_read== 0){
        if (!inputBuffer.empty()) sendCommand(sock, inputBuffer);
        return false;
    } else{
    inputBuffer.append(buffer, bytes_read);
    }

    while (true)
    {
        size_t pos= inputBuffer.find('\n');
        if(pos== std::string::npos){
            break; //no complete line ready. wait for more
        }
        std::string line= inputBuffer.substr(0, pos);
        inputBuffer.erase(0, pos + 1);
        if (!sendCommand(sock, line)) return false;
    }
    return true;    
}

//reads what the server sent and prints every complete frame
bool serverData(int sock, std::string &responsBuffer){
    char buffer[5000];
    ssize_t n= recv(sock, buffer, sizeof(buffer), 0);
    if(n== 0){
        printf("server closed the connection\n");
        return false;
    }
    else if(n < 0){
        if(errno== EINTR) return true; //interrupted, try again
        perror("receving the response from server failed");
        return false;
    } else{
        responsBuffer.append(buffer, n);
    }

    //one recv can hold part of a frame or several frames so we keep the bytes
    // in the responsBuffer and prints every complete frame
    std::string respons;
    while(unpackMsg(responsBuffer, respons)){
        printf("[%s] response: %s\n", timestamp().c_str(), respons.c_str());
    }
    fflush(stdout);
    return true;
}

int main(int argc, char* argv[]) {

    //check that we got 3 arguments
    if(argc != 3){
        printf("should be 3 arguments: <client> <ip> <port>\n");
        printf("ctrl-c to quit\n");
        exit(1);
    }

    //prevent killing the client if the conection to the server is closed
    signal(SIGPIPE, SIG_IGN);

    int serverSocket= connecttoServer(argv[1], argv[2]);
    if (serverSocket< 0) exit(1);

    printf("Connected to server\n");
    printf("type a command (SENDMSG,<from>,<to>,<msg> / GETMSG,<username> / STATUSREQ)\n");
    fflush(stdout);

    //watch stdin and the socket at the same time, to prevent busy 
    // waiting use poll() to block until one of them has data
    struct pollfd fds[2];
    fds[0]= {STDIN_FILENO, POLLIN, 0};
    fds[1]= {serverSocket, POLLIN, 0};

    std::string responsBuffer; //
    std::string inputBuffer; //

    bool running= true;
    while(running){
        int n= poll(fds, 2, -1);
        if(n < 0){
            if (errno== EINTR) continue;
            perror("poll failed");
            break;
        }

        //
        if(fds[1].revents & (POLLIN | POLLHUP | POLLERR)){
            running= serverData(serverSocket, responsBuffer);
        }
        if(running && (fds[0].revents & (POLLIN | POLLHUP))){
            running= userInput(serverSocket, inputBuffer);
        }
    }

   close(serverSocket);
   return 0;


    
}
