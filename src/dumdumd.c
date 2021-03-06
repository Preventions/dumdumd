/*
 * dumdumd - packets sent lightning fast to dev null
 * Copyright (c) 2017, OARC, Inc.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published
 * by the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "config.h"

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>

#undef ANYBACKEND

#if defined(HAVE_LIBEV) && defined(HAVE_EV_H)
#undef ANYBACKEND
#define ANYBACKEND 1
#include <ev.h>
#else
#undef HAVE_LIBEV
#endif

#if defined(HAVE_LIBUV) && defined(HAVE_UV_H)
#undef ANYBACKEND
#define ANYBACKEND 1
#include <uv.h>
#else
#undef HAVE_LIBUV
#endif

#ifndef ANYBACKEND
#error "No event library backend found, need at least libev or libuv"
#endif

char* program_name = 0;
#define STATS_INIT { 0, 0, 0, 0, 0 }
struct stats {
    size_t accept;
    size_t accdrop;
    size_t conns;
    size_t bytes;
    size_t pkts;
};
struct stats _stats0 = STATS_INIT;
struct stats _stats = STATS_INIT;

static void usage(void) {
    printf(
        "usage: %s [options] [ip] <port>\n"
        /* -o            description                                                 .*/
        "  -B ackend     Select backend: ev, uv (default)\n"
        "  -u            Use UDP\n"
        "  -t            Use TCP\n"
        "                Using both UDP and TCP if none of the above options are used\n"
        "  -A            Use SO_REUSEADDR on sockets\n"
        "  -R            Use SO_REUSEPORT on sockets\n"
        "  -L <sec>      Use SO_LINGER with the given seconds\n"
        "  -h            Print this help and exit\n"
        "  -V            Print version and exit\n",
        program_name
    );
}

static void version(void) {
    printf("%s version " PACKAGE_VERSION "\n", program_name);
}

static inline void stats_cb(void) {
    printf("accept(drop): %lu ( %lu ) conns: %lu pkts: %lu bytes %lu\n",
        _stats.accept,
        _stats.accdrop,
        _stats.conns,
        _stats.pkts,
        _stats.bytes
    );
    _stats = _stats0;
}

static char recvbuf[4*1024*1024];

#ifdef HAVE_LIBEV
static void _ev_stats_cb(struct ev_loop *loop, ev_timer *w, int revents) {
    stats_cb();
}

static void _ev_shutdown_cb(struct ev_loop *loop, ev_io *w, int revents) {
    int fd = w->data - (void*)0;

    if (recv(fd, recvbuf, sizeof(recvbuf), 0) > 0)
        return;

    ev_io_stop(loop, w);
    close(fd);
    free(w); /* TODO: Delayed free maybe? */
}

static void _ev_recv_cb(struct ev_loop *loop, ev_io *w, int revents) {
    int fd = w->data - (void*)0;
    ssize_t bytes;

    for (;;) {
        bytes = recv(fd, recvbuf, sizeof(recvbuf), 0);
        if (bytes < 1)
            break;
        _stats.pkts++;
        _stats.bytes += bytes;
        if (bytes < sizeof(recvbuf)) {
            return;
        }
    }

    ev_io_stop(loop, w);
    shutdown(fd, SHUT_RDWR);
    ev_io_init(w, _ev_shutdown_cb, fd, EV_READ);
    ev_io_start(loop, w);
}

