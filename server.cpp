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
#include <fstream>

#include <iostream>
#include <sstream>
#include <thread>
#include <map>

#include <unistd.h>

#ifndef SOCK_NONBLOCK
#include <fcntl.h>
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
    std::string name;    // Limit length of name of client's user
    std::string buffer;  // bytes received so far from this client
    std:: string ip;     // IP address of the other end
    int port;            // port of the other end
    std::string groupId; // their group ID

    Client(int socket) : sock(socket), port(0) {}

    ~Client() {} // Virtual destructor defined for base class
};

std::map<int, Client *> clients; // Lookup table for per Client information
int listenPort = 0; // the port we accept connections on

class Message
{
public:
    std::string sender; // holds the name of the sender of the text message
    std::string to;        // the group the message is addressed TO
    std::string timestamp; // original creation time, unix seconds as a string
    std::string text;   // holds the text from the text message
};

std::map<std::string, std::deque<Message>> mailboxes; // create a mailboxes variable for messages for every user

// a server we have heard about in a SERVERS response but may or may not
// be connected to. These are our candidates to dial when we need more peers.
struct KnownPeer
{
    std::string groupId;
    std::string ip;
    int port;
    time_t lastTry; // when we last heard from this peer
};
std::map<std::string, KnownPeer> knownPeers; // keyed by groupId, so no duplicates

// open socket for specified port.
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
#ifdef _APPLE_
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
        return (-1);
    }
    else
    {
        return (sock);
    }
}

// close a connection: log it as close, close the socket, and remove it from
// the list of file descriptors being watched by poll().
//
// this does not remove the Client from the "clients" map - main() does
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

// helper function, wraps a reply in a frame, send all of it on sock, and log it as send.
void sendReply(int sock, const std::string &reply)
{
    Client *client = clients[sock];   // socket -> its Client, for the log fields

    std::string framedReply = makeFrame(reply);
    bool sent = sendMessage(sock, framedReply);

    if (sent)
    {
        log_event_bytes("SEND", client->ip.c_str(), client->port, client->groupId.c_str(),
                        reply.data(), reply.size());
    }
    else
    {
        log_note("send to %s:%d failed", client->ip.c_str(), client->port);
    }
}

// helepr function
// Open an outgoing connection to another server, register it like an
// accepted one, log it as CONN and send our HELO.
// Returns the new socket, or -1 if the connection failed.
int connectToServer(const std::string &ip, int port, std::vector<struct pollfd> *fds)
{
    // create a socket
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0)
    {
        log_note("socket() failed when connecting to %s:%d", ip.c_str(), port);
        return -1;
    }

    // fill in the address to dial
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port); // normal int, network byte order
    if (inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) != 1)
    {
        log_note("not a valid IP address: %s", ip.c_str());
        close(sock);
        return -1;
    }

    // connect
    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        log_note("could not connect to %s:%d", ip.c_str(), port);
        close(sock);
        return -1;
    }

    // watch with poll() and remember who is at the other end
    fds->push_back({sock, POLLIN, 0});
    clients[sock] = new Client(sock);
    clients[sock]->ip = ip;
    clients[sock]->port = port; // the port we dialled is their listening port

    // log the outgoing connection
    log_event("CONN", ip.c_str(), port, NULL, "outgoing connection");

    // HELO must be the first thing we send
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

// helper function
// Our own IP address on this connection is the address the peer reached us on
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

