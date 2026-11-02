CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra

all: tsamserver tsamclient

SERVER_SRCS = tsamserver.cpp network.cpp peers.cpp server_cmd.cpp client_cmd.cpp \
			helpers.cpp protocol.cpp commands.cpp

SERVER_HDRS = server.h protocol.h commands.h identity.h log_event.h

tsamserver: $(SERVER_SRCS) $(SERVER_HDRS)
	$(CXX) $(CXXFLAGS) -o $@ $(SERVER_SRCS)

tsamclient: tsamclient.cpp protocol.cpp protocol.h
	$(CXX) $(CXXFLAGS) -o $@ tsamclient.cpp protocol.cpp

clean:
	rm -f tsamclient tsamserver

.PHONY: all clean
