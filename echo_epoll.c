#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

#define LISTEN_PORT 33333
#define MAX_EVENTS  64
#define BACKLOG     128

struct connection {
    int fd;
    char *outbuf;     
    size_t outlen;     
    size_t outoff;   
};

static int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int create_and_bind(int port) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return -1;
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);
    if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) < 0) { close(s); return -1; }
    if (listen(s, BACKLOG) < 0) { close(s); return -1; }
    return s;
}

static void free_conn(struct connection *c, int epollfd) {
    if (!c) return;
    epoll_ctl(epollfd, EPOLL_CTL_DEL, c->fd, NULL);
    close(c->fd);
    free(c->outbuf);
    free(c);
}

static int mod_epoll_events(int epollfd, struct connection *c, uint32_t events) {
    struct epoll_event ev;
    ev.events = events;
    ev.data.ptr = c;
    if (epoll_ctl(epollfd, EPOLL_CTL_MOD, c->fd, &ev) == -1) {
        return -1;
    }
    return 0;
}

int main(void) {
    int listenfd = create_and_bind(LISTEN_PORT);
    if (listenfd == -1) { perror("create_and_bind"); return 1; }
    if (set_nonblocking(listenfd) == -1) { perror("set_nonblocking"); close(listenfd); return 1; }

    int epollfd = epoll_create1(0);
    if (epollfd == -1) { perror("epoll_create1"); close(listenfd); return 1; }

    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.ptr = NULL;
    if (epoll_ctl(epollfd, EPOLL_CTL_ADD, listenfd, &ev) == -1) {
        perror("epoll_ctl ADD listen");
        close(listenfd); close(epollfd); return 1;
    }

    struct epoll_event events[MAX_EVENTS];

    for (;;) {
        int n = epoll_wait(epollfd, events, MAX_EVENTS, -1);
        if (n == -1) {
            if (errno == EINTR) continue;
            perror("epoll_wait");
            break;
        }
        for (int i = 0; i < n; ++i) {
            if (events[i].data.ptr == NULL) {
                while (1) {
                    struct sockaddr_in cli_addr;
                    socklen_t cli_len = sizeof(cli_addr);
                    int clientfd = accept(listenfd, (struct sockaddr *)&cli_addr, &cli_len);
                    if (clientfd == -1) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        if (errno == EINTR) continue;
                        perror("accept");
                        break;
                    }
                    if (set_nonblocking(clientfd) == -1) {
                        perror("set_nonblocking client");
                        close(clientfd);
                        continue;
                    }
                    struct connection *c = calloc(1, sizeof(*c));
                    if (!c) { perror("calloc"); close(clientfd); continue; }
                    c->fd = clientfd;
                    c->outbuf = NULL; c->outlen = 0; c->outoff = 0;
                    struct epoll_event cev;
                    cev.events = EPOLLIN | EPOLLRDHUP | EPOLLHUP | EPOLLERR;
                    cev.data.ptr = c;
                    if (epoll_ctl(epollfd, EPOLL_CTL_ADD, clientfd, &cev) == -1) {
                        perror("epoll_ctl add client");
                        free_conn(c, epollfd);
                        continue;
                    }
                    //printf("accepted fd=%d\n", clientfd);
                }
            } else {
                struct connection *c = (struct connection *)events[i].data.ptr;
                uint32_t revents = events[i].events;

                if (revents & (EPOLLHUP | EPOLLERR | EPOLLRDHUP)) {
                    free_conn(c, epollfd);
                    continue;
                }

                if (revents & EPOLLIN) {
                    char buf[4096];
                    while (1) {
                        ssize_t r = read(c->fd, buf, sizeof(buf));
                        if (r > 0) {
                            size_t to_write = (size_t)r;
                            size_t written = 0;
                            while (written < to_write) {
                                ssize_t w = write(c->fd, buf + written, to_write - written);
                                if (w >= 0) {
                                    written += (size_t)w;
                                } else {
                                    if (errno == EINTR) continue;
                                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                                        size_t remain = to_write - written;
                                        char *newbuf = realloc(c->outbuf, c->outlen + remain);
                                        if (!newbuf) { perror("realloc"); free_conn(c, epollfd); goto next_event; }
                                        c->outbuf = newbuf;
                                        memcpy(c->outbuf + c->outlen, buf + written, remain);
                                        c->outlen += remain;
                                        if (mod_epoll_events(epollfd, c, EPOLLIN | EPOLLOUT | EPOLLRDHUP | EPOLLHUP | EPOLLERR) == -1) {
                                            free_conn(c, epollfd);
                                            goto next_event;
                                        }
					break;
                                    } else {
                                        perror("write");
                                        free_conn(c, epollfd);
                                        goto next_event;
                                    }
                                }
                            }
                        } else if (r == 0) {
                            free_conn(c, epollfd);
                            goto next_event;
                        } else {
                            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                                break;
                            } else if (errno == EINTR) {
                                continue;
                            } else {
                                perror("read");
                                free_conn(c, epollfd);
                                goto next_event;
                            }
                        }
                    }
                }

                if (revents & EPOLLOUT) {
                    while (c->outoff < c->outlen) {
                        ssize_t w = write(c->fd, c->outbuf + c->outoff, c->outlen - c->outoff);
                        if (w > 0) {
                            c->outoff += (size_t)w;
                        } else if (w == -1) {
                            if (errno == EINTR) continue;
                            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                            perror("write");
                            free_conn(c, epollfd);
                            goto next_event;
                        }
                    }
                    if (c->outoff == c->outlen) {
                        free(c->outbuf);
                        c->outbuf = NULL;
                        c->outlen = c->outoff = 0;
                        if (mod_epoll_events(epollfd, c, EPOLLIN | EPOLLRDHUP | EPOLLHUP | EPOLLERR) == -1) {
                            free_conn(c, epollfd);
                            goto next_event;
                        }
                    }
                }
            }
            next_event:
            ; 
        }
    }

    close(listenfd);
    close(epollfd);
    return 0;
}

