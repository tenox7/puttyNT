/*
 * Windows networking abstraction, on WinSock 1.1 (IPv4 only), which
 * every version of Windows NT has.
 */

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>

#include "putty.h"
#include "network.h"
#include "tree234.h"
#include "ssh.h"

/*
 * WinSock 2 names that winsock.h (WinSock 1.1) lacks. Windows NT 4.0
 * SP4 and later accept SO_EXCLUSIVEADDRUSE, which stops other
 * programs from listening on the same port; older ones refuse it,
 * which does no harm.
 */
#ifndef SO_EXCLUSIVEADDRUSE
#define SO_EXCLUSIVEADDRUSE ((int)(~SO_REUSEADDR))
#endif
#define SD_SEND 1                      /* shutdown(): no more sending */
#ifndef HANDLE_FLAG_INHERIT
#define HANDLE_FLAG_INHERIT 0x00000001
#endif

#define ipv4_is_loopback(addr) \
        ((p_ntohl(addr.s_addr) & 0xFF000000L) == 0x7F000000L)

/*
 * Mutable state that goes with a SockAddr: stores information
 * about where in the list of candidate IP(v*) addresses we've
 * currently got to.
 */
typedef struct SockAddrStep_tag SockAddrStep;
struct SockAddrStep_tag {
    int curraddr;
};

typedef struct NetSocket NetSocket;
struct NetSocket {
    const char *error;
    SOCKET s;
    Plug *plug;
    bufchain output_data;
    bool connected;
    bool writable;
    bool frozen; /* this causes readability notifications to be ignored */
    bool frozen_readable; /* this means we missed at least one readability
                           * notification while we were frozen */
    bool incomingeof;  /* nothing more to read; for sk_poll */
    bool listener;
    bool localhost_only;               /* for listening sockets */
    char oobdata[1];
    size_t sending_oob;
    bool oobinline, nodelay, keepalive, privport;
    enum { EOF_NO, EOF_PENDING, EOF_SENT } outgoingeof;
    SockAddr *addr;
    SockAddrStep step;
    int port;
    int pending_error;             /* in case send() returns error */

    Socket sock;
};

/*
 * Top-level discriminator for SockAddr.
 *
 * UNRESOLVED means a host name not yet put through DNS; IP means a
 * resolved IP address (or list of them); UNIX indicates the AF_UNIX
 * network family (which Windows also has); NAMEDPIPE indicates that
 * this SockAddr is phony, holding a Windows named pipe pathname
 * instead of any address WinSock can understand.
 */
typedef enum SuperFamily {
    UNRESOLVED,
    IP,
    NAMEDPIPE
} SuperFamily;

struct SockAddr {
    int refcount;
    const char *error;
    SuperFamily superfamily;
    unsigned long *addresses;          /* Addresses IPv4 style. */
    int naddresses;
    char hostname[512];                /* Store an unresolved host name. */
};

/*
 * Which address family this address belongs to. AF_INET for IPv4;
 * AF_UNSPEC indicates that name resolution has not been done and a
 * simple host name is held in this SockAddr structure.
 */
static inline int sockaddr_family(SockAddr *addr, SockAddrStep step)
{
    switch (addr->superfamily) {
      case IP:
        return AF_INET;
      default:
        return AF_UNSPEC;
    }
}

/*
 * Start a SockAddrStep structure to step through multiple
 * addresses.
 */
#define START_STEP(addr, step) \
    ((step).curraddr = 0)

static tree234 *sktree;

static int cmpfortree(void *av, void *bv)
{
    NetSocket *a = (NetSocket *)av, *b = (NetSocket *)bv;
    uintptr_t as = (uintptr_t) a->s, bs = (uintptr_t) b->s;
    if (as < bs)
        return -1;
    if (as > bs)
        return +1;
    if (a < b)
        return -1;
    if (a > b)
        return +1;
    return 0;
}

static int cmpforsearch(void *av, void *bv)
{
    NetSocket *b = (NetSocket *)bv;
    uintptr_t as = (uintptr_t) av, bs = (uintptr_t) b->s;
    if (as < bs)
        return -1;
    if (as > bs)
        return +1;
    return 0;
}

DECL_WINDOWS_FUNCTION(static, int, WSAStartup, (WORD, LPWSADATA));
DECL_WINDOWS_FUNCTION(static, int, WSACleanup, (void));
DECL_WINDOWS_FUNCTION(static, int, closesocket, (SOCKET));
DECL_WINDOWS_FUNCTION(static, ULONG, ntohl, (ULONG));
DECL_WINDOWS_FUNCTION(static, ULONG, htonl, (ULONG));
DECL_WINDOWS_FUNCTION(static, USHORT, htons, (USHORT));
DECL_WINDOWS_FUNCTION(static, USHORT, ntohs, (USHORT));
DECL_WINDOWS_FUNCTION(static, int, gethostname, (char *, int));
DECL_WINDOWS_FUNCTION(static, struct hostent FAR *, gethostbyname,
                      (const char FAR *));
DECL_WINDOWS_FUNCTION(static, struct servent FAR *, getservbyname,
                      (const char FAR *, const char FAR *));
DECL_WINDOWS_FUNCTION(static, ULONG, inet_addr, (const char FAR *));
DECL_WINDOWS_FUNCTION(static, char FAR *, inet_ntoa, (struct in_addr));
DECL_WINDOWS_FUNCTION(static, int, connect,
                      (SOCKET, const struct sockaddr FAR *, int));
DECL_WINDOWS_FUNCTION(static, int, bind,
                      (SOCKET, const struct sockaddr FAR *, int));
DECL_WINDOWS_FUNCTION(static, int, setsockopt,
                      (SOCKET, int, int, const char FAR *, int));
DECL_WINDOWS_FUNCTION(static, SOCKET, socket, (int, int, int));
DECL_WINDOWS_FUNCTION(static, int, listen, (SOCKET, int));
DECL_WINDOWS_FUNCTION(static, int, send, (SOCKET, const char FAR *, int, int));
DECL_WINDOWS_FUNCTION(static, int, shutdown, (SOCKET, int));
DECL_WINDOWS_FUNCTION(static, SOCKET, accept,
                      (SOCKET, struct sockaddr FAR *, int FAR *));
DECL_WINDOWS_FUNCTION(static, int, getpeername,
                      (SOCKET, struct sockaddr FAR *, int FAR *));
DECL_WINDOWS_FUNCTION(static, int, getsockname,
                      (SOCKET, struct sockaddr FAR *, int FAR *));
DECL_WINDOWS_FUNCTION(static, int, recv, (SOCKET, char FAR *, int, int));
DECL_WINDOWS_FUNCTION(extern, int, select,
                      (int, fd_set FAR *, fd_set FAR *, fd_set FAR *,
                       const struct timeval FAR *));
DECL_WINDOWS_FUNCTION(static, int, getsockopt,
                      (SOCKET, int, int, char FAR *, int FAR *));

static HMODULE winsock_module = NULL;
static WSADATA wsadata;

