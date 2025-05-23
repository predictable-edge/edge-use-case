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
#include <atomic>   // Added for std::atomic
#include <fcntl.h>  // Added for fcntl and flags
#include <errno.h>  // Added for errno

// Include FFmpeg headers
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/error.h> 
#include <libswscale/swscale.h>
#include <libavutil/time.h>
#include <libavcodec/packet.h>
#include <libavutil/intreadwrite.h>
#include <inttypes.h>
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

// Structure for storing detection information
struct Detection {
    float x1, y1, x2, y2;  // Bounding box coordinates
    float confidence;      // Confidence score
    int class_id;          // Class ID
    
    // Constructor for easy creation
    Detection(float _x1, float _y1, float _x2, float _y2, float _conf, int _class_id)
        : x1(_x1), y1(_y1), x2(_x2), y2(_y2), confidence(_conf), class_id(_class_id) {}
};

// COCO class names that YOLOv8 is trained on (for reference)
const char* COCO_CLASSES[] = {
    "person", "bicycle", "car", "motorcycle", "airplane", "bus", "train", "truck", "boat", "traffic light",
    "fire hydrant", "stop sign", "parking meter", "bench", "bird", "cat", "dog", "horse", "sheep", "cow",
    "elephant", "bear", "zebra", "giraffe", "backpack", "umbrella", "handbag", "tie", "suitcase", "frisbee",
    "skis", "snowboard", "sports ball", "kite", "baseball bat", "baseball glove", "skateboard", "surfboard",
    "tennis racket", "bottle", "wine glass", "cup", "fork", "knife", "spoon", "bowl", "banana", "apple",
    "sandwich", "orange", "broccoli", "carrot", "hot dog", "pizza", "donut", "cake", "chair", "couch",
    "potted plant", "bed", "dining table", "toilet", "tv", "laptop", "mouse", "remote", "keyboard", "cell phone",
    "microwave", "oven", "toaster", "sink", "refrigerator", "book", "clock", "vase", "scissors", "teddy bear",
    "hair drier", "toothbrush"
};

// Map to store frame sending timestamps for latency calculation
std::map<int, int64_t> frame_timestamps;

// New map to store detection results for each frame
std::map<int, std::vector<Detection>> frame_detections;
std::mutex detections_mutex;

