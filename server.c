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
#include <stdint.h> // For uint8_t and uint16_t

#define PORT 8080
#define MAX_CLIENTS 15 // Due to 4-bit client IDs
#define MAX_PATTERN_LENGTH 8
#define MIN_PLAYERS 2

// Message Codes
#define MSG_LOSE     0b00
#define MSG_WIN      0b01
#define MSG_REGISTER 0b10
#define MSG_READY    0b11
#define MSG_TOSSING  0b11

// Bit Positions and Masks
#define BIT_TRANSMITTER 15
#define BIT_TOSS        14
#define BITS_MESSAGE    12
#define BITS_CLIENT_ID  8
#define BITS_SEQUENCE   0

#define MASK_TRANSMITTER (1 << BIT_TRANSMITTER)
#define MASK_TOSS        (1 << BIT_TOSS)
#define MASK_MESSAGE     (0b11 << BITS_MESSAGE)
#define MASK_CLIENT_ID   (0b1111 << BITS_CLIENT_ID)
#define MASK_SEQUENCE    (0xFF << BITS_SEQUENCE)

// Structure to hold client information
typedef struct {
    uint8_t client_id;  // Unique client ID (4 bits)
    struct sockaddr_in address;
    uint8_t pattern;    // 8-bit pattern
    int pattern_length;
    int registered;
    int has_won;
    int currently_playing; // Variable to track if the client is playing in the current game
} ClientInfo;

// Structure to hold statistics for patterns
typedef struct {
    uint8_t pattern; // 8-bit pattern
    int pattern_length;
    int wins;
    int total_games;
    int total_flips;
} PatternStats;

// Function prototypes
void initialize_clients(ClientInfo clients[]);
int find_client_index(ClientInfo clients[], uint8_t client_id);
void register_client(int server_fd, ClientInfo clients[], struct sockaddr_in client_addr, uint16_t message,
                     socklen_t addr_len, uint8_t *next_client_id);
void handle_client_message(int server_fd, ClientInfo clients[], PatternStats pattern_stats[],
                           int *pattern_stats_count, uint16_t message,
                           struct sockaddr_in client_addr, int *game_in_progress, uint8_t coin_sequence[],
                           int *coin_sequence_length, int *completed_games,
                           socklen_t addr_len, uint8_t *next_client_id);
void process_win_claim(int server_fd, ClientInfo clients[], int client_index,
                       uint8_t coin_sequence[], int coin_sequence_length, PatternStats pattern_stats[],
                       int *pattern_stats_count, int *game_in_progress, int *completed_games, socklen_t addr_len);
void update_pattern_stats(PatternStats pattern_stats[], int *pattern_stats_count,
                          ClientInfo client, int coin_sequence_length, int win);
void send_coin_flip(int server_fd, ClientInfo clients[], uint8_t coin_sequence[],
                    int *coin_sequence_length, socklen_t addr_len);
uint16_t create_server_message(uint8_t toss, uint8_t message_code, uint8_t client_id);
void parse_client_message(uint16_t message, uint8_t *message_code, uint8_t *client_id, uint8_t *sequence);
void print_diagnostics(int completed_games);
void print_statistics(PatternStats pattern_stats[], int pattern_stats_count);

int main() {
    int server_fd;
    struct sockaddr_in server_addr, client_addr;
    socklen_t addr_len = sizeof(struct sockaddr_in);
    uint16_t message;

    // For diagnostics
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

    // Sequence buffer to store coin flips
    uint8_t coin_sequence[1024]; // Store entire sequence for validation
    int coin_sequence_length = 0;

    fd_set readfds;
    struct timeval timeout;

    uint8_t next_client_id = 1; // Counter for assigning unique client IDs

    while (1) {
        // Set timeout for select
        timeout.tv_sec = 0;
        timeout.tv_usec = 10000; // 100 milliseconds

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
            ssize_t valread = recvfrom(server_fd, &message, sizeof(message), 0,
                                       (struct sockaddr *)&client_addr, &addr_len);
            if (valread > 0) {
                handle_client_message(server_fd, clients, pattern_stats, &pattern_stats_count,
                                      message, client_addr, &game_in_progress,
                                      coin_sequence, &coin_sequence_length, &completed_games,
                                      addr_len, &next_client_id);
            }
        }

        // If game is in progress, send coin flips
        if (game_in_progress) {
            send_coin_flip(server_fd, clients, coin_sequence, &coin_sequence_length, addr_len);
        }
    }

    close(server_fd);
    return 0;
}