// Process command from client on the server

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

    // A connection that has sent HELO is another server, our client never sends HELO.
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
	// Report the groups we are holding messages for, as
	// name: count pairs. mailboxes only has an entry when it
	// holds at least one message, which is what the spec wants.
	std::string list;
	for (auto const &entry : mailboxes)
	{
		if (list !="")
		{
			list += ",";
		}
		list += entry.first;
		list += ":";
		list += std::to_string(entry.second.size());
	}

	std::string reply;
	if (list == "")
	{
		reply ="STATUSRESP,EMPTY";
	}
	else
	{
		reply ="STATUSRESP," + list;
	}
	sendReply(clientSocket, reply);
}

    else if (tokens[0] == "GETMSGS" && tokens.size() >= 2)
    {
        // Send every message we are holding for this group, each as a
        // proper SENDMSG command (one frame each), then drop them.
        std::string group = tokens[1];
        if (mailboxes.count(group) == 0)
        {
            // Nothing held. The spec's GETMSGS has no "empty" reply, so
            // we say nothing rather than invent one that could confuse a peer.
            log_note("GETMSGS for %s: nothing held", group.c_str());
        }
        else
        {
            while (!mailboxes[group].empty())
            {
                Message m = mailboxes[group].front();
                mailboxes[group].pop_front();

                std::string msg = "SENDMSG," + m.sender + "," + m.to + "," +
                                  m.timestamp + "," + m.text;
                sendReply(clientSocket, msg);
            }
            mailboxes.erase(group); // deque is now empty
        }
    }
    else if (tokens[0] == "SENDMSG" && tokens.size() >= 4)
    {
        std::string from = tokens[1];
        std::string to = tokens[2];

        if (!fromServer)
        {
            // From our own client: SENDMSG,<from>,<to>,<message>
            // we are the origin, so we add the timestamp and use our own group id.
            std::string text = tokens[3];
            for (size_t i = 4; i < tokens.size(); i++)
            {
                text += "," + tokens[i];
            }

            std::string msg = "SENDMSG," + std::string(GROUP_ID) + "," + to + "," +
                            std::to_string(time(NULL)) + "," + text;

            int peer = findServer(to);
            if (peer >= 0)
            {
                sendReply(peer, msg); // logged as SEND to them
                sendReply(clientSocket, "OK, sent to " + to);
            }
            else
            {
                Message held;
                held.sender = GROUP_ID;
                held.to = to;
                held.timestamp = std::to_string(time(NULL));
                held.text = text;
                mailboxes[to].push_back(held);
                log_note("no route to %s right now, holding message", to.c_str());
                sendReply(clientSocket, "OK, no route to " + to + ", holding it");
            }
        }
        else
        {
            // From another server: SENDMSG,<from>,<to>,<timestamp>,<message>
            // store it, the text is everything after the timestamp.
            std::string text = "";
            if (tokens.size() >= 5)
            {
                text = tokens[4];
            }
            for (size_t i = 5; i < tokens.size(); i++)
            {
                text += "," + tokens[i];
            }
            Message newMessage;
            newMessage.sender = from;
            newMessage.to = to;
            newMessage.timestamp = tokens[3];
            newMessage.text = text;
            mailboxes[to].push_back(newMessage);
        }
    }
    else if (tokens[0] == "HELO" && tokens.size() >= 2)
    {
        // remember who is at the other end of this connection
        clients[clientSocket]->groupId = tokens[1];

        // reply with ourselves first so others learns where we listen,
        // then every connection that has introduced itself with HELO
         std::string reply = "SERVERS," + std::string(GROUP_ID) + "," + localIp(clientSocket) + "," + std::to_string(listenPort) + ";";
        for (auto const &entry : clients)
        {
            Client *c = entry.second;
            if (c->groupId != "")
            {
                reply += c->groupId + "," + c->ip + "," + std::to_string(c->port) + ";";
            }
        }
        sendReply(clientSocket, reply);
    }
    else if (tokens[0] == "SERVERS")
    {
	    // Learn about servers from this SERVERS response and remember them
        // as candidates to dial later. Re-parse the raw command rather than
        // the comma-split tokens, because the ";" entry separators are not
        // split there. Strip the "SERVERS," prefix, then split on ";" into
        // entries, and each entry on "," into group,ip,port.
        std::string body = command;
        if (body.rfind("SERVERS,", 0) == 0)
        {
            body = body.substr(8); // drop the leading "SERVERS,"
        }

        std::stringstream entries(body);
        std::string entry;
        while (std::getline(entries, entry, ';'))
        {
            if (entry == "")
            {
                continue; // trailing ";" gives an empty last entry
            }

            std::stringstream fields(entry);
            std::string g, ip, portStr;
            std::getline(fields, g, ',');
            std::getline(fields, ip, ',');
            std::getline(fields, portStr, ',');

            if (g == "" || ip == "" || portStr == "")
            {
                continue; // malformed entry, skip it (be tolerant)
            }

            KnownPeer kp;
            kp.groupId = g;
            kp.ip = ip;
            kp.port = atoi(portStr.c_str());
            knownPeers[g] = kp; // keyed by groupId, overwrites duplicates

            log_note("learned peer %s at %s:%d", g.c_str(), ip.c_str(), kp.port);
        }
    }

    else if (tokens[0] == "KEEPALIVE" && tokens.size() >= 2)
    {
        // A peer tells us how many messages it is holding for us.
        // We just note it for now; fetching them (GETMSGS) comes later.
        int waiting = atoi(tokens[1].c_str());
        if (waiting > 0)
        {
            log_note("KEEPALIVE from %s: holding %d messages for us",
                     clients[clientSocket]->groupId.c_str(), waiting);
        }
    }    else if (tokens[0] == "CONNECT" && tokens.size() >= 3)
    {
        // our client asks us to connect to another server: CONNECT,<ip>,<port>
        int port = atoi(tokens[2].c_str());
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

    // List of file descriptors being watched by poll(). By convention,
    // index 0 is always the listening socket; all other entries are
    // connected clients.
    std::vector<struct pollfd> fds;

    struct sockaddr_in client;
    socklen_t clientLen;
    char buffer[1025]; // buffer for reading from clients

    if (argc != 2)
    {
        printf("Usage: ./tsamserver <ip port>\n");
        exit(0);
    }

    // Setup socket for server to listen to
    listenPort = atoi(argv[1]);
    listenSock = open_socket(listenPort);

    if (listen(listenSock, BACKLOG) < 0)
    {
        printf("Listen failed on port %s\n", argv[1]);
        exit(0);
    }
    else
    // Add listen socket to the list of fds being polled.
    {
        fds.push_back({listenSock, POLLIN, 0});
        std::string print = "Listening on port: " + std::string(argv[1]);
        std::cout << print << std::endl;
        log_event("START", NULL, 0, GROUP_ID, print.c_str());
    
        // Seed the known-peers list with the one address we are allowed to
        // hard-code, so the maintenance tick has something to dial and we
        // join the botnet without a manual CONNECT.
        KnownPeer seed;
        seed.groupId = "I_instr1";
        seed.ip = "130.208.246.98";
        seed.port = 5001;
        knownPeers["I_instr1"] = seed;
}

    finished = false;
    time_t lastKeepalive = 0;	// when we last send KEEPALIVE to our peers
    while (!finished)
    {
                // --- periodic work: runs every loop iteration ---
        // Once a minute, tell each connected server how many messages
        // we are holding for it (0 is the normal idle heartbeat).
        time_t now = time(NULL);
        if (now - lastKeepalive >= 60)
        {
            lastKeepalive = now;
            for (auto const &entry : clients)
            {
                Client *c = entry.second;
                if (c->groupId != "")   // a real server, not our own client
                {
                    int held = 0;
                    if (mailboxes.count(c->groupId) > 0)
                    {
                        held = mailboxes[c->groupId].size();
                    }
                    sendReply(c->sock, "KEEPALIVE," + std::to_string(held));
                }
            }
        }
	         // --- peer maintenance: keep ourselves connected to 3-8 servers ---
        // Count how many real servers we are connected to right now.
        int serverCount = 0;
        for (auto const &entry : clients)
        {
            if (entry.second->groupId != "")
            {
                serverCount++;
            }
        }

        // If we are below 3, dial one un-connected known peer this tick.
        // Prefer student G_ peers over instructor I_ ones, so do two
        // passes, first G_ only, then anyone. One dial per tick keeps a slow
        // or dead peer from stalling the whole server more than once.
        if (serverCount < 3)
        {
            std::string target = "";

            // pass 1, a G_ peer we are not already connected to
            for (auto const &entry : knownPeers)
            {
                const KnownPeer &kp = entry.second;
                if (kp.groupId == GROUP_ID) continue;        // not ourselves
                if (findServer(kp.groupId) >= 0) continue;   // already connected
                if (kp.groupId.rfind("G_", 0) == 0)
                {
                    target = kp.groupId;
                    break;
                }
            }

            // pass 2: if no G_ found, accept any un-connected peer
            if (target == "")
            {
                for (auto const &entry : knownPeers)
                {
                    const KnownPeer &kp = entry.second;
                    if (kp.groupId == GROUP_ID) continue;
                    if (findServer(kp.groupId) >= 0) continue;
                    target = kp.groupId;
                    break;
                }
            }

            if (target != "")
            {
                KnownPeer kp = knownPeers[target];
                log_note("maintenance: %d peers, dialling %s at %s:%d",
                         serverCount, kp.groupId.c_str(), kp.ip.c_str(), kp.port);
                connectToServer(kp.ip, kp.port, &fds);
            }
        }
	memset(buffer, 0, sizeof(buffer));

        // wait until at least one of our sockets has
        // something to be read() on it.
        int n = poll(fds.data(), fds.size(), 1000);

        if (n < 0)
        {
            perror("poll failed - closing down\n");
            finished = true;
        }
        else
        {
            // take a snapshot of the current fds/revents before processing
            // them below, since fds itself may be modified while we go
            // (new clients connecting, existing clients disconnecting).
            std::vector<struct pollfd> readyFds = fds;

            // accept any new connections to the server on the listening socket
            if (readyFds[0].revents & POLLIN)
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

                    // decrement the number of sockets waiting to be dealt with
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
                        perror("receive failed");
                        disconnectedClients.push_back(client);
                        closeClient(sock, &fds);
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
