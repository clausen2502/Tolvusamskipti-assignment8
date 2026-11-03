// Command line: ./tsamclient <ip> <port>
//
// Author(s):
//    Isak Clausen (isakb23@ru.is)
//    Orri Hrafn Kjartansson (orrihk23@ru.is)
//    Sigurður Sigurðsson (sigurdursig25@ru.is)

#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/tcp.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <string.h>
#include <algorithm>
#include <map>
#include <vector>
#include <poll.h>

#include "frame.h"
#include <ctime>

#include <iostream>
#include <sstream>
#include <map>

// Prints one line to the terminal with the current date and time in front
void printTimestamped(const std::string &text)
{
    char timestamp[20];
    time_t current_time = time(NULL);
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", localtime(&current_time));
    std::cout << timestamp << " " << text << std::endl;
}

int main(int argc, char *argv[])
{
    struct addrinfo hints, *svr;  // Network host entry for server
    struct sockaddr_in serv_addr; // Socket address for server
    int serverSocket;             // Socket used for server
    char buffer[1025];            // buffer for writing to server
    bool finished;
    int set = 1;                  // Toggle for setsockopt

    if (argc != 3)
    {
        printf("Usage: ./tsamclient <ip  port>\n");
        printf("Ctrl-C to terminate\n");
        exit(0);
    }
    
    memset(&hints, 0, sizeof(hints));

    hints.ai_family = AF_INET; // IPv4 only addresses
    hints.ai_socktype = SOCK_STREAM;


    if (getaddrinfo(argv[1], argv[2], &hints, &svr) != 0)
    {
        perror("getaddrinfo failed: ");
        exit(0);
    }

    struct hostent *server;
    server = gethostbyname(argv[1]);

    bzero((char *)&serv_addr, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    bcopy((char *)server->h_addr,
          (char *)&serv_addr.sin_addr.s_addr,
          server->h_length);
    serv_addr.sin_port = htons(atoi(argv[2]));

    serverSocket = socket(AF_INET, SOCK_STREAM, 0);

    // Turn on SO_REUSEADDR to allow socket to be quickly reused after
    // program exit.

    if (setsockopt(serverSocket, SOL_SOCKET, SO_REUSEADDR, &set, sizeof(set)) < 0)
    {
        printf("Failed to set SO_REUSEADDR for port %s\n", argv[2]);
        perror("setsockopt failed: ");
    }

    if (connect(serverSocket, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0)
    {
        // EINPROGRESS means that the connection is still being setup. Typically this
        // only occurs with non-blocking sockets. (The serverSocket above is explicitly
        // not in non-blocking mode, so this check here is just an example of how to
        // handle this properly.)
        if (errno != EINPROGRESS)
        {
            printf("Failed to open socket to server: %s\n", argv[1]);
            perror("Connect failed: ");
            exit(0);
        }
    }

    // Watch both stdin (user typing a message) and the server socket
    // (server sending us something) with a single poll() call, instead of
    // using a separate thread for one of them.
    std::vector<struct pollfd> fds;
    fds.push_back({STDIN_FILENO, POLLIN, 0});
    fds.push_back({serverSocket, POLLIN, 0});

    finished = false;
    std::string receiveBuffer;
    while (!finished)
    {
        int n = poll(fds.data(), fds.size(), -1);

        if (n < 0)
        {
            perror("poll failed: ");
            finished = true;
            continue;
        }

        // Something typed by the user - read a line and send it to the server.
        if (fds[0].revents & POLLIN)
        {
            bzero(buffer, sizeof(buffer));

            // read the line, fgets gives back NULL when input has ended
            char *result = fgets(buffer, sizeof(buffer), stdin);
            if (result == NULL)
            {
                finished = true;
            }
            else
            {
                // turn it into a string
                std::string command = buffer;

                // remove the newline character at the end, if there is one
                if (command.size() > 0)
                {
                    char last_character = command.back();

                    if (last_character == '\n')
                    {
                        command.pop_back();
                    }
                }
                // only send if the user actually typed something
                if (command != "") 
                {
                    std::string framedCommand = makeFrame(command);
                    bool sent = sendMessage(serverSocket, framedCommand);  

                    if (sent == false)
                    {
                        perror("send to server failed");
                        finished = true;
                    }
                    else
                    {
                        printTimestamped("Sent: " + command);
                    }
                }
            }
        }

        // The server sent something, collect the bytes and print every complete reply
        if (fds[1].revents & POLLIN)
        {
            int bytesReceived = recv(serverSocket, buffer, sizeof(buffer), 0);

            if (bytesReceived == 0)
            {
                printTimestamped("Server closed the connection");
                finished = true;
            }
            else if (bytesReceived < 0)
            {
                perror("receive from server failed");
                finished = true;
            }
            else
            {
                // add the new bytes to what we already have
                receiveBuffer.append(buffer, bytesReceived);

                // take out every complete reply and print it
                std::string reply;
                int result = 1;     
                while (result != 0)   
                {
                    result = getFrame(receiveBuffer, reply);
                    if (result == 1)
                    {
                        printTimestamped("Received: " + reply);
                    }
                    else if (result == -1)
                    {
                        printTimestamped("Dropped malformed reply from server");
                    }
                }
            }
        }
    }
}