// Function to initialize client array
void initialize_clients(ClientInfo clients[]) {
    for (int i = 0; i < MAX_CLIENTS; i++) {
        clients[i].registered = 0;
        clients[i].has_won = 0;
        clients[i].currently_playing = 0;
    }
}

// Function to find client index based on client ID
int find_client_index(ClientInfo clients[], uint8_t client_id) {
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].registered && clients[i].client_id == client_id) {
            return i;
        }
    }
    return -1;
}

// Function to register a new client
void register_client(int server_fd, ClientInfo clients[], struct sockaddr_in client_addr, uint16_t message,
                     socklen_t addr_len, uint8_t *next_client_id) {
    uint8_t message_code, client_id, sequence;
    parse_client_message(message, &message_code, &client_id, &sequence);

    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (!clients[i].registered) {
            clients[i].client_id = (*next_client_id)++;
            clients[i].address = client_addr;
            clients[i].pattern = sequence;
            clients[i].pattern_length = 8 - __builtin_clz(sequence); // Calculate pattern length
            clients[i].registered = 1;
            clients[i].has_won = 0;
            clients[i].currently_playing = 1;

            printf("New client registered: %s:%d, assigned ID %d\n",
                   inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port),
                   clients[i].client_id);

            // Send the client ID to the client
            uint16_t id_message = create_server_message(0, MSG_REGISTER, clients[i].client_id);
            sendto(server_fd, &id_message, sizeof(id_message), 0, (struct sockaddr *)&client_addr, addr_len);

            break;
        }
    }
}

