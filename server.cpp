//
// Command line: ./tsamserver <port>
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
#include <list>
#include <poll.h>

#include "frame.h"
#include "identity.h"
#include "log_event.h"

#include <deque>
#include <ctime>

#include <iostream>
#include <sstream>
#include <fcntl.h>

#ifndef SOCK_NONBLOCK
#define SOCK_NONBLOCK O_NONBLOCK
#endif

#define BACKLOG 5 // Allowed length of queue of waiting connections

// Simple class for handling connections from clients.
//
// Client(int socket) - socket to send/receive traffic from client.
class Client
{
public:
    int sock;            // socket of client connection
    std::string buffer;  // bytes received so far from this client
    std::string ip;      // IP address of the other end
    int port;            // port of the other end
    std::string groupId;  // their group ID
    bool weDialled;      // true if we connected to their listening port

    Client(int socket) : sock(socket), port(0), weDialled(false) {}

    ~Client() {} // Virtual destructor defined for base class
};

std::map<int, Client *> clients; // Lookup table for per Client information
int listenPort = 0; // the port we accept connections on

class Message
{
public:
    std::string sender;    // group that originally sent the message
    std::string to;        // group that should receive the message
    std::string timestamp; // time the message was first created
    std::string text;      // message text
};

std::map<std::string, std::deque<Message>> mailboxes; // messages waiting for each group

// A server we have heard about in a SERVERS response but may or may not
// be connected to. These are our candidates to dial when we need more peers.
struct KnownPeer
{
    std::string groupId;
    std::string ip;
    int port;
    time_t lastTry = 0; // last time we tried connecting
};
std::map<std::string, KnownPeer> knownPeers; // servers we can try connecting to

// Open socket for specified port.
//
// Returns -1 if unable to create the socket for any reason.

int open_socket(int portno)
{
    struct sockaddr_in sk_addr; // address settings for bind()
    int sock;                   // socket opened for this port
    int set = 1;                // for setsockopt

    // Create socket for connection. Set to be non-blocking, so recv will
    // return immediately if there isn't anything waiting to be read.
#ifdef __APPLE__
    if ((sock = socket(AF_INET, SOCK_STREAM, 0)) < 0)
    {
        perror("Failed to open socket");
        return (-1);
    }
#else
    if ((sock = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0)) < 0)
    {
        perror("Failed to open socket");
        return (-1);
    }
#endif

    // Turn on SO_REUSEADDR to allow socket to be quickly reused after
    // program exit.

    if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &set, sizeof(set)) < 0)
    {
        perror("Failed to set SO_REUSEADDR:");
    }
    set = 1;
#ifdef __APPLE__
    if (setsockopt(sock, SOL_SOCKET, SOCK_NONBLOCK, &set, sizeof(set)) < 0)
    {
        perror("Failed to set SOCK_NOBBLOCK");
    }
#endif
    memset(&sk_addr, 0, sizeof(sk_addr));

    sk_addr.sin_family = AF_INET;
    sk_addr.sin_addr.s_addr = INADDR_ANY;
    sk_addr.sin_port = htons(portno);

    // Bind to socket to listen for connections from clients

    if (bind(sock, (struct sockaddr *)&sk_addr, sizeof(sk_addr)) < 0)
    {
        perror("Failed to bind to socket:");
        close(sock);
        return -1;
    }
    else
    {
        return (sock);
    }
}

// Close a connection: log it as close, close the socket, and remove it from
// the list of file descriptors being watched by poll().
//
// NOTE: this does NOT remove the Client from the "clients" map - main() does
// that after this returns, which is why client is still valid here.

void closeClient(int clientSocket, std::vector<struct pollfd> *fds)
{
    Client *client = clients[clientSocket];   // socket -> its Client, for the log fields

    std::string print = "Client closed connection: " + std::to_string(clientSocket);
    std::cout << print << std::endl;
    log_event("CLOSE", client->ip.c_str(), client->port,
              client->groupId.c_str(), print.c_str());

    close(clientSocket);

    // Find and remove this socket's entry from the list of fds polled.
    auto fdIt = std::find_if(fds->begin(), fds->end(),
                             [clientSocket](const struct pollfd &pfd)
                             {
                                 return pfd.fd == clientSocket;
                             });

    if (fdIt != fds->end())
    {
        fds->erase(fdIt);
    }
}

