#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/select.h>
#include <time.h>
#include <errno.h>

#define MAX_CLIENTS 64
#define BUFFER_SIZE 1024
#define NAME_SIZE 64
#define ADMIN_PASSWORD "protocol7"
#define LOG_FILE "history.log"

typedef struct {
    int fd;
    char name[NAME_SIZE];
    int is_admin;
    int authenticated; // 0=not yet named, 1=named, 2=waiting admin pass
} Client;

Client clients[MAX_CLIENTS];
int client_count = 0;
time_t server_start;

// ── Logging ──────────────────────────────────────────────────────────────────
void log_entry(const char *role, const char *msg) {
    FILE *f = fopen(LOG_FILE, "a");
    if (!f) return;

    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    fprintf(f, "[%04d-%02d-%02d %02d:%02d:%02d] [%s] [%s]\n",
            t->tm_year+1900, t->tm_mon+1, t->tm_mday,
            t->tm_hour, t->tm_min, t->tm_sec,
            role, msg);
    fclose(f);
}

// ── Helpers ───────────────────────────────────────────────────────────────────
int find_client(int fd) {
    for (int i = 0; i < client_count; i++)
        if (clients[i].fd == fd) return i;
    return -1;
}

int name_exists(const char *name) {
    for (int i = 0; i < client_count; i++)
        if (strcmp(clients[i].name, name) == 0) return 1;
    return 0;
}

void send_to(int fd, const char *msg) {
    send(fd, msg, strlen(msg), 0);
}

void broadcast(const char *msg, int exclude_fd) {
    for (int i = 0; i < client_count; i++) {
        if (clients[i].fd != exclude_fd && clients[i].authenticated == 1)
            send_to(clients[i].fd, msg);
    }
}

// ── Remove client ─────────────────────────────────────────────────────────────
void remove_client(int fd) {
    int idx = find_client(fd);
    if (idx < 0) return;

    char buf[BUFFER_SIZE];
    if (clients[idx].authenticated == 1) {
        snprintf(buf, sizeof(buf), "[System] [User '%s' disconnected]", clients[idx].name);
        log_entry("System", buf + 9); // skip "[System] "
        broadcast(buf, fd);
        printf("%s\n", buf);
    }

    close(fd);
    clients[idx] = clients[client_count - 1];
    client_count--;
}

// ── Admin RPC handler ─────────────────────────────────────────────────────────
void handle_admin_command(int idx, int cmd) {
    char buf[BUFFER_SIZE];
    int fd = clients[idx].fd;

    if (cmd == 1) {
        // RPC_GET_USERS
        log_entry("Admin", "RPC_GET_USERS");
        int count = 0;
        for (int i = 0; i < client_count; i++)
            if (clients[i].authenticated == 1 && !clients[i].is_admin) count++;
        snprintf(buf, sizeof(buf), "[RPC] Active users: %d\n", count);
        send_to(fd, buf);
    } else if (cmd == 2) {
        // RPC_GET_UPTIME
        log_entry("Admin", "RPC_GET_UPTIME");
        long uptime = (long)(time(NULL) - server_start);
        snprintf(buf, sizeof(buf), "[RPC] Uptime: %ld seconds\n", uptime);
        send_to(fd, buf);
    } else if (cmd == 3) {
        // RPC_SHUTDOWN
        log_entry("Admin", "RPC_SHUTDOWN");
        log_entry("System", "EMERGENCY SHUTDOWN INITIATED");
        const char *shutdown_msg = "[System] [EMERGENCY SHUTDOWN INITIATED]\n";
        // broadcast to all
        for (int i = 0; i < client_count; i++)
            send_to(clients[i].fd, shutdown_msg);
        printf("[System] EMERGENCY SHUTDOWN INITIATED\n");
        // close all and exit
        for (int i = 0; i < client_count; i++) close(clients[i].fd);
        exit(0);
    } else if (cmd == 4) {
        send_to(fd, "[System] Disconnecting from The Wired...\n");
        remove_client(fd);
    } else {
        send_to(fd, "[System] Invalid command.\n");
        send_to(fd, "=== THE KNIGHTS CONSOLE ===\n1. Check Active Entites (Users)\n2. Check Server Uptime\n3. Execute Emergency Shutdown\n4. Disconnect\nCommand >> ");
    }
}

