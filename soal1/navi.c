#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/select.h>
#include <signal.h>

#define BUFFER_SIZE 1024

int sock_fd = -1;

void handle_sigint(int sig) {
    (void)sig;
    if (sock_fd >= 0) {
        // Send /exit before disconnect
        send(sock_fd, "/exit\n", 6, 0);
        // Wait briefly for server response
        char buf[BUFFER_SIZE];
        struct timeval tv = {1, 0};
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(sock_fd, &fds);
        if (select(sock_fd+1, &fds, NULL, NULL, &tv) > 0) {
            int n = recv(sock_fd, buf, sizeof(buf)-1, 0);
            if (n > 0) { buf[n] = '\0'; printf("%s", buf); }
        }
        close(sock_fd);
    }
    printf("\n");
    exit(0);
}

int main() {
    // Read protocol file
    FILE *pf = fopen("protocol", "r");
    if (!pf) { perror("Cannot open 'protocol' file"); exit(1); }
    char ip[64]; int port;
    fscanf(pf, "%63s\n%d", ip, &port);
    fclose(pf);

    sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_fd < 0) { perror("socket"); exit(1); }

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, ip, &addr.sin_addr);

    if (connect(sock_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("connect"); exit(1);
    }

    signal(SIGINT, handle_sigint);

    fd_set read_fds;

    while (1) {
        FD_ZERO(&read_fds);
        FD_SET(STDIN_FILENO, &read_fds);
        FD_SET(sock_fd, &read_fds);

        int activity = select(sock_fd + 1, &read_fds, NULL, NULL, NULL);
        if (activity < 0) break;

        // Data from server
        if (FD_ISSET(sock_fd, &read_fds)) {
            char buf[BUFFER_SIZE];
            int n = recv(sock_fd, buf, sizeof(buf)-1, 0);
            if (n <= 0) {
                printf("[System] Connection to The Wired lost.\n");
                break;
            }
            buf[n] = '\0';
            printf("%s", buf);
            fflush(stdout);
        }

        // Input from user
        if (FD_ISSET(STDIN_FILENO, &read_fds)) {
            char buf[BUFFER_SIZE];
            if (fgets(buf, sizeof(buf), stdin) == NULL) {
                // EOF / Ctrl+D - send /exit
                send(sock_fd, "/exit\n", 6, 0);
                break;
            }
            // Print "> " prefix for user input visibility (only locally)
            send(sock_fd, buf, strlen(buf), 0);
        }
    }

    close(sock_fd);
    return 0;
}
