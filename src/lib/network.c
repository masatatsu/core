/* Copyright (c) 1999-2005 Timo Sirainen */

#include "lib.h"
#include "fd-set-nonblock.h"
#include "network.h"

#include <unistd.h>
#include <fcntl.h>
#include <ctype.h>
#include <sys/un.h>
#include <netinet/tcp.h>

union sockaddr_union {
	struct sockaddr sa;
	struct sockaddr_in sin;
#ifdef HAVE_IPV6
	struct sockaddr_in6 sin6;
#endif
};

#ifdef HAVE_IPV6
#  define SIZEOF_SOCKADDR(so) ((so).sa.sa_family == AF_INET6 ? \
	sizeof(so.sin6) : sizeof(so.sin))
#else
#  define SIZEOF_SOCKADDR(so) (sizeof(so.sin))
#endif

int net_ip_compare(const struct ip_addr *ip1, const struct ip_addr *ip2)
{
	if (ip1->family != ip2->family)
		return 0;

#ifdef HAVE_IPV6
	if (ip1->family == AF_INET6)
		return memcmp(&ip1->ip, &ip2->ip, sizeof(ip1->ip)) == 0;
#endif

	return memcmp(&ip1->ip, &ip2->ip, 4) == 0;
}


/* copy IP to sockaddr */
static inline void
sin_set_ip(union sockaddr_union *so, const struct ip_addr *ip)
{
	if (ip == NULL) {
#ifdef HAVE_IPV6
		so->sin6.sin6_family = AF_INET6;
		so->sin6.sin6_addr = in6addr_any;
#else
		so->sin.sin_family = AF_INET;
		so->sin.sin_addr.s_addr = INADDR_ANY;
#endif
		return;
	}

	so->sin.sin_family = ip->family;
#ifdef HAVE_IPV6
	if (ip->family == AF_INET6)
		memcpy(&so->sin6.sin6_addr, &ip->ip, sizeof(ip->ip));
	else
#endif
		memcpy(&so->sin.sin_addr, &ip->ip, 4);
}

static inline void
sin_get_ip(const union sockaddr_union *so, struct ip_addr *ip)
{
	ip->family = so->sin.sin_family;

#ifdef HAVE_IPV6
	if (ip->family == AF_INET6)
		memcpy(&ip->ip, &so->sin6.sin6_addr, sizeof(ip->ip));
	else
#endif
	if (ip->family == AF_INET)
		memcpy(&ip->ip, &so->sin.sin_addr, 4);
	else
		memset(&ip->ip, 0, sizeof(ip->ip));
}

static inline void sin_set_port(union sockaddr_union *so, unsigned int port)
{
#ifdef HAVE_IPV6
	if (so->sin.sin_family == AF_INET6)
                so->sin6.sin6_port = htons((unsigned short) port);
	else
#endif
		so->sin.sin_port = htons((unsigned short) port);
}

static inline unsigned int sin_get_port(union sockaddr_union *so)
{
#ifdef HAVE_IPV6
	if (so->sin.sin_family == AF_INET6)
		return ntohs(so->sin6.sin6_port);
#endif
	if (so->sin.sin_family == AF_INET)
		return ntohs(so->sin.sin_port);

	return 0;
}

static inline void close_save_errno(int fd)
{
	int old_errno = errno;
	(void)close(fd);
	errno = old_errno;
}

/* Connect to socket with ip address */
int net_connect_ip(const struct ip_addr *ip, unsigned int port,
		   const struct ip_addr *my_ip)
{
	union sockaddr_union so;
	int fd, ret, opt = 1;

	if (my_ip != NULL && ip->family != my_ip->family) {
		i_warning("net_connect_ip(): ip->family != my_ip->family");
                my_ip = NULL;
	}

	/* create the socket */
	memset(&so, 0, sizeof(so));
        so.sin.sin_family = ip->family;
	fd = socket(ip->family, SOCK_STREAM, 0);

	if (fd == -1) {
		i_error("socket() failed: %m");
		return -1;
	}

	/* set socket options */
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
	setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &opt, sizeof(opt));
	net_set_nonblock(fd, TRUE);

	/* set our own address */
	if (my_ip != NULL) {
		sin_set_ip(&so, my_ip);
		if (bind(fd, &so.sa, SIZEOF_SOCKADDR(so)) == -1) {
			/* failed, set it back to INADDR_ANY */
			i_error("bind(%s) failed: %m", net_ip2addr(my_ip));
			close_save_errno(fd);
			return -1;
		}
	}

	/* connect */
	sin_set_ip(&so, ip);
	sin_set_port(&so, port);
	ret = connect(fd, &so.sa, SIZEOF_SOCKADDR(so));