static bool sk_startup(int hi, int lo)
{
    WORD winsock_ver;

    winsock_ver = MAKEWORD(hi, lo);

    if (p_WSAStartup(winsock_ver, &wsadata)) {
        return false;
    }

    if (LOBYTE(wsadata.wVersion) != LOBYTE(winsock_ver)) {
        return false;
    }

    return true;
}

DEF_WINDOWS_FUNCTION(WSAAsyncSelect);
DEF_WINDOWS_FUNCTION(WSAGetLastError);
DEF_WINDOWS_FUNCTION(ioctlsocket);
DEF_WINDOWS_FUNCTION(select);

DECL_WINDOWS_FUNCTION(static, BOOL, SetHandleInformation,
                      (HANDLE, DWORD, DWORD));

/*
 * Stop a socket being inherited by child processes, such as a local
 * proxy command. Windows NT 3.1 has no SetHandleInformation, so there
 * sockets stay inheritable.
 */
static void socket_no_inherit(SOCKET s)
{
    if (p_SetHandleInformation)
        p_SetHandleInformation((HANDLE)s, HANDLE_FLAG_INHERIT, 0);
}

void sk_init(void)
{
    /* WinSock 1.1, which every version of Windows NT has; on NT 4.0
     * this DLL passes the calls on to WinSock 2 */
    winsock_module = load_system32_dll("wsock32.dll");
    if (!winsock_module)
        modalfatalbox("Unable to load any WinSock library");

    GET_WINDOWS_FUNCTION(winsock_module, WSAAsyncSelect);
    GET_WINDOWS_FUNCTION(winsock_module, WSAGetLastError);
    GET_WINDOWS_FUNCTION(winsock_module, WSAStartup);
    GET_WINDOWS_FUNCTION(winsock_module, WSACleanup);
    GET_WINDOWS_FUNCTION(winsock_module, closesocket);
    /* Winelib maps ntohl and friends to things like
     * __wine_ulong_swap, which fail these type checks hopelessly */
    GET_WINDOWS_FUNCTION_NO_TYPECHECK(winsock_module, ntohl);
    GET_WINDOWS_FUNCTION_NO_TYPECHECK(winsock_module, htonl);
    GET_WINDOWS_FUNCTION_NO_TYPECHECK(winsock_module, htons);
    GET_WINDOWS_FUNCTION_NO_TYPECHECK(winsock_module, ntohs);
    GET_WINDOWS_FUNCTION_NO_TYPECHECK(winsock_module, gethostname);
    GET_WINDOWS_FUNCTION(winsock_module, gethostbyname);
    GET_WINDOWS_FUNCTION(winsock_module, getservbyname);
    GET_WINDOWS_FUNCTION(winsock_module, inet_addr);
    GET_WINDOWS_FUNCTION(winsock_module, inet_ntoa);
    GET_WINDOWS_FUNCTION(winsock_module, connect);
    GET_WINDOWS_FUNCTION(winsock_module, bind);
    GET_WINDOWS_FUNCTION(winsock_module, setsockopt);
    GET_WINDOWS_FUNCTION(winsock_module, socket);
    GET_WINDOWS_FUNCTION(winsock_module, listen);
    GET_WINDOWS_FUNCTION(winsock_module, send);
    GET_WINDOWS_FUNCTION(winsock_module, shutdown);
    GET_WINDOWS_FUNCTION(winsock_module, ioctlsocket);
    GET_WINDOWS_FUNCTION(winsock_module, accept);
    GET_WINDOWS_FUNCTION(winsock_module, getpeername);
    GET_WINDOWS_FUNCTION(winsock_module, getsockname);
    GET_WINDOWS_FUNCTION(winsock_module, recv);
    GET_WINDOWS_FUNCTION(winsock_module, select);
    GET_WINDOWS_FUNCTION(winsock_module, getsockopt);

    /* Not in the Windows NT 3.1 headers, so no type check */
    GET_WINDOWS_FUNCTION_NO_TYPECHECK(load_system32_dll("kernel32.dll"),
                                      SetHandleInformation);

    if (!sk_startup(1,1)) {
        modalfatalbox("Unable to initialise WinSock");
    }

    sktree = newtree234(cmpfortree);
}

static void closesocket_wrap(SOCKET skt)
{
    p_closesocket(skt);
    done_with_socket(skt);
}

void sk_cleanup(void)
{
    NetSocket *s;
    int i;

    if (sktree) {
        for (i = 0; (s = index234(sktree, i)) != NULL; i++) {
            closesocket_wrap(s->s);
        }
        freetree234(sktree);
        sktree = NULL;
    }

    /*
     * The WinSock DLL stays loaded: on Windows NT 3.1 it keeps a
     * thread of its own running after WSACleanup, and unloading the
     * DLL under that thread made the program crash or hang on exit.
     */
    if (p_WSACleanup)
        p_WSACleanup();
}

const char *winsock_error_string(int error)
{
    /*
     * Error codes we know about and have historically had reasonably
     * sensible error messages for.
     */
    switch (error) {
      case WSAEACCES:
        return "Network error: Permission denied";
      case WSAEADDRINUSE:
        return "Network error: Address already in use";
      case WSAEADDRNOTAVAIL:
        return "Network error: Cannot assign requested address";
      case WSAEAFNOSUPPORT:
        return
            "Network error: Address family not supported by protocol family";
      case WSAEALREADY:
        return "Network error: Operation already in progress";
      case WSAECONNABORTED:
        return "Network error: Software caused connection abort";
      case WSAECONNREFUSED:
        return "Network error: Connection refused";
      case WSAECONNRESET:
        return "Network error: Connection reset by peer";
      case WSAEDESTADDRREQ:
        return "Network error: Destination address required";
      case WSAEFAULT:
        return "Network error: Bad address";
      case WSAEHOSTDOWN:
        return "Network error: Host is down";
      case WSAEHOSTUNREACH:
        return "Network error: No route to host";
      case WSAEINPROGRESS:
        return "Network error: Operation now in progress";
      case WSAEINTR:
        return "Network error: Interrupted function call";
      case WSAEINVAL:
        return "Network error: Invalid argument";
      case WSAEISCONN:
        return "Network error: Socket is already connected";
      case WSAEMFILE:
        return "Network error: Too many open files";
      case WSAEMSGSIZE:
        return "Network error: Message too long";
      case WSAENETDOWN:
        return "Network error: Network is down";
      case WSAENETRESET:
        return "Network error: Network dropped connection on reset";
      case WSAENETUNREACH:
        return "Network error: Network is unreachable";
      case WSAENOBUFS:
        return "Network error: No buffer space available";
      case WSAENOPROTOOPT:
        return "Network error: Bad protocol option";
      case WSAENOTCONN:
        return "Network error: Socket is not connected";
      case WSAENOTSOCK:
        return "Network error: Socket operation on non-socket";
      case WSAEOPNOTSUPP:
        return "Network error: Operation not supported";
      case WSAEPFNOSUPPORT:
        return "Network error: Protocol family not supported";
      case WSAEPROCLIM:
        return "Network error: Too many processes";
      case WSAEPROTONOSUPPORT:
        return "Network error: Protocol not supported";
      case WSAEPROTOTYPE:
        return "Network error: Protocol wrong type for socket";
      case WSAESHUTDOWN:
        return "Network error: Cannot send after socket shutdown";
      case WSAESOCKTNOSUPPORT:
        return "Network error: Socket type not supported";
      case WSAETIMEDOUT:
        return "Network error: Connection timed out";
      case WSAEWOULDBLOCK:
        return "Network error: Resource temporarily unavailable";
      case WSAEDISCON:
        return "Network error: Graceful shutdown in progress";

      /*
       * Windows NT 3.1 has no text for any WinSock error, so for these
       * FormatMessage fails there too.
       */
      case WSAEBADF:
        return "Network error: Bad file descriptor";
      case WSAETOOMANYREFS:
        return "Network error: Too many references";
      case WSAELOOP:
        return "Network error: Cannot translate name";
      case WSAENAMETOOLONG:
        return "Network error: Name too long";
      case WSAENOTEMPTY:
        return "Network error: Directory not empty";
      case WSAEUSERS:
        return "Network error: User quota exceeded";
      case WSAEDQUOT:
        return "Network error: Disk quota exceeded";
      case WSAESTALE:
        return "Network error: Stale file handle reference";
      case WSAEREMOTE:
        return "Network error: Item is remote";
      case WSASYSNOTREADY:
        return "Network error: Network subsystem is unavailable";
      case WSAVERNOTSUPPORTED:
        return "Network error: WinSock version not supported";
      case WSANOTINITIALISED:
        return "Network error: WinSock is not initialised";
      case WSAHOST_NOT_FOUND:
        return "Network error: Host does not exist";
      case WSATRY_AGAIN:
      case WSANO_DATA:
        return "Network error: Host not found";
      case WSANO_RECOVERY:
        return "Network error: Name lookup failed";
    }

    /*
     * Handle any other error code by delegating to win_strerror.
     */
    return win_strerror(error);
}

