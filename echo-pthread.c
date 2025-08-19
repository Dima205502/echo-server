#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>

#define PORT 33333
#define BACKLOG 128

static void *client_thread(void *arg) {
    int client_fd = *(int *)arg;
    free(arg);

    char buf[4096];
    for (;;) {
        ssize_t r = read(client_fd, buf, sizeof(buf));
        if (r > 0) {
            ssize_t sent = 0;
            while (sent < r) {
                ssize_t w = write(client_fd, buf + sent, r - sent);
                if (w >= 0) { sent += w; }
                else {
                    if (errno == EINTR) continue;
                    perror("write");
                    close(client_fd);
                    return NULL;
                }
            }
        } else if (r == 0) {
            close(client_fd);
            return NULL;
        } else {
            if (errno == EINTR) continue;
            perror("read");
            close(client_fd);
            return NULL;
        }
    }
    return NULL;
}

int main(void) {
    int listenfd = socket(AF_INET, SOCK_STREAM, 0);
    if (listenfd < 0) { perror("socket"); return 1; }

    int one = 1;
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(PORT);

    if (bind(listenfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) { perror("bind"); close(listenfd); return 1; }
    if (listen(listenfd, BACKLOG) < 0) { perror("listen"); close(listenfd); return 1; }

    printf("pthread echo server listening on %d\n", PORT);

    for (;;) {
        int clientfd = accept(listenfd, NULL, NULL);
        if (clientfd < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            break;
        }
        int *pfd = malloc(sizeof(int));
        if (!pfd) { close(clientfd); continue; }
        *pfd = clientfd;
        pthread_t tid;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        if (pthread_create(&tid, &attr, client_thread, pfd) != 0) {
            perror("pthread_create");
            close(clientfd); free(pfd);
        }
        pthread_attr_destroy(&attr);
    }

    close(listenfd);
    return 0;
}
