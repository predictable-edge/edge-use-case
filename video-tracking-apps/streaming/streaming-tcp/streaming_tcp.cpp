// video-push-pull.cpp

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <iostream>
#include <sstream>
#include <fstream>
#include <sys/time.h>
#include <ctime>
#include <iomanip> 
#include <chrono>
#include <thread>
#include <map>
#include <queue>
#include <mutex>
#include <vector>
#include <filesystem>
// Added for TCP client
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <cfloat>

// Include FFmpeg headers
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/error.h> 
#include <libswscale/swscale.h>
#include <libavutil/time.h>
#include <climits>
}

// For JSON parsing
#include <string>

#define CHECK_ERR(err, msg) \
    if ((err) < 0) { \
        char errbuf[AV_ERROR_MAX_STRING_SIZE]; \
        av_strerror(err, errbuf, sizeof(errbuf)); \
        fprintf(stderr, "Error: %s - %s\n", msg, errbuf); \
        exit(1); \
    }

// Mutex for synchronized console output
pthread_mutex_t cout_mutex = PTHREAD_MUTEX_INITIALIZER;

// Map to store frame sending timestamps for latency calculation
std::map<int, int64_t> frame_timestamps;

// Class for handling latency measurements and file output
class LatencyLogger {
public:
    LatencyLogger(const std::string& log_filename) : filename_(log_filename) {
        // Create directory if it doesn't exist
        std::filesystem::path filepath(filename_);
        std::filesystem::path parent_path = filepath.parent_path();
        try {
            if (!parent_path.empty() && !std::filesystem::exists(parent_path)) {
                std::filesystem::create_directories(parent_path);
                std::cout << "Created directories: " << parent_path << std::endl;
            }
        } catch (const std::filesystem::filesystem_error& e) {
            std::cerr << "Filesystem error: " << e.what() << std::endl;
        }
    }

    void add_entry(int frame_number, int64_t send_time_ms, int64_t receive_time_ms) {
        double latency_ms = receive_time_ms - send_time_ms;
        
        // Update statistics
        frame_count_++;
        total_latency_ms_ += latency_ms;
        min_latency_ms_ = std::min(min_latency_ms_, latency_ms);
        max_latency_ms_ = std::max(max_latency_ms_, latency_ms);
        
        // Store entry
        entries_.push_back({frame_number, latency_ms});
        
        // Log to console periodically
        if (frame_number % 30 == 0) {
            pthread_mutex_lock(&cout_mutex);
            printf("[Detection Thread] Frame %d: E2E Latency = %.2f ms\n", 
                   frame_number, latency_ms);
            pthread_mutex_unlock(&cout_mutex);
        }
    }

    void write_to_file() {
        std::ofstream ofs(filename_);
        if (!ofs.is_open()) {
            std::cerr << "Failed to open " << filename_ << " for writing." << std::endl;
            return;
        }

        ofs << std::left << std::setw(10) << "Frame" 
            << std::left << std::setw(20) << "E2E latency(ms)" 
            << "\n";

        for (const auto& entry : entries_) {
            ofs << std::left << std::setw(10) << entry.frame_number
                << std::left << std::setw(20) << std::to_string(entry.latency_ms) + " ms"
                << "\n";
        }

        ofs.close();
        std::cout << "Timing information written to " << filename_ << std::endl;
    }

    void print_summary() {
        if (frame_count_ > 0) {
            double avg_latency = total_latency_ms_ / frame_count_;
            pthread_mutex_lock(&cout_mutex);
            printf("\n[Detection Thread] Latency Statistics:\n");
            printf("  Frames processed: %d\n", frame_count_);
            printf("  Average latency: %.2f ms\n", avg_latency);
            printf("  Minimum latency: %.2f ms\n", min_latency_ms_);
            printf("  Maximum latency: %.2f ms\n", max_latency_ms_);
            printf("  Latency log saved to: %s\n", filename_.c_str());
            pthread_mutex_unlock(&cout_mutex);
        }
    }

private:
    struct LatencyEntry {
        int frame_number;
        double latency_ms;
    };

    std::vector<LatencyEntry> entries_;
    std::string filename_;
    int frame_count_ = 0;
    double total_latency_ms_ = 0.0;
    double min_latency_ms_ = DBL_MAX;
    double max_latency_ms_ = 0.0;
};

// Function to get current time in microseconds
int64_t get_current_time_us() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return ((int64_t)tv.tv_sec * 1000000) + tv.tv_usec;
}