#ifndef WIN32
	if (ret < 0 && errno != EINPROGRESS)
#else
	if (ret < 0 && WSAGetLastError() != WSAEWOULDBLOCK)
#endif
	{
                close_save_errno(fd);
		return -1;
	}

	return fd;
}

int net_connect_unix(const char *path)
{
	struct sockaddr_un sa;
	int fd, ret;

	memset(&sa, 0, sizeof(sa));
	sa.sun_family = AF_UNIX;
	if (strocpy(sa.sun_path, path, sizeof(sa.sun_path)) < 0) {
		/* too long path */
		errno = EINVAL;
		return -1;
	}

	/* create the socket */
	fd = socket(PF_UNIX, SOCK_STREAM, 0);
	if (fd == -1) {
		i_error("socket(%s) failed: %m", path);
		return -1;
	}

	net_set_nonblock(fd, TRUE);

	/* connect */
	ret = connect(fd, (struct sockaddr *) &sa, sizeof(sa));
	if (ret < 0 && errno != EINPROGRESS) {
                close_save_errno(fd);
		return -1;
	}

	return fd;
}

/* Disconnect socket */
void net_disconnect(int fd)
{
	if (close(fd) < 0)
		i_error("net_disconnect() failed: %m");
}

/* Set socket blocking/nonblocking */
void net_set_nonblock(int fd, int nonblock)
{
	if (fd_set_nonblock(fd, nonblock) < 0)
		i_fatal("fd_set_nonblock(%d) failed: %m", fd);
}

int net_set_cork(int fd __attr_unused__, int cork __attr_unused__)
{
#ifdef TCP_CORK
	return setsockopt(fd, IPPROTO_TCP, TCP_CORK, &cork, sizeof(cork));
#else
	errno = ENOPROTOOPT;
	return -1;
#endif
}

void net_get_ip_any4(struct ip_addr *ip)
{
	struct in_addr *in_ip = (struct in_addr *) &ip->ip;

	ip->family = AF_INET;
	in_ip->s_addr = INADDR_ANY;
}

void net_get_ip_any6(struct ip_addr *ip)
{
#ifdef HAVE_IPV6
	ip->family = AF_INET6;
	ip->ip = in6addr_any;
#else
	memset(ip, 0, sizeof(struct ip_addr));
#endif
}

/* Listen for connections on a socket. if `my_ip' is NULL, listen in any
   address. */
int net_listen(const struct ip_addr *my_ip, unsigned int *port, int backlog)
{
	union sockaddr_union so;
	int ret, fd, opt = 1;
	socklen_t len;

	i_assert(port != NULL);

	memset(&so, 0, sizeof(so));
	sin_set_port(&so, *port);
	sin_set_ip(&so, my_ip);

	/* create the socket */
	fd = socket(so.sin.sin_family, SOCK_STREAM, 0);
#ifdef HAVE_IPV6
	if (fd == -1 && (errno == EINVAL || errno == EAFNOSUPPORT)) {
		/* IPv6 is not supported by OS */
		so.sin.sin_family = AF_INET;
		so.sin.sin_addr.s_addr = INADDR_ANY;

		fd = socket(AF_INET, SOCK_STREAM, 0);
	}
#endif
	if (fd == -1) {
		i_error("socket() failed: %m");
		return -1;
	}

	/* set socket options */
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
	setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &opt, sizeof(opt));

	/* if using IPv6, bind both on the IPv4 and IPv6 addresses */
#ifdef IPV6_V6ONLY
	if (so.sin.sin_family == AF_INET6) {
		opt = 0;
		setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &opt, sizeof(opt));
	}
#endif
	/* specify the address/port we want to listen in */
	ret = bind(fd, &so.sa, SIZEOF_SOCKADDR(so));
	if (ret < 0) {
		if (errno != EADDRINUSE) {
			i_error("bind(%s, %u) failed: %m",
				net_ip2addr(my_ip), *port);
		}
	} else {
		/* get the actual port we started listen */
		len = SIZEOF_SOCKADDR(so);
		ret = getsockname(fd, &so.sa, &len);
		if (ret >= 0) {
			*port = sin_get_port(&so);

			/* start listening */
			if (listen(fd, backlog) >= 0)
				return fd;

			if (errno != EADDRINUSE)
				i_error("listen() failed: %m");
		}
	}

        /* error */
	close_save_errno(fd);
	return -1;
}

