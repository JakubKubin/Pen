// client.c

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <ctype.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <stdint.h> // For uint8_t and uint16_t

#define PORT 8080
#define BUFFER_SIZE 256
#define MAX_PATTERN_LENGTH 8 // Now limited to 8 bits

// Message Codes
#define MSG_LOSE     0b00
#define MSG_WIN      0b01
#define MSG_REGISTER 0b10
#define MSG_READY    0b11

// Bit Masks and Shifts
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

// Function prototypes
int create_udp_socket();
void get_user_pattern(char *pattern, uint8_t *pattern_binary, int *pattern_length);
void set_server_address(struct sockaddr_in *serv_addr);
void register_with_server(int sock, struct sockaddr_in *serv_addr, socklen_t addr_len, uint8_t pattern_binary, int pattern_length, uint8_t *client_id);
void game_loop(int sock, struct sockaddr_in *serv_addr, socklen_t addr_len, char *pattern, uint8_t pattern_binary, int pattern_length, uint8_t client_id);
uint16_t create_client_message(uint8_t message_code, uint8_t client_id, uint8_t sequence, uint8_t pattern_length);
void parse_server_message(uint16_t message, uint8_t *toss, uint8_t *message_code, uint8_t *client_id);

int main() {
    int sock = 0;
    struct sockaddr_in serv_addr;
    socklen_t addr_len = sizeof(struct sockaddr_in);
    char pattern[MAX_PATTERN_LENGTH + 1]; // User's pattern
    uint8_t pattern_binary = 0; // Pattern converted to binary
    int pattern_length;
    uint8_t client_id = 0;

    // Create UDP socket
    sock = create_udp_socket();

    // Get pattern from user and convert it
    get_user_pattern(pattern, &pattern_binary, &pattern_length);

    // Set server address
    set_server_address(&serv_addr);

    // Register with server
    register_with_server(sock, &serv_addr, addr_len, pattern_binary, pattern_length, &client_id);

    // Start game loop
    game_loop(sock, &serv_addr, addr_len, pattern, pattern_binary, pattern_length, client_id);

    // Close the socket
    close(sock);
    printf("Connection closed.\n");

    return 0;
}

