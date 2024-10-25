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
#define MAX_PATTERN_LENGTH 16
#define MIN_PLAYERS 2

// Structure to hold client information
typedef struct {
    int client_id;  // Unique client ID
    struct sockaddr_in address;
    char pattern[MAX_PATTERN_LENGTH];         // Stored as '0's and '1's
    char pattern_display[MAX_PATTERN_LENGTH]; // Stored as 'H's and 'T's for display
    int pattern_length;
    int registered;
    int has_won;
    int currently_playing; // New variable to track if the client is playing in the current game
} ClientInfo;

// Structure to hold statistics for patterns
typedef struct {
    char pattern[MAX_PATTERN_LENGTH];
    char pattern_display[MAX_PATTERN_LENGTH];
    int wins;
    int total_games;
    int total_flips;
} PatternStats;

// Function declarations
void initialize_clients(ClientInfo clients[]);
int find_client_index(ClientInfo clients[], int client_id);
int register_client(int server_fd, ClientInfo clients[], struct sockaddr_in client_addr, char buffer[],
                    int *max_pattern_length, socklen_t addr_len, int *next_client_id);
void handle_client_message(int server_fd, ClientInfo clients[], PatternStats pattern_stats[],
                           int *pattern_stats_count, char buffer[], int valread,
                           struct sockaddr_in client_addr, int *messages_sent, int *messages_received,
                           int *total_message_length, int *game_in_progress, char coin_sequence[],
                           int *coin_sequence_length, int *completed_games, int *max_pattern_length,
                           socklen_t addr_len, int *next_client_id);
void process_win_claim(int server_fd, ClientInfo clients[], int client_index,
                       char coin_sequence[], int *coin_sequence_length, PatternStats pattern_stats[],
                       int *pattern_stats_count, int *messages_sent, int *messages_received, int *total_message_length,
                       int *game_in_progress, int *completed_games, socklen_t addr_len);
void update_pattern_stats(PatternStats pattern_stats[], int *pattern_stats_count,
                          ClientInfo client, int coin_sequence_length, int win);
void print_diagnostics(int messages_sent, int messages_received, int total_message_length, int completed_games);
void print_statistics(PatternStats pattern_stats[], int pattern_stats_count);
void send_coin_flip(int server_fd, ClientInfo clients[], int *messages_sent, int *total_message_length,
                    char coin_sequence[], int *coin_sequence_length, socklen_t addr_len);

int main() {
    int server_fd, i, valread;
    struct sockaddr_in server_addr, client_addr;
    socklen_t addr_len = sizeof(struct sockaddr_in);
    char buffer[256];

    // For diagnostics
    int messages_sent = 0;
    int messages_received = 0;
    int total_message_length = 0;
    int completed_games = 0;

    // Initialize clients
    ClientInfo clients[MAX_CLIENTS];
    PatternStats pattern_stats[MAX_CLIENTS * 2]; // Assuming possible different patterns
    int pattern_stats_count = 0;

    initialize_clients(clients);

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
    server_addr.sin_family = AF_INET;      // IPv4
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

    // Sequence buffer to store coin flips
    char coin_sequence[1024]; // Store entire sequence for validation
    int coin_sequence_length = 0;

    fd_set readfds;
    struct timeval timeout;

    int next_client_id = 1; // Counter for assigning unique client IDs

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
            valread = recvfrom(server_fd, buffer, sizeof(buffer) - 1, 0,
                               (struct sockaddr *)&client_addr, &addr_len);
            if (valread > 0) {
                buffer[valread] = '\0';
                messages_received++;
                total_message_length += valread;

                handle_client_message(server_fd, clients, pattern_stats, &pattern_stats_count,
                                      buffer, valread, client_addr, &messages_sent, &messages_received,
                                      &total_message_length, &game_in_progress, coin_sequence,
                                      &coin_sequence_length, &completed_games, &max_pattern_length, addr_len, &next_client_id);
            }
        }

        // If game is in progress, send coin flips
        if (game_in_progress) {
            send_coin_flip(server_fd, clients, &messages_sent, &total_message_length,
                           coin_sequence, &coin_sequence_length, addr_len);
        }
    }

    close(server_fd);
    return 0;
}

