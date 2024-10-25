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
#include <netinet/in.h>
#include <sys/select.h>

#define PORT 8080
#define MAX_CLIENTS 10
#define BUFFER_SIZE 1024
#define MAX_PATTERN_LENGTH 16
#define MIN_PLAYERS 2  // Minimum number of players required to start the game

// Structure to hold client information
typedef struct {
    struct sockaddr_in address;
    char pattern[MAX_PATTERN_LENGTH];
    int pattern_length;
    int registered;
} ClientInfo;

// Structure to hold statistics for patterns
typedef struct {
    char pattern[MAX_PATTERN_LENGTH];
    int wins;
    int total_games;
    int total_flips;
} PatternStats;

int main() {
    int server_fd, i, valread;
    struct sockaddr_in server_addr, client_addr;
    socklen_t addr_len = sizeof(struct sockaddr_in);
    char buffer[BUFFER_SIZE];

    // For diagnostics
    int messages_sent = 0;
    int messages_received = 0;
    int total_message_length = 0;
    int completed_games = 0;

    // Initialize clients
    ClientInfo clients[MAX_CLIENTS];
    PatternStats pattern_stats[MAX_CLIENTS * 2]; // Assuming possible different patterns
    int pattern_stats_count = 0;

    for (i = 0; i < MAX_CLIENTS; i++) {
        clients[i].registered = 0;
    }

    // Seed the random number generator once
    srand(time(NULL));

    // Create UDP socket
    if ((server_fd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
        perror("Socket creation failed");
        exit(EXIT_FAILURE);
    }

    // Zero out the server address
    memset(&server_addr, 0, sizeof(server_addr));

    // Fill server information
    server_addr.sin_family = AF_INET; // IPv4
    server_addr.sin_addr.s_addr = INADDR_ANY; // Bind to all interfaces
    server_addr.sin_port = htons(PORT);

    // Bind the socket with the server address
    if (bind(server_fd, (const struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("Bind failed");
        exit(EXIT_FAILURE);
    }

    printf("UDP server listening on port %d\n", PORT);

    // Main loop
    int game_in_progress = 0;

    // Maximum pattern length among all clients
    int max_pattern_length = 0;

    // Sequence buffer of size max_pattern_length + 1
    char coin_sequence[MAX_PATTERN_LENGTH + 1];
    int coin_sequence_length = 0;

    int total_clients = 0;
    fd_set readfds;
    struct timeval timeout;

    while (1) {
        // Set timeout for select
        timeout.tv_sec = 0;
        timeout.tv_usec = 100; // 100 milliseconds

        // Set up the file descriptor set
        FD_ZERO(&readfds);
        FD_SET(server_fd, &readfds);
        int max_sd = server_fd;

        // Wait for activity or timeout
        int activity = select(max_sd + 1, &readfds, NULL, NULL, &timeout);

        if ((activity < 0) && (errno != EINTR)) {
            printf("Select error");
        }

        if (FD_ISSET(server_fd, &readfds)) {
            // Receive message from client
            valread = recvfrom(server_fd, buffer, BUFFER_SIZE, 0,
                               (struct sockaddr *)&client_addr, &addr_len);
            if (valread > 0) {
                buffer[valread] = '\0';
                messages_received++;
                total_message_length += valread;

                // Check if the client is already registered
                int client_index = -1;
                for (i = 0; i < MAX_CLIENTS; i++) {
                    if (clients[i].registered &&
                        clients[i].address.sin_addr.s_addr == client_addr.sin_addr.s_addr &&
                        clients[i].address.sin_port == client_addr.sin_port) {
                        client_index = i;
                        break;
                    }
                }

                // If not registered, register the client
                if (client_index == -1) {
                    // Register new client
                    for (i = 0; i < MAX_CLIENTS; i++) {
                        if (!clients[i].registered) {
                            clients[i].address = client_addr;
                            // Store the pattern as is (will be '0's and '1's)
                            strncpy(clients[i].pattern, buffer, MAX_PATTERN_LENGTH - 1);
                            clients[i].pattern[MAX_PATTERN_LENGTH - 1] = '\0';
                            clients[i].pattern_length = strlen(clients[i].pattern);
                            clients[i].registered = 1;
                            client_index = i;
                            printf("New client registered: %s:%d with pattern %s\n",
                                   inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port),
                                   clients[i].pattern);
                            total_clients++;

                            // Update maximum pattern length
                            if (clients[i].pattern_length > max_pattern_length) {
                                max_pattern_length = clients[i].pattern_length;
                            }

                            break;
                        }
                    }
                } else {
                    // Client is already registered
                    if (strncmp(buffer, "WIN", 3) == 0) {
                        // Handle WIN message
                        char client_pattern[MAX_PATTERN_LENGTH];
                        int flips_required;
                        // Parse the message
                        sscanf(buffer + 4, "%s %d", client_pattern, &flips_required);

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

                        printf("Client %s:%d won with pattern %s in %d flips\n",
                               inet_ntoa(clients[client_index].address.sin_addr),
                               ntohs(clients[client_index].address.sin_port),
                               client_pattern, flips_required);

                        // Increment completed games
                        completed_games++;

                        // Reset game state
                        game_in_progress = 0;
                        coin_sequence_length = 0;
                        memset(coin_sequence, 0, sizeof(coin_sequence));

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

                        // Check if enough clients are still connected
                        int registered_clients = 0;
                        max_pattern_length = 0;
                        for (int j = 0; j < MAX_CLIENTS; j++) {
                            if (clients[j].registered) {
                                registered_clients++;
                                if (clients[j].pattern_length > max_pattern_length) {
                                    max_pattern_length = clients[j].pattern_length;
                                }
                            }
                        }

                        // Resize coin_sequence buffer
                        memset(coin_sequence, 0, sizeof(coin_sequence));

                        if (registered_clients >= MIN_PLAYERS && !game_in_progress) {
                            printf("Starting new game...\n");
                            game_in_progress = 1;
                        }
                    }
                }

                // Check if enough clients have registered to start the game
                if (!game_in_progress) {
                    int registered_clients = 0;
                    for (int j = 0; j < MAX_CLIENTS; j++) {
                        if (clients[j].registered) {
                            registered_clients++;
                        }
                    }

                    if (registered_clients >= MIN_PLAYERS) {
                        printf("Minimum number of clients registered. Starting game...\n");
                        game_in_progress = 1;
                    }
                }
            }
        }

        // If game is in progress, send coin flips
        if (game_in_progress) {
            // Generate a random bit
            int rand_bit = rand() % 2;
            char coin_flip_char = rand_bit ? '1' : '0'; // Use '0' and '1'

            // Update coin_sequence buffer
            if (coin_sequence_length < max_pattern_length + 1) {
                coin_sequence[coin_sequence_length++] = coin_flip_char;
            } else {
                // Shift the buffer to the left by one and append the new coin flip
                memmove(coin_sequence, coin_sequence + 1, max_pattern_length);
                coin_sequence[max_pattern_length] = coin_flip_char;
            }

            // Send the bit to all clients
            for (i = 0; i < MAX_CLIENTS; i++) {
                if (clients[i].registered) {
                    sendto(server_fd, &coin_flip_char, 1, 0,
                           (struct sockaddr *)&clients[i].address, addr_len);
                    messages_sent++;
                    total_message_length += 1;
                }
            }

            // No need to sleep here since select() timeout provides timing
        }
    }

    close(server_fd);
    return 0;
}
