// Winsock for the WebAssembly build: UDP on WebTransport datagrams, so the web build joins servers through their
// lobby, which relays the datagrams to the game's UDP port (zombies-hub's server/relay.go). The engine's own net code
// (src/win32/win_net.cpp) runs as on Windows: one non-blocking UDP socket, sendto and recvfrom.
//
// A browser has no UDP, but a WebTransport session to host:port carries datagrams both ways. sendto(host:port) opens one
// on first use and sends the packet as one or more datagrams (framing: qcommon/net_webtransport.h); recvfrom returns
// what the sessions receive, from their host:port. The sessions live on a worker of their own (library_bo1.js,
// bo1_net_start), so packets go out and come in while the page's thread renders; it and the engine share two rings
// of datagrams in wasm memory (Bo1NetShared). TCP is not there: socket() for it fails, as before.
//
// Server names: gethostbyname gives a name an address of its own in 240.0.0.0/4, which no IPv4 host has, and sendto
// to it opens the session by the name (needed for a server with a real certificate rather than a hash).
#include <windows.h>
#include <winsock2.h>

#include <limits.h>
#include <mutex>
#include <string>

#include <emscripten.h>
#include <emscripten/threading.h>
#include <unistd.h>

#include <qcommon/net_webtransport.h>

#include "web_platform.h"

#define BO1_NET_SLOTS 256 // per direction
#define BO1_NET_HOST 64
#define BO1_NET_CLOSE_ALL (-1) // a Bo1NetSlot.length: closesocket, end every session

struct Bo1NetSlot
{
    uint32_t ip; // in_addr.s_addr, as the engine gave it
    int32_t port; // host byte order
    int32_t length; // of data, or BO1_NET_CLOSE_ALL
    char host[BO1_NET_HOST]; // engine -> page: what to connect to (a dotted address or a server name)
    uint8_t data[WT_MAX_DATAGRAM];
};

// single producer, single consumer: head is written by the producer, tail by the consumer
struct Bo1NetRing
{
    volatile uint32_t head;
    volatile uint32_t tail;
    volatile uint32_t dropped;
    uint32_t pad;
    Bo1NetSlot slots[BO1_NET_SLOTS];
};

struct Bo1NetShared
{
    Bo1NetRing toPage; // datagrams to send; the worker waits on head (Atomics.waitAsync)
    Bo1NetRing fromPage; // datagrams received
};

extern "C" {
// library_bo1.js, run on the page's thread: starts the network worker on these rings
void bo1_net_start(Bo1NetShared *shared, int fromPageOffset, int slotsOffset, int slotSize, int hostOffset, int hostSize, int dataOffset,
    int slots, int maxData);
}

static Bo1NetShared s_rings;
static bool s_netStarted;
static std::mutex s_netLock;

// the peers sendto or recvfrom has seen: their datagram reassembly and fragment numbering
struct Bo1NetPeer
{
    uint32_t ip;
    int port;
    unsigned int lastUse;
    unsigned short nextFragmentId;
    wtReassembly_t reassembly;
};
static Bo1NetPeer s_peers[16];
static unsigned int s_peerClock;

// server names gethostbyname has handed out an address for: 240.0.0.0 + index + 1 (network order)
static std::string s_names[256];
static int s_nameCount;

static thread_local int t_wsaError;

#define BO1_UDP_SOCKET ((SOCKET)0x5001)
static bool s_udpOpen;

static Bo1NetPeer *Bo1_NetPeer(uint32_t ip, int port)
{
    Bo1NetPeer *oldest = &s_peers[0];
    for (Bo1NetPeer &p : s_peers)
    {
        if (p.lastUse && p.ip == ip && p.port == port)
        {
            p.lastUse = ++s_peerClock;
            return &p;
        }
        if (p.lastUse < oldest->lastUse)
            oldest = &p;
    }
    WT_ClearReassembly(&oldest->reassembly);
    oldest->ip = ip;
    oldest->port = port;
    oldest->nextFragmentId = 0;
    oldest->lastUse = ++s_peerClock;
    return oldest;
}

static void Bo1_NetStart()
{
    if (s_netStarted)
        return;
    s_netStarted = true;
    bo1_net_start(&s_rings, (int)offsetof(Bo1NetShared, fromPage), (int)offsetof(Bo1NetRing, slots), (int)sizeof(Bo1NetSlot),
        (int)offsetof(Bo1NetSlot, host), BO1_NET_HOST, (int)offsetof(Bo1NetSlot, data), BO1_NET_SLOTS, WT_MAX_DATAGRAM);
}

