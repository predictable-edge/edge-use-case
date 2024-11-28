#include <iostream>
#include <fstream>
#include <cstring>
#include <vector>
#include <chrono>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#include <iomanip>
#include <thread>
#include <mutex>
#include <queue>
#include <condition_variable>

// Structure to keep track of each flow's index and send time
struct RequestInfo {
    uint32_t index;
    std::chrono::high_resolution_clock::time_point start_time;
};

// Thread-safe queue to manage RequestInfo between sender and receiver threads
class ThreadSafeQueue {
private:
    std::queue<RequestInfo> queue_;
    std::mutex mutex_;
    std::condition_variable cond_var_;

public:
    // Push a new RequestInfo into the queue
    void push(const RequestInfo& info) {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push(info);
        cond_var_.notify_one();
    }

    // Pop a RequestInfo from the queue
    bool pop(RequestInfo& info) {
        std::unique_lock<std::mutex> lock(mutex_);
        while (queue_.empty()) {
            if (cond_var_.wait_for(lock, std::chrono::seconds(2)) == std::cv_status::timeout) {
                return false; // Timeout, no info available
            }
        }
        if (!queue_.empty()) {
            info = queue_.front();
            queue_.pop();
            return true;
        }
        return false;
    }

    // Check if the queue is empty
    bool empty() {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.empty();
    }
};

// Sender thread function
void send_flows(int client_socket, uint32_t num_requests, uint32_t flow_size, uint32_t interval_ms, ThreadSafeQueue& send_queue, bool& sending_done) {
    for (uint32_t request_index = 0; request_index < num_requests; ++request_index) {
        // Record the send start time
        auto start_time = std::chrono::high_resolution_clock::now();

        // Create RequestInfo and push it to the queue
        RequestInfo req_info = {request_index, start_time};
        send_queue.push(req_info);

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

        // Wait for the specified interval before sending the next flow
        if (request_index < num_requests - 1) { // No need to wait after the last request
            std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
        }
    }
    sending_done = true;
}

// Receiver thread function
void receive_responses(int client_socket, uint32_t flow_size, uint32_t num_requests, ThreadSafeQueue& send_queue, std::ofstream& output_file, bool& sending_done) {
    std::vector<char> recv_buffer(flow_size, 0);
    uint32_t received_flows = 0;

    while (received_flows < num_requests || !send_queue.empty() || !sending_done) {
        // Receive data flow
        size_t total_received = 0;
        while (total_received < flow_size) {
            ssize_t bytes_received = recv(client_socket, recv_buffer.data() + total_received, flow_size - total_received, 0);
            if (bytes_received < 0) {
                std::cerr << "Error receiving data from server." << std::endl;
                return;
            } else if (bytes_received == 0) {
                std::cerr << "Connection closed by server." << std::endl;
                return;
            }
            total_received += bytes_received;
        }

        // Pop the corresponding RequestInfo from the queue
        RequestInfo req_info;
        bool success = send_queue.pop(req_info);
        if (success) {
            // Record the receive end time
            auto end_time = std::chrono::high_resolution_clock::now();

            // Calculate latency in milliseconds
            double latency = std::chrono::duration<double, std::milli>(end_time - req_info.start_time).count();

            // Output latency to console
            std::cout << "Flow " << req_info.index << " latency: " << latency << " ms" << std::endl;

            // Write latency to the output file
            output_file << std::left << std::setw(10) << req_info.index << std::setw(15) 
                        << std::fixed << std::setprecision(2) << latency << std::endl;

            received_flows++;
        } else {
            std::cerr << "No matching request info for the received response." << std::endl;
        }
    }
}

int main(int argc, char* argv[]) {
    // Check command-line arguments
    if (argc < 5) {
        std::cerr << "Usage: " << argv[0] << " <server_ip> <num_requests> <flow_size_bytes> <interval_ms> [--port PORT]" << std::endl;
        return 1;
    }

    // Parse command-line arguments
    std::string server_ip = argv[1];
    uint32_t num_requests = std::stoi(argv[2]);
    uint32_t flow_size = std::stoi(argv[3]);
    uint32_t interval_ms = std::stoi(argv[4]);

    uint16_t port = 10000; // Default port

    // Parse optional port argument
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

    // Configure server address
    sockaddr_in server_addr;
    std::memset(&server_addr, 0, sizeof(server_addr)); // Zero out the structure
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    if (inet_pton(AF_INET, server_ip.c_str(), &server_addr.sin_addr) <= 0) {
        std::cerr << "Invalid server IP address." << std::endl;
        close(client_socket);
        return 1;
    }

    // Connect to server
    if (connect(client_socket, reinterpret_cast<sockaddr*>(&server_addr), sizeof(server_addr)) < 0) {
        std::cerr << "Failed to connect to server." << std::endl;
        close(client_socket);
        return 1;
    }

    std::cout << "Connected to server " << server_ip << " on port " << port << "." << std::endl;

    // Send flow size to server
    uint32_t net_flow_size = htonl(flow_size);
    ssize_t sent = send(client_socket, &net_flow_size, sizeof(net_flow_size), 0);
    if (sent != sizeof(net_flow_size)) {
        std::cerr << "Failed to send flow size to server." << std::endl;
        close(client_socket);
        return 1;
    }
    std::cout << "Sent flow size: " << flow_size << " bytes." << std::endl;

    // Open output file for latency results
    std::ofstream output_file("latency.txt");
    if (!output_file.is_open()) {
        std::cerr << "Failed to open latency.txt for writing." << std::endl;
        close(client_socket);
        return 1;
    }

    // Write header to the output file
    output_file << std::left << std::setw(10) << "Index" << std::setw(15) << "Latency(ms)" << std::endl;

    // Create a thread-safe queue for managing RequestInfo
    ThreadSafeQueue send_queue;

    // Flag to indicate sending completion
    bool sending_done = false;

    // Start the sender thread
    std::thread sender_thread(send_flows, client_socket, num_requests, flow_size, interval_ms, std::ref(send_queue), std::ref(sending_done));

    // Start the receiver thread
    std::thread receiver_thread(receive_responses, client_socket, flow_size, num_requests, std::ref(send_queue), std::ref(output_file), std::ref(sending_done));

    // Wait for both threads to finish
    sender_thread.join();
    receiver_thread.join();

    // Close the output file and socket
    output_file.close();
    close(client_socket);

    std::cout << "Latency results written to latency.txt" << std::endl;

    return 0;
}