// Function definitions

void initialize_clients(ClientInfo clients[]) {
    for (int i = 0; i < MAX_CLIENTS; i++) {
        clients[i].registered = 0;
        clients[i].has_won = 0;
        clients[i].currently_playing = 0; // Initialize currently_playing to 0
    }
}

int find_client_index(ClientInfo clients[], int client_id) {
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].registered && clients[i].client_id == client_id) {
            return i;
        }
    }
    return -1;
}

int register_client(int server_fd, ClientInfo clients[], struct sockaddr_in client_addr, char buffer[],
                    int *max_pattern_length, socklen_t addr_len, int *next_client_id) {
    int client_index = -1;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (!clients[i].registered) {
            clients[i].client_id = (*next_client_id)++;
            clients[i].address = client_addr;
            // Store the pattern as received ('0's and '1's)
            strncpy(clients[i].pattern, buffer, MAX_PATTERN_LENGTH - 1);
            clients[i].pattern[MAX_PATTERN_LENGTH - 1] = '\0';
            clients[i].pattern_length = strlen(clients[i].pattern);
            clients[i].registered = 1;
            clients[i].has_won = 0;
            clients[i].currently_playing = 1; // Set currently_playing to 1 upon registration

            // Convert pattern to 'H's and 'T's for display
            for (int k = 0; k < clients[i].pattern_length; k++) {
                clients[i].pattern_display[k] = (clients[i].pattern[k] == '0') ? 'H' : 'T';
            }
            clients[i].pattern_display[clients[i].pattern_length] = '\0';

            client_index = i;
            printf("New client registered: %s:%d with pattern %s, assigned ID %d\n",
                   inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port),
                   clients[i].pattern_display, clients[i].client_id);

            // Send the client ID to the client
            char id_message[16];
            sprintf(id_message, "ID %d", clients[i].client_id);
            sendto(server_fd, id_message, strlen(id_message), 0, (struct sockaddr *)&client_addr, addr_len);

            // Update maximum pattern length
            if (clients[i].pattern_length > *max_pattern_length) {
                *max_pattern_length = clients[i].pattern_length;
            }

            break;
        }
    }
    return client_index;
}

void handle_client_message(int server_fd, ClientInfo clients[], PatternStats pattern_stats[],
                           int *pattern_stats_count, char buffer[], int valread,
                           struct sockaddr_in client_addr, int *messages_sent, int *messages_received,
                           int *total_message_length, int *game_in_progress, char coin_sequence[],
                           int *coin_sequence_length, int *completed_games, int *max_pattern_length,
                           socklen_t addr_len, int *next_client_id) {
    int client_id = -1;
    if (strncmp(buffer, "REGISTER", 8) == 0) {
        // New client registration
        char *pattern = buffer + 9; // Skip "REGISTER "
        register_client(server_fd, clients, client_addr, pattern, max_pattern_length, addr_len, next_client_id);
    } else if (strncmp(buffer, "ID", 2) == 0) {
        // Extract client ID
        sscanf(buffer + 3, "%d", &client_id);
        int client_index = find_client_index(clients, client_id);
        if (client_index != -1) {
            // Handle messages from registered clients
            if (strstr(buffer, "WIN") != NULL) {
                // Client claims to have won
                process_win_claim(server_fd, clients, client_index, coin_sequence, coin_sequence_length,
                                  pattern_stats, pattern_stats_count, messages_sent, messages_received, total_message_length,
                                  game_in_progress, completed_games, addr_len);
            } else if (strstr(buffer, "READY") != NULL) {
                // Client is ready to play again
                clients[client_index].currently_playing = 1;
                printf("Client ID %d is ready to play again.\n", client_id);
            }
        } else {
            printf("Received message from unknown client ID %d\n", client_id);
        }
    }

    // Check if enough clients are ready to start the game
    if (!(*game_in_progress)) {
        int ready_clients = 0;
        for (int j = 0; j < MAX_CLIENTS; j++) {
            if (clients[j].registered && clients[j].currently_playing) {
                ready_clients++;
            }
        }

        if (ready_clients >= MIN_PLAYERS) {
            printf("Minimum number of clients ready (%d). Starting game...\n", ready_clients);
            *game_in_progress = 1;
            *coin_sequence_length = 0;
            memset(coin_sequence, 0, sizeof(char) * 1024);

            // Reset clients' has_won flags at the start of the new game
            for (int j = 0; j < MAX_CLIENTS; j++) {
                if (clients[j].registered && clients[j].currently_playing) {
                    clients[j].has_won = 0;
                }
            }
        }
    }
}

