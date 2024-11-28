#include <iostream>
#include <thread>
#include <vector>
#include <cstring>
#include <cstdlib>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>

// Define the maximum buffer size
#define MAX_BUFFER_SIZE 10000

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

    // First, receive 4 bytes indicating the flow size
    uint32_t net_flow_size;
    bool success = recv_all(client_sock, reinterpret_cast<char*>(&net_flow_size), sizeof(net_flow_size));
    if (!success) {
        std::cerr << "Failed to receive flow size from client " << client_ip << ":" << client_port << "." << std::endl;
        close(client_sock);
        return;
    }

    uint32_t flow_size = ntohl(net_flow_size);
    std::cout << "Flow size received from client " << client_ip << ":" << client_port << " - " << flow_size << " bytes." << std::endl;

    // Allocate buffer for fixed-size flow
    std::vector<char> buffer(flow_size, 0);

    while (true) {
        // Receive fixed-size flow
        success = recv_all(client_sock, buffer.data(), flow_size);
        if (!success) {
            std::cout << "Connection closed by client " << client_ip << ":" << client_port << "." << std::endl;
            break;
        }

        // Here you can process the received data as needed
        // For this example, we simply echo back the same data

        // Send the same fixed-size flow back to the client
        size_t total_sent = 0;
        while (total_sent < flow_size) {
            ssize_t bytes_sent = send(client_sock, buffer.data() + total_sent, flow_size - total_sent, 0);
            if (bytes_sent <= 0) {
                std::cerr << "Failed to send response to client " << client_ip << ":" << client_port << "." << std::endl;
                close(client_sock);
                return;
            }
            total_sent += bytes_sent;
        }

        // Optional: Print debug information
        std::cout << "Echoed back " << flow_size << " bytes to client " << client_ip << ":" << client_port << "." << std::endl;
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

    // Disable Nagle's algorithm to send data immediately
    if (setsockopt(server_sock, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt)) < 0) {
        std::cerr << "Failed to disable Nagle's algorithm." << std::endl;
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

        // Disable Nagle's algorithm for client socket
        int opt_nodelay = 1;
        if (setsockopt(client_sock, IPPROTO_TCP, TCP_NODELAY, &opt_nodelay, sizeof(opt_nodelay)) < 0) {
            std::cerr << "Failed to disable Nagle's algorithm for client socket." << std::endl;
            close(client_sock);
            continue;
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