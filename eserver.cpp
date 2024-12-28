#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/ip.h>
#include <vector>
#include <iostream>
#include <map>

static void msg(const char *msg){
    fprintf(stderr, "%s\n", msg);
}

static void die(const char *msg) {
    int err = errno;
    fprintf(stderr, "[%d] %s\n", err, msg);
    abort();
}

static void fd_set_nb(int fd) {
    errno = 0;
    int flags = fcntl(fd, F_GETFL, 0);
    
    if (errno) {
        die("fcntl error");
        return;
    }

    flags |= O_NONBLOCK;

    errno = 0;
    (void)fcntl(fd, F_SETFL, flags);
    if (errno) {
        die("fcntl error");
    }
}



const size_t k_max_msg = 4096;

enum {
    STATE_REQ = 0, // reading requests
    STATE_RES = 1, // sending responses
    STATE_END = 2, // mark the connection for deletion
};

struct Conn {
    int fd = -1;
    uint32_t state = 0; // either STATE_REQ or STATE_RES

    // reading and writing buffers needed since IO operations are often deferred in nonblocking mode
    // reading buffer
    size_t rbuf_size = 0;
    uint8_t rbuf[4 + k_max_msg];

    // writing buffer
    size_t wbuf_size = 0;
    size_t wbuf_sent = 0;
    uint8_t wbuf[4 + k_max_msg];
};

static void conn_put(std::vector<Conn *> &fd2conn, struct Conn *conn) {
    if (fd2conn.size() <= (size_t)conn->fd) {
        fd2conn.resize(conn->fd + 1);
    }
    fd2conn[conn->fd] = conn;
}

static int32_t accept_new_conn(std::vector<Conn *> &fd2conn, int fd, int epfd) {
    // accept
    struct sockaddr_in client_addr = {};
    socklen_t socklen = sizeof(client_addr);
    int connfd = accept(fd, (struct sockaddr *)&client_addr, &socklen);
    if (connfd < 0) {
        msg("accept() error");
        return -1; // error
    }

    //set the new connection fd to nonblocking mode
    fd_set_nb(connfd);

    // creating the struct Conn
    struct Conn *conn = (struct Conn *)malloc(sizeof(struct Conn));
    if (!conn) {
        close(connfd);
        return -1;
    }
    conn->fd = connfd;
    conn->state = STATE_REQ;
    conn->rbuf_size = 0;
    conn->wbuf_size = 0;
    conn->wbuf_sent = 0;
    conn_put(fd2conn, conn);

    // add the client socket to epoll
    struct epoll_event ev = {};
    ev.events = EPOLLIN | EPOLLERR;
    ev.data.fd = connfd;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, connfd, &ev) < 0) {
        msg("epoll_ctl: add client");
        close(fd);
    }
    return 0;
}

static void state_req(Conn *conn);
static void state_res(Conn *conn);

const size_t k_max_args = 1024;

enum {
    RES_OK = 0,
    RES_ERR = 1,
    RES_NX = 2, // not exist
};

// the structure for the key space
// placeholder
static std::map<std::string, std::string> g_map;

static uint32_t do_get(
    const std::vector<std::string> &cmd, uint8_t *res, uint32_t *reslen
) {
    if (!g_map.count(cmd[1])) {
        return RES_NX;
    }
    std::string &val = g_map[cmd[1]];
    assert(val.size() < k_max_msg);
    memcpy(res, val.data(), val.size());
    *reslen = (uint32_t)val.size();
    return RES_OK;
}

static uint32_t do_set(
    const std::vector<std::string> &cmd, uint8_t *res, uint32_t *reslen
) {
    (void)res;
    (void)reslen;
    g_map[cmd[1]] = cmd[2];
    return RES_OK;
}

static uint32_t do_del(
    const std::vector<std::string> &cmd, uint8_t *res, uint32_t *reslen)
    {
        (void)res;
        (void)reslen;
        g_map.erase(cmd[1]);
        return RES_OK;
}

