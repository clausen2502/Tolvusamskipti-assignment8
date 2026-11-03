#include <string>
#include <sys/socket.h>

// public variables
char SOH = 0x01;
char STX = 0x02;
char ETX = 0x03;

// Wrap text in a frame <SOH><length><STX>text<ETX>
std::string makeFrame(const std::string &text)
{
    int total = text.length() + 5;
    std::string frame;

    char highByte = total / 256;
    char lowByte = total % 256;
    frame += SOH;
    frame += highByte;
    frame += lowByte;
    frame += STX;
    frame += text;
    frame += ETX;

    return frame;
}

// Given bytes, cut one complete frame off the front. if there's a complete
// frame, take the text out and remove the frame from the buffer. Return 0 if 
// the we have an incomplete frame, and need more bytes. Return 1 if a complete
// frame was cut out, and a -1 is garbage and we should throw away.
int getFrame(std::string &buffer, std::string &command)
{
    if (buffer.size() < 3) {
        return 0;
        }; // can't read length without bytes 0-2

    if (buffer[0] != SOH) {
        buffer.clear();
        return -1;
    } // incorrect frame

    unsigned char high = buffer[1];
    unsigned char low  = buffer[2];
    size_t length = high * 256 + low;

    if (5 > length || length > 5000) {
        buffer.clear();
        return -1;
    }

    if (length > buffer.size()) {
        return 0;
    } // the rest of the bytes have not arrived, return 0 so we can receive more

    if (buffer[3] != STX) {
        buffer.clear();
        return -1;
    } // incorrect frame

    if (buffer[(length - 1)] != ETX) {
        buffer.clear();
        return -1;
    } // incorrect frame

    command = buffer.substr(4, length - 5);
    buffer.erase(0, length);
    return 1;
}

// Send a message over a socket. send() may not be able to accept the full
// message at once, so keep going until everything has been written.
// Returns false if the connection is gone or send() failed.

bool sendMessage(int sock, const std::string &msg)
{
    const char *p = msg.c_str();
    size_t remaining = msg.length();

    while (remaining > 0)
    {
        int n = send(sock, p, remaining, MSG_DONTWAIT);

        if (n <= 0) // connection closed or error
        {
            return false;
        }

        p += n;
        remaining -= n;
    }

    return true;
}