int net_listen_unix(const char *path, int backlog)
{
	struct sockaddr_un sa;
	int fd;

	memset(&sa, 0, sizeof(sa));
	sa.sun_family = AF_UNIX;
	if (strocpy(sa.sun_path, path, sizeof(sa.sun_path)) < 0) {
		/* too long path */
		errno = EINVAL;
		return -1;
	}

	/* create the socket */
	fd = socket(PF_UNIX, SOCK_STREAM, 0);
	if (fd == -1) {
		i_error("socket() failed: %m");
		return -1;
	}

	/* bind */
	if (bind(fd, (struct sockaddr *) &sa, sizeof(sa)) < 0) {
		if (errno != EADDRINUSE)
			i_error("bind(%s) failed: %m", path);
	} else {
		/* start listening */
		if (listen(fd, backlog) == 0)
			return fd;

		if (errno != EADDRINUSE)
			i_error("listen() failed: %m");
	}

	close_save_errno(fd);
	return -1;
}

/* Accept a connection on a socket */
int net_accept(int fd, struct ip_addr *addr, unsigned int *port)
{
	union sockaddr_union so;
	int ret;
	socklen_t addrlen;

	i_assert(fd >= 0);

	addrlen = sizeof(so);
	ret = accept(fd, &so.sa, &addrlen);

	if (ret < 0) {
		if (errno == EBADF || errno == ENOTSOCK ||
		    errno == EOPNOTSUPP || errno == EFAULT || errno == EINVAL)
			return -2;
		else
			return -1;
	}

	if (addr != NULL) sin_get_ip(&so, addr);
	if (port != NULL) *port = sin_get_port(&so);

	return ret;
}

/* Read data from socket, return number of bytes read, -1 = error */
ssize_t net_receive(int fd, void *buf, size_t len)
{
	ssize_t ret;

	i_assert(fd >= 0);
	i_assert(buf != NULL);
	i_assert(len <= SSIZE_T_MAX);

	ret = read(fd, buf, len);
	if (ret == 0) {
		/* disconnected */
		errno = 0;
		return -2;
	}

	if (ret < 0) {
		if (errno == EINTR || errno == EAGAIN)
			return 0;

		if (errno == ECONNRESET || errno == ETIMEDOUT) {
                        /* treat as disconnection */
			return -2;
		}
	}

	return ret;
}

/* Transmit data, return number of bytes sent, -1 = error */
ssize_t net_transmit(int fd, const void *data, size_t len)
{
        ssize_t ret;

	i_assert(fd >= 0);
	i_assert(data != NULL);
	i_assert(len <= SSIZE_T_MAX);

	ret = send(fd, data, len, 0);
	if (ret == -1 && (errno == EINTR || errno == EAGAIN))
		return 0;

	if (errno == EPIPE)
		return -2;

        return ret;
}

/* Get IP addresses for host. ips contains ips_count of IPs, they don't need
   to be free'd. Returns 0 = ok, others = error code for net_gethosterror() */
int net_gethostbyname(const char *addr, struct ip_addr **ips, int *ips_count)
{
	/* @UNSAFE */
#ifdef HAVE_IPV6
	union sockaddr_union *so;
	struct addrinfo hints, *ai, *origai;
	char hbuf[NI_MAXHOST];
	int host_error;
#else
	struct hostent *hp;
#endif
        int count;

	i_assert(addr != NULL);
	i_assert(ips != NULL);
	i_assert(ips_count != NULL);

	*ips = NULL;
        *ips_count = 0;

#ifdef HAVE_IPV6
	memset(&hints, 0, sizeof(struct addrinfo));
	hints.ai_socktype = SOCK_STREAM;

	/* save error to host_error for later use */
	host_error = getaddrinfo(addr, NULL, &hints, &ai);
	if (host_error != 0)
		return host_error;

	if (getnameinfo(ai->ai_addr, ai->ai_addrlen, hbuf,
			sizeof(hbuf), NULL, 0, NI_NUMERICHOST) != 0)
		return 1;


        /* get number of IPs */
        origai = ai;
	for (count = 0; ai != NULL; ai = ai->ai_next)
                count++;

        *ips_count = count;
        *ips = t_malloc(sizeof(struct ip_addr) * count);

        count = 0;
	for (ai = origai; ai != NULL; ai = ai->ai_next, count++) {
		so = (union sockaddr_union *) ai->ai_addr;

		sin_get_ip(so, &(*ips)[count]);
	}
	freeaddrinfo(origai);
#else
	hp = gethostbyname(addr);
	if (hp == NULL)
		return h_errno;

        /* get number of IPs */
	count = 0;
	while (hp->h_addr_list[count] != NULL)
		count++;

        *ips_count = count;
        *ips = t_malloc(sizeof(struct ip_addr) * count);

	while (count > 0) {
		count--;

		(*ips)[count].family = AF_INET;
                memcpy(&(*ips)[count].ip, hp->h_addr_list[count], 4);
	}
#endif

	return 0;
}

