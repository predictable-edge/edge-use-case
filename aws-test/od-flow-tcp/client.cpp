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
#include <sys/socket.h>
#include <unistd.h>
#include <map>
#include <iomanip>

#define DEFAULT_PAYLOAD_SIZE 1400

std::mutex mtx;

struct RequestInfo {
    uint32_t index;
    std::chrono::high_resolution_clock::time_point start_time;
};

// Function to send payload size to the server
bool send_payload_size(int client_socket, uint32_t payload_size) {
    uint32_t net_payload_size = htonl(payload_size);
    ssize_t sent = send(client_socket, &net_payload_size, sizeof(net_payload_size), 0);
    if (sent != sizeof(net_payload_size)) {
        std::cerr << "Failed to send payload size to server." << std::endl;
        return false;
    }
    std::cout << "Sent payload size: " << payload_size << " bytes" << std::endl;
    return true;
}

// Function to send payloads periodically to the server
void send_payloads(int client_socket, uint32_t num_requests, uint32_t payload_size, uint32_t interval_ms, std::deque<RequestInfo>& send_queue, bool& sending_done) {
    for (uint32_t request_index = 0; request_index < num_requests; ++request_index) {
        auto start_time = std::chrono::high_resolution_clock::now();

        // Prepare the payload (filled with zeros)
        std::vector<char> payload(payload_size, 0);

        // Send the payload
        size_t total_sent = 0;
        while (total_sent < payload_size) {
            ssize_t bytes_sent = send(client_socket, payload.data() + total_sent, payload_size - total_sent, 0);
            if (bytes_sent <= 0) {
                std::cerr << "Failed to send payload for request " << request_index << std::endl;
                sending_done = true;
                return;
            }
            total_sent += bytes_sent;
        }

        {
            std::lock_guard<std::mutex> lock(mtx);
            send_queue.push_back({request_index, start_time});
        }

        // Calculate elapsed time and adjust sleep accordingly
        auto send_duration = std::chrono::high_resolution_clock::now() - start_time;
        int64_t remaining_time_ms = interval_ms - std::chrono::duration_cast<std::chrono::milliseconds>(send_duration).count();
        if (remaining_time_ms > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(remaining_time_ms));
        } else {
            std::cout << "Warning: Sending request " << request_index << " took longer than the interval." << std::endl;
        }
    }
    sending_done = true;
}

// Function to receive responses from the server
void receive_responses(int client_socket, std::deque<RequestInfo>& send_queue, std::map<uint32_t, double>& latency_results, bool& sending_done) {
    fd_set read_fds;
    timeval timeout;
    char buffer[4];

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
                if (bytes_received != 4) {
                    std::cerr << "Received unexpected response size: " << bytes_received << " bytes." << std::endl;
                    continue;
                }

                // Check if response is 'OKAY'
                if (memcmp(buffer, "OKAY", 4) != 0) {
                    std::cerr << "Received invalid response from server." << std::endl;
                    continue;
                }

                RequestInfo req_info;
                {
                    std::lock_guard<std::mutex> lock(mtx);
                    if (!send_queue.empty()) {
                        req_info = send_queue.front();
                        send_queue.pop_front();
                    } else {
                        std::cerr << "No matching request info for the received response." << std::endl;
                        continue;
                    }
                }

                // Calculate latency
                auto end_time = std::chrono::high_resolution_clock::now();
                double latency = std::chrono::duration<double, std::milli>(end_time - req_info.start_time).count();
                latency_results[req_info.index] = latency;

                std::cout << "Request " << req_info.index << " latency: " << latency << " ms" << std::endl;
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
        std::cerr << "Usage: " << argv[0] << " <server_ip> <num_requests> <payload_size_bytes> <interval_ms> [--port PORT]" << std::endl;
        return 1;
    }

    std::string server_ip = argv[1];
    uint32_t num_requests = std::stoi(argv[2]);
    uint32_t payload_size = std::stoi(argv[3]);
    uint32_t interval_ms = std::stoi(argv[4]);

    uint16_t port = 10000;

    // Optional arguments
    for (int i = 5; i < argc; ++i) {
        if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            port = std::stoi(argv[++i]);
        }
    }

    // Create socket
    int client_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (client_socket < 0) {
        std::cerr << "Failed to create socket." << std::endl;
        return 1;
    }

    // Connect to server
    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    if (inet_pton(AF_INET, server_ip.c_str(), &server_addr.sin_addr) <= 0) {
        std::cerr << "Invalid server IP address." << std::endl;
        close(client_socket);
        return 1;
    }

    if (connect(client_socket, (sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        std::cerr << "Failed to connect to server." << std::endl;
        close(client_socket);
        return 1;
    }

    std::cout << "Connected to server " << server_ip << " on port " << port << std::endl;

    // Send the payload size to the server once upon connection
    if (!send_payload_size(client_socket, payload_size)) {
        close(client_socket);
        return 1;
    }

    // Shared data structures
    std::deque<RequestInfo> send_queue;
    std::map<uint32_t, double> latency_results;
    bool sending_done = false;

    // Start sender and receiver threads
    std::thread sender_thread(send_payloads, client_socket, num_requests, payload_size, interval_ms, std::ref(send_queue), std::ref(sending_done));
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