#include "first.h"

#include "network.h"
#include "fdevent.h"
#include "log.h"
#include "connections.h"
#include "plugin.h"
#include "joblist.h"
#include "configfile.h"
#include "inet_ntop_cache.h"

#include "network_backends.h"
#include "sys-mmap.h"
#include "sys-socket.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>

#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>

void
network_accept_tcp_nagle_disable (const int fd)
{
    static int noinherit_tcpnodelay = -1;
    int opt;

    if (!noinherit_tcpnodelay) /* TCP_NODELAY inherited from listen socket */
        return;

    if (noinherit_tcpnodelay < 0) {
        socklen_t optlen = sizeof(opt);
        if (0 == getsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, &optlen)) {
            noinherit_tcpnodelay = !opt;
            if (opt)           /* TCP_NODELAY inherited from listen socket */
                return;
        }
    }

    opt = 1;
    (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
}

static handler_t network_server_handle_fdevent(server *srv, void *context, int revents) {
	server_socket *srv_socket = (server_socket *)context;
	connection *con;
	int loops = 0;

	UNUSED(context);

	if (0 == (revents & FDEVENT_IN)) {
		log_error_write(srv, __FILE__, __LINE__, "sdd",
				"strange event for server socket",
				srv_socket->fd,
				revents);
		return HANDLER_ERROR;
	}

	/* accept()s at most 100 connections directly
	 *
	 * we jump out after 100 to give the waiting connections a chance */
	for (loops = 0; loops < 100 && NULL != (con = connection_accept(srv, srv_socket)); loops++) {
		connection_state_machine(srv, con);
	}
	return HANDLER_GO_ON;
}

