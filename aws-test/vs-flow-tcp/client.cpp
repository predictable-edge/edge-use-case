#include <iostream>
#include <fstream>
#include <cstring>
#include <thread>
#include <mutex>
#include <vector>
#include <deque>
#include <chrono>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#include <map>
#include <iomanip>

// Define the maximum buffer size
#define MAX_BUFFER_SIZE 10000

std::mutex mtx;

// Structure to keep track of sent flows
struct RequestInfo {
    uint32_t index;
    std::chrono::high_resolution_clock::time_point start_time;
};

/**
 * @brief Sends the flow size to the server once upon connection.
 *
 * @param client_socket The client's socket file descriptor.
 * @param flow_size The size of each flow in bytes.
 * @return true if the flow size is sent successfully, false otherwise.
 */
bool send_flow_size(int client_socket, uint32_t flow_size) {
    uint32_t net_flow_size = htonl(flow_size);
    ssize_t sent = send(client_socket, &net_flow_size, sizeof(net_flow_size), 0);
    if (sent != sizeof(net_flow_size)) {
        std::cerr << "Failed to send flow size to server." << std::endl;
        return false;
    }
    std::cout << "Sent flow size: " << flow_size << " bytes." << std::endl;
    return true;
}

/**
 * @brief Sends fixed-size flows to the server periodically.
 *
 * @param client_socket The client's socket file descriptor.
 * @param num_requests The number of flows to send.
 * @param flow_size The size of each flow in bytes.
 * @param interval_ms The interval between flows in milliseconds.
 * @param send_queue A deque to keep track of sent flows for latency measurement.
 * @param sending_done A flag indicating whether sending is completed.
 */
void send_flows(int client_socket, uint32_t num_requests, uint32_t flow_size, uint32_t interval_ms, std::deque<RequestInfo>& send_queue, bool& sending_done) {
    for (uint32_t request_index = 0; request_index < num_requests; ++request_index) {
        auto start_time = std::chrono::high_resolution_clock::now();

        {
            std::lock_guard<std::mutex> lock(mtx);
            send_queue.push_back({request_index, start_time});
        }

        // Prepare the payload (filled with zeros)
        std::vector<char> payload(flow_size, 0);

        // Send the payload
        size_t total_sent = 0;
        while (total_sent < flow_size) {
            ssize_t bytes_sent = send(client_socket, payload.data() + total_sent, flow_size - total_sent, 0);
            if (bytes_sent <= 0) {
                std::cerr << "Failed to send flow " << request_index << "." << std::endl;
                sending_done = true;
                return;
            }
            total_sent += bytes_sent;
        }

        // Calculate elapsed time and adjust sleep accordingly
        auto send_duration = std::chrono::high_resolution_clock::now() - start_time;
        int64_t elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(send_duration).count();
        int64_t remaining_time_ms = interval_ms - elapsed_ms;
        if (remaining_time_ms > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(remaining_time_ms));
        } else {
            std::cout << "Warning: Sending flow " << request_index << " took longer than the interval." << std::endl;
        }
    }
    sending_done = true;
}

/**
 * @brief Receives responses from the server and calculates latency.
 *
 * @param client_socket The client's socket file descriptor.
 * @param send_queue A deque containing information about sent flows.
 * @param latency_results A map to store latency results.
 * @param sending_done A flag indicating whether sending is completed.
 */
