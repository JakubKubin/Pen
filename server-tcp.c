// server.c

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <ctype.h>
#include <time.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <sys/select.h>

#define PORT 8080
#define MAX_CLIENTS 10
#define BUFFER_SIZE 1024
#define MAX_PATTERN_LENGTH 32
#define MIN_PLAYERS 2  // Minimum number of players required to start the game

// Structure to hold client information
typedef struct {
    int socket_fd;
    char pattern[MAX_PATTERN_LENGTH];
    int pattern_length;
    int registered;
    char recv_buffer[BUFFER_SIZE];
    int recv_buffer_len;
} ClientInfo;

// Structure to hold statistics for patterns
typedef struct {
    char pattern[MAX_PATTERN_LENGTH];
    int wins;
    int total_games;
    int total_flips;
} PatternStats;

int main() {
    int server_fd, new_socket, activity, i, valread, sd;
    int max_sd;
    struct sockaddr_in address;
    fd_set readfds;
    char buffer[BUFFER_SIZE];

    // For diagnostics
    int messages_sent = 0;
    int messages_received = 0;
    int total_message_length = 0;
    int completed_games = 0;

    // Initialize all client_socket[] to 0
    ClientInfo clients[MAX_CLIENTS];
    PatternStats pattern_stats[MAX_CLIENTS * 2]; // Assuming possible different patterns
    int pattern_stats_count = 0;

    for (i = 0; i < MAX_CLIENTS; i++) {
        clients[i].socket_fd = 0;
        clients[i].registered = 0;
        clients[i].recv_buffer_len = 0;
    }

    // Seed the random number generator once
    srand(time(NULL));

    // Create a master socket
    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
        perror("Socket failed");
        exit(EXIT_FAILURE);
    }

    // Set master socket to allow multiple connections
    int opt = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR,
                   (char *)&opt, sizeof(opt)) < 0) {
        perror("Setsockopt");
        exit(EXIT_FAILURE);
    }

    // Type of socket created
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY; // Bind to all interfaces
    address.sin_port = htons(PORT);

    // Bind the socket to localhost and PORT
    if (bind(server_fd, (struct sockaddr *)&address,
             sizeof(address)) < 0) {
        perror("Bind failed");
        exit(EXIT_FAILURE);
    }

    printf("Listener on port %d \n", PORT);

    // Try to specify maximum of MAX_CLIENTS pending connections for the master socket
    if (listen(server_fd, MAX_CLIENTS) < 0) {
        perror("Listen");
        exit(EXIT_FAILURE);
    }

    // Accept the incoming connection
    int addrlen = sizeof(address);
    puts("Waiting for connections ...");

    // Main loop
    int game_in_progress = 0;
    char coin_sequence[BUFFER_SIZE] = {0};
    int coin_sequence_length = 0;

    while (1) {
        // Clear the socket set
        FD_ZERO(&readfds);

        // Add master socket to set
        FD_SET(server_fd, &readfds);
        max_sd = server_fd;

        // Add client sockets to set
        for (i = 0; i < MAX_CLIENTS; i++) {
            // Socket descriptor
            sd = clients[i].socket_fd;

            // If valid socket descriptor then add to read list
            if (sd > 0)
                FD_SET(sd, &readfds);

            // Highest file descriptor number, need it for the select function
            if (sd > max_sd)
                max_sd = sd;
        }

        // Set timeout for select
        struct timeval timeout;
        timeout.tv_sec = 0;
        timeout.tv_usec = 100000; // 100 milliseconds

        // Wait for an activity on one of the sockets, timeout after 100ms
        activity = select(max_sd + 1, &readfds, NULL, NULL, &timeout);

        if ((activity < 0) && (errno != EINTR)) {
            printf("Select error");
        }

        // If something happened on the master socket, then it's an incoming connection
        if (FD_ISSET(server_fd, &readfds)) {
            if ((new_socket = accept(server_fd,
                                     (struct sockaddr *)&address, (socklen_t *)&addrlen)) < 0) {
                perror("Accept");
                exit(EXIT_FAILURE);
            }

            // Inform user of socket number - used in send and receive commands
            printf("New connection, socket fd is %d, ip is: %s, port: %d \n",
                   new_socket, inet_ntoa(address.sin_addr), ntohs(address.sin_port));

            // Add new socket to array of clients
            for (i = 0; i < MAX_CLIENTS; i++) {
                // If position is empty
                if (clients[i].socket_fd == 0) {
                    clients[i].socket_fd = new_socket;
                    clients[i].registered = 0;
                    clients[i].recv_buffer_len = 0;
                    printf("Adding to list of sockets as %d\n", i);
                    break;
                }
            }

            // If maximum clients reached
            if (i == MAX_CLIENTS) {
                char *message = "Server is full\n";
                send(new_socket, message, strlen(message), 0);
                close(new_socket);
            }
        }

        // Else it's some IO operation on some other socket
        for (i = 0; i < MAX_CLIENTS; i++) {
            sd = clients[i].socket_fd;

            if (FD_ISSET(sd, &readfds)) {
                // Read data from client
                valread = read(sd, buffer, BUFFER_SIZE);
                if (valread <= 0) {
                    // Handle client disconnection
                    getpeername(sd, (struct sockaddr *)&address,
                                (socklen_t *)&addrlen);
                    printf("Host disconnected, ip %s, port %d \n",
                           inet_ntoa(address.sin_addr), ntohs(address.sin_port));

                    // Close the socket and mark as 0 in list for reuse
                    close(sd);
                    clients[i].socket_fd = 0;
                    clients[i].registered = 0;
                    clients[i].recv_buffer_len = 0;
                } else {
                    // Append data to client's recv_buffer
                    if (clients[i].recv_buffer_len + valread >= BUFFER_SIZE) {
                        // Prevent buffer overflow
                        printf("Buffer overflow detected for client %d. Disconnecting.\n", i);
                        close(sd);
                        clients[i].socket_fd = 0;
                        clients[i].registered = 0;
                        clients[i].recv_buffer_len = 0;
                        continue;
                    }

                    memcpy(clients[i].recv_buffer + clients[i].recv_buffer_len, buffer, valread);
                    clients[i].recv_buffer_len += valread;
                    clients[i].recv_buffer[clients[i].recv_buffer_len] = '\0'; // Null-terminate

                    messages_received++;
                    total_message_length += valread;

                    // Now process messages in recv_buffer
                    char *newline_pos;
                    while ((newline_pos = strchr(clients[i].recv_buffer, '\n')) != NULL) {
                        // We have a complete message
                        int message_len = newline_pos - clients[i].recv_buffer;
                        char message[BUFFER_SIZE];
                        memcpy(message, clients[i].recv_buffer, message_len);
                        message[message_len] = '\0';

                        // Remove the message from recv_buffer
                        clients[i].recv_buffer_len -= (message_len + 1); // +1 for '\n'
                        memmove(clients[i].recv_buffer, newline_pos + 1, clients[i].recv_buffer_len);
                        clients[i].recv_buffer[clients[i].recv_buffer_len] = '\0';

                        // Process the message
                        if (!clients[i].registered) {
                            // First message should be pattern registration
                            strncpy(clients[i].pattern, message, MAX_PATTERN_LENGTH - 1);
                            clients[i].pattern[MAX_PATTERN_LENGTH - 1] = '\0';
                            clients[i].pattern_length = strlen(clients[i].pattern);
                            clients[i].registered = 1;
                            printf("Client %d registered pattern: %s\n", i, clients[i].pattern);

                            // Check if enough clients have registered to start the game
                            int connected_clients = 0;
                            int registered_clients = 0;
                            int all_registered = 1;
                            for (int j = 0; j < MAX_CLIENTS; j++) {
                                if (clients[j].socket_fd > 0) {
                                    connected_clients++;
                                    if (clients[j].registered) {
                                        registered_clients++;
                                    } else {
                                        all_registered = 0;
                                    }
                                }
                            }

                            if (connected_clients >= MIN_PLAYERS && all_registered && !game_in_progress) {
                                printf("Minimum number of clients registered. Starting game...\n");
                                game_in_progress = 1;
                            }
                        } else {
                            // Handle game messages
                            if (strncmp(message, "WIN", 3) == 0) {
                                // Client has won
                                // Now, parse the message
                                char client_pattern[MAX_PATTERN_LENGTH];
                                int flips_required;
                                // Skip "WIN "
                                char *data = message + 3;
                                while (isspace(*data)) data++; // Skip any whitespace
                                // Now parse the rest
                                sscanf(data, "%s %d", client_pattern, &flips_required);

                                // Update statistics
                                int found = 0;
                                for (int j = 0; j < pattern_stats_count; j++) {
                                    if (strcmp(pattern_stats[j].pattern, client_pattern) == 0) {
                                        pattern_stats[j].wins++;
                                        pattern_stats[j].total_flips += flips_required;
                                        pattern_stats[j].total_games++;
                                        found = 1;
                                        break;
                                    }
                                }
                                if (!found) {
                                    // Add new pattern stats
                                    strcpy(pattern_stats[pattern_stats_count].pattern, client_pattern);
                                    pattern_stats[pattern_stats_count].wins = 1;
                                    pattern_stats[pattern_stats_count].total_flips = flips_required;
                                    pattern_stats[pattern_stats_count].total_games = 1;
                                    pattern_stats_count++;
                                }

                                printf("Client %d won with pattern %s in %d flips\n",
                                       i, client_pattern, flips_required);

                                // Increment completed games
                                completed_games++;

                                // Reset game state
                                game_in_progress = 0;
                                coin_sequence_length = 0;
                                memset(coin_sequence, 0, BUFFER_SIZE);

                                // Print diagnostics
                                printf("\n--- Diagnostics ---\n");
                                printf("Messages sent: %d\n", messages_sent);
                                printf("Messages received: %d\n", messages_received);
                                printf("Average message length: %.2f bytes\n",
                                       (messages_sent + messages_received) > 0
                                           ? (float)total_message_length / (messages_sent + messages_received)
                                           : 0.0);
                                printf("Completed games: %d\n", completed_games);
                                printf("\n--- Statistics ---\n");
                                for (int j = 0; j < pattern_stats_count; j++) {
                                    float win_probability = (float)pattern_stats[j].wins / pattern_stats[j].total_games;
                                    float average_flips = (float)pattern_stats[j].total_flips / pattern_stats[j].total_games;
                                    printf("Pattern: %s, Wins: %d, Total Games: %d, Win Probability: %.2f, Average Flips: %.2f\n",
                                           pattern_stats[j].pattern, pattern_stats[j].wins,
                                           pattern_stats[j].total_games, win_probability, average_flips);
                                }
                                printf("-------------------\n");

                                // After game ends, start new game if enough clients are still connected
                                int connected_clients = 0;
                                int registered_clients = 0;
                                for (int j = 0; j < MAX_CLIENTS; j++) {
                                    if (clients[j].socket_fd > 0 && clients[j].registered) {
                                        connected_clients++;
                                        registered_clients++;
                                    }
                                }
                                if (connected_clients >= MIN_PLAYERS && !game_in_progress) {
                                    printf("Starting new game...\n");
                                    game_in_progress = 1;
                                }
                            }
                        }
                    }
                }
            }
        }

        // If game is in progress, send coin flips
        if (game_in_progress) {
            // Generate a random bit
            int rand_bit = rand() % 2;
            char coin_flip = rand_bit ? 'H' : 'T';
            coin_sequence[coin_sequence_length++] = coin_flip;

            // Send the bit to all clients
            for (i = 0; i < MAX_CLIENTS; i++) {
                sd = clients[i].socket_fd;
                if (sd > 0 && clients[i].registered) {
                    send(sd, &coin_flip, 1, 0);
                    messages_sent++;
                    total_message_length += 1;
                }
            }

        }
    }

    return 0;
}