static inline const char *namelookup_strerror(DWORD err)
{
    /* PuTTY has traditionally translated a few of the likely error
     * messages into more concise strings than the standard Windows ones.
     * Windows NT 3.1 gives WSANO_DATA for every name it can't find: one
     * that doesn't exist, and any name at all when TCP/IP is set to use
     * the hosts file only. */
    return (err == WSAENETDOWN ? "Network is down" :
            err == WSAHOST_NOT_FOUND ? "Host does not exist" :
            err == WSATRY_AGAIN || err == WSANO_DATA ? "Host not found" :
            winsock_error_string(err));
}

SockAddr *sk_namelookup(const char *host, char **canonicalname,
                        int address_family)
{
    SockAddr *addr;
    unsigned long a;
    struct hostent *h;
    *canonicalname = NULL;

    addr = snew(SockAddr);
    memset(addr, 0, sizeof(SockAddr));
    addr->superfamily = UNRESOLVED;
    addr->refcount = 1;

    /*
     * WinSock 1.1 has no getaddrinfo: check for an IPv4 literal
     * first, then ask gethostbyname.
     */
    a = p_inet_addr(host);
    if (a != (unsigned long) INADDR_NONE) {
        addr->addresses = snew(unsigned long);
        addr->naddresses = 1;
        addr->addresses[0] = p_ntohl(a);
        addr->superfamily = IP;
        *canonicalname = dupstr(host);
        return addr;
    }

    h = p_gethostbyname(host);
    if (h) {
        size_t n;
        addr->superfamily = IP;

        for (n = 0; h->h_addr_list[n]; n++);
        addr->addresses = snewn(n, unsigned long);
        addr->naddresses = n;
        for (n = 0; n < addr->naddresses; n++) {
            uint32_t a;
            memcpy(&a, h->h_addr_list[n], sizeof(a));
            addr->addresses[n] = p_ntohl(a);
        }

        *canonicalname = dupstr(h->h_name);
    } else {
        DWORD err = p_WSAGetLastError();
        addr->error = namelookup_strerror(err);
    }
    return addr;
}

static SockAddr *sk_special_addr(SuperFamily superfamily, const char *name)
{
    SockAddr *addr = snew(SockAddr);
    addr->error = NULL;
    addr->superfamily = superfamily;
    addr->addresses = NULL;
    addr->naddresses = 0;
    addr->refcount = 1;
    strncpy(addr->hostname, name, lenof(addr->hostname));
    addr->hostname[lenof(addr->hostname)-1] = '\0';
    return addr;
}

SockAddr *sk_nonamelookup(const char *host)
{
    return sk_special_addr(UNRESOLVED, host);
}

SockAddr *sk_namedpipe_addr(const char *pipename)
{
    return sk_special_addr(NAMEDPIPE, pipename);
}

static bool sk_nextaddr(SockAddr *addr, SockAddrStep *step)
{
    if (step->curraddr+1 < addr->naddresses) {
        step->curraddr++;
        return true;
    } else {
        return false;
    }
}

void sk_getaddr(SockAddr *addr, char *buf, int buflen)
{
    SockAddrStep step;
    START_STEP(addr, step);

    if (sockaddr_family(addr, step) == AF_INET) {
        struct in_addr a;
        assert(addr->addresses && step.curraddr < addr->naddresses);
        a.s_addr = p_htonl(addr->addresses[step.curraddr]);
        strncpy(buf, p_inet_ntoa(a), buflen);
        buf[buflen-1] = '\0';
    } else {
        strncpy(buf, addr->hostname, buflen);
        buf[buflen-1] = '\0';
    }
}

/*
 * This constructs a SockAddr that points at one specific sub-address
 * of a parent SockAddr. The returned SockAddr does not own all its
 * own memory: it points into the old one's data structures, so it
 * MUST NOT be used after the old one is freed, and it MUST NOT be
 * passed to sk_addr_free. (The latter is why it's returned by value
 * rather than dynamically allocated - that should clue in anyone
 * writing a call to it that something is weird about it.)
 */
static SockAddr sk_extractaddr_tmp(
    SockAddr *addr, const SockAddrStep *step)
{
    SockAddr toret;
    toret = *addr;                    /* structure copy */
    toret.refcount = 1;

    if (sockaddr_family(addr, *step) == AF_INET)
        toret.addresses += step->curraddr;

    return toret;
}

bool sk_addr_needs_port(SockAddr *addr)
{
    return addr->superfamily != NAMEDPIPE;
}

bool sk_hostname_is_local(const char *name)
{
    return !strcmp(name, "localhost") ||
           !strcmp(name, "::1") ||
           !strncmp(name, "127.", 4);
}

/*
 * Only loopback addresses count as local: the list of this machine's
 * own addresses (WSAIoctl with SIO_GET_INTERFACE_LIST) needs WinSock 2.
 */
static bool ipv4_is_local_addr(struct in_addr addr)
{
    return ipv4_is_loopback(addr);
}

