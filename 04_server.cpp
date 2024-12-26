#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/ip.h>
#include <cassert>

const size_t k_max_msg = 4096;

static void msg(const char *msg) {
    fprintf(stderr, "%s\n", msg);
}

static void die(const char *msg) {
    int err = errno;
    fprintf(stderr, "[%d] %s\n", err, msg);
    abort();
}



static int32_t read_full(int fd, char *buf, size_t n) {
    // loop through all packets
    while (n > 0) {
        ssize_t rv = read(fd, buf, n);
        if (rv <= 0) {
            return -1; // error, or unexpected EOF
        }
        assert((size_t)rv <= n);
        n -= (size_t)rv;
        buf += rv;
    }
    return 0;
}

static int32_t write_all(int fd, const char *buf, size_t n) {
    while (n > 0) {
        ssize_t rv = write(fd, buf, n);
        if (rv <= 0) {
            return -1; // error
        }
        assert((size_t)rv <= n);
        n -= (size_t)rv;
        buf += rv;
    }
    return 0;
}

static int32_t one_request(int connfd) {
    // 4 byte header
    char rbuf[4 + k_max_msg + 1];
    errno = 0;
    int32_t err = read_full(connfd, rbuf, 4);
    if (err) {
        if (errno == 0) {
            msg("EOF");
        } else {
            msg("read() error");
        }
        return err;
    }

    uint32_t len = 0;
    memcpy(&len, rbuf, 4); // assuming little endian
    if (len > k_max_msg) {
        msg("too long");
        return -1;
    }

    // request body
    err = read_full(connfd, &rbuf[4], len);
    if (err) {
        msg("read() error");
        return err;
    }

    // do something
    rbuf[4 + len] = '\0';
    printf("client says: %s\n", &rbuf[4]);

    // reply using same protocol
    const char reply[] = "world";
    char wbuf[4 + sizeof(reply)];
    len = (uint32_t)strlen(reply);
    // write the size 
    memcpy(wbuf, &len, 4);
    // write the message
    memcpy(&wbuf[4], reply, len);

    return write_all(connfd, wbuf, 4 + len);
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

        while (true) {
            int32_t err = one_request(connfd);
            if (err) {
                break;
            }
            // do_something(connfd);
        }
        close(connfd);
    }

    return 0;
}