// Put a command inside a frame, send it and write it in the log.
// A socket might only send some of the bytes, so keep track of how many left.
bool sendReply(int sock, const std::string &reply)
{
    Client *client = clients[sock];
    std::string frame = makeFrame(reply);
    size_t sentBytes = 0;

    while (sentBytes < frame.size())
    {
        int flags = MSG_DONTWAIT;
        #ifdef MSG_NOSIGNAL
            flags |= MSG_NOSIGNAL;
        #endif
        int n = send(sock, frame.data() + sentBytes, frame.size() - sentBytes, flags);

        if (n > 0)
        {
            sentBytes += n;
        }
        else if (n < 0 && errno == EINTR)
        {
            continue; // interrupted by a signal, try again
        }
        else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        {
            // The socket is busy, wait briefly until it can send again.
            struct pollfd waiting;
            waiting.fd = sock;
            waiting.events = POLLOUT;
            waiting.revents = 0;

            if (poll(&waiting, 1, 1000) <= 0)
            {
                log_note("send timed out to %s:%d", client->ip.c_str(), client->port);
                return false;
            }
        }
        else
        {
            log_note("send failed to %s:%d", client->ip.c_str(), client->port);
            return false;
        }
    }

    log_event_bytes("SEND", client->ip.c_str(), client->port,
                    client->groupId.c_str(), reply.data(), reply.size());
    return true;
}

// connect to a server at the given IP address and port
// wait at most one second so an offline server does not freeze ours 
int connectToServer(const std::string &ip, int port, std::vector<struct pollfd> *fds)
{
    if (port < 1 || port > 65535)
    {
        return -1;
    }

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0)
    {
        perror("socket failed");
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    if (inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) != 1)
    {
        close(sock);
        return -1;
    }

    // Make the connection nonblocking so connect() cannot wait forever.
    int flags = fcntl(sock, F_GETFL, 0);
    if (flags < 0 || fcntl(sock, F_SETFL, flags | O_NONBLOCK) < 0)
    {
        close(sock);
        return -1;
    }

    int result = connect(sock, (struct sockaddr *)&addr, sizeof(addr));
    if (result < 0 && errno == EINPROGRESS)
    {
        struct pollfd waiting;
        waiting.fd = sock;
        waiting.events = POLLOUT;
        waiting.revents = 0;

        result = poll(&waiting, 1, 1000); // wait up to one second
        if (result > 0)
        {
            int error = 0;
            socklen_t errorSize = sizeof(error);
            if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &error, &errorSize) < 0 || error != 0)
            {
                result = -1;
            }
        }
        else
        {
            result = -1;
        }
    }
    else if (result < 0)
    {
        result = -1;
    }
    else
    {
        result = 1; // connect() worked immediately
    }

    if (result < 0)
    {
        log_note("could not connect to %s:%d", ip.c_str(), port);
        close(sock);
        return -1;
    }

    fds->push_back({sock, POLLIN, 0});
    clients[sock] = new Client(sock);
    clients[sock]->ip = ip;
    clients[sock]->port = port;
    clients[sock]->weDialled = true;

    log_event("CONN", ip.c_str(), port, NULL, "outgoing connection");
    sendReply(sock, std::string("HELO,") + GROUP_ID);
    return sock;
}

// helper function
// Return the socket of the directly connected server with this group ID,
// or -1 if we have no connection to it right now.
int findServer(const std::string &groupId)
{
    if (groupId == "")
    {
        return -1;
    }
    for (auto const &entry : clients)
    {
        if (entry.second->groupId == groupId)
        {
            return entry.first;      // the map key is the socket
        }
    }
    return -1;
}

// check whether we already have an outgoing connection to an IP and port.
bool isConnectedTo(const std::string &ip, int port)
{
    for (auto const &entry : clients)
    {
        Client *client = entry.second;
        if (client->weDialled && client->ip == ip && client->port == port)
        {
            return true;
        }
    }
    return false;
}