bool sk_address_is_local(SockAddr *addr)
{
    SockAddrStep step;
    int family;
    START_STEP(addr, step);
    family = sockaddr_family(addr, step);

    if (family == AF_INET) {
        {
            struct in_addr a;
            assert(addr->addresses && step.curraddr < addr->naddresses);
            a.s_addr = p_htonl(addr->addresses[step.curraddr]);
            return ipv4_is_local_addr(a);
        }
    } else {
        assert(family == AF_UNSPEC);
        return false;                  /* we don't know; assume not */
    }
}

bool sk_address_is_special_local(SockAddr *addr)
{
    return false;            /* no Unix-domain socket analogue here */
}

int sk_addrtype(SockAddr *addr)
{
    SockAddrStep step;
    int family;
    START_STEP(addr, step);
    family = sockaddr_family(addr, step);

    return (family == AF_INET ? ADDRTYPE_IPV4 :
            ADDRTYPE_NAME);
}

void sk_addrcopy(SockAddr *addr, char *buf)
{
    SockAddrStep step;
    int family;
    START_STEP(addr, step);
    family = sockaddr_family(addr, step);

    assert(family != AF_UNSPEC);
    if (family == AF_INET) {
        struct in_addr a;
        assert(addr->addresses && step.curraddr < addr->naddresses);
        a.s_addr = p_htonl(addr->addresses[step.curraddr]);
        memcpy(buf, (char*) &a.s_addr, 4);
    }
}

void sk_addr_free(SockAddr *addr)
{
    if (--addr->refcount > 0)
        return;
    if (addr->addresses)
        sfree(addr->addresses);
    sfree(addr);
}

SockAddr *sk_addr_dup(SockAddr *addr)
{
    addr->refcount++;
    return addr;
}

static Plug *sk_net_plug(Socket *sock, Plug *p)
{
    NetSocket *s = container_of(sock, NetSocket, sock);
    Plug *ret = s->plug;
    if (p)
        s->plug = p;
    return ret;
}

static void sk_net_close(Socket *s);
static size_t sk_net_write(Socket *s, const void *data, size_t len);
static size_t sk_net_write_oob(Socket *s, const void *data, size_t len);
static void sk_net_write_eof(Socket *s);
static void sk_net_set_frozen(Socket *s, bool is_frozen);
static const char *sk_net_socket_error(Socket *s);
static SocketEndpointInfo *sk_net_endpoint_info(Socket *s, bool peer);

static const SocketVtable NetSocket_sockvt = {
    /* .plug = */ sk_net_plug,
    /* .close = */ sk_net_close,
    /* .write = */ sk_net_write,
    /* .write_oob = */ sk_net_write_oob,
    /* .write_eof = */ sk_net_write_eof,
    /* .set_frozen = */ sk_net_set_frozen,
    /* .socket_error = */ sk_net_socket_error,
    /* .endpoint_info = */ sk_net_endpoint_info,
};

static Socket *sk_net_accept(accept_ctx_t ctx, Plug *plug)
{
    DWORD err;
    const char *errstr;
    NetSocket *s;

    /*
     * Create NetSocket structure.
     */
    s = snew(NetSocket);
    s->sock.vt = &NetSocket_sockvt;
    s->error = NULL;
    s->plug = plug;
    bufchain_init(&s->output_data);
    s->connected = true;
    s->writable = true;              /* to start with */
    s->sending_oob = 0;
    s->outgoingeof = EOF_NO;
    s->frozen = true;
    s->frozen_readable = false;
    s->incomingeof = false;
    s->listener = false;
    s->localhost_only = false;    /* unused, but best init anyway */
    s->pending_error = 0;
    s->addr = NULL;

    s->s = (SOCKET)ctx.p;

    if (s->s == INVALID_SOCKET) {
        err = p_WSAGetLastError();
        s->error = winsock_error_string(err);
        return &s->sock;
    }

    s->oobinline = false;

    /* Set up a select mechanism. This could be an AsyncSelect on a
     * window, or an EventSelect on an event object. */
    errstr = do_select(s->s, true);
    if (errstr) {
        s->error = errstr;
        return &s->sock;
    }

    add234(sktree, s);

    return &s->sock;
}

static DWORD try_connect(NetSocket *sock)
{
    SOCKET s;
    SOCKADDR_IN a;
    DWORD err;
    const char *errstr;
    short localport;
    int family;

    if (sock->s != INVALID_SOCKET) {
        do_select(sock->s, false);
        closesocket_wrap(sock->s);
    }

    {
        SockAddr thisaddr = sk_extractaddr_tmp(
            sock->addr, &sock->step);
        plug_log(sock->plug, &sock->sock, PLUGLOG_CONNECT_TRYING,
                 &thisaddr, sock->port, NULL, 0);
    }

    /*
     * Open socket.
     */
    family = sockaddr_family(sock->addr, sock->step);

    /*
     * Remove the socket from the tree before we overwrite its
     * internal socket id, because that forms part of the tree's
     * sorting criterion. We'll add it back before exiting this
     * function, whether we changed anything or not.
     */
    del234(sktree, sock);

    s = p_socket(family, SOCK_STREAM, 0);
    sock->s = s;

    if (s == INVALID_SOCKET) {
        err = p_WSAGetLastError();
        sock->error = winsock_error_string(err);
        goto ret;
    }

    socket_no_inherit(s);

    if (sock->oobinline) {
        BOOL b = true;
        p_setsockopt(s, SOL_SOCKET, SO_OOBINLINE, (void *) &b, sizeof(b));
    }

    if (sock->nodelay) {
        BOOL b = true;
        p_setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (void *) &b, sizeof(b));
    }

    if (sock->keepalive) {
        BOOL b = true;
        p_setsockopt(s, SOL_SOCKET, SO_KEEPALIVE, (void *) &b, sizeof(b));
    }

    /*
     * Bind to local address.
     */
    if (sock->privport)
        localport = 1023;              /* count from 1023 downwards */
    else
        localport = 0;                 /* just use port 0 (ie winsock picks) */

    /* Loop round trying to bind */
    while (1) {
        int sockcode;

        {
            a.sin_family = AF_INET;
            a.sin_addr.s_addr = p_htonl(INADDR_ANY);
            a.sin_port = p_htons(localport);
        }
        sockcode = p_bind(s, (struct sockaddr *) &a, sizeof(a));
        if (sockcode != SOCKET_ERROR) {
            err = 0;
            break;                     /* done */
        } else {
            err = p_WSAGetLastError();
            if (err != WSAEADDRINUSE)  /* failed, for a bad reason */
                break;
        }

        if (localport == 0)
            break;                     /* we're only looping once */
        localport--;
        if (localport == 0)
            break;                     /* we might have got to the end */
    }

    if (err) {
        sock->error = winsock_error_string(err);
        goto ret;
    }

    /*
     * Connect to remote address.
     */
    {
        assert(sock->addr->addresses && sock->step.curraddr < sock->addr->naddresses);
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = p_htonl(sock->addr->addresses[sock->step.curraddr]);
        a.sin_port = p_htons((short) sock->port);
    }

    /* Set up a select mechanism. This could be an AsyncSelect on a
     * window, or an EventSelect on an event object. */
    errstr = do_select(s, true);
    if (errstr) {
        sock->error = errstr;
        err = 1;
        goto ret;
    }

    if ((
            p_connect(s, (struct sockaddr *) &a, sizeof(a))
        ) == SOCKET_ERROR) {
        err = p_WSAGetLastError();
        /*
         * We expect a potential EWOULDBLOCK here, because the
         * chances are the front end has done a select for
         * FD_CONNECT, so that connect() will complete
         * asynchronously.
         */
        if ( err != WSAEWOULDBLOCK ) {
            sock->error = winsock_error_string(err);
            goto ret;
        }
    } else {
        SockAddr thisaddr;
        /*
         * If we _don't_ get EWOULDBLOCK, the connect has completed
         * and we should set the socket as writable.
         */
        sock->writable = true;
        thisaddr = sk_extractaddr_tmp(sock->addr, &sock->step);
        plug_log(sock->plug, &sock->sock, PLUGLOG_CONNECT_SUCCESS,
                 &thisaddr, sock->port, NULL, 0);
    }

    err = 0;

  ret:

    /*
     * No matter what happened, put the socket back in the tree.
     */
    add234(sktree, sock);

    if (err) {
        SockAddr thisaddr = sk_extractaddr_tmp(
            sock->addr, &sock->step);
        plug_log(sock->plug, &sock->sock, PLUGLOG_CONNECT_FAILED,
                 &thisaddr, sock->port, sock->error, err);
    }
    return err;
}

