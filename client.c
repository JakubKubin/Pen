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

#define PORT 8080
#define BUFFER_SIZE 256
#define MAX_PATTERN_LENGTH 16

int main() {
    int sock = 0, valread;
    struct sockaddr_in serv_addr;
    socklen_t addr_len = sizeof(struct sockaddr_in);
    char pattern[MAX_PATTERN_LENGTH];
    char pattern_binary[MAX_PATTERN_LENGTH]; // Pattern converted to '0's and '1's
    char buffer[BUFFER_SIZE];
    char coin_flip_char;
    char coin_flip;
    int pattern_length;

    // Input pattern from user
    printf("Enter your pattern (e.g., HHT): ");
    scanf("%s", pattern);

    // Validate and convert pattern to '0's and '1's
    pattern_length = strlen(pattern);
    if (pattern_length > MAX_PATTERN_LENGTH - 1) {
        printf("Pattern too long. Maximum length is %d\n", MAX_PATTERN_LENGTH - 1);
        return -1;
    }
    for (int i = 0; i < pattern_length; i++) {
        pattern[i] = toupper(pattern[i]);
        if (pattern[i] == 'H') {
            pattern_binary[i] = '0';
        } else if (pattern[i] == 'T') {
            pattern_binary[i] = '1';
        } else {
            printf("Invalid character in pattern. Use only 'H' or 'T'.\n");
            return -1;
        }
    }
    pattern_binary[pattern_length] = '\0'; // Null-terminate

    // Create UDP socket
    if ((sock = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
        printf("Socket creation error\n");
        return -1;
    }

    // Set server address
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(PORT);

    // Convert IPv4 address from text to binary form
    if (inet_pton(AF_INET, "127.0.0.1", &serv_addr.sin_addr) <= 0) {
        printf("Invalid address/ Address not supported\n");
        return -1;
    }

    // Send pattern to server for registration (send '0's and '1's)
    sendto(sock, pattern_binary, strlen(pattern_binary), 0, (const struct sockaddr *)&serv_addr, addr_len);
    printf("Pattern '%s' sent to server for registration.\n", pattern);

    // Wait for game to start
    printf("Waiting for game to start...\n");

    // Set socket timeout to prevent blocking indefinitely
    struct timeval timeout;
    timeout.tv_sec = 5;
    timeout.tv_usec = 0; // 5 seconds
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    int game_over = 0;

    while (!game_over) {
        // Receive data from the server
        valread = recvfrom(sock, buffer, sizeof(buffer) - 1, 0, NULL, NULL);
        if (valread > 0) {
            buffer[valread] = '\0';

            // Check if it's a win message
            if (strcmp(buffer, "WIN") == 0) {
                printf("You have won the game with pattern '%s'!\n", pattern);
                game_over = 1;
            } else {
                // Assume it's a coin flip
                coin_flip_char = buffer[0];

                // Convert '0'/'1' to 'H'/'T' for display
                if (coin_flip_char == '0') {
                    coin_flip = 'H';
                } else if (coin_flip_char == '1') {
                    coin_flip = 'T';
                } else {
                    printf("Received invalid coin flip from server.\n");
                    continue;
                }

                // Print the coin flip
                printf("Received coin flip: %c\n", coin_flip);
            }
        } else if (valread == 0 || (valread < 0 && errno == EWOULDBLOCK)) {
            // No data received, continue waiting
            continue;
        } else {
            // Error in receiving
            perror("recvfrom error");
            close(sock);
            return -1;
        }
    }

    // Close the socket
    close(sock);
    printf("Connection closed.\n");

    return 0;
}
