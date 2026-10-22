#include <iostream>
#include <cstring>
#include <cstdio>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <cerrno>
#include <ctime>

#include "parse_config.h"

using namespace std;

const int ON_PORT = 5000;
const uint8_t MSG_CONNECT = 1;
const uint8_t MSG_LINKSTATE = 2;
const int CONFIG_POLL_USEC = 500000;

struct VNInfo {
    int fd;
    in_addr ip;
    uint16_t port;
    bool active;
};

bool recv_all(int fd, void *buf, size_t len) {
    char *p = static_cast<char *>(buf);
    while (len > 0) {
        ssize_t r = recv(fd, p, len, 0);
        if (r <= 0) return false;
        p += r;
        len -= static_cast<size_t>(r);
    }
    return true;
}

bool send_all(int fd, const void *buf, size_t len) {
    const char *p = static_cast<const char *>(buf);
    while (len > 0) {
        ssize_t r = send(fd, p, len, MSG_NOSIGNAL);
        if (r <= 0) return false;
        p += r;
        len -= static_cast<size_t>(r);
    }
    return true;
}

bool readConnect(int fd, VNInfo &node) {
    unsigned char hdr[3];
    unsigned char body[6];
    if (!recv_all(fd, hdr, sizeof(hdr))) return false;

    uint16_t len;
    memcpy(&len, hdr + 1, sizeof(len));
    len = ntohs(len);
    if (hdr[0] != MSG_CONNECT || len != sizeof(body) ||
        !recv_all(fd, body, sizeof(body))) {
        return false;
    }

    memcpy(&node.ip, body, 4);
    uint16_t port;
    memcpy(&port, body + 4, sizeof(port));
    node.port = ntohs(port);
    uint32_t address = ntohl(node.ip.s_addr);
    if (node.port == 0 || address == 0 || (address >> 24) == 127) {
        return false;
    }
    node.fd = fd;
    node.active = true;
    return true;
}

bool sendLinkState(int i, int configured_nodes, int total_nodes,
                   int cost[MAX_NODES][MAX_NODES], VNInfo vn[]) {
    unsigned char msg[3 + MAX_NODES * 9];
    size_t off = 3;

    if (i < configured_nodes) {
        for (int j = 0; j < configured_nodes; ++j) {
            if (cost[i][j] < 0) continue;

            msg[off++] = static_cast<unsigned char>('A' + j);
            memcpy(msg + off, &vn[j].ip, 4);
            off += 4;
            uint16_t p = htons(vn[j].port);
            memcpy(msg + off, &p, 2);
            off += 2;
            uint16_t c = htons(static_cast<uint16_t>(cost[i][j]));
            memcpy(msg + off, &c, 2);
            off += 2;
        }
    } else {
        msg[off++] = static_cast<unsigned char>('A' + i);
        memcpy(msg + off, &vn[i].ip, 4);
        off += 4;
        uint16_t p = htons(vn[i].port);
        memcpy(msg + off, &p, 2);
        off += 2;
        uint16_t c = 0;
        memcpy(msg + off, &c, 2);
        off += 2;
    }

    msg[0] = MSG_LINKSTATE;
    uint16_t len = htons(static_cast<uint16_t>(off - 3));
    memcpy(msg + 1, &len, 2);
    return i < total_nodes && vn[i].active && send_all(vn[i].fd, msg, off);
}

void printTopology(int n, int cost[MAX_NODES][MAX_NODES]) {
    cout << "Current topology (" << n << " configured nodes):\n";
    for (int i = 0; i < n; ++i) {
        for (int j = 0; j < n; ++j) {
            if (j > 0) cout << ' ';
            cout << cost[i][j];
        }
        cout << '\n';
    }
}

bool sameTopology(int n, int a[MAX_NODES][MAX_NODES],
                  int b[MAX_NODES][MAX_NODES]) {
    for (int i = 0; i < n; ++i) {
        for (int j = 0; j < n; ++j) {
            if (a[i][j] != b[i][j]) return false;
        }
    }
    return true;
}

bool sameFileTime(const struct stat &a, const struct stat &b) {
    return a.st_mtim.tv_sec == b.st_mtim.tv_sec &&
           a.st_mtim.tv_nsec == b.st_mtim.tv_nsec &&
           a.st_size == b.st_size;
}