// give a connected server the messages we have been holding for its group.
// Keep a message if sending it fails.
void deliverHeld(int sock, const std::string &group)
{
    if (mailboxes.count(group) == 0)
    {
        return;
    }

    while (!mailboxes[group].empty())
    {
        Message message = mailboxes[group].front();
        std::string command = "SENDMSG," + message.sender + "," + message.to + "," +
                              message.timestamp + "," + message.text;

        if (sendReply(sock, command) == false)
        {
            return;
        }
        mailboxes[group].pop_front();
    }
    mailboxes.erase(group);
}

// helper function
// our own IP address on this connection is the address the peer reached us on
std::string localIp(int sock)
{
    struct sockaddr_in me;
    socklen_t len = sizeof(me);
    if (getsockname(sock, (struct sockaddr *)&me, &len) < 0)
    {
        return "-";
    }
    return inet_ntoa(me.sin_addr);
}

// process command from client on the server

void clientCommand(int clientSocket, std::vector<struct pollfd> *fds,
                   std::string command)
{
    // split the command into fields at every comma
    std::stringstream stream(command);
    std::string field;
    std::vector<std::string> tokens;

    while (std::getline(stream, field, ','))
    {
        tokens.push_back(field);
    }

    // a connection that has sent HELO is another server, our client never sends HELO.
    bool fromServer = (clients[clientSocket]->groupId != "");

    if (tokens.size() == 0)
    {
        if (!fromServer)
        {
            sendReply(clientSocket, "ERROR, empty command!");
        }
        else
        {
            log_note("ignored empty command from %s", clients[clientSocket]->groupId.c_str());
        }
    }
    else if (tokens[0] == "STATUSREQ")
    {
        
        // count the messages stored for each group.
        std::string list;
        for (auto const &entry : mailboxes)
        {
            if (list != "")
            {
                list += ",";
            }
            list += entry.first + ":" + std::to_string(entry.second.size());
        }

        if (fromServer)
        {
            if (list == "")
            {
                sendReply(clientSocket, "STATUSRESP,EMPTY");
            }
            else
            {
                sendReply(clientSocket, "STATUSRESP," + list);
            }
        }
        else
        {
            // Keep the Assignment 6 client response unchanged.
            if (list == "")
            {
                sendReply(clientSocket, "EMPTY");
            }
            else
            {
                sendReply(clientSocket, list);
            }
        }
    }
    else if (tokens[0] == "GETMSG" && tokens.size() >= 2)
    {
        // our client collects the oldest message addressed to our group.
        if (fromServer)
        {
            return;
        }
        std::string group = tokens[1];
        if (group != GROUP_ID || mailboxes.count(group) == 0)
        {
            sendReply(clientSocket, "no messages for " + group);
        }
        else
        {
            Message message = mailboxes[group].front();
            if (sendReply(clientSocket, message.sender + ": " + message.text))
            {
                mailboxes[group].pop_front();
                if (mailboxes[group].empty())
                {
                    mailboxes.erase(group);
                }
            }
        }
    }
    else if (tokens[0] == "GETMSGS" && tokens.size() >= 2)
    {
        // a server wants to collect the messages we are holding for it.
        // only allow it to ask for its own group.
        std::string group = tokens[1];
        if (fromServer && clients[clientSocket]->groupId == group)
        {
            deliverHeld(clientSocket, group);
        }
    }
    else if (tokens[0] == "SENDMSG" && tokens.size() >= 4)
    {
        std::string from = tokens[1];
        std::string to = tokens[2];
        std::string text;
        std::string timestamp;

        if (!fromServer)
        {
            // our client does not supply a timestamp. We add it here.
            timestamp = std::to_string(time(NULL));
            text = tokens[3];
            for (size_t i = 4; i < tokens.size(); i++)
            {
                text += "," + tokens[i];
            }
            from = GROUP_ID;
        }
        else
        {
            // messages from another server already have a timestamp.
            if (tokens.size() < 5)
            {
                log_note("ignored incomplete SENDMSG from server");
                return;
            }
            timestamp = tokens[3];
            text = tokens[4];
            for (size_t i = 5; i < tokens.size(); i++)
            {
                text += "," + tokens[i];
            }
        }

        Message message;
        message.sender = from;
        message.to = to;
        message.timestamp = timestamp;
        message.text = text;

        if (to == GROUP_ID)
        {
            // the message is for us, save it until our client asks for it.
            mailboxes[to].push_back(message);
            log_note("stored message for our group from %s", from.c_str());
        }
        else
        {
            int peer = findServer(to);
            if (peer >= 0 && peer != clientSocket)
            {
                // we can pass it directly to the destination.
                std::string outgoing = "SENDMSG," + from + "," + to + "," +
                                       timestamp + "," + text;
                if (sendReply(peer, outgoing))
                {
                    log_note("forwarded message from %s to %s", from.c_str(), to.c_str());
                }
                else
                {
                    mailboxes[to].push_back(message);
                    log_note("could not forward to %s, holding message", to.c_str());
                }
            }
            else
            {
                mailboxes[to].push_back(message);
                log_note("no route to %s right now, holding message", to.c_str());
            }
        }

        if (!fromServer)
        {
            sendReply(clientSocket, "OK, message accepted for " + to);
        }
    }
    else if (tokens[0] == "HELO" && tokens.size() >= 2)
    {
        // nobody else may use our group ID
        if (tokens[1] == GROUP_ID)
        {
            log_note("%s:%d sent HELO with our own group ID", clients[clientSocket]->ip.c_str(), clients[clientSocket]->port);
            return;
        }
        // this connection belongs to another server
        clients[clientSocket]->groupId = tokens[1];

        // tell it about ourselves and the servers whose listening ports we know
        std::string reply = "SERVERS," + std::string(GROUP_ID) + "," +
                            localIp(clientSocket) + "," + std::to_string(listenPort);

        for (auto const &entry : clients)
        {
            Client *c = entry.second;
            if (c->groupId != "" && c->weDialled && c->sock != clientSocket)
            {
                reply += ";" + c->groupId + "," + c->ip + "," + std::to_string(c->port);
            }
        }
        sendReply(clientSocket, reply);

        // the group might have been offline when messages for it arrived
        deliverHeld(clientSocket, tokens[1]);
    }
    else if (tokens[0] == "SERVERS")
    {
        // save the addresses other servers tell us about.
        // each entry has GROUP,IP,PORT and entries are separated by ';'
        if (command.size() < 8 || command.substr(0, 8) != "SERVERS,")
        {
            return;
        }

        std::string list = command.substr(8);
        std::stringstream entries(list);
        std::string entry;

        while (std::getline(entries, entry, ';'))
        {
            std::stringstream fields(entry);
            std::string group;
            std::string ip;
            std::string portText;
            std::getline(fields, group, ',');
            std::getline(fields, ip, ',');
            std::getline(fields, portText, ',');

            if (group == "" || ip == "" || portText == "" || group == GROUP_ID)
            {
                continue;
            }
            if (ip == "127.0.0.1")
            {
                continue; // the other server's localhost is not our localhost
            }

            // check the port before adding the address to our list
            int port = atoi(portText.c_str());
            if (port < 1 || port > 65535)
            {
                continue;
            }
            struct in_addr address;
            if (inet_pton(AF_INET, ip.c_str(), &address) != 1)
            {
                continue;
            }

            if (knownPeers.count(group) == 0)
            {
                KnownPeer peer;
                peer.groupId = group;
                peer.ip = ip;
                peer.port = port;
                knownPeers[group] = peer;
                log_note("learned peer %s at %s:%d", group.c_str(), ip.c_str(), port);
            }
            else
            {
                // Update the address but keep the last connection attempt time
                knownPeers[group].ip = ip;
                knownPeers[group].port = port;
            }
        }
    }
    else if (tokens[0] == "KEEPALIVE" && tokens.size() >= 2)
    {
        // a connected server says it is holding messages for our group
        int waiting = atoi(tokens[1].c_str());
        if (fromServer && waiting > 0)
        {
            sendReply(clientSocket, std::string("GETMSGS,") + GROUP_ID);
        }
    }
    else if (tokens[0] == "CONNECT" && tokens.size() >= 3)
    {
        // our client asks us to connect to another server: CONNECT,<ip>,<port>
        int port = atoi(tokens[2].c_str());
        if (fromServer)
        {
            return;
        }
        if (port < 1 || port > 65535)
        {
            sendReply(clientSocket, "ERROR: invalid port");
            return;
        }
        if (connectToServer(tokens[1], port, fds) >= 0)
        {
            sendReply(clientSocket, "connecting to " + tokens[1] + "," + tokens[2]);
        }
        else
        {
            sendReply(clientSocket, "ERROR: could not connect to " + tokens[1] + "," + tokens[2]);
        }
    }
    else
    {
        if (fromServer)
        {
            // don't answer back ERROR, because it could answer back and therefore start an endless loop
            log_note("ignored unknown command from %s", clients[clientSocket]->groupId.c_str());
        }
        else
        {
            sendReply(clientSocket, "ERROR: unknown or incomplete command");
        }
    }
}

