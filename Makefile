# Builds the TSAM Assignment 6 server and client

all: tsamserver tsamclient

CXXFLAGS = -Wall -std=c++17

tsamserver: server.cpp frame.h
	g++ ${CXXFLAGS} server.cpp -o $@

tsamclient: client.cpp frame.h
	g++ ${CXXFLAGS} client.cpp -o $@

clean:
	rm -f tsamserver tsamclient