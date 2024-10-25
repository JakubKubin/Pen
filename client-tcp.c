// client.c

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <ctype.h>
#include <time.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#define PORT 8080
#define BUFFER_SIZE 1024
#define MAX_PATTERN_LENGTH 32

int main() {
    int sock = 0, valread;
    struct sockaddr_in serv_addr;
    char pattern[MAX_PATTERN_LENGTH];
    char buffer[BUFFER_SIZE] = {0};
    char coin_flip;
    char sequence[BUFFER_SIZE] = {0};
    int flips = 0;
    int pattern_length;
    int sequence_length = 0;

    // Input pattern from user
    printf("Enter your pattern (e.g., HHT): ");
    scanf("%s", pattern);

    // Validate pattern
    pattern_length = strlen(pattern);
    if (pattern_length > MAX_PATTERN_LENGTH - 1) {
        printf("Pattern too long. Maximum length is %d\n", MAX_PATTERN_LENGTH - 1);
        return -1;
    }
    for (int i = 0; i < pattern_length; i++) {
        pattern[i] = toupper(pattern[i]);
        if (pattern[i] != 'H' && pattern[i] != 'T') {
            printf("Invalid character in pattern. Use only 'H' or 'T'.\n");
            return -1;
        }
    }

    // Create socket
    if ((sock = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        printf("Socket creation error\n");
        return -1;
    }

    // Set server address
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(PORT);

    // Convert IPv4 address from text to binary form
    if (inet_pton(AF_INET, "127.0.0.1", &serv_addr.sin_addr) <= 0) {
        printf("Invalid address/ Address not supported\n");
        return -1;
    }

    // Connect to server
    if (connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        printf("Connection Failed\n");
        return -1;
    }

    // Send pattern to server for registration, append '\n'
    sprintf(buffer, "%s\n", pattern);
    send(sock, buffer, strlen(buffer), 0);
    printf("Pattern '%s' sent to server for registration.\n", pattern);

    // Wait for game to start
    printf("Waiting for game to start...\n");

    // Initialize variables for pattern detection
    flips = 0;
    sequence_length = 0;

    while (1) {
        // Start receiving coin flips from the server
        valread = read(sock, &coin_flip, 1);
        if (valread > 0) {
            // Add coin flip to sequence
            sequence[sequence_length++] = coin_flip;
            flips++;

            // Print the coin flip
            printf("Received coin flip: %c\n", coin_flip);

            // Check if the latest part of the sequence matches the pattern
            if (sequence_length >= pattern_length) {
                if (strncmp(&sequence[sequence_length - pattern_length], pattern, pattern_length) == 0) {
                    // Pattern matched
                    printf("Your pattern '%s' occurred after %d flips!\n", pattern, flips);

                    // Prepare the message with pattern and flips required
                    sprintf(buffer, "WIN %s %d\n", pattern, flips);
                    // Send the message
                    send(sock, buffer, strlen(buffer), 0);
                    printf("Win reported to server.\n");

                    // Break to continue or exit
                    break;
                }
            }
        } else if (valread == 0) {
            // Server closed connection
            printf("Server closed the connection.\n");
            close(sock);
            return 0;
        } else {
            // Error in reading
            perror("Read error");
            close(sock);
            return -1;
        }
    }

    // Close the socket
    close(sock);
    printf("Connection closed.\n");

    return 0;
}
