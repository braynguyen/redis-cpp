#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/ip.h>


static void msg(const char *msg) {
    fprintf(stderr, "%s\n", msg);
}

static void die(const char *msg) {
    int err = errno;
    fprintf(stderr, "[%d] %s\n", err, msg);
    abort();
}

// 1 read and 1 write
static void do_something(int connfd) {
    char rbuf[64] = {};
    ssize_t n = read(connfd, rbuf, sizeof(rbuf) - 1);
    if (n < 0) {
        msg("read() error");
        return;
    }

    printf("client says: %s\n", rbuf);

    char wbuf[] = "world";
    // return value is number of bytes writte
    write(connfd, wbuf, strlen(wbuf));
}

int main() {
    // OBTAIN SOCKET HANDLE
    // AF_INET = IPv4 or AF_INET6 = IPv6
    // SOCK_STREAM is for TCP and SOCK_DGRAM for UDP
    // 0 is useless for this
    int fd = socket(AF_INET, SOCK_STREAM, 0);


    // CONFIGURE SOCKET
    // SOL_SOCKET = 
    // SO_REUSEADDR is set to 1 for all listening sockets (necessary for bind)
    int val = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val));


    // BIND TO AN ADDRESS
    // holds IPv4 address and port
    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = ntohs(1234);
    // 0 = wildcard address 0.0.0.0
    // INADDR_LOOPBACK = 127.0.0.1
    // ntohl just formats it little endian
    addr.sin_addr.s_addr = ntohl(0);
    int rv = bind(fd, (const sockaddr *)&addr, sizeof(addr));
    if (rv) {
        die("bind");
    }

    // LISTEN
    // backlog argument is the size of the queue: SOMAXCONN = 128
    rv = listen(fd, SOMAXCONN);
    if (rv) {
        die("listen()");
    }

    while (true) {
        // accept
        struct sockaddr_in client_addr = {};
        socklen_t addrlen = sizeof(client_addr);
        // accept returns the peer's address
        int connfd = accept(fd, (struct sockaddr *)&client_addr, &addrlen);

        if (connfd < 0){
            continue; // error
        }

        do_something(connfd);
        close(connfd);
    }

    return 0;
}