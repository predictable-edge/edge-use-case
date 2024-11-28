#include <iostream>
#include <thread>
#include <vector>
#include <set>
#include <map>
#include <cstring>
#include <cstdlib>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

// Define the size of each packet
#define PACKET_SIZE 1500

/**
 * @brief Receives exactly 'length' bytes from the socket.
 *
 * @param sockfd The socket file descriptor.
 * @param buffer The buffer to store received data.
 * @param length The number of bytes to receive.
 * @return true if exactly 'length' bytes are received, false otherwise.
 */
bool recv_all(int sockfd, char* buffer, size_t length) {
    size_t total_received = 0;
    while (total_received < length) {
        ssize_t bytes_received = recv(sockfd, buffer + total_received, length - total_received, 0);
        if (bytes_received <= 0) {
            // Connection closed or error
            return false;
        }
        total_received += bytes_received;
    }
    return true;
}

/**
 * @brief Handles communication with a connected client.
 *
 * @param client_sock The client's socket file descriptor.
 * @param client_addr The client's address information.
 */
void handle_client(int client_sock, sockaddr_in client_addr) {
    char client_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &(client_addr.sin_addr), client_ip, INET_ADDRSTRLEN);
    uint16_t client_port = ntohs(client_addr.sin_port);
    std::cout << "Connected by " << client_ip << ":" << client_port << std::endl;

    // Map to keep track of received sequence numbers for each request
    std::map<uint32_t, std::set<uint32_t>> requests;

    while (true) {
        char data[PACKET_SIZE];
        // Receive exactly PACKET_SIZE bytes
        bool success = recv_all(client_sock, data, PACKET_SIZE);
        if (!success) {
            std::cout << "Connection closed by client " << client_ip << ":" << client_port << "." << std::endl;
            break; // Exit the loop and close the connection
        }

        // Extract fields from the received data
        uint32_t request_index = ntohl(*reinterpret_cast<uint32_t*>(data));
        uint32_t total_packets = ntohl(*reinterpret_cast<uint32_t*>(data + 4));
        uint32_t sequence_number = ntohl(*reinterpret_cast<uint32_t*>(data + 8));

        // Debug output
        // std::cout << "Received packet - Request Index: " << request_index
        //           << ", Total Packets: " << total_packets
        //           << ", Sequence Number: " << sequence_number << std::endl;

        // Initialize the set for the request index if not present
        if (requests.find(request_index) == requests.end()) {
            requests[request_index] = std::set<uint32_t>();
        }

        // Add the sequence number to the set
        requests[request_index].insert(sequence_number);

        // Check if all packets for the request have been received
        if (requests[request_index].size() == total_packets) {
            // Send response packets to the client
            for (uint32_t seq_num = 0; seq_num < total_packets; ++seq_num) {
                // Construct response packet
                std::vector<char> response(PACKET_SIZE, 0);
                uint32_t net_request_index = htonl(request_index);
                uint32_t net_total_packets = htonl(total_packets);
                uint32_t net_sequence_number = htonl(seq_num);

                // Copy the fields into the response buffer
                memcpy(response.data(), &net_request_index, 4);
                memcpy(response.data() + 4, &net_total_packets, 4);
                memcpy(response.data() + 8, &net_sequence_number, 4);

                // Send the response packet
                ssize_t bytes_sent = send(client_sock, response.data(), PACKET_SIZE, 0);
                if (bytes_sent != PACKET_SIZE) {
                    std::cerr << "Failed to send response packet for request " << request_index << ", sequence " << seq_num << "." << std::endl;
                    break;
                }
            }
            std::cout << "Responded to request " << request_index << " with " << total_packets << " packets." << std::endl;

            // Remove the request from the map as it has been handled
            requests.erase(request_index);
        }
    }

    // Close the client socket
    close(client_sock);
    std::cout << "Connection with " << client_ip << ":" << client_port << " closed.\nWaiting for new connections..." << std::endl;
}

/**
 * @brief Starts the TCP server on the specified port.
 *
 * @param port The port number to listen on. Default is 10000.
 */
void start_server(uint16_t port = 10000) {
    // Create a TCP socket
    int server_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (server_sock < 0) {
        std::cerr << "Failed to create socket." << std::endl;
        exit(EXIT_FAILURE);
    }

    // Set socket options to allow reuse of address and port
    int opt = 1;
    if (setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        std::cerr << "Failed to set socket options." << std::endl;
        close(server_sock);
        exit(EXIT_FAILURE);
    }

    // Bind the socket to all available interfaces on the specified port
    sockaddr_in server_addr;
    std::memset(&server_addr, 0, sizeof(server_addr)); // Zero out the structure
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY; // Bind to all interfaces
    server_addr.sin_port = htons(port);       // Convert port to network byte order

    if (bind(server_sock, reinterpret_cast<sockaddr*>(&server_addr), sizeof(server_addr)) < 0) {
        std::cerr << "Failed to bind socket to port " << port << "." << std::endl;
        close(server_sock);
        exit(EXIT_FAILURE);
    }

    // Listen for incoming connections
    if (listen(server_sock, SOMAXCONN) < 0) {
        std::cerr << "Failed to listen on port " << port << "." << std::endl;
        close(server_sock);
        exit(EXIT_FAILURE);
    }

    std::cout << "Server listening on port " << port << "." << std::endl;

    while (true) {
        // Accept a new connection
        sockaddr_in client_addr;
        socklen_t client_addr_len = sizeof(client_addr);
        int client_sock = accept(server_sock, reinterpret_cast<sockaddr*>(&client_addr), &client_addr_len);
        if (client_sock < 0) {
            std::cerr << "Failed to accept incoming connection." << std::endl;
            continue; // Continue accepting new connections
        }

        // Handle the client in a new thread
        std::thread client_thread(handle_client, client_sock, client_addr);
        client_thread.detach(); // Detach the thread to allow independent execution
    }

    // Close the server socket (unreachable code in this example)
    close(server_sock);
}

int main(int argc, char* argv[]) {
    // Default port
    uint16_t port = 10000;

    // Optional: Allow port to be specified via command-line arguments
    if (argc >= 2) {
        port = static_cast<uint16_t>(std::stoi(argv[1]));
    }

    // Start the server
    start_server(port);

    return 0;
}