Socket *sk_new(SockAddr *addr, int port, bool privport, bool oobinline,
               bool nodelay, bool keepalive, Plug *plug)
{
    NetSocket *s;
    DWORD err;

    /*
     * Create NetSocket structure.
     */
    s = snew(NetSocket);
    s->sock.vt = &NetSocket_sockvt;
    s->error = NULL;
    s->plug = plug;
    bufchain_init(&s->output_data);
    s->connected = false;            /* to start with */
    s->writable = false;             /* to start with */
    s->sending_oob = 0;
    s->outgoingeof = EOF_NO;
    s->frozen = false;
    s->frozen_readable = false;
    s->incomingeof = false;
    s->listener = false;
    s->localhost_only = false;    /* unused, but best init anyway */
    s->pending_error = 0;
    s->oobinline = oobinline;
    s->nodelay = nodelay;
    s->keepalive = keepalive;
    s->privport = privport;
    s->port = port;
    s->addr = addr;
    START_STEP(s->addr, s->step);
    s->s = INVALID_SOCKET;

    err = 0;
    do {
        err = try_connect(s);
    } while (err && sk_nextaddr(s->addr, &s->step));

    return &s->sock;
}

static Socket *sk_newlistener_internal(
    const char *srcaddr, int port, Plug *plug,
    bool local_host_only, int orig_address_family)
{
    SOCKET sk;
    SOCKADDR_IN a;
    struct sockaddr *bindaddr;
    unsigned bindsize;

    DWORD err;
    const char *errstr;
    NetSocket *s;
    int retcode;

    int address_family = orig_address_family;

    /*
     * Create NetSocket structure.
     */
    s = snew(NetSocket);
    s->sock.vt = &NetSocket_sockvt;
    s->error = NULL;
    s->plug = plug;
    bufchain_init(&s->output_data);
    s->connected = false;
    s->writable = false;             /* to start with */
    s->sending_oob = 0;
    s->outgoingeof = EOF_NO;
    s->frozen = false;
    s->frozen_readable = false;
    s->incomingeof = false;
    s->listener = true;
    s->localhost_only = local_host_only;
    s->pending_error = 0;
    s->addr = NULL;

    /*
     * IPv4 is all there is: the `don't care' value ADDRTYPE_UNSPEC
     * means IPv4 too.
     */
    if (address_family == AF_UNSPEC) address_family = AF_INET;

    /*
     * Open socket.
     */
    sk = p_socket(address_family, SOCK_STREAM, 0);
    s->s = sk;

    if (sk == INVALID_SOCKET) {
        err = p_WSAGetLastError();
        s->error = winsock_error_string(err);
        return &s->sock;
    }

    socket_no_inherit(sk);

    s->oobinline = false;

    {
        BOOL on = true;
        p_setsockopt(sk, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                     (const char *)&on, sizeof(on));
    }

    switch (address_family) {
      case AF_INET: {
        bool got_addr = false;
        a.sin_family = AF_INET;

        /*
         * Bind to source address. First try an explicitly
         * specified one...
         */
        if (srcaddr) {
            a.sin_addr.s_addr = p_inet_addr(srcaddr);
            if (a.sin_addr.s_addr != INADDR_NONE) {
                /* Override localhost_only with specified listen addr. */
                s->localhost_only = ipv4_is_loopback(a.sin_addr);
                got_addr = true;
            }
        }

        /*
         * ... and failing that, go with one of the standard ones.
         */
        if (!got_addr) {
            if (local_host_only)
                a.sin_addr.s_addr = p_htonl(INADDR_LOOPBACK);
            else
                a.sin_addr.s_addr = p_htonl(INADDR_ANY);
        }

        a.sin_port = p_htons((short)port);
        bindaddr = (struct sockaddr *)&a;
        bindsize = sizeof(a);
        break;
      }
      default:
        unreachable("bad address family in sk_newlistener_internal");
    }

    retcode = p_bind(sk, bindaddr, bindsize);
    if (retcode != SOCKET_ERROR) {
        err = 0;
    } else {
        err = p_WSAGetLastError();
    }

    if (err) {
        closesocket_wrap(sk);
        s->error = winsock_error_string(err);
        return &s->sock;
    }


    if (p_listen(sk, SOMAXCONN) == SOCKET_ERROR) {
        closesocket_wrap(sk);
        s->error = winsock_error_string(p_WSAGetLastError());
        return &s->sock;
    }

    /* Set up a select mechanism. This could be an AsyncSelect on a
     * window, or an EventSelect on an event object. */
    errstr = do_select(sk, true);
    if (errstr) {
        closesocket_wrap(sk);
        s->error = errstr;
        return &s->sock;
    }

    add234(sktree, s);

    return &s->sock;
}

Socket *sk_newlistener(const char *srcaddr, int port, Plug *plug,
                       bool local_host_only, int orig_address_family)
{
    /*
     * Translate address_family from platform-independent constants
     * into local reality.
     */
    int address_family = (orig_address_family == ADDRTYPE_IPV4 ? AF_INET :
                          AF_UNSPEC);

    return sk_newlistener_internal(srcaddr, port, plug, local_host_only,
                                   address_family);
}

static void sk_net_close(Socket *sock)
{
    NetSocket *s = container_of(sock, NetSocket, sock);

    bufchain_clear(&s->output_data);

    del234(sktree, s);
    do_select(s->s, false);
    closesocket_wrap(s->s);
    if (s->addr)
        sk_addr_free(s->addr);
    delete_callbacks_for_context(s);
    sfree(s);
}