static int32_t parse_req(
    const uint8_t *data, size_t len, std::vector<std::string> &out)
    {
        if (len < 4) {
            return -1;
        }

        // get the number of strings
        uint32_t n = 0;
        memcpy(&n, &data[0], 4);
        if (n > k_max_args) {
            return -1;
        }

        // start just after the number of strings
        size_t pos = 4;
        while (n--) {
            if (pos + 4 > len) {
                return -1;
            }
            // get the size of the string
            uint32_t sz = 0;
            memcpy(&sz, &data[pos], 4);
            if (pos + 4 + sz > len) {
                return -1;
            }

            // save the string in our vector
            out.push_back(std::string((char *)&data[pos + 4], sz));
            pos += 4 + sz;
        }

        if (pos != len) {
            return -1;
        }
        return 0;
}

static bool cmd_is(const std::string &word, const char *cmd) {
    return 0 == strcasecmp(word.c_str(), cmd);
}

static int32_t do_request(
    const uint8_t *req, uint32_t reqlen,
    uint32_t *rescode, uint8_t *res, uint32_t *reslen)
    {
        std::vector<std::string> cmd;
        if (0 != parse_req(req, reqlen, cmd)) {
            msg("bad req");
            return -1;
        }

        if (cmd.size() == 2 && cmd_is(cmd[0], "get")) {
            *rescode = do_get(cmd, res, reslen);
        } else if (cmd.size() == 3 && cmd_is(cmd[0], "set")) {
            *rescode = do_set(cmd, res, reslen);
        } else if (cmd.size() == 2 && cmd_is(cmd[0], "del")) {
            *rescode = do_del(cmd, res, reslen);
        } else {
            // command not recognized
            *rescode = RES_ERR;
            const char *msg = "Unknown cmd";
            strcpy((char *)res, msg);
            *reslen = strlen(msg);
            return 0;
        }

        return 0;
    }

static bool try_one_request(Conn *conn) {
    //try to parse a request from te buffer
    if (conn->rbuf_size < 4) {
        // not enough data in buffer
        // retry in next iteration
        return false;
    }
    
    // read the length of packet
    uint32_t len = 0;
    memcpy(&len, &conn->rbuf[0], 4);
    if (len > k_max_msg) {
        msg("too long");
        conn->state = STATE_END;
        return false;
    }

    if (4 + len > conn->rbuf_size) {
        // not enough data in bugger
        // retry in next iteratiob
        return false;
    }

    // got one request, generate the response
    uint32_t rescode = 0;
    uint32_t wlen = 0;
    int32_t err = do_request(
        &conn->rbuf[4], len,
        &rescode, &conn->wbuf[4 + 4], &wlen);
    
    if (err) {
        conn->state = STATE_END;
        return false;
    }

    wlen += 4;
    // generating response of the length and resocde
    memcpy(&conn->wbuf[0], &wlen, 4);
    memcpy(&conn->wbuf[4], &rescode, 4);
    conn->wbuf_size = 4 + wlen;

    // remove the request from the buffer
    // frequent memmove is inefficient
    // need better handling for prod code
    size_t remain = conn->rbuf_size - 4 - len;
    if (remain) {
        memmove(conn->rbuf, &conn->rbuf[4 + len], remain);
    }
    conn->rbuf_size = remain;

    //change state
    conn->state = STATE_RES;
    state_res(conn);

    // continue the outer loop if the request was fully processes
    return (conn->state == STATE_REQ);
}

static bool try_fill_buffer(Conn *conn) {
    // try to fill the buffer
    assert(conn->rbuf_size < sizeof(conn->rbuf));
    ssize_t rv = 0;
    
    // process data already in read buffer
    do {
        size_t cap = sizeof(conn->rbuf) - conn->rbuf_size;
        rv = read(conn->fd, &conn->rbuf[conn->rbuf_size], cap);
    } while (rv < 0 && errno == EINTR); //EINTR is signal interrupt
    
    if (rv < 0 && errno == EAGAIN) {
        // got EGAIN so stop
        return false;
    }

    if (rv < 0) {
        msg("read() error");
        conn->state = STATE_END;
        return false;
    }

    if (rv == 0) {
        if (conn->rbuf_size > 0) {
            msg("unexpected EOF");
        } else {
            msg("EOF");
        }
        conn->state = STATE_END;
        return false;
    }

    conn->rbuf_size += (size_t)rv;
    assert(conn->rbuf_size <= sizeof(conn->rbuf));

    // try to process requests one by one
    // loop because pipelining as there are more than one request in read bugffer
    while (try_one_request(conn)) {}
    return (conn->state == STATE_REQ);
}