static int network_server_init(server *srv, buffer *host_token, size_t sidx) {
	int val;
	socklen_t addr_len;
	server_socket *srv_socket;
	unsigned int port = 0;
	const char *host;
	specific_config *s = srv->config_storage[sidx];
	buffer *b;
	int err;

#ifdef __WIN32
	WORD wVersionRequested;
	WSADATA wsaData;

	wVersionRequested = MAKEWORD( 2, 2 );

	err = WSAStartup( wVersionRequested, &wsaData );
	if ( err != 0 ) {
		    /* Tell the user that we could not find a usable */
		    /* WinSock DLL.                                  */
		    return -1;
	}
#endif
	err = -1;

	srv_socket = calloc(1, sizeof(*srv_socket));
	force_assert(NULL != srv_socket);
	srv_socket->addr.plain.sa_family = AF_INET; /* default */
	srv_socket->fd = -1;
	srv_socket->fde_ndx = -1;
	srv_socket->sidx = sidx;

	srv_socket->srv_token = buffer_init();
	buffer_copy_buffer(srv_socket->srv_token, host_token);

	b = buffer_init();
	buffer_copy_buffer(b, host_token); /*(allocates b->ptr even if host_token is NULL)*/

	host = b->ptr;

	if (host[0] == '/') {
		/* host is a unix-domain-socket */
#ifdef HAVE_SYS_UN_H
		srv_socket->addr.plain.sa_family = AF_UNIX;
#else
		log_error_write(srv, __FILE__, __LINE__, "s",
				"ERROR: Unix Domain sockets are not supported.");
		goto error_free_socket;
#endif
	} else {
		/* ipv4:port
		 * [ipv6]:port
		 */
		size_t len = buffer_string_length(b);
		char *sp = NULL;
		if (0 == len) {
			log_error_write(srv, __FILE__, __LINE__, "s", "value of $SERVER[\"socket\"] must not be empty");
			goto error_free_socket;
		}
		if ((b->ptr[0] == '[' && b->ptr[len-1] == ']') || NULL == (sp = strrchr(b->ptr, ':'))) {
			/* use server.port if set in config, or else default from config_set_defaults() */
			port = srv->srvconf.port;
			sp = b->ptr + len; /* point to '\0' at end of string so end of IPv6 address can be found below */
		} else {
			/* found ip:port separator at *sp; port doesn't end in ']', so *sp hopefully doesn't split an IPv6 address */
			*sp = '\0';
			port = strtol(sp+1, NULL, 10);
		}

		/* check for [ and ] */
		if (b->ptr[0] == '[' && *(sp-1) == ']') {
			*(sp-1) = '\0';
			host++;

			s->use_ipv6 = 1;
		}

		if (port == 0 || port > 65535) {
			log_error_write(srv, __FILE__, __LINE__, "sd", "port not set or out of range:", port);

			goto error_free_socket;
		}
	}

#ifdef HAVE_IPV6
	if (s->use_ipv6) {
		srv_socket->addr.plain.sa_family = AF_INET6;
	}
#endif

	if (*host == '\0') {
		if (srv_socket->addr.plain.sa_family == AF_INET6) {
			log_error_write(srv, __FILE__, __LINE__, "s", "warning: please use server.use-ipv6 only for hostnames, not without server.bind / empty address; your config will break if the kernel default for IPV6_V6ONLY changes");
			host = "::";
		} else if (srv_socket->addr.plain.sa_family == AF_INET) {
			host = "0.0.0.0";
		}
	}

	if (1 != sock_addr_from_str_hints(srv, &srv_socket->addr, &addr_len, host, srv_socket->addr.plain.sa_family, port)) {
		goto error_free_socket;
	}

	if (srv->srvconf.preflight_check) {
		err = 0;
		goto error_free_socket;
	}

	if (srv->sockets_disabled) { /* lighttpd -1 (one-shot mode) */
		goto srv_sockets_append;
	}

#ifdef HAVE_SYS_UN_H
	if (AF_UNIX == srv_socket->addr.plain.sa_family) {
		/* check if the socket exists and try to connect to it. */
		force_assert(host); /*(static analysis hint)*/
		if (-1 == (srv_socket->fd = fdevent_socket_cloexec(srv_socket->addr.plain.sa_family, SOCK_STREAM, 0))) {
			log_error_write(srv, __FILE__, __LINE__, "ss", "socket failed:", strerror(errno));
			goto error_free_socket;
		}
		if (0 == connect(srv_socket->fd, (struct sockaddr *) &(srv_socket->addr), addr_len)) {
			log_error_write(srv, __FILE__, __LINE__, "ss",
				"server socket is still in use:",
				host);


			goto error_free_socket;
		}

		/* connect failed */
		switch(errno) {
		case ECONNREFUSED:
			unlink(host);
			break;
		case ENOENT:
			break;
		default:
			log_error_write(srv, __FILE__, __LINE__, "sds",
				"testing socket failed:",
				host, strerror(errno));

			goto error_free_socket;
		}

		fdevent_fcntl_set_nb(srv->ev, srv_socket->fd);
	} else
#endif
	{
		if (-1 == (srv_socket->fd = fdevent_socket_nb_cloexec(srv_socket->addr.plain.sa_family, SOCK_STREAM, IPPROTO_TCP))) {
			log_error_write(srv, __FILE__, __LINE__, "ss", "socket failed:", strerror(errno));
			goto error_free_socket;
		}

#ifdef HAVE_IPV6
		if (AF_INET6 == srv_socket->addr.plain.sa_family
		    && host != NULL) {
			if (s->set_v6only) {
				val = 1;
				if (-1 == setsockopt(srv_socket->fd, IPPROTO_IPV6, IPV6_V6ONLY, &val, sizeof(val))) {
					log_error_write(srv, __FILE__, __LINE__, "ss", "socketsockopt(IPV6_V6ONLY) failed:", strerror(errno));
					goto error_free_socket;
				}
			} else {
				log_error_write(srv, __FILE__, __LINE__, "s", "warning: server.set-v6only will be removed soon, update your config to have different sockets for ipv4 and ipv6");
			}
		}
#endif
	}

	/* */
	srv->cur_fds = srv_socket->fd;

	val = 1;
	if (setsockopt(srv_socket->fd, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val)) < 0) {
		log_error_write(srv, __FILE__, __LINE__, "ss", "socketsockopt(SO_REUSEADDR) failed:", strerror(errno));
		goto error_free_socket;
	}

	if (srv_socket->addr.plain.sa_family != AF_UNIX) {
		val = 1;
		if (setsockopt(srv_socket->fd, IPPROTO_TCP, TCP_NODELAY, &val, sizeof(val)) < 0) {
			log_error_write(srv, __FILE__, __LINE__, "ss", "socketsockopt(TCP_NODELAY) failed:", strerror(errno));
			goto error_free_socket;
		}
	}

	if (0 != bind(srv_socket->fd, (struct sockaddr *) &(srv_socket->addr), addr_len)) {
		switch(srv_socket->addr.plain.sa_family) {
		case AF_UNIX:
			log_error_write(srv, __FILE__, __LINE__, "sds",
					"can't bind to socket:",
					host, strerror(errno));
			break;
		default:
			log_error_write(srv, __FILE__, __LINE__, "ssds",
					"can't bind to port:",
					host, port, strerror(errno));
			break;
		}
		goto error_free_socket;
	}

	if (srv_socket->addr.plain.sa_family == AF_UNIX && !buffer_string_is_empty(s->socket_perms)) {
		mode_t m = 0;
		for (char *str = s->socket_perms->ptr; *str; ++str) {
			m <<= 3;
			m |= (*str - '0');
		}
		if (0 != m && -1 == chmod(host, m)) {
			log_error_write(srv, __FILE__, __LINE__, "sssbss", "chmod(\"", host, "\", ", s->socket_perms, "):", strerror(errno));
		}
	}

	if (-1 == listen(srv_socket->fd, s->listen_backlog)) {
		log_error_write(srv, __FILE__, __LINE__, "ss", "listen failed: ", strerror(errno));
		goto error_free_socket;
	}

	if (s->ssl_enabled) {
#ifdef TCP_DEFER_ACCEPT
	} else if (s->defer_accept) {
		int v = s->defer_accept;
		if (-1 == setsockopt(srv_socket->fd, IPPROTO_TCP, TCP_DEFER_ACCEPT, &v, sizeof(v))) {
			log_error_write(srv, __FILE__, __LINE__, "ss", "can't set TCP_DEFER_ACCEPT: ", strerror(errno));
		}
#endif
#if defined(__FreeBSD__) || defined(__NetBSD__) \
 || defined(__OpenBSD__) || defined(__DragonFly__)
	} else if (!buffer_is_empty(s->bsd_accept_filter)
		   && (buffer_is_equal_string(s->bsd_accept_filter, CONST_STR_LEN("httpready"))
			|| buffer_is_equal_string(s->bsd_accept_filter, CONST_STR_LEN("dataready")))) {
#ifdef SO_ACCEPTFILTER
		/* FreeBSD accf_http filter */
		struct accept_filter_arg afa;
		memset(&afa, 0, sizeof(afa));
		strncpy(afa.af_name, s->bsd_accept_filter->ptr, sizeof(afa.af_name));
		if (setsockopt(srv_socket->fd, SOL_SOCKET, SO_ACCEPTFILTER, &afa, sizeof(afa)) < 0) {
			if (errno != ENOENT) {
				log_error_write(srv, __FILE__, __LINE__, "SBss", "can't set accept-filter '", s->bsd_accept_filter, "':", strerror(errno));
			}
		}
#endif
#endif
	}