// the slot the next datagram to the page goes in, or NULL when the ring is full (the datagram is lost, as UDP's)
static Bo1NetSlot *Bo1_NetSendSlot()
{
    Bo1NetRing *ring = &s_rings.toPage;
    uint32_t head = __atomic_load_n(&ring->head, __ATOMIC_ACQUIRE);
    if (head - __atomic_load_n(&ring->tail, __ATOMIC_ACQUIRE) >= BO1_NET_SLOTS)
    {
        ++ring->dropped;
        return NULL;
    }
    return &ring->slots[head % BO1_NET_SLOTS];
}

static void Bo1_NetSendCommit()
{
    Bo1NetRing *ring = &s_rings.toPage;
    __atomic_fetch_add(&ring->head, 1, __ATOMIC_RELEASE);
    emscripten_futex_wake((void *)&ring->head, INT_MAX);
}

struct Bo1NetSendTo
{
    uint32_t ip;
    int port;
    const char *host;
};

static void Bo1_NetEmitDatagram(void *context, const unsigned __int8 *datagram, int length)
{
    const Bo1NetSendTo *to = (const Bo1NetSendTo *)context;
    Bo1NetSlot *slot = Bo1_NetSendSlot();
    if (!slot)
        return;
    slot->ip = to->ip;
    slot->port = to->port;
    slot->length = length;
    I_strncpyz_web(slot->host, to->host, sizeof(slot->host));
    memcpy(slot->data, datagram, length);
    Bo1_NetSendCommit();
}

// ============================================================================================================
// winsock
// ============================================================================================================

extern "C" int WINAPI WSAStartup(WORD wVersionRequested, LPWSADATA d)
{
    memset(d, 0, sizeof(*d));
    d->wVersion = wVersionRequested;
    d->wHighVersion = wVersionRequested;
    strcpy(d->szDescription, "WebAssembly (UDP over WebTransport)");
    d->iMaxSockets = 1;
    d->iMaxUdpDg = WT_MAX_PACKET;
    return 0;
}
extern "C" int WINAPI WSACleanup(void) { return 0; }
extern "C" int WINAPI WSAGetLastError(void) { return t_wsaError; }
extern "C" void WINAPI WSASetLastError(int e) { t_wsaError = e; }

extern "C" SOCKET WINAPI socket(int af, int type, int)
{
    std::lock_guard<std::mutex> guard(s_netLock);
    if (af != AF_INET || type != SOCK_DGRAM)
    {
        t_wsaError = af != AF_INET ? WSAEAFNOSUPPORT : WSAESOCKTNOSUPPORT;
        return INVALID_SOCKET;
    }
    if (s_udpOpen)
    {
        t_wsaError = WSAEMFILE; // one: the engine's ip_socket
        return INVALID_SOCKET;
    }
    Bo1_NetStart();
    s_udpOpen = true;
    return BO1_UDP_SOCKET;
}

extern "C" int WINAPI closesocket(SOCKET s)
{
    std::lock_guard<std::mutex> guard(s_netLock);
    if (s != BO1_UDP_SOCKET || !s_udpOpen)
    {
        t_wsaError = WSAENOTSOCK;
        return SOCKET_ERROR;
    }
    s_udpOpen = false;
    Bo1NetSlot *slot = Bo1_NetSendSlot();
    if (slot)
    {
        slot->length = BO1_NET_CLOSE_ALL;
        Bo1_NetSendCommit();
    }
    for (Bo1NetPeer &p : s_peers)
    {
        WT_ClearReassembly(&p.reassembly);
        p.lastUse = 0;
    }
    return 0;
}

static bool Bo1_IsUdpSocket(SOCKET s)
{
    if (s == BO1_UDP_SOCKET && s_udpOpen)
        return true;
    t_wsaError = WSAENOTSOCK;
    return false;
}

// the address it is bound to does not matter: datagrams come from the sessions
extern "C" int WINAPI bind(SOCKET s, const struct sockaddr *, int) { return Bo1_IsUdpSocket(s) ? 0 : SOCKET_ERROR; }
extern "C" int WINAPI ioctlsocket(SOCKET s, long cmd, ULONG *argp)
{
    if (!Bo1_IsUdpSocket(s))
        return SOCKET_ERROR;
    if (cmd == FIONREAD)
        *argp = 0;
    return 0; // FIONBIO: it never blocks
}
extern "C" int WINAPI setsockopt(SOCKET s, int, int, const char *, int) { return Bo1_IsUdpSocket(s) ? 0 : SOCKET_ERROR; }
extern "C" int WINAPI getsockopt(SOCKET s, int, int, char *optval, int *optlen)
{
    if (!Bo1_IsUdpSocket(s))
        return SOCKET_ERROR;
    if (optval && optlen && *optlen >= 4)
    {
        memset(optval, 0, 4);
        *optlen = 4;
    }
    return 0;
}