static void _ev_accept_cb(struct ev_loop *loop, ev_io *w, int revents) {
    int fd = w->data - (void*)0, newfd, flags;
    struct sockaddr addr;
    socklen_t len;

    for (;;) {
        memset(&addr, 0, sizeof(struct sockaddr));
        len = sizeof(struct sockaddr);
        newfd = accept(fd, &addr, &len);
        _stats.accept++;
        if (newfd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return;
            fprintf(stderr, "accept(%d) ", fd);
            perror("");
            ev_io_stop(loop, w);
            shutdown(fd, SHUT_RDWR);
            close(fd);
            free(w); /* TODO: Delayed free maybe? */
            _stats.accdrop++;
            return;
        }

        if ((flags = fcntl(newfd, F_GETFL)) == -1
            || fcntl(newfd, F_SETFL, flags | O_NONBLOCK))
        {
            perror("fcntl()");
            shutdown(newfd, SHUT_RDWR);
            close(newfd);
            _stats.accdrop++;
            return;
        }

        {
            ev_io* io = calloc(1, sizeof(ev_io));
            if (!io) {
                perror("calloc()");
                shutdown(newfd, SHUT_RDWR);
                close(newfd);
                _stats.accdrop++;
                return;
            }
            io->data += newfd;
            ev_io_init(io, _ev_recv_cb, newfd, EV_READ);
            ev_io_start(loop, io);
            _stats.conns++;
        }
    }
}
#endif

#ifdef HAVE_LIBUV
static void _uv_stats_cb(uv_timer_t* w) {
    stats_cb();
}

static void _uv_alloc_cb(uv_handle_t* handle, size_t suggested_size, uv_buf_t* buf) {
    buf->base = recvbuf;
    buf->len = sizeof(recvbuf);
}

static void _uv_close_cb(uv_handle_t* handle) {
    free(handle);
}

static void _uv_udp_recv_cb(uv_udp_t* handle, ssize_t nread, const uv_buf_t* buf, const struct sockaddr* addr, unsigned flags) {
    if (nread < 0) {
        uv_udp_recv_stop(handle);
        uv_close((uv_handle_t*)handle, _uv_close_cb);
        return;
    }

    _stats.pkts++;
    _stats.bytes += nread;
}

static void _uv_tcp_recv_cb(uv_stream_t* handle, ssize_t nread, const uv_buf_t* buf) {
    if (nread < 0) {
        uv_read_stop(handle);
        uv_close((uv_handle_t*)handle, _uv_close_cb);
        return;
    }

    _stats.pkts++;
    _stats.bytes += nread;
}

static void _uv_on_connect_cb(uv_stream_t* server, int status) {
    uv_tcp_t* tcp;
    int err;

    if (status) {
        _stats.accdrop++;
        return;
    }

    tcp = calloc(1, sizeof(uv_tcp_t));
    if ((err = uv_tcp_init(uv_default_loop(), tcp))) {
        fprintf(stderr, "uv_tcp_init() %s\n", uv_strerror(err));
        free(tcp);
        _stats.accdrop++;
        return;
    }
    if ((err = uv_accept(server, (uv_stream_t*)tcp))) {
        fprintf(stderr, "uv_accept() %s\n", uv_strerror(err));
        uv_close((uv_handle_t*)tcp, _uv_close_cb);
        _stats.accdrop++;
        return;
    }
    _stats.accept++;
    if ((err = uv_read_start((uv_stream_t*)tcp, _uv_alloc_cb, _uv_tcp_recv_cb))) {
        fprintf(stderr, "uv_read_start() %s\n", uv_strerror(err));
        uv_close((uv_handle_t*)tcp, _uv_close_cb);
        return;
    }
    _stats.conns++;
}
#endif