void process_win_claim(int server_fd, ClientInfo clients[], int client_index,
                       char coin_sequence[], int *coin_sequence_length, PatternStats pattern_stats[],
                       int *pattern_stats_count, int *messages_sent, int *messages_received, int *total_message_length,
                       int *game_in_progress, int *completed_games, socklen_t addr_len) {
    if (clients[client_index].has_won) {
        // Client has already won
        return;
    }

    printf("Client %s:%d (ID %d) claims to have won.\n",
           inet_ntoa(clients[client_index].address.sin_addr),
           ntohs(clients[client_index].address.sin_port),
           clients[client_index].client_id);

    // Validate the client's claim
    int pattern_length = clients[client_index].pattern_length;
    if (*coin_sequence_length >= pattern_length) {
        int start_index = *coin_sequence_length - pattern_length;
        if (strncmp(&coin_sequence[start_index], clients[client_index].pattern, pattern_length) == 0) {
            // Client's pattern matches the coin sequence
            printf("Client %s:%d (ID %d) is validated as winner.\n",
                   inet_ntoa(clients[client_index].address.sin_addr),
                   ntohs(clients[client_index].address.sin_port),
                   clients[client_index].client_id);

            clients[client_index].has_won = 1;

            // Update statistics
            update_pattern_stats(pattern_stats, pattern_stats_count, clients[client_index], *coin_sequence_length, 1);

            // Send win message to the winner
            const char *win_message = "WIN";
            sendto(server_fd, win_message, strlen(win_message), 0,
                   (struct sockaddr *)&clients[client_index].address, addr_len);
            (*messages_sent)++;
            *total_message_length += strlen(win_message);

            // Inform all other clients that they have lost
            const char *lose_message = "LOSE";
            for (int i = 0; i < MAX_CLIENTS; i++) {
                if (clients[i].registered && i != client_index && !clients[i].has_won && clients[i].currently_playing) {
                    sendto(server_fd, lose_message, strlen(lose_message), 0,
                           (struct sockaddr *)&clients[i].address, addr_len);
                    (*messages_sent)++;
                    *total_message_length += strlen(lose_message);

                    // Print information about the client who lost
                    printf("Client %s:%d (ID %d) lost with pattern %s\n",
                           inet_ntoa(clients[i].address.sin_addr),
                           ntohs(clients[i].address.sin_port),
                           clients[i].client_id,
                           clients[i].pattern_display);

                    clients[i].has_won = 1; // Mark as having finished the game

                    // Update statistics for losing client
                    update_pattern_stats(pattern_stats, pattern_stats_count, clients[i], *coin_sequence_length, 0);
                }
            }

            // End the game
            *game_in_progress = 0;
            (*completed_games)++;

            // Set currently_playing to 0 for all clients
            for (int i = 0; i < MAX_CLIENTS; i++) {
                if (clients[i].registered) {
                    clients[i].currently_playing = 0;
                }
            }

            // Print diagnostics
            print_diagnostics(*messages_sent, *messages_received, *total_message_length, *completed_games);

            // Print statistics
            print_statistics(pattern_stats, *pattern_stats_count);

            // Reset the game state
            memset(coin_sequence, 0, sizeof(char) * 1024);
            *coin_sequence_length = 0;

        } else {
            // Invalid win claim
            const char *invalid_message = "INVALID WIN";
            sendto(server_fd, invalid_message, strlen(invalid_message), 0,
                   (struct sockaddr *)&clients[client_index].address, addr_len);
            (*messages_sent)++;
            *total_message_length += strlen(invalid_message);
            printf("Client %s:%d (ID %d) made an invalid win claim.\n",
                   inet_ntoa(clients[client_index].address.sin_addr),
                   ntohs(clients[client_index].address.sin_port),
                   clients[client_index].client_id);
        }
    } else {
        // Not enough coin flips yet
        const char *invalid_message = "INVALID WIN";
        sendto(server_fd, invalid_message, strlen(invalid_message), 0,
               (struct sockaddr *)&clients[client_index].address, addr_len);
        (*messages_sent)++;
        *total_message_length += strlen(invalid_message);
        printf("Client %s:%d (ID %d) made an invalid win claim (not enough flips).\n",
               inet_ntoa(clients[client_index].address.sin_addr),
               ntohs(clients[client_index].address.sin_port),
               clients[client_index].client_id);
    }
}

