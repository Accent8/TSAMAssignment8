CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra

all: tsamserver tsamclient

tsamserver: tsamserver.cpp protocol.cpp commands.cpp protocol.h commands.h identity.h log_event.h 
	$(CXX) $(CXXFLAGS) -o $@ tsamserver.cpp protocol.cpp commands.cpp

tsamclient: tsamclient.cpp protocol.cpp protocol.h
	$(CXX) $(CXXFLAGS) -o $@ tsamclient.cpp protocol.cpp

clean:
	rm -f tsamclient tsamserver

.PHONY: all clean