int main(int argc, char* argv[]) {
    int opt, use_udp = 0, use_tcp = 0, reuse_addr = 0, reuse_port = 0, linger = 0;
    struct addrinfo* addrinfo = 0;
    struct addrinfo hints;
    const char* node = 0;
    const char* service = 0;
    int use_ev = 0, use_uv = 0;

#if defined(HAVE_LIBUV)
    use_uv = 1;
#elif defined(HAVE_LIBEV)
    use_ev = 1;
#endif

    if ((program_name = strrchr(argv[0], '/'))) {
        program_name++;
    }
    else {
        program_name = argv[0];
    }

    while ((opt = getopt(argc, argv, "B:utARL:hV")) != -1) {
        switch (opt) {
            case 'B':
                if (!strcmp(optarg, "ev")) {
#ifdef HAVE_LIBEV
                    use_uv = 0;
                    use_ev = 1;
#else
                    fprintf(stderr, "No libev support compiled in\n");
                    return 2;
#endif
                }
                else if(!strcmp(optarg, "uv")) {
#ifdef HAVE_LIBUV
                    use_ev = 0;
                    use_uv = 1;
#else
                    fprintf(stderr, "No libuv support compiled in\n");
                    return 2;
#endif
                }
                break;

            case 'u':
                use_udp = 1;
                break;

            case 't':
                use_tcp = 1;
                break;

            case 'A':
                reuse_addr = 1;
                break;

            case 'R':
                reuse_port = 1;
                break;

            case 'L':
                linger = atoi(optarg);
                if (linger < 1) {
                    usage();
                    return 2;
                }
                break;

            case 'h':
                usage();
                return 0;

            case 'V':
                version();
                return 0;

            default:
                usage();
                return 2;
        }
    }

    if (!use_udp && !use_tcp) {
        use_udp = 1;
        use_tcp = 1;
    }

    if (optind < argc) {
        service = argv[optind++];
    }
    else {
        usage();
        return 2;
    }
    if (optind < argc) {
        node = service;
        service = argv[optind++];
    }
    if (optind < argc) {
        usage();
        return 2;
    }

    memset(&hints, 0, sizeof(struct addrinfo));
    hints.ai_family = AF_UNSPEC;
    if (getaddrinfo(node, service, &hints, &addrinfo)) {
        perror("getaddrinfo()");
        return 1;
    }
    if (!addrinfo) {
        return 1;
    }

    {
        struct addrinfo* ai = addrinfo;
        int fd, optval, flags;

        for (; ai; ai = ai->ai_next) {
            switch (ai->ai_socktype) {
                case SOCK_DGRAM:
                case SOCK_STREAM:
                    break;
                default:
                    continue;
            }

            switch (ai->ai_protocol) {
                case IPPROTO_UDP:
                    if (!use_udp)
                        continue;
                    break;
                case IPPROTO_TCP:
                    if (!use_tcp)
                        continue;
                    break;
                default:
                    continue;
            }

            fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
            if (fd < 0) {
                perror("socket()");
                return 1;
            }

#ifdef SO_REUSEADDR
            if (reuse_addr) {
                optval = 1;
                if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval))) {
                    perror("setsockopt(SO_REUSEADDR)");
                    return 1;
                }
            }
#endif
#ifdef SO_REUSEPORT
            if (reuse_port) {
                optval = 1;
                if (setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &optval, sizeof(optval))) {
                    perror("setsockopt(SO_REUSEPORT)");
                    return 1;
                }
            }
#endif
            {
                struct linger l = { 0, 0 };
                if (linger > 0) {
                    l.l_onoff = 1;
                    l.l_linger = linger;
                }
                if (setsockopt(fd, SOL_SOCKET, SO_LINGER, &l, sizeof(l))) {
                    perror("setsockopt(SO_LINGER)");
                    return 1;
                }
            }

            if ((flags = fcntl(fd, F_GETFL)) == -1) {
                perror("fcntl(F_GETFL)");
                return 1;
            }
            if (fcntl(fd, F_SETFL, flags | O_NONBLOCK)) {
                perror("fcntl(F_SETFL)");
                return 1;
            }

#ifdef HAVE_LIBEV
            if (use_ev) {
                if (bind(fd, ai->ai_addr, ai->ai_addrlen) < 0) {
                    perror("bind()");
                    return 1;
                }
                if (ai->ai_socktype == SOCK_STREAM && listen(fd, 10)) {
                    perror("listen()");
                    return 1;
                }

                {
                    ev_io* io = calloc(1, sizeof(ev_io));
                    if (!io) {
                        perror("calloc()");
                        return 1;
                    }
                    io->data += fd;
                    ev_io_init(io, ai->ai_socktype == SOCK_STREAM ? _ev_accept_cb : _ev_recv_cb, fd, EV_READ);
                    ev_io_start(EV_DEFAULT, io);
                }
            }
            else