static void state_req(Conn *conn) {
    while (try_fill_buffer(conn)) {}
}

static bool try_flush_buffer(Conn *conn) {
    ssize_t rv = 0;
    do {
        size_t remain = conn->wbuf_size - conn->wbuf_sent;
        rv = write(conn->fd, &conn->wbuf[conn->wbuf_sent], remain);
    } while (rv < 0 && errno == EINTR);
    if (rv < 0 && errno == EAGAIN) {
        // got EAGAIN, stop.
        return false;
    }
    if (rv < 0) {
        msg("write() error");
        conn->state = STATE_END;
        return false;
    }
    conn->wbuf_sent += (size_t)rv;
    assert(conn->wbuf_sent <= conn->wbuf_size);
    if (conn->wbuf_sent == conn->wbuf_size) {
        // response was fully sent, change state back
        conn->state = STATE_REQ;
        conn->wbuf_sent = 0;
        conn->wbuf_size = 0;
        return false;
    }
    // still got some data in wbuf, could try to write again
    return true;
}

static void state_res(Conn *conn) {
    while (try_flush_buffer(conn)) {}
}

static void connection_io(Conn *conn) {
    if (conn->state == STATE_REQ) {
        state_req(conn);
    } else if (conn->state == STATE_RES) {
        state_res(conn);
    } else {
        assert(0); // not expected
    }
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
        die("bind()");
    }

    // LISTEN
    // backlog argument is the size of the queue: SOMAXCONN = 128
    rv = listen(fd, SOMAXCONN);
    if (rv) {
        die("listen()");
    }

    // map of all client connections
    // keys are fd
    std::vector<Conn *> fd2conn(1024, nullptr);

    // set the listen fd to nonblocking mode
    fd_set_nb(fd);

    // event loop
    int epfd = epoll_create1(0);
    if (epfd == -1) {
        die("epoll_create1()");
    }
    struct epoll_event ev = {};
    ev.events = EPOLLIN | EPOLLERR;
    ev.data.fd = fd;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev) < 0) {
        die("epoll_ctl: add listen_fd");
    }
    
    // std::cout << fd << std::endl;

    std::vector<struct epoll_event> events(1024);

    while (true) {
        int n = epoll_wait(epfd, events.data(), events.size(), 1000);
        if (n < 0) {
            die("epoll_wait");
        }

        for (int i = 0; i < n; ++i) {
            int event_fd = events[i].data.fd;
            
            if (event_fd == fd) {
                // listening socket is read
                // accept a new connection
                accept_new_conn(fd2conn, fd, epfd);
            } else {
                //existing client socket is ready
                Conn *conn = fd2conn[event_fd];
                if (conn) {
                    connection_io(conn);

                    if (conn->state == STATE_END) {
                        //remove the client from epoll and clean up
                        epoll_ctl(epfd, EPOLL_CTL_DEL, conn->fd, nullptr);
                        fd2conn[conn->fd] = NULL;
                        (void)close(conn->fd);
                        free(conn);
                    } else {
                        //update events if needed
                        struct epoll_event ev = {};
                        ev.events = (conn->state == STATE_REQ ? EPOLLIN : EPOLLOUT) | EPOLLERR;
                        ev.data.fd = conn->fd;
                        epoll_ctl(epfd, EPOLL_CTL_ADD, conn->fd, &ev);
                    }
                }
            }
        }
    }

    close(fd);
    close(epfd);

    return 0;
}