void update_pattern_stats(PatternStats pattern_stats[], int *pattern_stats_count,
                          ClientInfo client, int coin_sequence_length, int win) {
    int found = 0;
    for (int j = 0; j < *pattern_stats_count; j++) {
        if (strcmp(pattern_stats[j].pattern, client.pattern) == 0) {
            if (win) {
                pattern_stats[j].wins++;
            }
            pattern_stats[j].total_flips += coin_sequence_length;
            pattern_stats[j].total_games++;
            found = 1;
            break;
        }
    }
    if (!found) {
        // Add new pattern stats
        strcpy(pattern_stats[*pattern_stats_count].pattern, client.pattern);
        strcpy(pattern_stats[*pattern_stats_count].pattern_display, client.pattern_display);
        pattern_stats[*pattern_stats_count].wins = win ? 1 : 0;
        pattern_stats[*pattern_stats_count].total_flips = coin_sequence_length;
        pattern_stats[*pattern_stats_count].total_games = 1;
        (*pattern_stats_count)++;
    }
}

void print_diagnostics(int messages_sent, int messages_received, int total_message_length, int completed_games) {
    printf("\n--- Diagnostics ---\n");
    printf("Messages sent: %d\n", messages_sent);
    printf("Messages received: %d\n", messages_received);
    printf("Average message length: %.2f bytes\n",
           (messages_sent + messages_received) > 0
               ? (float)total_message_length / (messages_sent + messages_received)
               : 0.0);
    printf("Completed games: %d\n", completed_games);
}

void print_statistics(PatternStats pattern_stats[], int pattern_stats_count) {
    printf("\n--- Statistics ---\n");
    for (int j = 0; j < pattern_stats_count; j++) {
        float win_probability = (float)pattern_stats[j].wins / pattern_stats[j].total_games;
        float average_flips = (float)pattern_stats[j].total_flips / pattern_stats[j].total_games;
        printf("Pattern: %s, Wins: %d, Total Games: %d, Win Probability: %.2f, Average Flips: %.2f\n",
               pattern_stats[j].pattern_display, pattern_stats[j].wins,
               pattern_stats[j].total_games, win_probability, average_flips);
    }
    printf("-------------------\n");
}

void send_coin_flip(int server_fd, ClientInfo clients[], int *messages_sent, int *total_message_length,
                    char coin_sequence[], int *coin_sequence_length, socklen_t addr_len) {
    // Generate a random bit
    int rand_bit = rand() % 2;
    char coin_flip_char = rand_bit ? '1' : '0'; // Use '0' and '1'

    // Append the coin flip to the coin sequence
    if (*coin_sequence_length < 1023) { // Leave space for null terminator
        coin_sequence[(*coin_sequence_length)++] = coin_flip_char;
        coin_sequence[*coin_sequence_length] = '\0';
    } else {
        // Shift the sequence to make room
        memmove(coin_sequence, coin_sequence + 1, *coin_sequence_length - 1);
        coin_sequence[*coin_sequence_length - 1] = coin_flip_char;
    }

    // Send the coin flip to all clients who are currently playing
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].registered && clients[i].currently_playing) {
            sendto(server_fd, &coin_flip_char, 1, 0,
                   (struct sockaddr *)&clients[i].address, addr_len);
            (*messages_sent)++;
            *total_message_length += 1;
        }
    }
}