void receive_responses(int client_socket, std::deque<RequestInfo>& send_queue, std::map<uint32_t, double>& latency_results, bool& sending_done) {
    fd_set read_fds;
    timeval timeout;
    char buffer[MAX_BUFFER_SIZE];
    std::vector<char> recv_buffer;

    while (!sending_done || !send_queue.empty()) {
        FD_ZERO(&read_fds);
        FD_SET(client_socket, &read_fds);

        // Set timeout to 2 seconds
        timeout.tv_sec = 2;
        timeout.tv_usec = 0;

        int ret = select(client_socket + 1, &read_fds, nullptr, nullptr, &timeout);
        if (ret > 0 && FD_ISSET(client_socket, &read_fds)) {
            ssize_t bytes_received = recv(client_socket, buffer, sizeof(buffer), 0);
            if (bytes_received > 0) {
                recv_buffer.insert(recv_buffer.end(), buffer, buffer + bytes_received);

                while (recv_buffer.size() >= MAX_BUFFER_SIZE) {
                    std::vector<char> packet(recv_buffer.begin(), recv_buffer.begin() + MAX_BUFFER_SIZE);
                    recv_buffer.erase(recv_buffer.begin(), recv_buffer.begin() + MAX_BUFFER_SIZE);

                    // Process the received packet (in this case, we just acknowledge it)
                    // You can add verification if needed

                    RequestInfo req_info;
                    bool found = false;
                    {
                        std::lock_guard<std::mutex> lock(mtx);
                        if (!send_queue.empty()) {
                            req_info = send_queue.front();
                            send_queue.pop_front();
                            found = true;
                        }
                    }

                    if (found) {
                        // Calculate latency
                        auto end_time = std::chrono::high_resolution_clock::now();
                        double latency = std::chrono::duration<double, std::milli>(end_time - req_info.start_time).count();
                        latency_results[req_info.index] = latency;

                        std::cout << "Flow " << req_info.index << " latency: " << latency << " ms" << std::endl;
                    }
                }
            } else if (bytes_received == 0) {
                // Connection closed
                std::cerr << "Connection closed by server." << std::endl;
                break;
            } else {
                // Error occurred
                std::cerr << "Error receiving data from server." << std::endl;
                break;
            }
        } else {
            // Timeout or no data
            if (sending_done && send_queue.empty()) {
                break;
            }
        }
    }
}

int main(int argc, char* argv[]) {
    // Parse command-line arguments
    if (argc < 5) {
        std::cerr << "Usage: " << argv[0] << " <server_ip> <num_requests> <flow_size_bytes> <interval_ms> [--port PORT]" << std::endl;
        return 1;
    }

    std::string server_ip = argv[1];
    uint32_t num_requests = std::stoi(argv[2]);
    uint32_t flow_size = std::stoi(argv[3]);
    uint32_t interval_ms = std::stoi(argv[4]);

    uint16_t port = 10000;

    // Optional arguments
    for (int i = 5; i < argc; ++i) {
        if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            port = static_cast<uint16_t>(std::stoi(argv[++i]));
        }
    }

    // Create socket
    int client_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (client_socket < 0) {
        std::cerr << "Failed to create socket." << std::endl;
        return 1;
    }

    // Disable Nagle's algorithm to send data immediately
    int opt = 1;
    if (setsockopt(client_socket, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt)) < 0) {
        std::cerr << "Failed to disable Nagle's algorithm." << std::endl;
        close(client_socket);
        return 1;
    }

    // Connect to server
    sockaddr_in server_addr;
    std::memset(&server_addr, 0, sizeof(server_addr)); // Zero out the structure
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    if (inet_pton(AF_INET, server_ip.c_str(), &server_addr.sin_addr) <= 0) {
        std::cerr << "Invalid server IP address." << std::endl;
        close(client_socket);
        return 1;
    }

    if (connect(client_socket, reinterpret_cast<sockaddr*>(&server_addr), sizeof(server_addr)) < 0) {
        std::cerr << "Failed to connect to server." << std::endl;
        close(client_socket);
        return 1;
    }

    std::cout << "Connected to server " << server_ip << " on port " << port << "." << std::endl;

    // Send the flow size to the server once upon connection
    if (!send_flow_size(client_socket, flow_size)) {
        close(client_socket);
        return 1;
    }

    // Shared data structures
    std::deque<RequestInfo> send_queue;
    std::map<uint32_t, double> latency_results;
    bool sending_done = false;

    // Start sender and receiver threads
    std::thread sender_thread(send_flows, client_socket, num_requests, flow_size, interval_ms, std::ref(send_queue), std::ref(sending_done));
    std::thread receiver_thread(receive_responses, client_socket, std::ref(send_queue), std::ref(latency_results), std::ref(sending_done));

    sender_thread.join();
    receiver_thread.join();

    // Close socket
    close(client_socket);

    // Write latency results to file
    std::ofstream output_file("latency.txt");
    output_file << std::left << std::setw(10) << "Index" << std::setw(15) << "Latency(ms)" << std::endl;
    for (const auto& [index, latency] : latency_results) {
        output_file << std::left << std::setw(10) << index << std::setw(15) << std::fixed << std::setprecision(2) << latency << std::endl;
    }
    output_file.close();

    std::cout << "Latency results written to latency.txt" << std::endl;

    return 0;
}