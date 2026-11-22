// Winsock for the WebAssembly build: the types and calls the engine names. The browser has no UDP: a UDP socket's
// datagrams go over WebTransport to servers that run natively (web/src/web_net.cpp); TCP calls fail.
#pragma once
#include "windows.h"
#include <unistd.h> // gethostname

typedef UINT_PTR SOCKET;
typedef unsigned long u_long;
typedef unsigned short u_short;
typedef unsigned int u_int;
typedef unsigned char u_char;
#define INVALID_SOCKET (SOCKET)(~0)
#define SOCKET_ERROR (-1)
#define FD_SETSIZE 64
#define AF_UNSPEC 0
#define AF_INET 2
#define AF_IPX 6
#define AF_INET6 23
#define PF_INET AF_INET
#define SOCK_STREAM 1
#define SOCK_DGRAM 2
#define IPPROTO_IP 0
#define IPPROTO_TCP 6
#define IPPROTO_UDP 17
#define INADDR_ANY (ULONG)0x00000000
#define INADDR_LOOPBACK 0x7f000001
#define INADDR_BROADCAST (ULONG)0xffffffff
#define INADDR_NONE 0xffffffff
#define SOL_SOCKET 0xffff
#define SO_BROADCAST 0x0020
#define SO_REUSEADDR 0x0004
#define SO_RCVBUF 0x1002
#define SO_SNDBUF 0x1001
#define SO_ERROR 0x1007
#define TCP_NODELAY 0x0001
#define IP_MULTICAST_TTL 10
#define FIONBIO 0x8004667e
#define FIONREAD 0x4004667f
#define MSG_PEEK 0x2
#define SD_BOTH 0x02
#define WSADESCRIPTION_LEN 256
#define WSASYS_STATUS_LEN 128
#define WSABASEERR 10000
#define WSAEINTR (WSABASEERR + 4)
#define WSAEBADF (WSABASEERR + 9)
#define WSAEACCES (WSABASEERR + 13)
#define WSAEFAULT (WSABASEERR + 14)
#define WSAEINVAL (WSABASEERR + 22)
#define WSAEMFILE (WSABASEERR + 24)
#define WSAEWOULDBLOCK (WSABASEERR + 35)
#define WSAEINPROGRESS (WSABASEERR + 36)
#define WSAEALREADY (WSABASEERR + 37)
#define WSAENOTSOCK (WSABASEERR + 38)
#define WSAEDESTADDRREQ (WSABASEERR + 39)
#define WSAEMSGSIZE (WSABASEERR + 40)
#define WSAEPROTOTYPE (WSABASEERR + 41)
#define WSAENOPROTOOPT (WSABASEERR + 42)
#define WSAEPROTONOSUPPORT (WSABASEERR + 43)
#define WSAESOCKTNOSUPPORT (WSABASEERR + 44)
#define WSAEOPNOTSUPP (WSABASEERR + 45)
#define WSAEPFNOSUPPORT (WSABASEERR + 46)
#define WSAEAFNOSUPPORT (WSABASEERR + 47)
#define WSAEADDRINUSE (WSABASEERR + 48)
#define WSAEADDRNOTAVAIL (WSABASEERR + 49)
#define WSAENETDOWN (WSABASEERR + 50)
#define WSAENETUNREACH (WSABASEERR + 51)
#define WSAENETRESET (WSABASEERR + 52)
#define WSAECONNABORTED (WSABASEERR + 53)
#define WSAECONNRESET (WSABASEERR + 54)
#define WSAENOBUFS (WSABASEERR + 55)
#define WSAEISCONN (WSABASEERR + 56)
#define WSAENOTCONN (WSABASEERR + 57)
#define WSAESHUTDOWN (WSABASEERR + 58)
#define WSAETOOMANYREFS (WSABASEERR + 59)
#define WSAETIMEDOUT (WSABASEERR + 60)
#define WSAECONNREFUSED (WSABASEERR + 61)
#define WSAELOOP (WSABASEERR + 62)
#define WSAENAMETOOLONG (WSABASEERR + 63)
#define WSAEHOSTDOWN (WSABASEERR + 64)
#define WSAEHOSTUNREACH (WSABASEERR + 65)
#define WSASYSNOTREADY (WSABASEERR + 91)
#define WSAVERNOTSUPPORTED (WSABASEERR + 92)
#define WSANOTINITIALISED (WSABASEERR + 93)
#define WSAHOST_NOT_FOUND (WSABASEERR + 1001)
#define WSATRY_AGAIN (WSABASEERR + 1002)
#define WSANO_RECOVERY (WSABASEERR + 1003)
#define WSANO_DATA (WSABASEERR + 1004)

typedef struct WSAData
{
    WORD wVersion;
    WORD wHighVersion;
    char szDescription[WSADESCRIPTION_LEN + 1];
    char szSystemStatus[WSASYS_STATUS_LEN + 1];
    unsigned short iMaxSockets;
    unsigned short iMaxUdpDg;
    char *lpVendorInfo;
} WSADATA, *LPWSADATA;