// Function to handle messages received from clients
void handle_client_message(int server_fd, ClientInfo clients[], PatternStats pattern_stats[],
                           int *pattern_stats_count, uint16_t message,
                           struct sockaddr_in client_addr, int *game_in_progress, uint8_t coin_sequence[],
                           int *coin_sequence_length, int *completed_games,
                           socklen_t addr_len, uint8_t *next_client_id) {
    uint8_t message_code, client_id, sequence;
    parse_client_message(message, &message_code, &client_id, &sequence);
    printf("Received message from client ID %d with message code %d\n", client_id, message_code);


    if (message_code == MSG_REGISTER) {
        // New client registration
        register_client(server_fd, clients, client_addr, message, addr_len, next_client_id);
    } else {
        int client_index = find_client_index(clients, client_id);
        if (client_index != -1) {
            // Handle messages from registered clients
            if (message_code == MSG_WIN) {
                // Client claims to have won
                process_win_claim(server_fd, clients, client_index, coin_sequence, *coin_sequence_length,
                                  pattern_stats, pattern_stats_count, game_in_progress, completed_games, addr_len);
            } else if (message_code == MSG_READY) {
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
            memset(coin_sequence, 0, sizeof(uint8_t) * 1024);

            // Reset clients' has_won flags at the start of the new game
            for (int j = 0; j < MAX_CLIENTS; j++) {
                if (clients[j].registered && clients[j].currently_playing) {
                    clients[j].has_won = 0;
                }
            }
        }
    }
}

// Function to process a win claim from a client
void process_win_claim(int server_fd, ClientInfo clients[], int client_index,
                       uint8_t coin_sequence[], int coin_sequence_length, PatternStats pattern_stats[],
                       int *pattern_stats_count, int *game_in_progress, int *completed_games, socklen_t addr_len) {
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
    if (coin_sequence_length >= pattern_length) {
        uint8_t pattern_mask = (1 << pattern_length) - 1;
        uint8_t sequence_pattern = 0;
        for (int i = coin_sequence_length - pattern_length; i < coin_sequence_length; i++) {
            sequence_pattern = (sequence_pattern << 1) | coin_sequence[i];
        }
        if (sequence_pattern == (clients[client_index].pattern >> (8 - pattern_length))) {
            // Client's pattern matches the coin sequence
            printf("Client %s:%d (ID %d) is validated as winner.\n",
                   inet_ntoa(clients[client_index].address.sin_addr),
                   ntohs(clients[client_index].address.sin_port),
                   clients[client_index].client_id);

            clients[client_index].has_won = 1;

            // Update statistics
            update_pattern_stats(pattern_stats, pattern_stats_count, clients[client_index], coin_sequence_length, 1);

            // Send win message to the winner
            uint16_t win_message = create_server_message(0, MSG_WIN, clients[client_index].client_id);
            sendto(server_fd, &win_message, sizeof(win_message), 0,
                   (struct sockaddr *)&clients[client_index].address, addr_len);

            // Inform all other clients that they have lost
            for (int i = 0; i < MAX_CLIENTS; i++) {
                if (clients[i].registered && i != client_index && !clients[i].has_won && clients[i].currently_playing) {
                    uint16_t lose_message = create_server_message(0, MSG_LOSE, clients[i].client_id);
                    sendto(server_fd, &lose_message, sizeof(lose_message), 0,
                           (struct sockaddr *)&clients[i].address, addr_len);

                    // Print information about the client who lost
                    printf("Client %s:%d (ID %d) lost.\n",
                           inet_ntoa(clients[i].address.sin_addr),
                           ntohs(clients[i].address.sin_port),
                           clients[i].client_id);

                    clients[i].has_won = 1; // Mark as having finished the game

                    // Update statistics for losing client
                    update_pattern_stats(pattern_stats, pattern_stats_count, clients[i], coin_sequence_length, 0);
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

            // Print diagnostics and statistics
            print_diagnostics(*completed_games);
            print_statistics(pattern_stats, *pattern_stats_count);

            // Reset the game state
            memset(coin_sequence, 0, sizeof(uint8_t) * 1024);
            coin_sequence_length = 0;

        } else {
            // Invalid win claim
            printf("Client %s:%d (ID %d) made an invalid win claim.\n",
                   inet_ntoa(clients[client_index].address.sin_addr),
                   ntohs(clients[client_index].address.sin_port),
                   clients[client_index].client_id);
        }
    } else {
        // Not enough coin flips yet
        printf("Client %s:%d (ID %d) made an invalid win claim (not enough flips).\n",
               inet_ntoa(clients[client_index].address.sin_addr),
               ntohs(clients[client_index].address.sin_port),
               clients[client_index].client_id);
    }
}

// Function to update pattern statistics
void update_pattern_stats(PatternStats pattern_stats[], int *pattern_stats_count,
                          ClientInfo client, int coin_sequence_length, int win) {
    int found = 0;
    for (int j = 0; j < *pattern_stats_count; j++) {
        if (pattern_stats[j].pattern == client.pattern && pattern_stats[j].pattern_length == client.pattern_length) {
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
        pattern_stats[*pattern_stats_count].pattern = client.pattern;
        pattern_stats[*pattern_stats_count].pattern_length = client.pattern_length;
        pattern_stats[*pattern_stats_count].wins = win ? 1 : 0;
        pattern_stats[*pattern_stats_count].total_flips = coin_sequence_length;
        pattern_stats[*pattern_stats_count].total_games = 1;
        (*pattern_stats_count)++;
    }
}

// Function to send a coin flip to clients
void send_coin_flip(int server_fd, ClientInfo clients[], uint8_t coin_sequence[],
                    int *coin_sequence_length, socklen_t addr_len) {
    // Generate a random bit (0 or 1)
    uint8_t rand_bit = rand() % 2;
    char coin_flip_char = rand_bit ? '1' : '0'; // Use '0' and '1'
    // Append the coin flip to the coin sequence
    coin_sequence[(*coin_sequence_length)++] = rand_bit;

    // Send the coin flip to all clients who are currently playing
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].registered && clients[i].currently_playing) {
            uint16_t message = create_server_message(rand_bit, MSG_TOSSING, clients[i].client_id);
            sendto(server_fd, &message, sizeof(message), 0,
                   (struct sockaddr *)&clients[i].address, addr_len);
        }
    }
    // Print the coin flip in 'H' or 'T'
    //char coin_display = (coin_flip_char == '0') ? 'H' : 'T';
    //printf("Coin flip: %c\n", coin_display);
}

// Function to create a server message according to the ALP protocol
uint16_t create_server_message(uint8_t toss, uint8_t message_code, uint8_t client_id) {
    uint16_t message = 0;
    // Transmitter flag is 1 (server)
    message |= 1 << BIT_TRANSMITTER;
    // Set toss bit
    message |= (toss & 0b1) << BIT_TOSS;
    // Set message code in bits 13-12
    message |= (message_code & 0b11) << BITS_MESSAGE;
    // Set client ID in bits 11-8
    message |= (client_id & 0b1111) << BITS_CLIENT_ID;
    // Bits 7-0 are unused
    return htons(message); // Convert to network byte order
}

// Function to parse a client message according to the ALP protocol
void parse_client_message(uint16_t message, uint8_t *message_code, uint8_t *client_id, uint8_t *sequence) {
    // Convert message from network byte order to host byte order
    message = ntohs(message);
    // Extract bits according to the protocol
    uint8_t transmitter_flag = (message >> BIT_TRANSMITTER) & 0b1;
    // Ignore toss bit (should be 0)
    *message_code = (message >> BITS_MESSAGE) & 0b11;
    *client_id = (message >> BITS_CLIENT_ID) & 0b1111;
    *sequence = (message >> BITS_SEQUENCE) & 0xFF;
    printf("Parsed client message:\n");
    printf("  Transmitter Flag: %d\n", transmitter_flag);
    printf("  Message Code: %d\n", *message_code);
    printf("  Client ID: %d\n", *client_id);
    printf("  Sequence: %d\n", *sequence);
}

// Function to print diagnostics
void print_diagnostics(int completed_games) {
    printf("\n--- Diagnostics ---\n");
    printf("Completed games: %d\n", completed_games);
    printf("-------------------\n");
}

// Function to print statistics
void print_statistics(PatternStats pattern_stats[], int pattern_stats_count) {
    printf("\n--- Statistics ---\n");
    for (int j = 0; j < pattern_stats_count; j++) {
        float win_probability = (float)pattern_stats[j].wins / pattern_stats[j].total_games;
        float average_flips = (float)pattern_stats[j].total_flips / pattern_stats[j].total_games;

        // Convert pattern to displayable format ('H' and 'T')
        char pattern_display[MAX_PATTERN_LENGTH + 1];
        uint8_t pattern = pattern_stats[j].pattern;
        int length = pattern_stats[j].pattern_length;
        for (int i = 0; i < length; i++) {
            uint8_t bit = (pattern >> (8 - length + i)) & 0b1;
            pattern_display[i] = (bit == 0) ? 'H' : 'T';
        }
        pattern_display[length] = '\0';

        printf("Pattern: %s, Wins: %d, Total Games: %d, Win Probability: %.2f, Average Flips: %.2f\n",
               pattern_display, pattern_stats[j].wins,
               pattern_stats[j].total_games, win_probability, average_flips);
    }
    printf("-------------------\n");
}