int main(int argc, char *argv[]) {
    if (argc != 2) {
        cerr << "Usage: " << argv[0] << " <config-file>\n";
        return 1;
    }

    setvbuf(stdout, nullptr, _IOLBF, 0);
    const char *config_path = argv[1];

    int cost[MAX_NODES][MAX_NODES];
    int n = 0;
    if (!parseConfig(config_path, cost, n)) return 1;
    printTopology(n, cost);
    struct stat known_file_stat{};
    bool have_file_stat = stat(config_path, &known_file_stat) == 0;
    if (!have_file_stat) perror("stat config");

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket");
        return 1;
    }
    int yes = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) < 0) {
        perror("setsockopt");
        close(server_fd);
        return 1;
    }

    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(ON_PORT);
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(server_fd, reinterpret_cast<sockaddr *>(&server_addr),
             sizeof(server_addr)) < 0) {
        perror("bind");
        close(server_fd);
        return 1;
    }
    if (listen(server_fd, 10) < 0) {
        perror("listen");
        close(server_fd);
        return 1;
    }
    cout << "Oracle Node listening on TCP port " << ON_PORT << "...\n";

    VNInfo vn[MAX_NODES]{};
    int total_nodes = 0;
    while (total_nodes < n) {
        int fd = accept(server_fd, nullptr, nullptr);
        if (fd < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            close(server_fd);
            return 1;
        }
        VNInfo candidate{};
        if (!readConnect(fd, candidate)) {
            cerr << "Rejected invalid CONNECT message or VN address\n";
            close(fd);
            continue;
        }
        vn[total_nodes] = candidate;
        printf("Registered %c = %s:%u\n", 'A' + total_nodes,
               inet_ntoa(vn[total_nodes].ip), vn[total_nodes].port);
        ++total_nodes;
    }

    cout << "All " << n << " configured nodes registered.\n";
    struct stat current_file_stat{};
    if (stat(config_path, &current_file_stat) == 0 &&
        (!have_file_stat || !sameFileTime(known_file_stat, current_file_stat))) {
        int updated_cost[MAX_NODES][MAX_NODES];
        int updated_n = 0;
        if (parseConfig(config_path, updated_cost, updated_n) && updated_n == n) {
            memcpy(cost, updated_cost, sizeof(cost));
            cout << "Config changed during registration; using latest costs.\n";
            printTopology(n, cost);
        } else {
            cerr << "Config changed during registration but could not be applied; "
                    "using previously parsed topology\n";
        }
        known_file_stat = current_file_stat;
        have_file_stat = true;
    }
    for (int i = 0; i < n; ++i) {
        if (!sendLinkState(i, n, total_nodes, cost, vn)) {
            cerr << "Failed to send LINK-STATE to " << static_cast<char>('A' + i) << "\n";
        }
    }

    while (true) {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(server_fd, &readfds);
        int max_fd = server_fd;
        for (int i = 0; i < total_nodes; ++i) {
            if (!vn[i].active) continue;
            FD_SET(vn[i].fd, &readfds);
            if (vn[i].fd > max_fd) max_fd = vn[i].fd;
        }

        timeval timeout{};
        timeout.tv_usec = CONFIG_POLL_USEC;
        int ready = select(max_fd + 1, &readfds, nullptr, nullptr, &timeout);
        if (ready < 0 && errno != EINTR) {
            perror("select");
            break;
        }

        if (ready > 0 && FD_ISSET(server_fd, &readfds)) {
            int fd = accept(server_fd, nullptr, nullptr);
            if (fd >= 0) {
                if (total_nodes >= MAX_NODES) {
                    cerr << "Maximum of " << MAX_NODES
                         << " virtual nodes reached; rejecting connection\n";
                    close(fd);
                } else {
                    VNInfo candidate{};
                    if (!readConnect(fd, candidate)) {
                        cerr << "Rejected invalid CONNECT message or VN address\n";
                        close(fd);
                    } else {
                        int id = total_nodes++;
                        vn[id] = candidate;
                        printf("Registered isolated node %c = %s:%u\n", 'A' + id,
                               inet_ntoa(vn[id].ip), vn[id].port);
                        if (!sendLinkState(id, n, total_nodes, cost, vn)) {
                            cerr << "Failed to send LINK-STATE to "
                                 << static_cast<char>('A' + id) << "\n";
                            close(vn[id].fd);
                            vn[id].active = false;
                        }
                    }
                }
            } else if (errno != EINTR) {
                perror("accept");
            }
        }

        if (ready > 0) {
            for (int i = 0; i < total_nodes; ++i) {
                if (!vn[i].active || !FD_ISSET(vn[i].fd, &readfds)) continue;
                char unexpected;
                ssize_t r = recv(vn[i].fd, &unexpected, 1, 0);
                if (r <= 0) {
                    cout << "Node " << static_cast<char>('A' + i)
                         << " disconnected\n";
                    close(vn[i].fd);
                    vn[i].active = false;
                } else {
                    cerr << "Unexpected VN-to-ON data from "
                         << static_cast<char>('A' + i) << "; ignoring\n";
                }
            }
        }

        struct stat current_file_stat{};
        if (stat(config_path, &current_file_stat) == 0 &&
            (!have_file_stat || !sameFileTime(known_file_stat, current_file_stat))) {
            int updated_cost[MAX_NODES][MAX_NODES];
            int updated_n = 0;
            if (!parseConfig(config_path, updated_cost, updated_n)) {
                cerr << "Config reload failed; keeping previous topology\n";
                known_file_stat = current_file_stat;
                have_file_stat = true;
            } else if (updated_n != n) {
                cerr << "Config reload rejected: node count cannot change "
                        "during emulation\n";
                known_file_stat = current_file_stat;
                have_file_stat = true;
            } else {
                bool changed = !sameTopology(n, cost, updated_cost);
                memcpy(cost, updated_cost, sizeof(cost));
                known_file_stat = current_file_stat;
                have_file_stat = true;
                if (changed) {
                    cout << "Config changed; distributing updated link costs.\n";
                    printTopology(n, cost);
                    for (int i = 0; i < n; ++i) {
                        if (vn[i].active && !sendLinkState(i, n, total_nodes, cost, vn)) {
                            cerr << "Failed to update LINK-STATE for "
                                 << static_cast<char>('A' + i) << "\n";
                        }
                    }
                } else {
                    cout << "Config file changed, but topology is unchanged.\n";
                }
            }
        }
    }

    for (int i = 0; i < total_nodes; ++i) {
        if (vn[i].active) close(vn[i].fd);
    }
    close(server_fd);
    return 0;
}