// Shared variables for thread synchronization
std::atomic<bool> all_frames_sent(false);
std::atomic<int> total_frames_sent(0);
std::atomic<int> frames_processed(0);
int max_wait_time_seconds = 10;  // Maximum time to wait after all frames are sent

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

    void add_entry(int frame_number, int64_t send_time_us, int64_t receive_time_us) {
        // Calculate latency in microseconds for precision
        double latency_us = receive_time_us - send_time_us;
        // Convert to milliseconds for display
        double latency_ms = latency_us / 1000.0;
        
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
        
        // Increment the counter of processed frames
        frames_processed++;
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

int64_t get_current_time_ms() {
    auto now = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
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
    std::string log_dir = "result/latency-" + timestamp;
    std::string log_filename = log_dir + ".txt";
    LatencyLogger logger(log_filename);
    
    // Create TCP socket
    int client_fd;
    struct sockaddr_in server_addr;
    
    if ((client_fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        pthread_mutex_lock(&cout_mutex);
        fprintf(stderr, "[Detection Thread] Socket creation error\n");
        pthread_mutex_unlock(&cout_mutex);
        return NULL;
    }
    
    // Set up server address
    memset(&server_addr, 0, sizeof(server_addr));
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
    
    // Connect to server
    pthread_mutex_lock(&cout_mutex);
    printf("[Detection Thread] Connecting to YOLO result server at %s:%d\n", server_ip, server_port);
    pthread_mutex_unlock(&cout_mutex);
    
    // Set socket to non-blocking for connection with timeout
    int flags = fcntl(client_fd, F_GETFL, 0);
    fcntl(client_fd, F_SETFL, flags | O_NONBLOCK);
    
    // Attempt connection
    int res = connect(client_fd, (struct sockaddr*)&server_addr, sizeof(server_addr));
    if (res < 0) {
        if (errno == EINPROGRESS) {
            // Connection in progress
            fd_set fdset;
            struct timeval tv;
            
            FD_ZERO(&fdset);
            FD_SET(client_fd, &fdset);
            tv.tv_sec = 5;  // 5 second timeout
            tv.tv_usec = 0;
            
            res = select(client_fd + 1, NULL, &fdset, NULL, &tv);
            if (res < 0) {
                pthread_mutex_lock(&cout_mutex);
                fprintf(stderr, "[Detection Thread] Error in select: %s\n", strerror(errno));
                pthread_mutex_unlock(&cout_mutex);
                close(client_fd);
                return NULL;
            } else if (res == 0) {
                pthread_mutex_lock(&cout_mutex);
                fprintf(stderr, "[Detection Thread] Connection timeout\n");
                pthread_mutex_unlock(&cout_mutex);
                close(client_fd);
                return NULL;
            }
            
            // Check if connection was successful
            int so_error;
            socklen_t len = sizeof(so_error);
            if (getsockopt(client_fd, SOL_SOCKET, SO_ERROR, &so_error, &len) < 0) {
                pthread_mutex_lock(&cout_mutex);
                fprintf(stderr, "[Detection Thread] Error in getsockopt: %s\n", strerror(errno));
                pthread_mutex_unlock(&cout_mutex);
                close(client_fd);
                return NULL;
            }
            
            if (so_error) {
                pthread_mutex_lock(&cout_mutex);
                fprintf(stderr, "[Detection Thread] Connection error: %s\n", strerror(so_error));
                pthread_mutex_unlock(&cout_mutex);
                close(client_fd);
                return NULL;
            }
        } else {
            pthread_mutex_lock(&cout_mutex);
            fprintf(stderr, "[Detection Thread] Connection failed: %s\n", strerror(errno));
            pthread_mutex_unlock(&cout_mutex);
            close(client_fd);
            return NULL;
        }
    }
    
    // Set socket back to blocking mode
    fcntl(client_fd, F_SETFL, flags);
    
    pthread_mutex_lock(&cout_mutex);
    printf("[Detection Thread] Connected to YOLO result server\n");
    pthread_mutex_unlock(&cout_mutex);
    
    // Variables to track idle time once all frames are sent
    time_t last_activity_time = time(NULL);
    bool should_exit = false;
    
    // Buffer for receiving detection data
    std::vector<char> buffer(8192);  // Increased buffer size for detection data
    
    // Receive data from server
    while (!should_exit) {
        // Check if we should exit based on frame count
        if (all_frames_sent) {
            // If we've received responses for all frames, or waited too long, exit
            if (frames_processed >= total_frames_sent || 
                difftime(time(NULL), last_activity_time) > max_wait_time_seconds) {
                pthread_mutex_lock(&cout_mutex);
                printf("[Detection Thread] All frames processed (%d/%d) or max wait time reached. Exiting.\n", 
                      frames_processed.load(), total_frames_sent.load());
                pthread_mutex_unlock(&cout_mutex);
                should_exit = true;
                break;
            }
        }
        
        // First receive the frame number and detection count (8 bytes total)
        int header[2];  // To store frame_num and num_detections
        ssize_t bytes_read = recv(client_fd, header, sizeof(header), 0);
        
        if (bytes_read == sizeof(header)) {
            int frame_num = header[0];
            int num_detections = header[1];
            
            // Update last activity time
            last_activity_time = time(NULL);
            
            // Prepare to receive detection data if there are any
            std::vector<Detection> detections;
            
            if (num_detections > 0) {
                // Calculate expected size of detection data
                size_t detection_data_size = num_detections * 24;  // 24 bytes per detection
                
                // Ensure buffer is large enough
                if (buffer.size() < detection_data_size) {
                    buffer.resize(detection_data_size);
                }
                
                // Receive detection data
                bytes_read = recv(client_fd, buffer.data(), detection_data_size, 0);
                
                if (bytes_read == static_cast<ssize_t>(detection_data_size)) {
                    // Process all detections
                    char* ptr = buffer.data();
                    
                    for (int i = 0; i < num_detections; i++) {
                        // Read bounding box coordinates (4 floats)
                        float x1 = *reinterpret_cast<float*>(ptr);
                        ptr += sizeof(float);
                        float y1 = *reinterpret_cast<float*>(ptr);
                        ptr += sizeof(float);
                        float x2 = *reinterpret_cast<float*>(ptr);
                        ptr += sizeof(float);
                        float y2 = *reinterpret_cast<float*>(ptr);
                        ptr += sizeof(float);
                        
                        // Read confidence (1 float)
                        float confidence = *reinterpret_cast<float*>(ptr);
                        ptr += sizeof(float);
                        
                        // Read class ID (1 int)
                        int class_id = *reinterpret_cast<int*>(ptr);
                        ptr += sizeof(int);
                        
                        // Create Detection object and add to list
                        detections.emplace_back(x1, y1, x2, y2, confidence, class_id);
                    }
                    
                    // Store detections for this frame
                    {
                        std::lock_guard<std::mutex> lock(detections_mutex);
                        frame_detections[frame_num] = detections;
                    }
                } else {
                    pthread_mutex_lock(&cout_mutex);
                    fprintf(stderr, "[Detection Thread] Error receiving detection data: expected %zu bytes, got %zd\n", 
                            detection_data_size, bytes_read);
                    pthread_mutex_unlock(&cout_mutex);
                }
            }
            
            // Process frame information for latency calculation
            if (frame_timestamps.find(frame_num) != frame_timestamps.end()) {
                // Calculate latency
                int64_t receive_time_us = get_current_time_us();
                int64_t send_time_us = frame_timestamps[frame_num];
                
                // Log latency
                logger.add_entry(frame_num + 1, send_time_us, receive_time_us);
                
                // Remove from map to avoid memory growth
                frame_timestamps.erase(frame_num);
                
                if (frame_num % 100 == 0) {
                    pthread_mutex_lock(&cout_mutex);
                    printf("[Detection Thread] Received frame %d with %d detections\n", 
                           frame_num, num_detections);
                    
                    // Print some sample detections if available
                    if (num_detections > 0 && !detections.empty()) {
                        printf("[Detection Thread] Sample detections:\n");
                        int print_count = std::min(3, num_detections);  // Print at most 3 detections
                        for (int i = 0; i < print_count; i++) {
                            const Detection& det = detections[i];
                            const char* class_name = (det.class_id >= 0 && det.class_id < 80) ? 
                                                     COCO_CLASSES[det.class_id] : "unknown";
                            printf("  - %s (%.2f): [%.1f, %.1f, %.1f, %.1f]\n",
                                   class_name, det.confidence, 
                                   det.x1, det.y1, det.x2, det.y2);
                        }
                    }
                    pthread_mutex_unlock(&cout_mutex);
                }
            }
        } else if (bytes_read == 0) {
            // Connection closed by server
            pthread_mutex_lock(&cout_mutex);
            printf("[Detection Thread] Server closed connection\n");
            pthread_mutex_unlock(&cout_mutex);
            should_exit = true;
        } else {
            // Error or would block
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                pthread_mutex_lock(&cout_mutex);
                fprintf(stderr, "[Detection Thread] Error reading from socket: %s\n", strerror(errno));
                pthread_mutex_unlock(&cout_mutex);
                should_exit = true;
            } else {
                // No data available, sleep a bit to avoid spinning
                usleep(10000); // 10ms
            }
        }
    }
    
    // Clean up detection data
    {
        std::lock_guard<std::mutex> lock(detections_mutex);
        frame_detections.clear();
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


int embed_timestamp(AVPacket *packet) {
    int64_t timestamp = get_current_time_us();
    uint8_t sei_content[32];
    int sei_content_size = 0;
    
    sei_content[sei_content_size++] = 0x06;
    sei_content[sei_content_size++] = 0x05;
    int payload_size = 16 + sizeof(int64_t);
    sei_content[sei_content_size++] = payload_size;
    
    const uint8_t uuid[16] = {
        0x54, 0x69, 0x6D, 0x65, // "Time"
        0x53, 0x74, 0x61, 0x6D, // "Stam"
        0x70, 0x00, 0x01, 0x02, 
        0x03, 0x04, 0x05, 0x06 
    };
    memcpy(sei_content + sei_content_size, uuid, 16);
    sei_content_size += 16;
    
    memcpy(sei_content + sei_content_size, &timestamp, sizeof(int64_t));
    sei_content_size += sizeof(int64_t);
    sei_content[sei_content_size++] = 0x80;
    
    int new_size = packet->size + sei_content_size + 4;
    uint8_t *new_data = (uint8_t*)av_malloc(new_size);
    if (!new_data) return AVERROR(ENOMEM);
    
    new_data[0] = (sei_content_size >> 24) & 0xFF;
    new_data[1] = (sei_content_size >> 16) & 0xFF;
    new_data[2] = (sei_content_size >> 8) & 0xFF;
    new_data[3] = sei_content_size & 0xFF;
    
    memcpy(new_data + 4, sei_content, sei_content_size);

    memcpy(new_data + 4 + sei_content_size, packet->data, packet->size);
    av_buffer_unref(&packet->buf);
    packet->buf = av_buffer_create(new_data, new_size, 
                                  av_buffer_default_free, NULL, 0);
    packet->data = new_data;
    packet->size = new_size;
    return 0;
}


AVPacket* create_timestamp_packet(const AVPacket* base ,uint64_t ts_us)
{
    static const uint8_t SEI_PREFIX[5] = {0,0,0,1,0x06};
    static const uint8_t AUD_NAL[6]    = {0,0,0,1,0x09,0x10};

    const int SIZE = 16 + 6;

    AVPacket* pkt = av_packet_alloc();
    if (!pkt) return nullptr;
    if (av_packet_copy_props(pkt, base) < 0) { av_packet_free(&pkt); return nullptr; }

    uint8_t* data = (uint8_t*)av_malloc(SIZE);
    if (!data){ av_packet_free(&pkt); return nullptr; }
    uint8_t* p = data;

    memcpy(p, SEI_PREFIX, 5);  p += 5;
    *p++ = 5;
    *p++ = 8;
    for (int i = 7; i >= 0; --i) *p++ = (ts_us >> (i*8)) & 0xFF;
    *p++ = 0x80;

    memcpy(p, AUD_NAL, sizeof(AUD_NAL));
    pkt->data = data;
    pkt->size = SIZE;
    pkt->buf  = av_buffer_create(data, SIZE, av_buffer_default_free, nullptr, 0);

    pkt->pts   = base->pts + 1;
    pkt->dts   = base->dts + 1;
    pkt->flags = 0;
    pkt->stream_index = base->stream_index;
    return pkt;
}

void* push_stream_directly(void* args) {
    char **my_args = (char **)args;
    char *input_filename = my_args[0];
    char *output_url = my_args[1];

    pthread_mutex_lock(&cout_mutex);
    printf("[Push Thread] Starting push_stream with RTSP...\n");
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
    ret = avformat_alloc_output_context2(&output_fmt_ctx, NULL, "rtp", output_url);
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

    // Set up UDP-specific options for low latency
    AVDictionary* rtp_options = NULL;
    av_dict_set(&rtp_options, "listen_timeout", "5000000", 0);    // Listen timeout 5 seconds
    av_dict_set(&rtp_options, "max_delay", "500000", 0);          // Max delay 500ms
    av_dict_set(&rtp_options, "reorder_queue_size", "10", 0);     // Reorder queue size
    av_dict_set(&rtp_options, "buffer_size", "1048576", 0);       // 1MB buffer
    av_dict_set(&rtp_options, "pkt_size", "1316", 0);             // Optimal packet size
    av_dict_set(&rtp_options, "flush_packets", "1", 0);           // Flush packets immediately

    char sdp_buffer[4096];
    int sdp_ret = av_sdp_create(&output_fmt_ctx, 1, sdp_buffer, sizeof(sdp_buffer));
    if (sdp_ret < 0) {
        std::cerr << "Failed to create SDP" << std::endl;
    } else {
        // Save SDP to file
        std::ofstream sdp_file("stream_ul.sdp");
        if (sdp_file.is_open()) {
            sdp_file << sdp_buffer;
            sdp_file.close();
            std::cout << "SDP file generated: stream_ul.sdp" << std::endl;
            std::cout << "SDP Content:\n" << sdp_buffer << std::endl;
            std::cout << "----------------------\n";
        } else {
            std::cerr << "Could not open stream_ul.sdp for writing" << std::endl;
        }
    }

    // Open output URL with UDP options
    ret = avio_open2(&output_fmt_ctx->pb, output_url, AVIO_FLAG_WRITE, NULL, &rtp_options);
    CHECK_ERR(ret, "Could not open output URL");

    // Set the maximum interleave delta to a very low value for decreased latency
    output_fmt_ctx->max_interleave_delta = 0;

    // Write header
    ret = avformat_write_header(output_fmt_ctx, &rtp_options);
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

            // Write packet
            frame_timestamps[frame_count] = get_current_time_us();
            ret = av_write_frame(output_fmt_ctx, packet);
            if (ret < 0) {
                pthread_mutex_lock(&cout_mutex);
                char errbuf[AV_ERROR_MAX_STRING_SIZE];
                av_strerror(ret, errbuf, sizeof(errbuf));
                fprintf(stderr, "[Push Thread] Error writing packet: %s\n", errbuf);
                pthread_mutex_unlock(&cout_mutex);
                break;
            }
            AVPacket* empty_pkt = create_timestamp_packet(packet, get_current_time_us());
            if (empty_pkt) {
                int empty_write_ret = av_write_frame(output_fmt_ctx, empty_pkt);
                if (empty_write_ret < 0) {
                    std::cerr << "Error writing empty packet to output" << std::endl;
                }
                av_packet_free(&empty_pkt);
            }
            frame_count++;
            // std::cout << "Frame " << frame_count << " pushed at " << get_current_time_us() << std::endl;
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

// void* push_stream_directly(void* args) {
//     char **my_args = (char **)args;
//     char *input_filename = my_args[0];
//     char *output_url = my_args[1];

//     pthread_mutex_lock(&cout_mutex);
//     printf("[Push Thread] Starting push_stream...\n");
//     pthread_mutex_unlock(&cout_mutex);

//     AVFormatContext* input_fmt_ctx = NULL;
//     int ret = avformat_open_input(&input_fmt_ctx, input_filename, NULL, NULL);
//     CHECK_ERR(ret, "Could not open input file for push_stream");

//     ret = avformat_find_stream_info(input_fmt_ctx, NULL);
//     CHECK_ERR(ret, "Failed to retrieve input stream information for push_stream");

//     int video_stream_idx = -1;
//     for (unsigned int i = 0; i < input_fmt_ctx->nb_streams; i++) {
//         if (input_fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
//             video_stream_idx = i;
//             break;
//         }
//     }

//     if (video_stream_idx == -1) {
//         fprintf(stderr, "[Push Thread] Could not find a video stream in the input.\n");
//         exit(1);
//     }

//     AVCodecParameters* codecpar = input_fmt_ctx->streams[video_stream_idx]->codecpar;

//     // Allocate output format context
//     AVFormatContext* output_fmt_ctx = NULL;
//     ret = avformat_alloc_output_context2(&output_fmt_ctx, NULL, "flv", output_url);
//     if (!output_fmt_ctx) {
//         fprintf(stderr, "[Push Thread] Could not create output context.\n");
//         exit(1);
//     }

//     // Create new stream for output
//     AVStream* out_stream = avformat_new_stream(output_fmt_ctx, NULL);
//     if (!out_stream) {
//         fprintf(stderr, "[Push Thread] Failed allocating output stream.\n");
//         exit(1);
//     }

//     // Copy codec parameters from input to output
//     ret = avcodec_parameters_copy(out_stream->codecpar, codecpar);
//     CHECK_ERR(ret, "Failed to copy codec parameters to output stream");

//     out_stream->codecpar->codec_tag = 0;

//     // Set up SRT options
//     AVDictionary* tcp_options = NULL;
//     av_dict_set(&tcp_options, "tcp_nodelay", "1", 0);

//     // Open output URL
//     ret = avio_open2(&output_fmt_ctx->pb, output_url, AVIO_FLAG_WRITE, NULL, &tcp_options);
//     CHECK_ERR(ret, "Could not open output URL");

//     // Write header
//     ret = avformat_write_header(output_fmt_ctx, NULL);
//     CHECK_ERR(ret, "Error occurred when writing header to output");

//     AVPacket* packet = av_packet_alloc();
//     if (!packet) {
//         fprintf(stderr, "[Push Thread] Could not allocate packet.\n");
//         exit(1);
//     }

//     // Get the stream's time base
//     AVRational time_base = input_fmt_ctx->streams[video_stream_idx]->time_base;

//     // Record the start time
//     auto start_time = av_gettime();
//     int64_t frame_count = 0;
//     while (av_read_frame(input_fmt_ctx, packet) >= 0) {
//         if (packet->stream_index == video_stream_idx) {
//             // Rescale packet timestamps
//             packet->stream_index = out_stream->index;
//             int64_t pts = packet->pts;
//             if (pts == AV_NOPTS_VALUE) {
//                 pts = packet->dts;
//             }
//             if (pts == AV_NOPTS_VALUE) {
//                 fprintf(stderr, "Packet has no valid pts or dts.\n");
//                 continue;
//             }

//             // Record the time for this frame for latency calculation

//             int64_t pts_time = av_rescale_q(pts, time_base, AV_TIME_BASE_Q);
//             av_packet_rescale_ts(packet, time_base, out_stream->time_base);
//             // Calculate the expected send time
//             int64_t now = av_gettime() - start_time;

//             if (pts_time > now) {
//                 int64_t sleep_time = pts_time - now;
//                 if (sleep_time > 0) {
//                     int ret = av_usleep(sleep_time);
//                     if (ret < 0) {
//                         pthread_mutex_lock(&cout_mutex);
//                         fprintf(stderr, "[Push Thread] av_usleep was interrupted.\n");
//                         pthread_mutex_unlock(&cout_mutex);
//                     }
//                 }
//             }
//             frame_timestamps[frame_count] = get_current_time_us();
//             embed_timestamp(packet);
//             ret = av_interleaved_write_frame(output_fmt_ctx, packet);
//             if (ret < 0) {
//                 pthread_mutex_lock(&cout_mutex);
//                 char errbuf[AV_ERROR_MAX_STRING_SIZE];
//                 av_strerror(ret, errbuf, sizeof(errbuf));
//                 fprintf(stderr, "[Push Thread] Error muxing packet: %s\n", errbuf);
//                 pthread_mutex_unlock(&cout_mutex);
//                 break;
//             }
//             frame_count++;
//             total_frames_sent = frame_count;  // Update atomic counter
//         }
//         av_packet_unref(packet);
//     }

//     // Write trailer
//     ret = av_write_trailer(output_fmt_ctx);
//     if (ret < 0) {
//         pthread_mutex_lock(&cout_mutex);
//         char errbuf[AV_ERROR_MAX_STRING_SIZE];
//         av_strerror(ret, errbuf, sizeof(errbuf));
//         fprintf(stderr, "[Push Thread] Error writing trailer: %s\n", errbuf);
//         pthread_mutex_unlock(&cout_mutex);
//     }

//     av_packet_free(&packet);
//     if (!(output_fmt_ctx->oformat->flags & AVFMT_NOFILE)) {
//         avio_closep(&output_fmt_ctx->pb);
//     }
//     avformat_free_context(output_fmt_ctx);
//     avformat_close_input(&input_fmt_ctx);

//     // Signal that all frames have been sent
//     all_frames_sent = true;
    
//     pthread_mutex_lock(&cout_mutex);
//     printf("[Push Thread] Finished push_stream. Sent %d frames.\n", total_frames_sent.load());
//     pthread_mutex_unlock(&cout_mutex);

//     return NULL;
// }

int main(int argc, char* argv[]) {
    if (argc < 5) {
        fprintf(stderr, "Usage: %s <push_input_file> <push_output_url> <detection_server_ip> <detection_server_port> [<max_wait_time>]\n", argv[0]);
        fprintf(stderr, "Example: %s snow-scene.mp4 \"tcp://192.168.2.3:9000\" 127.0.0.1 9876 20\n", argv[0]);
        fprintf(stderr, "Note: Communication with detection server uses TCP protocol\n");
        return 1;
    }

    char *push_input_file = argv[1];
    char *push_output_url = argv[2];
    char *detection_server_ip = argv[3];
    char *detection_server_port = argv[4];
    
    // Optional parameter for max wait time
    if (argc >= 6) {
        max_wait_time_seconds = atoi(argv[5]);
    }

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
    detection_args[0] = strdup(detection_server_ip);
    detection_args[1] = strdup(detection_server_port);
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
    
    // Now the detection thread should notice that all frames have been sent
    // and will exit after receiving all responses or timing out
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