/* Get socket address/port */
int net_getsockname(int fd, struct ip_addr *addr, unsigned int *port)
{
	union sockaddr_union so;
	socklen_t addrlen;

	i_assert(fd >= 0);

	addrlen = sizeof(so);
	if (getsockname(fd, (struct sockaddr *) &so, &addrlen) == -1)
		return -1;

        if (addr != NULL) sin_get_ip(&so, addr);
	if (port != NULL) *port = sin_get_port(&so);

	return 0;
}

int net_getpeername(int fd, struct ip_addr *addr, unsigned int *port)
{
	union sockaddr_union so;
	socklen_t addrlen;

	i_assert(fd >= 0);

	addrlen = sizeof(so);
	if (getpeername(fd, (struct sockaddr *) &so, &addrlen) == -1)
		return -1;

        if (addr != NULL) sin_get_ip(&so, addr);
	if (port != NULL) *port = sin_get_port(&so);

	return 0;
}

const char *net_ip2addr(const struct ip_addr *ip)
{
#ifdef HAVE_IPV6
	char addr[MAX_IP_LEN+1];

	addr[MAX_IP_LEN] = '\0';
	if (inet_ntop(ip->family, &ip->ip, addr, MAX_IP_LEN) == NULL)
		return NULL;

	return t_strdup(addr);
#else
	unsigned long ip4;

	if (ip->family != AF_INET)
		return NULL;

	ip4 = ntohl(ip->ip.s_addr);
	return t_strdup_printf("%lu.%lu.%lu.%lu",
			       (ip4 & 0xff000000UL) >> 24,
			       (ip4 & 0x00ff0000) >> 16,
			       (ip4 & 0x0000ff00) >> 8,
			       (ip4 & 0x000000ff));
#endif
}

int net_addr2ip(const char *addr, struct ip_addr *ip)
{
	if (strchr(addr, ':') != NULL) {
		/* IPv6 */
		ip->family = AF_INET6;
#ifdef HAVE_IPV6
		if (inet_pton(AF_INET6, addr, &ip->ip) == 0)
			return -1;
#else
		ip->ip.s_addr = 0;
#endif
 	} else {
		/* IPv4 */
		ip->family = AF_INET;
		if (inet_aton(addr, (struct in_addr *) &ip->ip) == 0)
			return -1;
	}

	return 0;
}

/* Get socket error */
int net_geterror(int fd)
{
	int data;
	socklen_t len = sizeof(data);

	if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &data, &len) == -1)
		return -1;

	return data;
}

/* get error of net_gethostname() */
const char *net_gethosterror(int error)
{
#ifdef HAVE_IPV6
	i_assert(error != 0);

	if (error == 1) {
		/* getnameinfo() failed */
		return strerror(errno);
	}

	return gai_strerror(error);
#else
	switch (error) {
	case HOST_NOT_FOUND:
		return "Host not found";
	case NO_ADDRESS:
		return "No IP address found for name";
	case NO_RECOVERY:
		return "A non-recovable name server error occurred";
	case TRY_AGAIN:
		return "A temporary error on an authoritative name server";
	}

	/* unknown error */
	return NULL;
#endif
}

/* return TRUE if host lookup failed because it didn't exist (ie. not
   some error with name server) */
int net_hosterror_notfound(int error)
{
#ifdef HAVE_IPV6
#ifdef EAI_NODATA /* NODATA is depricated */
	return error != 1 && (error == EAI_NONAME || error == EAI_NODATA);
#else
	return error != 1 && (error == EAI_NONAME);
#endif
#else
	return error == HOST_NOT_FOUND || error == NO_ADDRESS;
#endif
}

/* Get name of TCP service */
const char *net_getservbyport(unsigned short port)
{
	struct servent *entry;

	entry = getservbyport(htons(port), "tcp");
	return entry == NULL ? NULL : entry->s_name;
}

int is_ipv4_address(const char *addr)
{
	while (*addr != '\0') {
		if (*addr != '.' && !i_isdigit(*addr))
			return 0;
                addr++;
	}

	return 1;
}

int is_ipv6_address(const char *addr)
{
	while (*addr != '\0') {
		if (*addr != ':' && !i_isxdigit(*addr))
			return 0;
                addr++;
	}

	return 1;
}
