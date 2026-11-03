# Assignment 8

## How to compile

First run 'make'

Then you can run the tsam server with:
./tsamserver <port>

and run the tsam client with:
./tsamclient <ip> <port>

We tested everything on macOS, and ran it on the tsamserver


## Commands

Server adds a timestamp and our group ID, and sends directly if we are connected, otherwhise we hold
- SENDMSG,<from>,<to>,<text>

Gets the oldest message for our group
- GETMSG,G_everton

The server replies with the groups it is holding messages for and the number of messages for each, or EMPTY if there are no messages.
- STATUSREQ

Our own command, the server will dial the address, log CONN and send HELO.
- CONNECT,<ip>,<port>

## Server-to-server commands

Sent immediately after connecting to another server, and when we accept a connection. When we receive, we save the group ID, reply with SERVERS and deliver any held messages. We reject and log servers using our own group ID.
- HELO,<group ID>

Sent as a reply to HELO, listing our own server first and then the servers we connected to. When we receive, we check the IP addresses and ports and save valid servers for later connections.
- SERVERS,<group>,<ip>,<port>;…

Sent once a minute to connected servers, showing how many messages we are holding for them. If we receive a KEEPALIVE with messages waiting, we send GETMSGS to collect them.
- KEEPALIVE,<n>

Sends all messages we are holding for the requesting group and removes them after sending. Servers can only collect messages for their own group.
- GETMSGS,<group ID>

Stores messages for our group, forwards them if connected to the destination, or holds them if the destination is unreachable.
- SENDMSG,<from>,<to>,<timestamp>,<text>

Replies with the groups we are holding messages for and how many, or STATUSRESP,EMPTY if there are none. We do not reply to STATUSRESP.
- STATUSREQ / STATUSRESP

Unknown commands are logged, not answered with an error


## Main points

### Another server connected to us (1b)

Another group's server must connect to our server, and we must exchange HELO and SERVERS in both directions.

- 27 Oct `2026-10-27T10:49:54Z` ACCEPT from G_swaws at 130.208.246.98:48160
- `2026-10-27T10:49:54Z` SEND HELO,G_everton
- `2026-10-27T10:49:54Z` RECV HELO,G_swaws
- `2026-10-27T10:49:54Z` SEND SERVERS to G_swaws
- `2026-10-27T10:49:54Z` RECV SERVERS from G_swaws

### Messages received from two groups (1c)

Our server must receive messages addressed to our group from at least 2 different groups, and our client must fetch them from our server.

- G_swaws `2026-10-27T15:00:28Z` RECV SENDMSG (salutations) `2026-11-03T15:47:13Z` SEND to our client (G_swaws: salutations)
- G_yourname `2026-10-27T17:29:45Z` RECV SENDMSG `2026-11-03T15:47:16Z` SEND to our client (G_yourname: Hi from …)

Both messages arrived before the deadline, but our client fetched them on 3 Nov, after the deadline.

### Messages sent to two groups (1d)

- G_minions `2026-10-27T10:43:01Z` RECV SENDMSG from our client `2026-10-27T10:43:01Z` SEND to G_minions
- G_yourname `2026-11-03T15:54:29Z` RECV SENDMSG from our client `2026-11-03T15:54:29Z` SEND to G_yourname
- G_leo `2026-11-03T15:55:08Z` RECV SENDMSG from our client `2026-11-03T15:55:08Z` SEND to G_leo

The message to G_minions was sent before the deadline, but the messages to G_yourname and G_leo were sent on 3 nov, after the deadline

## Bonus points

### Ready Bot (2b)

Our server was connected to the botnet and responding on at least 5 days, for at least 2 hours each.
Each window starts at the timestamp, commands from other servers arrive at least once a minute throughout, and our server replies

- 28 Oct  `2026-10-28T10:00:39Z` RECV from G_minions (KEEPALIVE)  `2026-10-28T10:02:18Z` SEND SERVERS to G_unit 
- 29 Oct  `2026-10-29T10:00:44Z` RECV from G_yourname (KEEPALIVE)  `2026-10-29T10:19:44Z` SEND SERVERS to G_minions 
- 30 Oct  `2026-10-30T10:00:48Z` RECV from G_yourname (KEEPALIVE)  `2026-10-30T10:46:40Z` SEND SERVERS to G_leo 
- 31 Oct  `2026-10-31T10:00:01Z` RECV from G_nafn (KEEPALIVE)  `2026-10-31T10:06:04Z` SEND SERVERS to G_nafn 
- 1 Nov   `2026-11-01T10:00:11Z` RECV from G_idiosyncrasy (KEEPALIVE)  `2026-11-01T10:43:15Z` SEND SERVERS to G_unit 

### Edge Bot (2e)

The SERVERS replies in our log show at least 3 student servers connected at each timestamp.

- 27 Oct `2026-10-27T13:02:34Z` SEND SERVERS to G_SHE (G_SHE, G_minions, G_swaws)
- 28 Oct `2026-10-28T09:31:23Z` SEND SERVERS to G_blueberries (G_blueberries, G_minions, G_yourname)
- 28 Oct `2026-10-28T10:32:39Z` SEND SERVERS to G_unit (G_minions, G_unit, G_yourname)
- 28 Oct `2026-10-28T11:37:08Z` SEND SERVERS to G_ellur (G_ellur, G_minions, G_yourname)
- 28 Oct `2026-10-28T12:49:09Z` SEND SERVERS to G_ellur (G_ellur, G_minions, G_yourname)

### Courier (2g)

The dispatch arrived before the deadline, but we completed the task and sent the reply on 3 Nov, which is after the deadline
We needed to name at least 2 servers we were connected to.

- 31 Oct `2026-10-31T21:50:11Z` RECV DISPATCH 0 through G_kiwi (token 8f3b106534f13b87)
- 3 Nov `2026-11-03T15:52:19Z` CONN to I_dispatch at 130.208.246.98:5010
- 3 Nov `2026-11-03T15:52:45Z` SEND REPLY 8f3b106534f13b87 G_leo G_unit G_yourname
- 3 Nov `2026-11-03T15:52:45Z` RECV from I_dispatch: Credited: dispatch 0 (Roll call), with the fragment
- 3 Nov `2026-11-03T15:53:39Z` Our client fetched the credited reply and fragment "Löng ekkasog haustfiðlanna"

## Implementation

We started early by taking our assignment6 tsamserver and client, and made some additions to be able to run the server on the tsamserver, so we could gather logs for some bonus points.  The log however was produced by the first version of the tsamserver, which just included framing, dropping malformed data, logging all events, sending HELO on every connection,, connect from the client to dial another server, SENDMSG, and store messages. Then later we finished the code and it includes everything in our submission.
We were very busy and had much to do and unfortunately could not proceed to finish on time but at least we managed to get the server up to receive some inital logs for some bonus points. Therefore in the logs you will for example see a lot of "STATUSREQ not implemented yet". 
We used an AI assistant to explain important concepts, to simply break them down and explain them more in depth so we could understand better what was going on.