// Function to create a UDP socket
int create_udp_socket() {
    int sock;
    if ((sock = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
        perror("Socket creation error");
        exit(EXIT_FAILURE);
    }
    return sock;
}

// Function to get user pattern and convert it to binary
void get_user_pattern(char *pattern, uint8_t *pattern_binary, int *pattern_length) {
    printf("Enter your pattern (max %d characters, e.g., HHT): ", MAX_PATTERN_LENGTH);
    scanf("%s", pattern);

    // Validate and convert pattern to binary
    *pattern_length = strlen(pattern);
    if (*pattern_length > MAX_PATTERN_LENGTH) {
        printf("Pattern too long. Maximum length is %d\n", MAX_PATTERN_LENGTH);
        exit(EXIT_FAILURE);
    }
    *pattern_binary = 0;
    for (int i = 0; i < *pattern_length; i++) {
        pattern[i] = toupper(pattern[i]);
        if (pattern[i] == 'H') {
            *pattern_binary = (*pattern_binary << 1) | 0;
        } else if (pattern[i] == 'T') {
            *pattern_binary = (*pattern_binary << 1) | 1;
        } else {
            printf("Invalid character in pattern. Use only 'H' or 'T'.\n");
            exit(EXIT_FAILURE);
        }
    }
    // Remove left-aligning
    // *pattern_binary <<= (8 - *pattern_length);
}

// Function to set the server address
void set_server_address(struct sockaddr_in *serv_addr) {
    memset(serv_addr, 0, sizeof(struct sockaddr_in));
    serv_addr->sin_family = AF_INET;
    serv_addr->sin_port = htons(PORT);

    // Convert IPv4 address from text to binary form
    if (inet_pton(AF_INET, "127.0.0.1", &serv_addr->sin_addr) <= 0) {
        printf("Invalid address/ Address not supported\n");
        exit(EXIT_FAILURE);
    }
}

// Function to register with the server
void register_with_server(int sock, struct sockaddr_in *serv_addr, socklen_t addr_len, uint8_t pattern_binary, int pattern_length, uint8_t *client_id) {
    uint16_t message;
    ssize_t valread;

    // Create registration message
    message = create_client_message(MSG_REGISTER, 0, pattern_binary, pattern_length);

    // Send registration message to server
    sendto(sock, &message, sizeof(message), 0, (const struct sockaddr *)serv_addr, addr_len);
    printf("Pattern sent to server for registration.\n");

    // Receive client ID from server
    valread = recvfrom(sock, &message, sizeof(message), 0, NULL, NULL);
    if (valread > 0) {
        uint8_t toss, message_code, server_client_id;
        parse_server_message(message, &toss, &message_code, &server_client_id);
        if (message_code == MSG_REGISTER) {
            *client_id = server_client_id;
            printf("Received client ID: %d\n", *client_id);
        } else {
            printf("Failed to receive client ID from server.\n");
            close(sock);
            exit(EXIT_FAILURE);
        }
    } else {
        perror("Failed to receive client ID from server");
        close(sock);
        exit(EXIT_FAILURE);
    }
}

// Main game loop function
void game_loop(int sock, struct sockaddr_in *serv_addr, socklen_t addr_len, char *pattern, uint8_t pattern_binary, int pattern_length, uint8_t client_id) {
    uint16_t message;
    ssize_t valread;
    int flips = 0;
    int game_over = 0;

    // Wait for game to start
    printf("Waiting for game to start...\n");

    // Set socket timeout to prevent blocking indefinitely
    struct timeval timeout;
    timeout.tv_sec = 5;
    timeout.tv_usec = 0; // 5 seconds
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    uint8_t sequence_buffer = 0;

    while (1) {
        while (!game_over) {
            // Receive data from the server
            valread = recvfrom(sock, &message, sizeof(message), 0, NULL, NULL);
            if (valread > 0) {
                uint8_t toss, message_code, server_client_id;

                parse_server_message(message, &toss, &message_code, &server_client_id);
                if (message_code == MSG_LOSE && server_client_id == client_id) {
                    printf("You have lost the game after %d flips. Better luck next time!\n", flips);
                    game_over = 1;
                    break;
                }
                if (!game_over) {
                    // This is a coin flip message
                    flips++;
                    // Convert toss bit to 'H' or 'T'
                    char coin_flip = (toss == 0) ? 'H' : 'T';

                    printf("Received coin flip: %c\n", coin_flip);
                    // Parse the message
                    // Update sequence buffer to keep last pattern_length bits
                    sequence_buffer = ((sequence_buffer << 1) | toss) & ((1 << pattern_length) - 1);

                    if (flips >= pattern_length) {
                        if (sequence_buffer == pattern_binary) {
                            // Send WIN message to server
                            uint16_t win_message = create_client_message(MSG_WIN, client_id, 0, pattern_length);
                            sendto(sock, &win_message, sizeof(win_message), 0, (const struct sockaddr *)serv_addr, addr_len);
                            printf("Your pattern '%s' occurred after %d flips. Claiming win...\n", pattern, flips);
                            game_over = 1;
                        }
                    }
                }
                
            }
        }
        // After game over
        // Prompt the user to play again
        char choice;
        printf("Do you want to play again? (y/n): ");
        scanf(" %c", &choice);
        if (choice == 'y' || choice == 'Y') {
            // Send READY message to the server
            uint16_t ready_message = create_client_message(MSG_READY, client_id, 0, pattern_length);
            sendto(sock, &ready_message, sizeof(ready_message), 0, (const struct sockaddr *)serv_addr, addr_len);
            printf("Sent READY message to server.\n");
            // Reset game variables
            game_over = 0;
            flips = 0;
            sequence_buffer = 0;
            printf("Waiting for game to start...\n");
        } else {
            // Close the socket and exit
            break;
        }
    }
}

// Function to create a client message according to the ALP protocol
uint16_t create_client_message(uint8_t message_code, uint8_t client_id, uint8_t sequence, uint8_t pattern_length) {
    uint16_t message = 0;
    // Transmitter flag is 0 (client)
    // Toss bit is 0
    // Set message code in bits 13-12
    message |= (message_code & 0b11) << BITS_MESSAGE;

    if (message_code == MSG_REGISTER) {
        // In registration, encode pattern length in bits 11-9
        message |= ((pattern_length - 1) & 0b111) << 9;
        // Bit 8 is unused and set to 0
    } else {
        // Set client ID in bits 11-8
        message |= (client_id & 0b1111) << BITS_CLIENT_ID;
    }

    // Set sequence/pattern in bits 7-0
    message |= (sequence & 0xFF);

    return htons(message); // Convert to network byte order
}

// Function to parse a server message according to the ALP protocol
void parse_server_message(uint16_t message, uint8_t *toss, uint8_t *message_code, uint8_t *client_id) {
    // Convert message from network byte order to host byte order
    message = ntohs(message);
    // Extract bits according to the protocol
    uint8_t transmitter_flag = (message >> BIT_TRANSMITTER) & 0b1;
    *toss = (message >> BIT_TOSS) & 0b1;
    *message_code = (message >> BITS_MESSAGE) & 0b11;
    *client_id = (message >> BITS_CLIENT_ID) & 0b1111;
    // Bits 7-0 (sequence) are ignored for server messages
    // printf("Parsed server message:\n");
    // printf("  Transmitter Flag: %d\n", transmitter_flag);
    // printf("  Toss: %d\n", *toss);
    // printf("  Message Code: %d\n", *message_code);
    // printf("  Client ID: %d\n", *client_id);
}