std::string get_timestamp_with_ms() {
    auto now = std::chrono::system_clock::now();
    auto ms_part = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
    auto now_time_t = std::chrono::system_clock::to_time_t(now);
    std::tm* now_tm = std::localtime(&now_time_t);
    char buffer[20];
    std::strftime(buffer, sizeof(buffer), "%Y%m%d-%H%M%S", now_tm);
    std::ostringstream oss;
    oss << buffer << std::setw(3) << std::setfill('0') << ms_part.count();
    return oss.str();
}

// Function to extract just the frame number from the JSON response
int extract_frame_number(const std::string& json_str) {
    // Simple extraction of frame number from JSON
    size_t frame_pos = json_str.find("\"frame\":");
    if (frame_pos != std::string::npos) {
        size_t colon_pos = json_str.find(':', frame_pos);
        size_t comma_pos = json_str.find(',', colon_pos);
        if (comma_pos != std::string::npos && colon_pos != std::string::npos) {
            std::string frame_str = json_str.substr(colon_pos + 1, comma_pos - colon_pos - 1);
            try {
                return std::stoi(frame_str);
            } catch (...) {
                return -1;
            }
        }
    }
    return -1;
}

// Function to receive YOLO detection results via TCP
void* receive_yolo_results(void* args) {
    char** my_args = (char**)args;
    char* server_ip = my_args[0];
    int server_port = atoi(my_args[1]);
    
    pthread_mutex_lock(&cout_mutex);
    printf("[Detection Thread] Starting TCP client to receive YOLO detection results...\n");
    pthread_mutex_unlock(&cout_mutex);
    
    // Setup latency logging
    std::string timestamp = get_timestamp_with_ms();
    std::string log_dir = "result/" + timestamp;
    std::string log_filename = log_dir + "/latency.log";
    LatencyLogger logger(log_filename);
    
    // Create socket
    int client_fd;
    struct sockaddr_in server_addr;
    
    if ((client_fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        pthread_mutex_lock(&cout_mutex);
        fprintf(stderr, "[Detection Thread] Socket creation error\n");
        pthread_mutex_unlock(&cout_mutex);
        return NULL;
    }
    
    // Set up server address
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(server_port);
    
    // Convert IPv4 and IPv6 addresses from text to binary
    if (inet_pton(AF_INET, server_ip, &server_addr.sin_addr) <= 0) {
        pthread_mutex_lock(&cout_mutex);
        fprintf(stderr, "[Detection Thread] Invalid address or address not supported\n");
        pthread_mutex_unlock(&cout_mutex);
        close(client_fd);
        return NULL;
    }
    
    // Connect to the YOLO server
    pthread_mutex_lock(&cout_mutex);
    printf("[Detection Thread] Attempting to connect to %s:%d...\n", server_ip, server_port);
    pthread_mutex_unlock(&cout_mutex);
    
    if (connect(client_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        pthread_mutex_lock(&cout_mutex);
        fprintf(stderr, "[Detection Thread] Connection failed\n");
        pthread_mutex_unlock(&cout_mutex);
        close(client_fd);
        return NULL;
    }
    
    pthread_mutex_lock(&cout_mutex);
    printf("[Detection Thread] Connected to YOLO detection server\n");
    pthread_mutex_unlock(&cout_mutex);
    
    // Buffer for receiving data
    char buffer[4096];
    std::string json_data;
    
    // Receive data from server
    while (true) {
        memset(buffer, 0, sizeof(buffer));
        int bytes_read = recv(client_fd, buffer, sizeof(buffer) - 1, 0);
        
        if (bytes_read <= 0) {
            // Connection closed or error
            pthread_mutex_lock(&cout_mutex);
            fprintf(stderr, "[Detection Thread] Connection closed or error\n");
            pthread_mutex_unlock(&cout_mutex);
            break;
        }
        
        // Add received data to json_data buffer
        json_data.append(buffer, bytes_read);
        
        // Process complete JSON objects (delimited by newlines)
        size_t newline_pos;
        while ((newline_pos = json_data.find('\n')) != std::string::npos) {
            // Extract one JSON object
            std::string json_obj = json_data.substr(0, newline_pos);
            json_data.erase(0, newline_pos + 1);
            
            // Extract only the frame number from the JSON
            int frame_num = extract_frame_number(json_obj);
            if (frame_num >= 0 && frame_timestamps.find(frame_num) != frame_timestamps.end()) {
                // Calculate latency
                int64_t receive_time_ms = get_current_time_us() / 1000;
                int64_t send_time_ms = frame_timestamps[frame_num] / 1000;
                
                // Log latency
                logger.add_entry(frame_num, send_time_ms, receive_time_ms);
                
                // Remove from map to avoid memory growth
                frame_timestamps.erase(frame_num);
            }
        }
    }
    
    // Close the socket
    close(client_fd);
    
    // Write latency statistics to file
    logger.write_to_file();
    
    // Print summary
    logger.print_summary();
    
    pthread_mutex_lock(&cout_mutex);
    printf("[Detection Thread] Finished receiving YOLO detection results\n");
    pthread_mutex_unlock(&cout_mutex);
    
    return NULL;
}

// PushStreamContext structure remains unchanged
struct PushStreamContext {
    AVFormatContext* input_fmt_ctx;
    AVCodecContext* video_dec_ctx;
    int video_stream_idx;
    AVFormatContext* output_fmt_ctx;
    AVCodecContext* video_enc_ctx;
    AVStream* out_video_stream;
    struct SwsContext* sws_ctx;
    int64_t start_time;
};

void* push_stream_directly(void* args) {
    char **my_args = (char **)args;
    char *input_filename = my_args[0];
    char *output_url = my_args[1];

    pthread_mutex_lock(&cout_mutex);
    printf("[Push Thread] Starting push_stream...\n");
    pthread_mutex_unlock(&cout_mutex);

    AVFormatContext* input_fmt_ctx = NULL;
    int ret = avformat_open_input(&input_fmt_ctx, input_filename, NULL, NULL);
    CHECK_ERR(ret, "Could not open input file for push_stream");

    ret = avformat_find_stream_info(input_fmt_ctx, NULL);
    CHECK_ERR(ret, "Failed to retrieve input stream information for push_stream");

    int video_stream_idx = -1;
    for (unsigned int i = 0; i < input_fmt_ctx->nb_streams; i++) {
        if (input_fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            video_stream_idx = i;
            break;
        }
    }

    if (video_stream_idx == -1) {
        fprintf(stderr, "[Push Thread] Could not find a video stream in the input.\n");
        exit(1);
    }

    AVCodecParameters* codecpar = input_fmt_ctx->streams[video_stream_idx]->codecpar;

    // Allocate output format context
    AVFormatContext* output_fmt_ctx = NULL;
    ret = avformat_alloc_output_context2(&output_fmt_ctx, NULL, "flv", output_url);
    if (!output_fmt_ctx) {
        fprintf(stderr, "[Push Thread] Could not create output context.\n");
        exit(1);
    }

    // Create new stream for output
    AVStream* out_stream = avformat_new_stream(output_fmt_ctx, NULL);
    if (!out_stream) {
        fprintf(stderr, "[Push Thread] Failed allocating output stream.\n");
        exit(1);
    }

    // Copy codec parameters from input to output
    ret = avcodec_parameters_copy(out_stream->codecpar, codecpar);
    CHECK_ERR(ret, "Failed to copy codec parameters to output stream");

    out_stream->codecpar->codec_tag = 0;

    // Set up SRT options
    AVDictionary* tcp_options = NULL;

    // Open output URL
    ret = avio_open2(&output_fmt_ctx->pb, output_url, AVIO_FLAG_WRITE, NULL, &tcp_options);
    CHECK_ERR(ret, "Could not open output URL");

    // Write header
    ret = avformat_write_header(output_fmt_ctx, NULL);
    CHECK_ERR(ret, "Error occurred when writing header to output");

    AVPacket* packet = av_packet_alloc();
    if (!packet) {
        fprintf(stderr, "[Push Thread] Could not allocate packet.\n");
        exit(1);
    }

    // Get the stream's time base
    AVRational time_base = input_fmt_ctx->streams[video_stream_idx]->time_base;

    // Record the start time
    auto start_time = av_gettime();
    int64_t frame_count = 0;
    while (av_read_frame(input_fmt_ctx, packet) >= 0) {
        if (packet->stream_index == video_stream_idx) {
            // Rescale packet timestamps
            packet->stream_index = out_stream->index;
            int64_t pts = packet->pts;
            if (pts == AV_NOPTS_VALUE) {
                pts = packet->dts;
            }
            if (pts == AV_NOPTS_VALUE) {
                fprintf(stderr, "Packet has no valid pts or dts.\n");
                continue;
            }

            // Record the time for this frame for latency calculation
            int frame_num = frame_count + 1;  // Frame numbers typically start at 1
            frame_timestamps[frame_num] = get_current_time_us();

            int64_t pts_time = av_rescale_q(pts, time_base, AV_TIME_BASE_Q);
            av_packet_rescale_ts(packet, time_base, out_stream->time_base);
            // Calculate the expected send time
            int64_t now = av_gettime() - start_time;

            if (pts_time > now) {
                int64_t sleep_time = pts_time - now;
                if (sleep_time > 0) {
                    int ret = av_usleep(sleep_time);
                    if (ret < 0) {
                        pthread_mutex_lock(&cout_mutex);
                        fprintf(stderr, "[Push Thread] av_usleep was interrupted.\n");
                        pthread_mutex_unlock(&cout_mutex);
                    }
                }
            }
            ret = av_interleaved_write_frame(output_fmt_ctx, packet);
            if (ret < 0) {
                pthread_mutex_lock(&cout_mutex);
                char errbuf[AV_ERROR_MAX_STRING_SIZE];
                av_strerror(ret, errbuf, sizeof(errbuf));
                fprintf(stderr, "[Push Thread] Error muxing packet: %s\n", errbuf);
                pthread_mutex_unlock(&cout_mutex);
                break;
            }
            frame_count++;
        }
        av_packet_unref(packet);
    }

    // Write trailer
    ret = av_write_trailer(output_fmt_ctx);
    if (ret < 0) {
        pthread_mutex_lock(&cout_mutex);
        char errbuf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, errbuf, sizeof(errbuf));
        fprintf(stderr, "[Push Thread] Error writing trailer: %s\n", errbuf);
        pthread_mutex_unlock(&cout_mutex);
    }

    av_packet_free(&packet);
    if (!(output_fmt_ctx->oformat->flags & AVFMT_NOFILE)) {
        avio_closep(&output_fmt_ctx->pb);
    }
    avformat_free_context(output_fmt_ctx);
    avformat_close_input(&input_fmt_ctx);

    pthread_mutex_lock(&cout_mutex);
    printf("[Push Thread] Finished push_stream.\n");
    pthread_mutex_unlock(&cout_mutex);

    return NULL;
}

int main(int argc, char* argv[]) {
    if (argc < 5) {
        fprintf(stderr, "Usage: %s <push_input_file> <push_output_url> <yolo_server_ip> <yolo_server_port>\n", argv[0]);
        fprintf(stderr, "Example: %s snow-scene.mp4 \"tcp://192.168.2.3:9000\" 127.0.0.1 9001\n", argv[0]);
        return 1;
    }

    char *push_input_file = argv[1];
    char *push_output_url = argv[2];
    char *yolo_server_ip = argv[3];
    char *yolo_server_port = argv[4];

    // Initialize FFmpeg network
    avformat_network_init();

    // Create push thread
    pthread_t push_thread_id;
    char **push_args = (char **)malloc(2 * sizeof(char *));
    if (!push_args) {
        fprintf(stderr, "Could not allocate memory for push_args.\n");
        exit(1);
    }
    push_args[0] = strdup(push_input_file);
    push_args[1] = strdup(push_output_url);
    if (!push_args[0] || !push_args[1]) {
        fprintf(stderr, "Could not duplicate push arguments.\n");
        exit(1);
    }

    // Create detection thread
    pthread_t detection_thread_id;
    char **detection_args = (char **)malloc(2 * sizeof(char *));
    if (!detection_args) {
        fprintf(stderr, "Could not allocate memory for detection_args.\n");
        exit(1);
    }
    detection_args[0] = strdup(yolo_server_ip);
    detection_args[1] = strdup(yolo_server_port);
    if (!detection_args[0] || !detection_args[1]) {
        fprintf(stderr, "Could not duplicate detection arguments.\n");
        exit(1);
    }

    // Start detection thread
    int ret_detect = pthread_create(&detection_thread_id, NULL, receive_yolo_results, detection_args);
    if (ret_detect != 0) {
        fprintf(stderr, "Failed to create detection thread.\n");
        exit(1);
    }

    // Optionally, wait for a few seconds before starting push
    sleep(5);

    // Start push thread
    int ret_create = pthread_create(&push_thread_id, NULL, push_stream_directly, push_args);
    if (ret_create != 0) {
        fprintf(stderr, "Failed to create push thread.\n");
        exit(1);
    }

    // Wait for push thread to finish
    pthread_join(push_thread_id, NULL);
    
    // Signal detection thread to stop (can be implemented with a flag)
    pthread_join(detection_thread_id, NULL);

    // Free push_args
    free(push_args[0]);
    free(push_args[1]);
    free(push_args);
    
    // Free detection_args
    free(detection_args[0]);
    free(detection_args[1]);
    free(detection_args);

    avformat_network_deinit();

    return 0;
}