#endif
#ifdef HAVE_LIBUV
            if (use_uv) {
                int err;
                if (ai->ai_socktype == SOCK_DGRAM) {
                    uv_udp_t* udp = calloc(1, sizeof(uv_udp_t));

                    if ((err = uv_udp_init(uv_default_loop(), udp))) {
                        fprintf(stderr, "uv_udp_init() %s\n", uv_strerror(err));
                        return 1;
                    }
                    if ((err = uv_udp_open(udp, fd))) {
                        fprintf(stderr, "uv_udp_open() %s\n", uv_strerror(err));
                        return 1;
                    }
                    if ((err = uv_udp_bind(udp, ai->ai_addr, UV_UDP_REUSEADDR))) {
                        fprintf(stderr, "uv_udp_bind() %s\n", uv_strerror(err));
                        return 1;
                    }
                    if ((err = uv_udp_recv_start(udp, _uv_alloc_cb, _uv_udp_recv_cb))) {
                        fprintf(stderr, "uv_udp_recv_start() %s\n", uv_strerror(err));
                        return 1;
                    }
                }
                else if(ai->ai_socktype == SOCK_STREAM) {
                    uv_tcp_t* tcp = calloc(1, sizeof(uv_tcp_t));

                    if ((err = uv_tcp_init(uv_default_loop(), tcp))) {
                        fprintf(stderr, "uv_tcp_init() %s\n", uv_strerror(err));
                        return 1;
                    }
                    if ((err = uv_tcp_open(tcp, fd))) {
                        fprintf(stderr, "uv_tcp_open() %s\n", uv_strerror(err));
                        return 1;
                    }
                    if ((err = uv_tcp_bind(tcp, ai->ai_addr, UV_UDP_REUSEADDR))) {
                        fprintf(stderr, "uv_tcp_bind() %s\n", uv_strerror(err));
                        return 1;
                    }
                    if ((err = uv_listen((uv_stream_t*)tcp, 10, _uv_on_connect_cb))) {
                        fprintf(stderr, "uv_listen() %s\n", uv_strerror(err));
                        return 1;
                    }
                }
                else {
                    continue;
                }
            }
            else
#endif
            {
                return 3;
            }

            {
                char h[NI_MAXHOST], s[NI_MAXSERV];
                if (getnameinfo(ai->ai_addr, ai->ai_addrlen, h, NI_MAXHOST, s, NI_MAXSERV, NI_NUMERICHOST | NI_NUMERICSERV)) {
                    perror("getnameinfo()");
                    h[0] = 0;
                    s[0] = 0;
                }

                printf("listen: %d fam: %d type: %d proto: %d host: %s service: %s\n", fd, ai->ai_family, ai->ai_socktype, ai->ai_protocol, h, s);
            }
        }
    }

    freeaddrinfo(addrinfo);

#ifdef HAVE_LIBEV
    if (use_ev) {
        ev_timer stats;

        printf("backend: libev\n");
        ev_timer_init(&stats, _ev_stats_cb, 1.0, 1.0);
        ev_timer_start(EV_DEFAULT, &stats);

        ev_run(EV_DEFAULT, 0);
    }
    else
#endif
#ifdef HAVE_LIBUV
    if (use_uv) {
        uv_timer_t stats;

        printf("backend: libuv\n");
        uv_timer_init(uv_default_loop(), &stats);
        uv_timer_start(&stats, _uv_stats_cb, 1000, 1000);

        uv_run(uv_default_loop(), 0);
    }
    else
#endif
    {
        printf("backend: none\n");
    }

    return 0;
}