// ── New data from client ──────────────────────────────────────────────────────
void handle_client_data(int fd) {
    char buf[BUFFER_SIZE];
    int idx = find_client(fd);
    if (idx < 0) return;

    int n = recv(fd, buf, sizeof(buf)-1, 0);
    if (n <= 0) {
        remove_client(fd);
        return;
    }
    buf[n] = '\0';
    // strip trailing newline
    buf[strcspn(buf, "\r\n")] = '\0';

    // ── State: not yet named ──
    if (clients[idx].authenticated == 0) {
        if (strlen(buf) == 0) {
            send_to(fd, "Enter your name: ");
            return;
        }
        if (name_exists(buf)) {
            char msg[BUFFER_SIZE];
            snprintf(msg, sizeof(msg),
                "[System] The identity '%s' is already synchronized in The Wired.\nEnter your name: ", buf);
            send_to(fd, msg);
            return;
        }
        strncpy(clients[idx].name, buf, NAME_SIZE-1);

        // Check if admin name
        if (strcmp(buf, "The Knights") == 0) {
            clients[idx].authenticated = 2; // waiting for password
            send_to(fd, "Enter Password: ");
            return;
        }

        clients[idx].authenticated = 1;
        char welcome[BUFFER_SIZE];
        snprintf(welcome, sizeof(welcome),
            "--- Welcome to The Wired, %s ---\n", buf);
        send_to(fd, welcome);

        char sys[BUFFER_SIZE];
        snprintf(sys, sizeof(sys), "User '%s' connected", buf);
        log_entry("System", sys);
        snprintf(sys, sizeof(sys), "[System] [User '%s' connected]", buf);
        broadcast(sys, fd);
        printf("[System] [User '%s' connected]\n", buf);
        return;
    }

    // ── State: waiting admin password ──
    if (clients[idx].authenticated == 2) {
        if (strcmp(buf, ADMIN_PASSWORD) == 0) {
            clients[idx].authenticated = 1;
            clients[idx].is_admin = 1;
            send_to(fd, "[System] Authentication Successful. Granted Admin privileges.\n\n");
            send_to(fd, "=== THE KNIGHTS CONSOLE ===\n1. Check Active Entites (Users)\n2. Check Server Uptime\n3. Execute Emergency Shutdown\n4. Disconnect\nCommand >> ");

            char sys[BUFFER_SIZE];
            snprintf(sys, sizeof(sys), "User 'The Knights' connected");
            log_entry("System", sys);
            snprintf(sys, sizeof(sys), "[System] [User 'The Knights' connected]");
            broadcast(sys, fd);
            printf("[System] [User 'The Knights' connected]\n");
        } else {
            send_to(fd, "[System] Authentication Failed.\nEnter Password: ");
        }
        return;
    }

    // ── Admin console ──
    if (clients[idx].is_admin) {
        int cmd = atoi(buf);
        log_entry("Admin", buf);
        handle_admin_command(idx, cmd);
        if (cmd >= 1 && cmd <= 3) {
            // re-show prompt after RPC (except shutdown)
            if (cmd != 3) {
                send_to(fd, "=== THE KNIGHTS CONSOLE ===\n1. Check Active Entites (Users)\n2. Check Server Uptime\n3. Execute Emergency Shutdown\n4. Disconnect\nCommand >> ");
            }
        }
        return;
    }

    // ── Normal user ──
    if (strcmp(buf, "/exit") == 0) {
        send_to(fd, "[System] Disconnecting from The Wired...\n");
        remove_client(fd);
        return;
    }

    // Broadcast chat
    char msg[BUFFER_SIZE];
    snprintf(msg, sizeof(msg), "[%s]: %s\n", clients[idx].name, buf);
    broadcast(msg, fd);

    char logmsg[BUFFER_SIZE];
    snprintf(logmsg, sizeof(logmsg), "[%s]: %s", clients[idx].name, buf);
    log_entry("User", logmsg);
}

// ── Main ──────────────────────────────────────────────────────────────────────
int main() {
    // Read protocol file
    FILE *pf = fopen("protocol", "r");
    if (!pf) { perror("Cannot open 'protocol' file"); exit(1); }
    char ip[64]; int port;
    fscanf(pf, "%63s\n%d", ip, &port);
    fclose(pf);

    server_start = time(NULL);

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) { perror("socket"); exit(1); }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, ip, &addr.sin_addr);

    if (bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind"); exit(1);
    }
    listen(server_fd, 10);

    log_entry("System", "SERVER ONLINE");
    printf("[System] [SERVER ONLINE] - listening on %s:%d\n", ip, port);

    fd_set read_fds;
    int max_fd = server_fd;

    while (1) {
        FD_ZERO(&read_fds);
        FD_SET(server_fd, &read_fds);
        for (int i = 0; i < client_count; i++) {
            FD_SET(clients[i].fd, &read_fds);
            if (clients[i].fd > max_fd) max_fd = clients[i].fd;
        }

        int activity = select(max_fd + 1, &read_fds, NULL, NULL, NULL);
        if (activity < 0 && errno != EINTR) { perror("select"); break; }

        // New connection
        if (FD_ISSET(server_fd, &read_fds)) {
            struct sockaddr_in cli_addr;
            socklen_t cli_len = sizeof(cli_addr);
            int new_fd = accept(server_fd, (struct sockaddr*)&cli_addr, &cli_len);
            if (new_fd >= 0 && client_count < MAX_CLIENTS) {
                clients[client_count].fd = new_fd;
                clients[client_count].name[0] = '\0';
                clients[client_count].is_admin = 0;
                clients[client_count].authenticated = 0;
                client_count++;
                send_to(new_fd, "Enter your name: ");
            }
        }

        // Data from existing clients
        for (int i = 0; i < client_count; i++) {
            if (FD_ISSET(clients[i].fd, &read_fds)) {
                handle_client_data(clients[i].fd);
                break; // client_count may have changed
            }
        }
    }

    close(server_fd);
    return 0;
}