void plug_closing_system_error(Plug *plug, DWORD error)
{
    PlugCloseType type = PLUGCLOSE_ERROR;
    if (error == ERROR_BROKEN_PIPE)
        type = PLUGCLOSE_BROKEN_PIPE;
    plug_closing(plug, type, win_strerror(error));
}

void plug_closing_winsock_error(Plug *plug, DWORD error)
{
    plug_closing(plug, PLUGCLOSE_ERROR, winsock_error_string(error));
}

/*
 * Deal with socket errors detected in try_send().
 */
static void socket_error_callback(void *vs)
{
    NetSocket *s = (NetSocket *)vs;

    /*
     * Just in case other socket work has caused this socket to vanish
     * or become somehow non-erroneous before this callback arrived...
     */
    if (!find234(sktree, s, NULL) || !s->pending_error)
        return;

    /*
     * An error has occurred on this socket. Pass it to the plug.
     */
    plug_closing_winsock_error(s->plug, s->pending_error);
}

/*
 * The function which tries to send on a socket once it's deemed
 * writable.
 */
static void try_send(NetSocket *s)
{
    while (s->sending_oob || bufchain_size(&s->output_data) > 0) {
        int nsent;
        DWORD err;
        const void *data;
        size_t len;
        int urgentflag;

        if (s->sending_oob) {
            urgentflag = MSG_OOB;
            len = s->sending_oob;
            data = &s->oobdata;
        } else {
            ptrlen bufdata;
            urgentflag = 0;
            bufdata = bufchain_prefix(&s->output_data);
            data = bufdata.ptr;
            len = bufdata.len;
        }
        len = min(len, INT_MAX);       /* WinSock send() takes an int */
#ifdef WIN32S_COMPAT
        /* Win32s WinSock 1.1 fails with WSAEFAULT for large buffers */
        len = min(len, 4096);
#endif
        nsent = p_send(s->s, data, len, urgentflag);
        noise_ultralight(NOISE_SOURCE_IOLEN, nsent);
        if (nsent <= 0) {
            err = (nsent < 0 ? p_WSAGetLastError() : 0);
            if ((err < WSABASEERR && nsent < 0) || err == WSAEWOULDBLOCK) {
                /*
                 * Perfectly normal: we've sent all we can for the moment.
                 *
                 * (Some WinSock send() implementations can return
                 * <0 but leave no sensible error indication -
                 * WSAGetLastError() is called but returns zero or
                 * a small number - so we check that case and treat
                 * it just like WSAEWOULDBLOCK.)
                 */
                s->writable = false;
                return;
            } else {
                /*
                 * If send() returns a socket error, we unfortunately
                 * can't just call plug_closing(), because it's quite
                 * likely that we're currently _in_ a call from the
                 * code we'd be calling back to, so we'd have to make
                 * half the SSH code reentrant. Instead we flag a
                 * pending error on the socket, to be dealt with (by
                 * calling plug_closing()) at some suitable future
                 * moment.
                 */
                s->pending_error = err;
                queue_toplevel_callback(socket_error_callback, s);
                return;
            }
        } else {
            if (s->sending_oob) {
                if (nsent < len) {
                    memmove(s->oobdata, s->oobdata+nsent, len-nsent);
                    s->sending_oob = len - nsent;
                } else {
                    s->sending_oob = 0;
                }
            } else {
                bufchain_consume(&s->output_data, nsent);
            }
        }
    }

    /*
     * If we reach here, we've finished sending everything we might
     * have needed to send. Send EOF, if we need to.
     */
    if (s->outgoingeof == EOF_PENDING) {
        p_shutdown(s->s, SD_SEND);
        s->outgoingeof = EOF_SENT;
    }
}

static size_t sk_net_write(Socket *sock, const void *buf, size_t len)
{
    NetSocket *s = container_of(sock, NetSocket, sock);

    assert(s->outgoingeof == EOF_NO);

    /*
     * Add the data to the buffer list on the socket.
     */
    bufchain_add(&s->output_data, buf, len);

    /*
     * Now try sending from the start of the buffer list.
     */
    if (s->writable)
        try_send(s);

    return bufchain_size(&s->output_data);
}

static size_t sk_net_write_oob(Socket *sock, const void *buf, size_t len)
{
    NetSocket *s = container_of(sock, NetSocket, sock);

    assert(s->outgoingeof == EOF_NO);

    /*
     * Replace the buffer list on the socket with the data.
     */
    bufchain_clear(&s->output_data);
    assert(len <= sizeof(s->oobdata));
    memcpy(s->oobdata, buf, len);
    s->sending_oob = len;

    /*
     * Now try sending from the start of the buffer list.
     */
    if (s->writable)
        try_send(s);

    return s->sending_oob;
}

static void sk_net_write_eof(Socket *sock)
{
    NetSocket *s = container_of(sock, NetSocket, sock);

    assert(s->outgoingeof == EOF_NO);

    /*
     * Mark the socket as pending outgoing EOF.
     */
    s->outgoingeof = EOF_PENDING;

    /*
     * Now try sending from the start of the buffer list.
     */
    if (s->writable)
        try_send(s);
}

/*
 * Whether recv() on a socket would find data. Windows NT 3.1's
 * WinSock gives a socket no more data once recv() on it has failed
 * with WSAEWOULDBLOCK: recv() goes on failing, and neither select()
 * nor WSAAsyncSelect reports what comes, although FIONREAD counts
 * it. So call recv() only when FIONREAD counts something.
 */
static bool sk_net_readable(SOCKET skt)
{
    u_long avail = 0;

    if (p_ioctlsocket(skt, FIONREAD, &avail) == SOCKET_ERROR)
        return true;                   /* can't tell, so try recv() */
    return avail != 0;
}