srv_sockets_append:
	srv_socket->is_ssl = s->ssl_enabled;

	if (srv->srv_sockets.size == 0) {
		srv->srv_sockets.size = 4;
		srv->srv_sockets.used = 0;
		srv->srv_sockets.ptr = malloc(srv->srv_sockets.size * sizeof(server_socket*));
		force_assert(NULL != srv->srv_sockets.ptr);
	} else if (srv->srv_sockets.used == srv->srv_sockets.size) {
		srv->srv_sockets.size += 4;
		srv->srv_sockets.ptr = realloc(srv->srv_sockets.ptr, srv->srv_sockets.size * sizeof(server_socket*));
		force_assert(NULL != srv->srv_sockets.ptr);
	}

	srv->srv_sockets.ptr[srv->srv_sockets.used++] = srv_socket;

	buffer_free(b);

	return 0;

error_free_socket:
	if (srv_socket->fd != -1) {
		network_unregister_sock(srv, srv_socket);
		close(srv_socket->fd);
	}
	buffer_free(srv_socket->srv_token);
	free(srv_socket);

	buffer_free(b);

	return err; /* -1 if error; 0 if srv->srvconf.preflight_check successful */
}

int network_close(server *srv) {
	size_t i;
	for (i = 0; i < srv->srv_sockets.used; i++) {
		server_socket *srv_socket = srv->srv_sockets.ptr[i];
		if (srv_socket->fd != -1) {
			network_unregister_sock(srv, srv_socket);
			close(srv_socket->fd);
		}

		buffer_free(srv_socket->srv_token);

		free(srv_socket);
	}

	free(srv->srv_sockets.ptr);
	srv->srv_sockets.ptr = NULL;
	srv->srv_sockets.used = 0;
	srv->srv_sockets.size = 0;

	return 0;
}

typedef enum {
	NETWORK_BACKEND_UNSET,
	NETWORK_BACKEND_WRITE,
	NETWORK_BACKEND_WRITEV,
	NETWORK_BACKEND_SENDFILE,
} network_backend_t;