extern "C" int WINAPI sendto(SOCKET s, const char *buf, int len, int, const struct sockaddr *to, int tolen)
{
    std::lock_guard<std::mutex> guard(s_netLock);
    if (!Bo1_IsUdpSocket(s))
        return SOCKET_ERROR;
    const sockaddr_in *sin = (const sockaddr_in *)to;
    if (!to || tolen < (int)sizeof(sockaddr_in) || sin->sin_family != AF_INET)
    {
        t_wsaError = WSAEAFNOSUPPORT;
        return SOCKET_ERROR;
    }
    uint32_t ip = sin->sin_addr.s_addr;
    if (ip == INADDR_BROADCAST)
        return len; // LAN broadcasts reach nobody from a browser
    if (ip == INADDR_ANY || !sin->sin_port)
    {
        t_wsaError = WSAEADDRNOTAVAIL;
        return SOCKET_ERROR;
    }
    if (len > WT_MAX_PACKET)
    {
        t_wsaError = WSAEMSGSIZE;
        return SOCKET_ERROR;
    }

    const unsigned char *b = (const unsigned char *)&ip;
    char dotted[16];
    const char *host = dotted;
    if (b[0] == 240 && !b[1] && !b[2] && b[3] && b[3] <= s_nameCount)
        host = s_names[b[3] - 1].c_str();
    else
        snprintf(dotted, sizeof(dotted), "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);

    Bo1NetSendTo dest = { ip, ntohs(sin->sin_port), host };
    Bo1NetPeer *peer = Bo1_NetPeer(ip, dest.port);
    WT_FramePacket((const unsigned __int8 *)buf, len, &peer->nextFragmentId, Bo1_NetEmitDatagram, &dest);
    return len;
}

extern "C" int WINAPI recvfrom(SOCKET s, char *buf, int len, int, struct sockaddr *from, int *fromlen)
{
    std::lock_guard<std::mutex> guard(s_netLock);
    if (!Bo1_IsUdpSocket(s))
        return SOCKET_ERROR;
    Bo1NetRing *ring = &s_rings.fromPage;
    for (;;)
    {
        uint32_t tail = __atomic_load_n(&ring->tail, __ATOMIC_ACQUIRE);
        if (tail == __atomic_load_n(&ring->head, __ATOMIC_ACQUIRE))
        {
            t_wsaError = WSAEWOULDBLOCK;
            return SOCKET_ERROR;
        }
        const Bo1NetSlot *slot = &ring->slots[tail % BO1_NET_SLOTS];
        uint32_t ip = slot->ip;
        int port = slot->port;
        int length = slot->length;
        Bo1NetPeer *peer = Bo1_NetPeer(ip, port);
        int packetLength = 0;
        const unsigned __int8 *packet = length >= 0 && length <= WT_MAX_DATAGRAM
            ? WT_ReceiveDatagram(&peer->reassembly, slot->data, length, &packetLength) : NULL;
        int copied = packet ? (packetLength < len ? packetLength : len) : 0;
        if (packet)
            memcpy(buf, packet, copied);
        __atomic_store_n(&ring->tail, tail + 1, __ATOMIC_RELEASE);
        if (!packet)
            continue; // a piece of a packet still missing others
        if (from && fromlen && *fromlen >= (int)sizeof(sockaddr_in))
        {
            sockaddr_in *sin = (sockaddr_in *)from;
            memset(sin, 0, sizeof(*sin));
            sin->sin_family = AF_INET;
            sin->sin_port = htons((USHORT)port);
            sin->sin_addr.s_addr = ip;
            *fromlen = sizeof(sockaddr_in);
        }
        if (packetLength > len)
        {
            t_wsaError = WSAEMSGSIZE;
            return SOCKET_ERROR;
        }
        return copied;
    }
}

extern "C" int WINAPI getsockname(SOCKET s, struct sockaddr *name, int *namelen)
{
    if (!Bo1_IsUdpSocket(s))
        return SOCKET_ERROR;
    if (name && namelen && *namelen >= (int)sizeof(sockaddr_in))
    {
        memset(name, 0, sizeof(sockaddr_in));
        ((sockaddr_in *)name)->sin_family = AF_INET;
        *namelen = sizeof(sockaddr_in);
    }
    return 0;
}

// no TCP
extern "C" int WINAPI connect(SOCKET, const struct sockaddr *, int)
{
    t_wsaError = WSAENOTSOCK;
    return SOCKET_ERROR;
}
extern "C" int WINAPI listen(SOCKET, int)
{
    t_wsaError = WSAENOTSOCK;
    return SOCKET_ERROR;
}
extern "C" SOCKET WINAPI accept(SOCKET, struct sockaddr *, int *)
{
    t_wsaError = WSAENOTSOCK;
    return INVALID_SOCKET;
}
extern "C" int WINAPI shutdown(SOCKET, int) { return 0; }
extern "C" int WINAPI send(SOCKET, const char *, int, int)
{
    t_wsaError = WSAENOTCONN;
    return SOCKET_ERROR;
}
extern "C" int WINAPI recv(SOCKET, char *, int, int)
{
    t_wsaError = WSAENOTCONN;
    return SOCKET_ERROR;
}
extern "C" int WINAPI getpeername(SOCKET, struct sockaddr *, int *)
{
    t_wsaError = WSAENOTCONN;
    return SOCKET_ERROR;
}
extern "C" int WINAPI select(int, fd_set *r, fd_set *w, fd_set *e, const struct timeval *)
{
    if (r)
        r->fd_count = 0;
    if (w)
        w->fd_count = 0;
    if (e)
        e->fd_count = 0;
    return 0;
}

// "localhost" and server names; this machine's own name has no address here (NET_GetLocalAddress asks for it)
extern "C" struct hostent *WINAPI gethostbyname(const char *name)
{
    static thread_local struct hostent h;
    static thread_local char *aliases[1];
    static thread_local char *addrs[2];
    static thread_local ULONG addr;
    static thread_local char hostName[256];

    char self[256] = "";
    gethostname(self, sizeof(self));
    if (!name || !*name || !strcmp(name, self) || strlen(name) >= sizeof(hostName))
    {
        t_wsaError = WSAHOST_NOT_FOUND;
        return NULL;
    }
    if (!_stricmp(name, "localhost"))
    {
        addr = htonl(INADDR_LOOPBACK);
    }
    else
    {
        std::lock_guard<std::mutex> guard(s_netLock);
        int i = 0;
        while (i < s_nameCount && _stricmp(s_names[i].c_str(), name))
            ++i;
        if (i == s_nameCount)
        {
            if (s_nameCount == 255)
            {
                t_wsaError = WSAHOST_NOT_FOUND;
                return NULL;
            }
            s_names[s_nameCount++] = name;
        }
        addr = htonl(0xF0000000u | (unsigned)(i + 1));
    }
    I_strncpyz_web(hostName, name, sizeof(hostName));
    aliases[0] = NULL;
    addrs[0] = (char *)&addr;
    addrs[1] = NULL;
    h.h_name = hostName;
    h.h_aliases = aliases;
    h.h_addrtype = AF_INET;
    h.h_length = 4;
    h.h_addr_list = addrs;
    return &h;
}

extern "C" struct hostent *WINAPI gethostbyaddr(const char *, int, int)
{
    t_wsaError = WSANO_DATA;
    return NULL;
}

extern "C" ULONG WINAPI inet_addr(const char *cp)
{
    unsigned a, b, c, d;
    if (sscanf(cp, "%u.%u.%u.%u", &a, &b, &c, &d) != 4 || a > 255 || b > 255 || c > 255 || d > 255)
        return INADDR_NONE;
    return (ULONG)(a | (b << 8) | (c << 16) | (d << 24));
}

extern "C" char *WINAPI inet_ntoa(struct in_addr in)
{
    static thread_local char buf[16];
    ULONG a = in.S_un.S_addr;
    snprintf(buf, sizeof(buf), "%u.%u.%u.%u", (unsigned)(a & 0xFF), (unsigned)((a >> 8) & 0xFF), (unsigned)((a >> 16) & 0xFF),
        (unsigned)(a >> 24));
    return buf;
}

extern "C" USHORT WINAPI htons(USHORT v) { return __builtin_bswap16(v); }
extern "C" USHORT WINAPI ntohs(USHORT v) { return __builtin_bswap16(v); }
extern "C" ULONG WINAPI htonl(ULONG v) { return __builtin_bswap32((uint32_t)v); }
extern "C" ULONG WINAPI ntohl(ULONG v) { return __builtin_bswap32((uint32_t)v); }

extern "C" void bo1_FD_CLR(SOCKET fd, fd_set *set)
{
    for (UINT i = 0; i < set->fd_count; ++i)
    {
        if (set->fd_array[i] == fd)
        {
            set->fd_array[i] = set->fd_array[--set->fd_count];
            return;
        }
    }
}

extern "C" void bo1_FD_SET(SOCKET fd, fd_set *set)
{
    for (UINT i = 0; i < set->fd_count; ++i)
    {
        if (set->fd_array[i] == fd)
            return;
    }
    if (set->fd_count < FD_SETSIZE)
        set->fd_array[set->fd_count++] = fd;
}

extern "C" int WINAPI __WSAFDIsSet(SOCKET fd, fd_set *set)
{
    for (UINT i = 0; i < set->fd_count; ++i)
    {
        if (set->fd_array[i] == fd)
            return 1;
    }
    return 0;
}