void select_result(WPARAM wParam, LPARAM lParam)
{
    int ret;
    DWORD err;
    char buf[20480];                   /* nice big buffer for plenty of speed */
    NetSocket *s;
    bool atmark;

    /* wParam is the socket itself */

    if (wParam == 0)
        return;                /* boggle */

    s = find234(sktree, (void *) wParam, cmpforsearch);
    if (!s)
        return;                /* boggle */

    if ((err = WSAGETSELECTERROR(lParam)) != 0) {
        /*
         * An error has occurred on this socket. Pass it to the
         * plug.
         */
        if (s->addr) {
            SockAddr thisaddr = sk_extractaddr_tmp(
                s->addr, &s->step);
            plug_log(s->plug, &s->sock, PLUGLOG_CONNECT_FAILED, &thisaddr,
                     s->port, winsock_error_string(err), err);
            while (err && s->addr && sk_nextaddr(s->addr, &s->step)) {
                err = try_connect(s);
            }
        }
        if (err != 0)
            plug_closing_winsock_error(s->plug, err);
        return;
    }

    noise_ultralight(NOISE_SOURCE_IOID, wParam);

    switch (WSAGETSELECTEVENT(lParam)) {
      case FD_CONNECT:
        s->connected = true;
        s->writable = true;

        /*
         * Once a socket is connected, we can stop falling back
         * through the candidate addresses to connect to. But first,
         * let the plug know we were successful.
         */
        if (s->addr) {
            SockAddr thisaddr = sk_extractaddr_tmp(
                s->addr, &s->step);
            plug_log(s->plug, &s->sock, PLUGLOG_CONNECT_SUCCESS,
                     &thisaddr, s->port, NULL, 0);

            sk_addr_free(s->addr);
            s->addr = NULL;
        }

        /*
         * Send what was written before the connection was made.
         * Other WinSocks post an FD_WRITE for that after
         * FD_CONNECT, but Windows NT 3.1's doesn't, and then the
         * SSH version string would wait for ever.
         */
        {
            size_t bufsize_before, bufsize_after;
            bufsize_before = s->sending_oob + bufchain_size(&s->output_data);
            try_send(s);
            bufsize_after = s->sending_oob + bufchain_size(&s->output_data);
            if (bufsize_after < bufsize_before)
                plug_sent(s->plug, bufsize_after);
        }
        break;
      case FD_READ:
        /* The end has been reported already */
        if (s->incomingeof)
            break;

        /* In the case the socket is still frozen, we don't even bother */
        if (s->frozen) {
            s->frozen_readable = true;
            break;
        }

        /*
         * A message can come when there is nothing to read, if
         * sk_poll has read its data already. recv() would then fail
         * with WSAEWOULDBLOCK, which Windows NT 3.1 doesn't survive
         * (see sk_net_readable).
         */
        if (!sk_net_readable(s->s))
            break;

        /*
         * We have received data on the socket. For an oobinline
         * socket, this might be data _before_ an urgent pointer,
         * in which case we send it to the back end with type==1
         * (data prior to urgent).
         */
        if (s->oobinline) {
            u_long atmark_from_ioctl = 1;
            p_ioctlsocket(s->s, SIOCATMARK, &atmark_from_ioctl);
            /*
             * Avoid checking the return value from ioctlsocket(),
             * on the grounds that some WinSock wrappers don't
             * support it. If it does nothing, we get atmark==1,
             * which is equivalent to `no OOB pending', so the
             * effect will be to non-OOB-ify any OOB data.
             */
            atmark = atmark_from_ioctl != 0;
        } else
            atmark = true;

        ret = p_recv(s->s, buf, sizeof(buf), 0);
        noise_ultralight(NOISE_SOURCE_IOLEN, ret);
        if (ret < 0) {
            err = p_WSAGetLastError();
            if (err == WSAEWOULDBLOCK) {
                break;
            }
        }
        if (ret < 0) {
            plug_closing_winsock_error(s->plug, err);
        } else if (0 == ret) {
            s->incomingeof = true;
            plug_closing_normal(s->plug);
        } else {
            plug_receive(s->plug, atmark ? 0 : 1, buf, ret);
        }
        break;
      case FD_OOB:
        /*
         * This will only happen on a non-oobinline socket. It
         * indicates that we can immediately perform an OOB read
         * and get back OOB data, which we will send to the back
         * end with type==2 (urgent data).
         */
        ret = p_recv(s->s, buf, sizeof(buf), MSG_OOB);
        noise_ultralight(NOISE_SOURCE_IOLEN, ret);
        if (ret <= 0) {
            int err = p_WSAGetLastError();
            plug_closing_winsock_error(s->plug, err);
        } else {
            plug_receive(s->plug, 2, buf, ret);
        }
        break;
      case FD_WRITE: {
        int bufsize_before, bufsize_after;
        s->writable = true;
        bufsize_before = s->sending_oob + bufchain_size(&s->output_data);
        try_send(s);
        bufsize_after = s->sending_oob + bufchain_size(&s->output_data);
        if (bufsize_after < bufsize_before)
            plug_sent(s->plug, bufsize_after);
        break;
      }
      case FD_CLOSE:
        /* Reported already, if sk_poll found the end first */
        if (s->incomingeof)
            return;

        /* Signal a close on the socket. First read any outstanding data. */
        do {
            ret = p_recv(s->s, buf, sizeof(buf), 0);
            if (ret < 0) {
                err = p_WSAGetLastError();
                if (err == WSAEWOULDBLOCK)
                    break;
                plug_closing_winsock_error(s->plug, err);
            } else {
                if (ret) {
                    plug_receive(s->plug, 0, buf, ret);
                } else {
                    s->incomingeof = true;
                    plug_closing_normal(s->plug);
                }
            }
        } while (ret > 0);
        return;
      case FD_ACCEPT: {
        struct sockaddr_in isa;
        int addrlen = sizeof(isa);
        SOCKET t;  /* socket of connection */
        accept_ctx_t actx;

        memset(&isa, 0, sizeof(isa));
        err = 0;
        t = p_accept(s->s,(struct sockaddr *)&isa,&addrlen);
        if (t == INVALID_SOCKET) {
            err = p_WSAGetLastError();
            /* WSAEWOULDBLOCK: sk_poll and a message both reported
             * the same connection, and it was accepted already */
            if (err == WSATRY_AGAIN || err == WSAEWOULDBLOCK)
                break;
        }

        actx.p = (void *)t;

        if (s->localhost_only && !ipv4_is_local_addr(isa.sin_addr))
        {
            closesocket_wrap(t);  /* dodgy WinSock let nonlocal through */
        } else if (plug_accepting(s->plug, sk_net_accept, actx)) {
            closesocket_wrap(t);  /* denied or error */
        }
        break;
      }
    }
}

static bool fd_in(SOCKET skt, const fd_set *set)
{
    u_int i;
    for (i = 0; i < set->fd_count; i++)
        if (set->fd_array[i] == skt)
            return true;
    return false;
}

/*
 * Look at the sockets (the first FD_SETSIZE of them) with select(),
 * and pass what it finds to select_result as the events
 * WSAAsyncSelect would have sent. Returns true if there was any.
 *
 * With 'all', this is how the console tools find network activity
 * when they have no window for WSAAsyncSelect to send it to (see
 * select-cli.c). Without, it is a safety net that the event loops run
 * every SK_POLL_INTERVAL milliseconds. Windows NT 3.1's WinSock
 * sometimes never sends an event, such as FD_READ for the first data
 * after a connect that didn't finish at once, and then the connection
 * would wait for ever. The messages still come as well, so an event
 * can be reported twice. That does no harm, as select_result reads
 * only what FIONREAD counts, except for urgent data, which the safety
 * net leaves out. A socket that select() finds readable with nothing
 * to read has reached its end, so it gets FD_CLOSE.
 */