int network_init(server *srv) {
	buffer *b;
	size_t i, j;
	network_backend_t backend;

	struct nb_map {
		network_backend_t nb;
		const char *name;
	} network_backends[] = {
		/* lowest id wins */
#if defined USE_SENDFILE
		{ NETWORK_BACKEND_SENDFILE,   "sendfile" },
#endif
#if defined USE_LINUX_SENDFILE
		{ NETWORK_BACKEND_SENDFILE,   "linux-sendfile" },
#endif
#if defined USE_FREEBSD_SENDFILE
		{ NETWORK_BACKEND_SENDFILE,   "freebsd-sendfile" },
#endif
#if defined USE_SOLARIS_SENDFILEV
		{ NETWORK_BACKEND_SENDFILE,   "solaris-sendfilev" },
#endif
#if defined USE_WRITEV
		{ NETWORK_BACKEND_WRITEV,     "writev" },
#endif
		{ NETWORK_BACKEND_WRITE,      "write" },
		{ NETWORK_BACKEND_UNSET,       NULL }
	};

	b = buffer_init();

	buffer_copy_buffer(b, srv->srvconf.bindhost);
	buffer_append_string_len(b, CONST_STR_LEN(":"));
	buffer_append_int(b, srv->srvconf.port);

	/* check if we already know this socket, and if yes, don't init it */
	for (j = 0; j < srv->srv_sockets.used; j++) {
		if (buffer_is_equal(srv->srv_sockets.ptr[j]->srv_token, b)) {
			break;
		}
	}
	if (j == srv->srv_sockets.used) {
		if (0 != network_server_init(srv, b, 0)) {
			buffer_free(b);
			return -1;
		}
	}
	buffer_free(b);

	/* get a usefull default */
	backend = network_backends[0].nb;

	/* match name against known types */
	if (!buffer_string_is_empty(srv->srvconf.network_backend)) {
		for (i = 0; network_backends[i].name; i++) {
			/**/
			if (buffer_is_equal_string(srv->srvconf.network_backend, network_backends[i].name, strlen(network_backends[i].name))) {
				backend = network_backends[i].nb;
				break;
			}
		}
		if (NULL == network_backends[i].name) {
			/* we don't know it */

			log_error_write(srv, __FILE__, __LINE__, "sb",
					"server.network-backend has a unknown value:",
					srv->srvconf.network_backend);

			return -1;
		}
	}

	switch(backend) {
	case NETWORK_BACKEND_WRITE:
		srv->network_backend_write = network_write_chunkqueue_write;
		break;
#if defined(USE_WRITEV)
	case NETWORK_BACKEND_WRITEV:
		srv->network_backend_write = network_write_chunkqueue_writev;
		break;
#endif
#if defined(USE_SENDFILE)
	case NETWORK_BACKEND_SENDFILE:
		srv->network_backend_write = network_write_chunkqueue_sendfile;
		break;
#endif
	default:
		return -1;
	}

	/* check for $SERVER["socket"] */
	for (i = 1; i < srv->config_context->used; i++) {
		data_config *dc = (data_config *)srv->config_context->data[i];

		/* not our stage */
		if (COMP_SERVER_SOCKET != dc->comp) continue;

		if (dc->cond != CONFIG_COND_EQ) continue;

		/* check if we already know this socket,
		 * if yes, don't init it */
		for (j = 0; j < srv->srv_sockets.used; j++) {
			if (buffer_is_equal(srv->srv_sockets.ptr[j]->srv_token, dc->string)) {
				break;
			}
		}

		if (j == srv->srv_sockets.used) {
			if (0 != network_server_init(srv, dc->string, i)) return -1;
		}
	}

	return 0;
}

void network_unregister_sock(server *srv, server_socket *srv_socket) {
	if (-1 == srv_socket->fd || -1 == srv_socket->fde_ndx) return;
	fdevent_event_del(srv->ev, &srv_socket->fde_ndx, srv_socket->fd);
	fdevent_unregister(srv->ev, srv_socket->fd);
}

int network_register_fdevents(server *srv) {
	size_t i;

	if (-1 == fdevent_reset(srv->ev)) {
		return -1;
	}

	if (srv->sockets_disabled) return 0; /* lighttpd -1 (one-shot mode) */

	/* register fdevents after reset */
	for (i = 0; i < srv->srv_sockets.used; i++) {
		server_socket *srv_socket = srv->srv_sockets.ptr[i];

		fdevent_register(srv->ev, srv_socket->fd, network_server_handle_fdevent, srv_socket);
		fdevent_event_set(srv->ev, &(srv_socket->fde_ndx), srv_socket->fd, FDEVENT_IN);
	}
	return 0;
}