typedef struct in_addr
{
    union
    {
        struct
        {
            UCHAR s_b1, s_b2, s_b3, s_b4;
        } S_un_b;
        struct
        {
            USHORT s_w1, s_w2;
        } S_un_w;
        ULONG S_addr;
    } S_un;
} IN_ADDR, *PIN_ADDR, *LPIN_ADDR;
#define s_addr S_un.S_addr
#define s_net S_un.S_un_b.s_b1
#define s_host S_un.S_un_b.s_b2
#define s_lh S_un.S_un_b.s_b3
#define s_impno S_un.S_un_b.s_b4

typedef USHORT ADDRESS_FAMILY;
struct sockaddr
{
    ADDRESS_FAMILY sa_family;
    CHAR sa_data[14];
};
typedef struct sockaddr SOCKADDR, *PSOCKADDR, *LPSOCKADDR;
struct sockaddr_in
{
    short sin_family;
    USHORT sin_port;
    struct in_addr sin_addr;
    CHAR sin_zero[8];
};
typedef struct sockaddr_in SOCKADDR_IN, *PSOCKADDR_IN, *LPSOCKADDR_IN;
typedef struct sockaddr_ipx
{
    short sa_family;
    char sa_netnum[4];
    char sa_nodenum[6];
    unsigned short sa_socket;
} SOCKADDR_IPX, *PSOCKADDR_IPX;
struct in6_addr
{
    union
    {
        UCHAR Byte[16];
        USHORT Word[8];
    } u;
};
struct sockaddr_in6
{
    short sin6_family;
    USHORT sin6_port;
    ULONG sin6_flowinfo;
    struct in6_addr sin6_addr;
    ULONG sin6_scope_id;
};
struct sockaddr_storage
{
    short ss_family;
    char __ss_pad1[6];
    long long __ss_align;
    char __ss_pad2[112];
};
struct hostent
{
    char *h_name;
    char **h_aliases;
    short h_addrtype;
    short h_length;
    char **h_addr_list;
};
#define h_addr h_addr_list[0]
typedef struct hostent HOSTENT, *PHOSTENT, *LPHOSTENT;
typedef struct fd_set
{
    UINT fd_count;
    SOCKET fd_array[FD_SETSIZE];
} fd_set, FD_SET, *PFD_SET, *LPFD_SET;
// struct timeval as musl lays it out, claiming musl's guard (its sys/time.h also brings sys/select.h, whose fd_set
// and select() differ from Winsock's)
#ifndef __DEFINED_struct_timeval
#define __DEFINED_struct_timeval
struct timeval
{
    long long tv_sec;
    long long tv_usec;
};
#endif
typedef struct timeval TIMEVAL, *PTIMEVAL, *LPTIMEVAL;
#define FD_CLR(fd, set) bo1_FD_CLR((SOCKET)(fd), (set))
#define FD_SET(fd, set) bo1_FD_SET((SOCKET)(fd), (set))
#define FD_ZERO(set) (((fd_set *)(set))->fd_count = 0)
#define FD_ISSET(fd, set) __WSAFDIsSet((SOCKET)(fd), (fd_set *)(set))
typedef int socklen_t;

BO1_EXTERN_C_BEGIN
void bo1_FD_CLR(SOCKET fd, fd_set *set);
void bo1_FD_SET(SOCKET fd, fd_set *set);
int WINAPI __WSAFDIsSet(SOCKET fd, fd_set *set);
int WINAPI WSAStartup(WORD wVersionRequested, LPWSADATA lpWSAData);
int WINAPI WSACleanup(void);
int WINAPI WSAGetLastError(void);
void WINAPI WSASetLastError(int iError);
SOCKET WINAPI socket(int af, int type, int protocol);
int WINAPI closesocket(SOCKET s);
int WINAPI bind(SOCKET s, const struct sockaddr *name, int namelen);
int WINAPI connect(SOCKET s, const struct sockaddr *name, int namelen);
int WINAPI listen(SOCKET s, int backlog);
SOCKET WINAPI accept(SOCKET s, struct sockaddr *addr, int *addrlen);
int WINAPI shutdown(SOCKET s, int how);
int WINAPI send(SOCKET s, const char *buf, int len, int flags);
int WINAPI recv(SOCKET s, char *buf, int len, int flags);
int WINAPI sendto(SOCKET s, const char *buf, int len, int flags, const struct sockaddr *to, int tolen);
int WINAPI recvfrom(SOCKET s, char *buf, int len, int flags, struct sockaddr *from, int *fromlen);
int WINAPI select(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds, const struct timeval *timeout);
int WINAPI ioctlsocket(SOCKET s, long cmd, ULONG *argp);
int WINAPI setsockopt(SOCKET s, int level, int optname, const char *optval, int optlen);
int WINAPI getsockopt(SOCKET s, int level, int optname, char *optval, int *optlen);
int WINAPI getsockname(SOCKET s, struct sockaddr *name, int *namelen);
int WINAPI getpeername(SOCKET s, struct sockaddr *name, int *namelen);

struct hostent *WINAPI gethostbyname(const char *name);
struct hostent *WINAPI gethostbyaddr(const char *addr, int len, int type);
ULONG WINAPI inet_addr(const char *cp);
char *WINAPI inet_ntoa(struct in_addr in);
USHORT WINAPI htons(USHORT hostshort);
USHORT WINAPI ntohs(USHORT netshort);
ULONG WINAPI htonl(ULONG hostlong);
ULONG WINAPI ntohl(ULONG netlong);
BO1_EXTERN_C_END