int main(int argc, char *argv[])
{
    bool finished;
    int listenSock; // Socket for connections to server
    int clientSock; // Socket of connecting client

    // list of file descriptors being watched by poll()
    // index 0 is always the listening socket, all other entries are
    // connected clients
    std::vector<struct pollfd> fds;

    struct sockaddr_in client;
    socklen_t clientLen;
    char buffer[1025]; // buffer for reading from clients

    if (argc != 2)
    {
        printf("Usage: ./tsamserver <port>\n");
        exit(0);
    }

    // Setup socket for server to listen to
    listenPort = atoi(argv[1]);
    listenSock = open_socket(listenPort);

    if (listenSock < 0 || listen(listenSock, BACKLOG) < 0)
    {
        printf("Listen failed on port %s\n", argv[1]);
        exit(0);
    }
    else
    // add listen socket to the list of fds being polled
    {
        fds.push_back({listenSock, POLLIN, 0});
        std::string print = "Listening on port: " + std::string(argv[1]);
        std::cout << print << std::endl;
        log_event("START", NULL, 0, GROUP_ID, print.c_str());
    
        // first server to contact
        KnownPeer firstServer;
        firstServer.groupId = ""; // we do not know its group ID yet
        firstServer.ip = "130.208.246.98";
        firstServer.port = 5001;
        knownPeers["seed"] = firstServer;
    }

    finished = false;
    time_t lastKeepalive = 0;
    time_t lastMaintenance = 0;

    while (!finished)
    {
        time_t now = time(NULL);

        // once per minute, tell connected servers how many messages we hold for them
        if (now - lastKeepalive >= 60)
        {
            lastKeepalive = now;
            for (auto const &entry : clients)
            {
                Client *c = entry.second;
                if (c->groupId != "")
                {
                    int waiting = 0;
                    if (mailboxes.count(c->groupId) > 0)
                    {
                        waiting = mailboxes[c->groupId].size();
                    }
                    sendReply(c->sock, "KEEPALIVE," + std::to_string(waiting));
                }
            }
        }

        // every 15 seconds, check if we need another server connection
        if (now - lastMaintenance >= 15)
        {
            lastMaintenance = now;
            int serverCount = 0;
            for (auto const &entry : clients)
            {
                if (entry.second->groupId != "" || entry.second->weDialled)
                {
                    serverCount++;
                }
            }

            if (serverCount < 3)
            {
                std::string target = "";

                // try student servers first
                for (auto const &entry : knownPeers)
                {
                    const KnownPeer &peer = entry.second;
                    if (peer.groupId == GROUP_ID || peer.groupId.substr(0, 2) != "G_")
                    {
                        continue;
                    }
                    if (findServer(peer.groupId) >= 0 || isConnectedTo(peer.ip, peer.port))
                    {
                        continue;
                    }
                    if (now - peer.lastTry < 300)
                    {
                        continue; // do not retry this address for five minutes
                    }
                    target = entry.first;
                    break;
                }

                // if there are no students available, try any other server
                if (target == "")
                {
                    for (auto const &entry : knownPeers)
                    {
                        const KnownPeer &peer = entry.second;
                        if (peer.groupId == GROUP_ID)
                        {
                            continue;
                        }
                        if (findServer(peer.groupId) >= 0 || isConnectedTo(peer.ip, peer.port))
                        {
                            continue;
                        }
                        if (now - peer.lastTry < 300)
                        {
                            continue;
                        }
                        target = entry.first;
                        break;
                    }
                }

                if (target != "")
                {
                    // save the retry time in the map, not in a temporary copy
                    knownPeers[target].lastTry = now;
                    KnownPeer peer = knownPeers[target];
                    log_note("connecting to %s:%d", peer.ip.c_str(), peer.port);
                    connectToServer(peer.ip, peer.port, &fds);
                }
            }
        }

        memset(buffer, 0, sizeof(buffer));

        // wait up to one second for an incoming command
        int n = poll(fds.data(), fds.size(), 1000);

        if (n < 0)
        {
            if (errno != EINTR)
            {
                perror("poll failed - closing down");
                finished = true;
            }
        }
        else
        {
            // take a snapshot of the current fds/revents before processing
            // them below, since fds itself may be modified while we go
            // new clients connecting, existing clients disconnecting.
            std::vector<struct pollfd> readyFds = fds;

            // first, accept any new connections to the server on the listening socket
            if (readyFds.size() > 0 && (readyFds[0].revents & POLLIN))
            {
                clientLen = sizeof(client); // tell accept() how much room the address struct has
                clientSock = accept(listenSock, (struct sockaddr *)&client, &clientLen);

                if (clientSock < 0)
                {
                    perror("Accept failed");
                }
                else
                {

                    // add new client to the list of fds being polled
                    fds.push_back({clientSock, POLLIN, 0});

                    // create a Client for this connection and record the address of the other end,
                    // which we need for the log

                    clients[clientSock] = new Client(clientSock);
                    clients[clientSock]->ip = inet_ntoa(client.sin_addr);
                    clients[clientSock]->port = ntohs(client.sin_port);

                    // Decrement the number of sockets waiting to be dealt with
                    n--;

                    std::string print = "Client connected on server: " + std::to_string(clientSock);
                    log_event("ACCEPT", clients[clientSock]->ip.c_str(), clients[clientSock]->port, NULL, print.c_str());
                    sendReply(clientSock, std::string("HELO,") + GROUP_ID);
                }
            }
            // now check for commands from clients
            std::list<Client *> disconnectedClients;
            for (size_t i = 1; i < readyFds.size(); i++)
            {
                if (readyFds[i].revents & POLLIN)
                {
                    int sock = readyFds[i].fd;
                    Client *client = clients[sock];

                    int receivedMessage = recv(sock, buffer, sizeof(buffer), MSG_DONTWAIT);

                    // recv() == 0 means client has closed connection
                    if (receivedMessage == 0)
                    {
                        disconnectedClients.push_back(client);
                        closeClient(sock, &fds);
                    }

                    // if negative, something failed and we received an error. print
                    // with perror to get specific error
                    else if (receivedMessage < 0)
                    {
                        // EAGAIN only means there was nothing to read right now.
                        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
                        {
                            perror("receive failed");
                            disconnectedClients.push_back(client);
                            closeClient(sock, &fds);
                        }
                    }

                    // if we receive more than 0, we append the received bytes to the client's
                    // buffer. client -> is a pointer to client object
                    else
                    {
                        client->buffer.append(buffer, receivedMessage);
                        std::string command;
                        int result = 1;
                        while (result != 0) 
                        {
                            result = getFrame(client->buffer, command);
                            if (result == 1)
                            {
                                std::string print = "Received from " + std::to_string(sock) + ": " + command;
                                log_event_bytes("RECV", client->ip.c_str(), client->port, client->groupId.c_str(), command.data(), command.size());
                                clientCommand(sock, &fds, command);
                            }
                            else if (result == -1)
                            {
                                std::string print = "Dropped malformed data from " + std::to_string(sock);
                                log_event("DROP", client->ip.c_str(), client->port, client->groupId.c_str(), print.c_str());
                            }
                        }
                    }
                }
            }
            // remove client from the clients list
            for (auto const &c : disconnectedClients)
            {
                clients.erase(c->sock);
                delete c; // destroy the object itself so we don't get a memory leak
            }
        }
    }
}