bool sk_poll(bool all)
{
    fd_set rfds, wfds, efds;
    struct timeval tv;
    SOCKET ready[3 * FD_SETSIZE], skt;
    u_int nready = 0, i, j;
    NetSocket *s;
    int k;

    FD_ZERO(&rfds);
    FD_ZERO(&wfds);
    FD_ZERO(&efds);
    for (k = 0; (s = index234(sktree, k)) != NULL; k++) {
        if (s->s == INVALID_SOCKET || s->error || s->pending_error)
            continue;
        if (s->listener) {
            FD_SET(s->s, &rfds);         /* FD_ACCEPT */
        } else if (!s->connected) {
            FD_SET(s->s, &wfds);         /* FD_CONNECT */
            FD_SET(s->s, &efds);         /* FD_CONNECT, failed */
        } else {
            if (!s->frozen && !s->incomingeof)
                FD_SET(s->s, &rfds);     /* FD_READ */
            if (!s->writable)
                FD_SET(s->s, &wfds);     /* FD_WRITE */
            if (all && !s->oobinline && !s->incomingeof)
                FD_SET(s->s, &efds);     /* FD_OOB */
        }
    }
    if (rfds.fd_count == 0 && wfds.fd_count == 0 && efds.fd_count == 0)
        return false;

    tv.tv_sec = tv.tv_usec = 0;
    if (p_select(0, &rfds, &wfds, &efds, &tv) <= 0)
        return false;

    for (i = 0; i < 3; i++) {
        fd_set *set = i == 0 ? &efds : i == 1 ? &wfds : &rfds;
        for (j = 0; j < set->fd_count; j++) {
            skt = set->fd_array[j];
            for (k = 0; k < (int)nready && ready[k] != skt; k++);
            if (k == (int)nready)
                ready[nready++] = skt;
        }
    }

    /*
     * select_result can close sockets, and the plugs can open new
     * ones, so each event looks its socket up again.
     */
    for (i = 0; i < nready; i++) {
        NetSocket *s0;
        skt = ready[i];
        s0 = find234(sktree, (void *)skt, cmpforsearch);
        if (!s0)
            continue;
        if (s0->listener) {
            select_result((WPARAM)skt, WSAMAKESELECTREPLY(FD_ACCEPT, 0));
        } else if (!s0->connected) {
            /* A failed connection is in efds, or, from some WinSocks,
             * in wfds like a successful one */
            int err = 0, len = sizeof(err);
            p_getsockopt(skt, SOL_SOCKET, SO_ERROR, (char *)&err, &len);
            if (err == 0 && fd_in(skt, &efds))
                err = WSAECONNREFUSED;
            select_result((WPARAM)skt, WSAMAKESELECTREPLY(FD_CONNECT, err));
        } else {
            if (fd_in(skt, &efds))
                select_result((WPARAM)skt, WSAMAKESELECTREPLY(FD_OOB, 0));
            if (fd_in(skt, &wfds) &&
                find234(sktree, (void *)skt, cmpforsearch) == s0)
                select_result((WPARAM)skt, WSAMAKESELECTREPLY(FD_WRITE, 0));
            if (fd_in(skt, &rfds) &&
                find234(sktree, (void *)skt, cmpforsearch) == s0)
                select_result((WPARAM)skt, WSAMAKESELECTREPLY(
                                  sk_net_readable(skt) ? FD_READ : FD_CLOSE,
                                  0));
        }
    }
    return true;
}

/*
 * Special error values are returned from sk_namelookup and sk_new
 * if there's a problem. These functions extract an error message,
 * or return NULL if there's no problem.
 */
const char *sk_addr_error(SockAddr *addr)
{
    return addr->error;
}
static const char *sk_net_socket_error(Socket *sock)
{
    NetSocket *s = container_of(sock, NetSocket, sock);
    return s->error;
}

static SocketEndpointInfo *sk_net_endpoint_info(Socket *sock, bool peer)
{
    NetSocket *s = container_of(sock, NetSocket, sock);
    struct sockaddr_in addr;
    int addrlen = sizeof(addr);
    SocketEndpointInfo *pi;

    {
        int retd = (peer ?
                    p_getpeername(s->s, (struct sockaddr *)&addr, &addrlen) :
                    p_getsockname(s->s, (struct sockaddr *)&addr, &addrlen));
        if (retd < 0)
            return NULL;
    }

    pi = snew(SocketEndpointInfo);
    pi->addressfamily = ADDRTYPE_UNSPEC;
    pi->addr_text = NULL;
    pi->port = -1;
    pi->log_text = NULL;

    if (((struct sockaddr *)&addr)->sa_family == AF_INET) {
        pi->addressfamily = ADDRTYPE_IPV4;
        memcpy(pi->addr_bin.ipv4, &((struct sockaddr_in *)&addr)->sin_addr, 4);
        pi->port = p_ntohs(((struct sockaddr_in *)&addr)->sin_port);
        pi->addr_text = dupstr(
            p_inet_ntoa(((struct sockaddr_in *)&addr)->sin_addr));
        pi->log_text = dupprintf("%s:%d", pi->addr_text, pi->port);

    } else {
        sfree(pi);
        return NULL;
    }

    return pi;
}

static void sk_net_set_frozen(Socket *sock, bool is_frozen)
{
    NetSocket *s = container_of(sock, NetSocket, sock);
    if (s->frozen == is_frozen)
        return;
    s->frozen = is_frozen;
    if (!is_frozen) {
        do_select(s->s, true);
        if (s->frozen_readable && sk_net_readable(s->s)) {
            char c;
            p_recv(s->s, &c, 1, MSG_PEEK);
        }
    }
    s->frozen_readable = false;
}

void socket_reselect_all(void)
{
    NetSocket *s;
    int i;

    for (i = 0; (s = index234(sktree, i)) != NULL; i++) {
        if (!s->frozen)
            do_select(s->s, true);
    }
}

/*
 * For Plink: enumerate all sockets currently active.
 */
SOCKET first_socket(int *state)
{
    NetSocket *s;
    *state = 0;
    s = index234(sktree, (*state)++);
    return s ? s->s : INVALID_SOCKET;
}

SOCKET next_socket(int *state)
{
    NetSocket *s = index234(sktree, (*state)++);
    return s ? s->s : INVALID_SOCKET;
}

bool socket_writable(SOCKET skt)
{
    NetSocket *s = find234(sktree, (void *)skt, cmpforsearch);

    if (s)
        return bufchain_size(&s->output_data) > 0;
    else
        return false;
}

int net_service_lookup(const char *service)
{
    struct servent *se;
    se = p_getservbyname(service, NULL);
    if (se != NULL)
        return p_ntohs(se->s_port);
    else
        return 0;
}

char *get_hostname(void)
{
    char hostbuf[256]; /* MSDN docs for gethostname() promise this is enough */
    if (p_gethostname(hostbuf, sizeof(hostbuf)) < 0)
        return NULL;
    return dupstr(hostbuf);
}

SockAddr *platform_get_x11_unix_address(const char *display, int displaynum)
{
    SockAddr *addr = snew(SockAddr);
    memset(addr, 0, sizeof(SockAddr));
    addr->error = "unix sockets for X11 not supported on this platform";
    addr->refcount = 1;
    return addr;
}
