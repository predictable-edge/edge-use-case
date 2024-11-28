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
#include <set>

#define PACKET_SIZE 1400

std::mutex mtx;

struct RequestInfo {
    uint32_t index;
    std::chrono::high_resolution_clock::time_point start_time;
};

void send_requests(int client_socket, uint32_t num_requests, uint32_t packets_per_request, uint32_t interval_ms, uint32_t packet_size, std::map<uint32_t, RequestInfo>& request_info_map, bool& sending_done) {
    for (uint32_t request_index = 0; request_index < num_requests; ++request_index) {
        auto start_time = std::chrono::high_resolution_clock::now();

        // Send packets for current request
        for (uint32_t sequence_number = 0; sequence_number < packets_per_request; ++sequence_number) {
            std::vector<char> data(packet_size, 0);

            uint32_t net_request_index = htonl(request_index);
            uint32_t net_packets_per_request = htonl(packets_per_request);
            uint32_t net_sequence_number = htonl(sequence_number);

            memcpy(data.data(), &net_request_index, 4);
            memcpy(data.data() + 4, &net_packets_per_request, 4);
            memcpy(data.data() + 8, &net_sequence_number, 4);

            send(client_socket, data.data(), packet_size, 0);
        }

        {
            std::lock_guard<std::mutex> lock(mtx);
            request_info_map[request_index] = {request_index, start_time};
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

void receive_responses(int client_socket, std::map<uint32_t, RequestInfo>& request_info_map, std::map<uint32_t, double>& latency_results, bool& sending_done) {
    fd_set read_fds;
    timeval timeout;
    char buffer[1500];
    std::vector<char> recv_buffer;
    
    // Map to keep track of received packets per request
    std::map<uint32_t, std::set<uint32_t>> received_packets;
    std::map<uint32_t, uint32_t> total_packets_map;

    while (!sending_done || !request_info_map.empty()) {
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
                
                while (recv_buffer.size() >= PACKET_SIZE) {
                    std::vector<char> packet(recv_buffer.begin(), recv_buffer.begin() + PACKET_SIZE);
                    recv_buffer.erase(recv_buffer.begin(), recv_buffer.begin() + PACKET_SIZE);

                    if (packet.size() < 12) {
                        // Invalid packet
                        continue;
                    }

                    uint32_t net_request_index, net_total_packets, net_sequence_number;
                    memcpy(&net_request_index, packet.data(), 4);
                    memcpy(&net_total_packets, packet.data() + 4, 4);
                    memcpy(&net_sequence_number, packet.data() + 8, 4);

                    uint32_t request_index = ntohl(net_request_index);
                    uint32_t total_packets = ntohl(net_total_packets);
                    uint32_t sequence_number = ntohl(net_sequence_number);

                    {
                        std::lock_guard<std::mutex> lock(mtx);

                        if (total_packets_map.find(request_index) == total_packets_map.end()) {
                            total_packets_map[request_index] = total_packets;
                        }

                        received_packets[request_index].insert(sequence_number);
                        if (received_packets[request_index].size() == total_packets_map[request_index]) {
                            auto end_time = std::chrono::high_resolution_clock::now();

                            auto it = request_info_map.find(request_index);
                            if (it != request_info_map.end()) {
                                double latency = std::chrono::duration<double, std::milli>(end_time - it->second.start_time).count();
                                latency_results[request_index] = latency;
                                std::cout << "Request " << request_index << " latency: " << latency << " ms" << std::endl;
                                request_info_map.erase(it);
                            }

                            received_packets.erase(request_index);
                            total_packets_map.erase(request_index);
                        }
                    }
                }
            } else if (bytes_received == 0) {
                std::cerr << "Connection closed by server." << std::endl;
                break;
            } else {
                std::cerr << "Receive error." << std::endl;
                break;
            }
        } else {
            // Timeout or error
            if (sending_done && request_info_map.empty()) {
                break;
            }
        }
    }
}

int main(int argc, char* argv[]) {
    // Parse command-line arguments
    if (argc < 5) {
        std::cerr << "Usage: " << argv[0] << " <server_ip> <num_requests> <packets_per_request> <interval_ms> [--port PORT] [--packet_size PACKET_SIZE]" << std::endl;
        return 1;
    }

    std::string server_ip = argv[1];
    uint32_t num_requests = std::stoi(argv[2]);
    uint32_t packets_per_request = std::stoi(argv[3]);
    uint32_t interval_ms = std::stoi(argv[4]);

    uint16_t port = 10000;
    uint32_t packet_size = PACKET_SIZE;

    // Optional arguments
    for (int i = 5; i < argc; ++i) {
        if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            port = std::stoi(argv[++i]);
        } else if (strcmp(argv[i], "--packet_size") == 0 && i + 1 < argc) {
            packet_size = std::stoi(argv[++i]);
        }
    }

    // Create socket
    int client_socket = socket(AF_INET, SOCK_STREAM, 0);

    // Connect to server
    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    inet_pton(AF_INET, server_ip.c_str(), &server_addr.sin_addr);

    if (connect(client_socket, (sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        std::cerr << "Failed to connect to server." << std::endl;
        return 1;
    }

    // Shared data structures
    std::map<uint32_t, RequestInfo> request_info_map;
    std::map<uint32_t, double> latency_results;
    bool sending_done = false;

    // Start sender and receiver threads
    std::thread sender_thread(send_requests, client_socket, num_requests, packets_per_request, interval_ms, packet_size, std::ref(request_info_map), std::ref(sending_done));
    std::thread receiver_thread(receive_responses, client_socket, std::ref(request_info_map), std::ref(latency_results), std::ref(sending_done));

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

    return 0;
}
