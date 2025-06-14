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
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <regex>

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

// Forward declarations
int64_t extract_frame_id_from_packet(AVPacket* pkt, int count = 1);
void add_frame_index_to_packet(AVPacket* pkt, uint64_t frame_index, int count);

#define CHECK_ERR(err, msg) \
    if ((err) < 0) { \
        char errbuf[AV_ERROR_MAX_STRING_SIZE]; \
        av_strerror(err, errbuf, sizeof(errbuf)); \
        fprintf(stderr, "Error: %s - %s\n", msg, errbuf); \
        exit(1); \
    }

// Mutex for synchronized console output
pthread_mutex_t cout_mutex = PTHREAD_MUTEX_INITIALIZER;

// Mutex for accessing push_timestamps and related maps
std::mutex push_mutex;

// Global timing maps
std::map<int64_t, int64_t> push_timestamps;
std::map<int64_t, int64_t> push_timestamps_after_enc;

// Function to get current time in microseconds
uint64_t get_current_time_us() {
    auto now = std::chrono::system_clock::now();
    auto duration = now.time_since_epoch();
    return std::chrono::duration_cast<std::chrono::microseconds>(duration).count();
}

std::string get_timestamp_with_ms() {
    auto now = std::chrono::system_clock::now();
    auto ms_part = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
    auto now_time_t = std::chrono::system_clock::to_time_t(now);
    std::tm* now_tm = std::localtime(&now_time_t);
    char buffer[20];
    std::strftime(buffer, sizeof(buffer), "%Y%m%d_%H%M%S", now_tm);
    std::ostringstream oss;
    oss << buffer << std::setw(3) << std::setfill('0') << ms_part.count();
    return oss.str();
}

// TimingLogger class for thread-safe logging
class TimingLogger {
public:
    TimingLogger(const std::string& log_filename) : filename_(log_filename) {}

    void add_entry(int frame_number, int64_t push_time_ms, int64_t pull_time_ms, int64_t push_after_enc_ms, int64_t pull_before_dec_ms) {
        std::lock_guard<std::mutex> lock(mutex_);
        log_entries.emplace_back(LogEntry(frame_number, push_time_ms, pull_time_ms, push_after_enc_ms, pull_before_dec_ms));
    }

    void write_to_file() {
        std::lock_guard<std::mutex> lock(mutex_);
        std::filesystem::path filepath(filename_);
        std::filesystem::path parent_path = filepath.parent_path();
        try {
            if (!parent_path.empty() && !std::filesystem::exists(parent_path)) {
                std::filesystem::create_directories(parent_path);
                std::cout << "Created directories: " << parent_path << std::endl;
            }
        } catch (const std::filesystem::filesystem_error& e) {
            std::cerr << "Filesystem error: " << e.what() << std::endl;
            return;
        }
        std::ofstream ofs(filename_);
        if (!ofs.is_open()) {
            std::cerr << "Failed to open " << filename_ << " for writing." << std::endl;
            return;
        }

        ofs << std::left << std::setw(10) << "Frame" 
            << std::left << std::setw(20) << "E2E latency(ms)" 
            // << std::left << std::setw(25) << "Trans latency(ms)" 
            << "\n";

        for (const auto& entry : log_entries) {
            int64_t e2e_latency = (entry.pull_time_ms != -1 && entry.push_time_ms != -1) ? 
                                   (entry.pull_time_ms - entry.push_time_ms) : -1;
            // int64_t trans_latency = (entry.pull_before_dec_ms != -1 && entry.push_after_enc_ms != -1) ? 
            //                          (entry.pull_before_dec_ms - entry.push_after_enc_ms) : -1;

            ofs << std::left << std::setw(10) << entry.frame_number
                << std::left << std::setw(20) << (e2e_latency != -1 ? std::to_string(e2e_latency) + " ms" : "N/A")
                // << std::left << std::setw(25) << (trans_latency != -1 ? std::to_string(trans_latency) + " ms" : "N/A")
                << "\n";
        }

        ofs.close();
        std::cout << "Timing information written to " << filename_ << std::endl;
    }

private:
    struct LogEntry {
        int frame_number;
        int64_t push_time_ms;
        int64_t pull_time_ms;
        int64_t push_after_enc_ms;
        int64_t pull_before_dec_ms;

        LogEntry(int fn, int64_t pt, int64_t plt, int64_t pae, int64_t pbd)
            : frame_number(fn), push_time_ms(pt), pull_time_ms(plt), push_after_enc_ms(pae), pull_before_dec_ms(pbd) {}
    };

    std::vector<LogEntry> log_entries;
    std::mutex mutex_;
    std::string filename_;
};

// Timing maps for pull threads (map from frame number to pull time)
struct PullTiming {
    int64_t pull_time_ms_before_dec;
    int64_t pull_time_ms;
};

// FrameQueue class remains unchanged
class FrameQueue {
public:
    std::queue<AVFrame*> queue;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    bool finished;
    size_t max_size;

    FrameQueue(size_t max = 1000) : finished(false), max_size(max) {
        pthread_mutex_init(&mutex, NULL);
        pthread_cond_init(&cond, NULL);
    }

    ~FrameQueue() {
        pthread_mutex_destroy(&mutex);
        pthread_cond_destroy(&cond);
    }

    void enqueue(AVFrame* frame) {
        pthread_mutex_lock(&mutex);
        while (queue.size() >= max_size && !finished) {
            pthread_cond_wait(&cond, &mutex);
        }
        if (finished) {
            pthread_mutex_unlock(&mutex);
            av_frame_free(&frame);
            return;
        }
        queue.push(frame);
        pthread_cond_signal(&cond);
        pthread_mutex_unlock(&mutex);
    }

    AVFrame* dequeue() {
        pthread_mutex_lock(&mutex);
        while (queue.empty() && !finished) {
            pthread_cond_wait(&cond, &mutex);
        }
        AVFrame* frame = NULL;
        if (!queue.empty()) {
            frame = queue.front();
            queue.pop();
            pthread_cond_signal(&cond);
        }
        pthread_mutex_unlock(&mutex);
        return frame;
    }

    bool is_empty() {
        return queue.empty();
    }

    void set_finished() {
        pthread_mutex_lock(&mutex);
        finished = true;
        pthread_cond_broadcast(&cond);
        pthread_mutex_unlock(&mutex);
    }
};

// PushStreamContext structure remains unchanged
struct PushStreamContext {
    AVFormatContext* input_fmt_ctx;
    AVCodecContext* video_dec_ctx;
    int video_stream_idx;
    AVFormatContext* output_fmt_ctx;
    AVCodecContext* video_enc_ctx;
    AVStream* out_video_stream;
    struct SwsContext* sws_ctx;
    FrameQueue* frame_queue;
    int64_t start_time;
};

// PullArgs structure to pass multiple arguments to pull_stream
struct PullArgs {
    char* input_url;
    int index;
    int num_pull;
};

// Modified pull_stream function to handle multiple pull URLs and logging
void* pull_stream(void* args) {
    PullArgs* pull_args = static_cast<PullArgs*>(args);
    int index = pull_args->index;

    // Create a TimingLogger instance for this pull thread
    std::stringstream ss;
    ss <<  "result/latency_" << get_timestamp_with_ms() << ".txt"; 
    std::string log_filename = ss.str();
    TimingLogger logger(log_filename);

    pthread_mutex_lock(&cout_mutex);
    std::cout << "[Pull Thread " << index << "] Starting pull_stream with RTP..." << std::endl;
    pthread_mutex_unlock(&cout_mutex);

    avformat_network_init();

    AVFormatContext* input_fmt_ctx = nullptr;
    int ret = 0;

    AVDictionary* options = nullptr;
    // RTP specific options
    av_dict_set(&options, "buffer_size", "1048576", 0);      // Increase buffer size to 1MB
    av_dict_set(&options, "reorder_queue_size", "0", 0);     // Disable reordering for lower latency
    av_dict_set(&options, "max_delay", "0", 0);              // Minimize buffering delay
    av_dict_set(&options, "flags", "low_delay", 0);          // Enable low delay
    av_dict_set(&options, "protocol_whitelist", "file,rtp,udp", 0);
    av_dict_set(&options, "aud", "1", 0);
    // Set timeout for RTP stream
    av_dict_set(&options, "timeout", "5000000", 0);  // 5 seconds timeout
    av_dict_set(&options, "probesize", "32648", 0);          // Probe size 32648
    av_dict_set(&options, "analyzeduration", "0", 0);        // Analyze duration 0 second

    // Set input format to RTP
    std::string sdp_file = "stream.sdp";
    ret = avformat_open_input(&input_fmt_ctx, sdp_file.c_str(), nullptr, &options);
    if (ret < 0) {
        pthread_mutex_lock(&cout_mutex);
        char errbuf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, errbuf, sizeof(errbuf));
        std::cerr << "[Pull Thread " << index << "] Could not open SDP file: " << errbuf << std::endl;
        pthread_mutex_unlock(&cout_mutex);

        av_dict_free(&options);
        avformat_network_deinit();
        return nullptr;
    }

    // Open RTP input
    // ret = avformat_open_input(&input_fmt_ctx, input_url, input_format, &options);
    // if (ret < 0) {
    //     pthread_mutex_lock(&cout_mutex);
    //     char errbuf[AV_ERROR_MAX_STRING_SIZE];
    //     av_strerror(ret, errbuf, sizeof(errbuf));
    //     std::cerr << "[Pull Thread " << index << "] Could not open RTP input: " << errbuf << std::endl;
    //     pthread_mutex_unlock(&cout_mutex);

    //     av_dict_free(&options);
    //     avformat_network_deinit();
    //     return nullptr;
    // }

    ret = avformat_find_stream_info(input_fmt_ctx, nullptr);
    if (ret < 0) {
        pthread_mutex_lock(&cout_mutex);
        char errbuf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, errbuf, sizeof(errbuf));
        std::cerr << "[Pull Thread " << index << "] Failed to retrieve input stream information: " << errbuf << std::endl;
        pthread_mutex_unlock(&cout_mutex);
        avformat_close_input(&input_fmt_ctx);
        av_dict_free(&options);
        avformat_network_deinit();
        return nullptr;
    }

    int video_stream_index = -1;
    for (unsigned int i = 0; i < input_fmt_ctx->nb_streams; i++) {
        AVStream* in_stream = input_fmt_ctx->streams[i];
        if (in_stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            video_stream_index = i;
            break;
        }
    }

    if (video_stream_index == -1) {
        pthread_mutex_lock(&cout_mutex);
        std::cerr << "[Pull Thread " << index << "] No video stream found" << std::endl;
        pthread_mutex_unlock(&cout_mutex);
        avformat_close_input(&input_fmt_ctx);
        av_dict_free(&options);
        avformat_network_deinit();
        return nullptr;
    }

    AVCodecParameters* codecpar = input_fmt_ctx->streams[video_stream_index]->codecpar;
    const AVCodec* codec = avcodec_find_decoder(codecpar->codec_id);
    if (!codec) {
        pthread_mutex_lock(&cout_mutex);
        std::cerr << "[Pull Thread " << index << "] Could not find the decoder" << std::endl;
        pthread_mutex_unlock(&cout_mutex);
        avformat_close_input(&input_fmt_ctx);
        av_dict_free(&options);
        avformat_network_deinit();
        return nullptr;
    }

    AVCodecContext* codec_ctx = avcodec_alloc_context3(codec);
    if (!codec_ctx) {
        pthread_mutex_lock(&cout_mutex);
        std::cerr << "[Pull Thread " << index << "] Could not allocate codec context" << std::endl;
        pthread_mutex_unlock(&cout_mutex);
        avformat_close_input(&input_fmt_ctx);
        av_dict_free(&options);
        avformat_network_deinit();
        return nullptr;
    }

    ret = avcodec_parameters_to_context(codec_ctx, codecpar);
    if (ret < 0) {
        pthread_mutex_lock(&cout_mutex);
        std::cerr << "[Pull Thread " << index << "] Failed to copy codec parameters to codec context" << std::endl;
        pthread_mutex_unlock(&cout_mutex);
        avcodec_free_context(&codec_ctx);
        avformat_close_input(&input_fmt_ctx);
        av_dict_free(&options);
        avformat_network_deinit();
        return nullptr;
    }
    // codec_ctx->thread_count = 1;
    codec_ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
    // codec_ctx->thread_count = 0;
    // codec_ctx->flags2 |= AV_CODEC_FLAG2_FAST;

    ret = avcodec_open2(codec_ctx, codec, nullptr);
    if (ret < 0) {
        pthread_mutex_lock(&cout_mutex);
        std::cerr << "[Pull Thread " << index << "] Could not open codec" << std::endl;
        pthread_mutex_unlock(&cout_mutex);
        avcodec_free_context(&codec_ctx);
        avformat_close_input(&input_fmt_ctx);
        av_dict_free(&options);
        avformat_network_deinit();
        return nullptr;
    }

    AVFrame* frame = av_frame_alloc();
    if (!frame) {
        pthread_mutex_lock(&cout_mutex);
        std::cerr << "[Pull Thread " << index << "] Could not allocate frame" << std::endl;
        pthread_mutex_unlock(&cout_mutex);
        avcodec_free_context(&codec_ctx);
        avformat_close_input(&input_fmt_ctx);
        av_dict_free(&options);
        avformat_network_deinit();
        return nullptr;
    }

    AVPacket* packet = av_packet_alloc();
    if (!packet) {
        pthread_mutex_lock(&cout_mutex);
        std::cerr << "[Pull Thread " << index << "] Could not allocate packet" << std::endl;
        pthread_mutex_unlock(&cout_mutex);
        av_frame_free(&frame);
        avcodec_free_context(&codec_ctx);
        avformat_close_input(&input_fmt_ctx);
        av_dict_free(&options);
        avformat_network_deinit();
        return nullptr;
    }

    int64_t frame_count = 0;
    int64_t last_packet_time = get_current_time_us();
    const int64_t timeout_us = 5000000; // 5 seconds timeout

    // Set timeout for read operations
    input_fmt_ctx->interrupt_callback.callback = [](void* ctx) -> int {
        int64_t* last_time = static_cast<int64_t*>(ctx);
        if (get_current_time_us() - *last_time > timeout_us) {
            return 1; // Interrupt operation
        }
        return 0; // Continue operation
    };
    input_fmt_ctx->interrupt_callback.opaque = &last_packet_time;

    while (true) {
        // Check for timeout
        if (get_current_time_us() - last_packet_time > timeout_us) {
            pthread_mutex_lock(&cout_mutex);
            std::cout << "[Pull Thread " << index << "] Timeout reached - no packets received for 5 seconds. Exiting." << std::endl;
            pthread_mutex_unlock(&cout_mutex);
            break;
        }

        ret = av_read_frame(input_fmt_ctx, packet);
        if (ret < 0) {
            // Check if it's EOF or another error
            if (ret == AVERROR_EOF) {
                pthread_mutex_lock(&cout_mutex);
                std::cout << "[Pull Thread " << index << "] End of stream reached." << std::endl;
                pthread_mutex_unlock(&cout_mutex);
                break;
            } else if (ret == AVERROR(EAGAIN)) {
                // Resource temporarily unavailable, continue and check timeout
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            } else {
                // Some other error occurred
                char errbuf[AV_ERROR_MAX_STRING_SIZE];
                av_strerror(ret, errbuf, sizeof(errbuf));
                pthread_mutex_lock(&cout_mutex);
                std::cerr << "[Pull Thread " << index << "] Error reading frame: " << errbuf << std::endl;
                pthread_mutex_unlock(&cout_mutex);
                
                // Don't exit, just try to read the next frame
                continue;
            }
        }

        // Update the last packet time
        last_packet_time = get_current_time_us();

        if (packet->stream_index == video_stream_index) {
            // Extract embedded timestamp from packet
            int64_t embedded_frame_id = extract_frame_id_from_packet(packet, 8);
            
            int64_t pull_time_ms_before_dec = get_current_time_us() / 1000;
            int64_t pull_time_ms = get_current_time_us() / 1000;

            
            if (embedded_frame_id < 10000 && embedded_frame_id > 0) {
                std::cout << "Frame " << embedded_frame_id << " pulled at " << get_current_time_us() << std::endl;

                // Add entry to TimingLogger
                logger.add_entry(
                    static_cast<int>(embedded_frame_id),
                    push_timestamps[embedded_frame_id - 1],
                    pull_time_ms,
                    push_timestamps_after_enc[embedded_frame_id - 1],
                    pull_time_ms_before_dec
                );
                frame_count++;
            }
        }

        av_packet_unref(packet);
    }

    // Write the log to file
    logger.write_to_file();

    av_packet_free(&packet);
    av_frame_free(&frame);
    avcodec_free_context(&codec_ctx);
    avformat_close_input(&input_fmt_ctx);
    av_dict_free(&options);
    avformat_network_deinit();

    pthread_mutex_lock(&cout_mutex);
    std::cout << "[Pull Thread " << index << "] Finished pull_stream." << std::endl;
    pthread_mutex_unlock(&cout_mutex);

    return nullptr;
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

// void* push_stream_directly(void* args) {
//     char **my_args = (char **)args;
//     char *input_filename = my_args[0];
//     char *output_url = my_args[1];

//     pthread_mutex_lock(&cout_mutex);
//     printf("[Push Thread] Starting push_stream with RTSP...\n");
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
//     ret = avformat_alloc_output_context2(&output_fmt_ctx, NULL, "rtp", output_url);
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

//     // Set up UDP-specific options for low latency
//     AVDictionary* rtp_options = NULL;
//     av_dict_set(&rtp_options, "listen_timeout", "5000000", 0);    // Listen timeout 5 seconds
//     av_dict_set(&rtp_options, "max_delay", "500000", 0);          // Max delay 500ms
//     av_dict_set(&rtp_options, "reorder_queue_size", "10", 0);     // Reorder queue size
//     av_dict_set(&rtp_options, "buffer_size", "1048576", 0);       // 1MB buffer
//     av_dict_set(&rtp_options, "pkt_size", "1316", 0);             // Optimal packet size
//     av_dict_set(&rtp_options, "flush_packets", "1", 0);           // Flush packets immediately

//     char sdp_buffer[4096];
//     int sdp_ret = av_sdp_create(&output_fmt_ctx, 1, sdp_buffer, sizeof(sdp_buffer));
//     if (sdp_ret < 0) {
//         std::cerr << "Failed to create SDP" << std::endl;
//     } else {
//         // Save SDP to file
//         std::ofstream sdp_file("stream_ul.sdp");
//         if (sdp_file.is_open()) {
//             sdp_file << sdp_buffer;
//             sdp_file.close();
//             std::cout << "SDP file generated: stream_ul.sdp" << std::endl;
//             std::cout << "SDP Content:\n" << sdp_buffer << std::endl;
//             std::cout << "----------------------\n";
//         } else {
//             std::cerr << "Could not open stream_ul.sdp for writing" << std::endl;
//         }
//     }

//     // Open output URL with UDP options
//     ret = avio_open2(&output_fmt_ctx->pb, output_url, AVIO_FLAG_WRITE, NULL, &rtp_options);
//     CHECK_ERR(ret, "Could not open output URL");

//     // Set the maximum interleave delta to a very low value for decreased latency
//     output_fmt_ctx->max_interleave_delta = 0;

//     // Write header
//     ret = avformat_write_header(output_fmt_ctx, &rtp_options);
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

//             // Write packet
//             int64_t push_time_ms = get_current_time_us() / 1000;
//             push_timestamps[frame_count] = push_time_ms;
//             push_timestamps_after_enc[frame_count] = push_time_ms;
//             // add_frame_index_to_packet(packet, frame_count, 8);
//             ret = av_write_frame(output_fmt_ctx, packet);
//             if (ret < 0) {
//                 pthread_mutex_lock(&cout_mutex);
//                 char errbuf[AV_ERROR_MAX_STRING_SIZE];
//                 av_strerror(ret, errbuf, sizeof(errbuf));
//                 fprintf(stderr, "[Push Thread] Error writing packet: %s\n", errbuf);
//                 pthread_mutex_unlock(&cout_mutex);
//                 break;
//             }
//             AVPacket* empty_pkt = create_timestamp_packet(packet, get_current_time_us());
//             if (empty_pkt) {
//                 int empty_write_ret = av_write_frame(output_fmt_ctx, empty_pkt);
//                 if (empty_write_ret < 0) {
//                     std::cerr << "Error writing empty packet to output" << std::endl;
//                 }
//                 av_packet_free(&empty_pkt);
//             }
//             frame_count++;
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

//     pthread_mutex_lock(&cout_mutex);
//     printf("[Push Thread] Finished push_stream.\n");
//     pthread_mutex_unlock(&cout_mutex);

//     return NULL;
// }

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

    const AVBitStreamFilter *bsf = av_bsf_get_by_name("h264_mp4toannexb");
    AVBSFContext *bsf_ctx = NULL;
    
    if (bsf) {
        ret = av_bsf_alloc(bsf, &bsf_ctx);
        if (ret < 0) {
            fprintf(stderr, "Failed to allocate bitstream filter context\n");
            exit(1);
        }

        ret = avcodec_parameters_copy(bsf_ctx->par_in, codecpar);
        if (ret < 0) {
            fprintf(stderr, "Failed to copy codec parameters\n");
            exit(1);
        }

        ret = av_bsf_init(bsf_ctx);
        if (ret < 0) {
            fprintf(stderr, "Failed to initialize bitstream filter\n");
            exit(1);
        }
    }

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

    // Copy codec parameters from BSF output (if used) or input
    if (bsf_ctx) {
        ret = avcodec_parameters_copy(out_stream->codecpar, bsf_ctx->par_out);
    } else {
        ret = avcodec_parameters_copy(out_stream->codecpar, codecpar);
    }
    CHECK_ERR(ret, "Failed to copy codec parameters to output stream");

    out_stream->codecpar->codec_tag = 0;

    // Set up UDP-specific options for low latency
    AVDictionary* rtp_options = NULL;
    av_dict_set(&rtp_options, "listen_timeout", "5000000", 0);
    av_dict_set(&rtp_options, "max_delay", "500000", 0);
    av_dict_set(&rtp_options, "reorder_queue_size", "10", 0);
    av_dict_set(&rtp_options, "buffer_size", "1048576", 0);
    av_dict_set(&rtp_options, "pkt_size", "1316", 0);
    av_dict_set(&rtp_options, "flush_packets", "1", 0);

    // Generate SDP
    char sdp_buffer[4096];
    int sdp_ret = av_sdp_create(&output_fmt_ctx, 1, sdp_buffer, sizeof(sdp_buffer));
    if (sdp_ret < 0) {
        std::cerr << "Failed to create SDP" << std::endl;
    } else {
        std::ofstream sdp_file("stream_ul.sdp");
        if (sdp_file.is_open()) {
            sdp_file << sdp_buffer;
            sdp_file.close();
            std::cout << "SDP file generated: stream_ul.sdp" << std::endl;
            std::cout << "SDP Content:\n" << sdp_buffer << std::endl;
            std::cout << "----------------------\n";
        }
    }

    // Open output URL
    ret = avio_open2(&output_fmt_ctx->pb, output_url, AVIO_FLAG_WRITE, NULL, &rtp_options);
    CHECK_ERR(ret, "Could not open output URL");

    output_fmt_ctx->max_interleave_delta = 0;

    ret = avformat_write_header(output_fmt_ctx, &rtp_options);
    CHECK_ERR(ret, "Error occurred when writing header to output");

    AVPacket* packet = av_packet_alloc();
    AVPacket* filtered_packet = av_packet_alloc();
    if (!packet || !filtered_packet) {
        fprintf(stderr, "[Push Thread] Could not allocate packet.\n");
        exit(1);
    }

    AVRational time_base = input_fmt_ctx->streams[video_stream_idx]->time_base;
    auto start_time = av_gettime();
    int64_t frame_count = 0;

    while (av_read_frame(input_fmt_ctx, packet) >= 0) {
        if (packet->stream_index == video_stream_idx) {
            AVPacket* pkt_to_process = packet;

            if (bsf_ctx) {
                ret = av_bsf_send_packet(bsf_ctx, packet);
                if (ret < 0) {
                    fprintf(stderr, "Failed to send packet to BSF\n");
                    av_packet_unref(packet);
                    continue;
                }

                ret = av_bsf_receive_packet(bsf_ctx, filtered_packet);
                if (ret < 0) {
                    if (ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) {
                        fprintf(stderr, "Failed to receive packet from BSF\n");
                    }
                    av_packet_unref(packet);
                    continue;
                }
                pkt_to_process = filtered_packet;
            }

            add_frame_index_to_packet(pkt_to_process, frame_count + 1, 4);

            pkt_to_process->stream_index = out_stream->index;
            int64_t pts = pkt_to_process->pts;
            if (pts == AV_NOPTS_VALUE) {
                pts = pkt_to_process->dts;
            }
            if (pts == AV_NOPTS_VALUE) {
                fprintf(stderr, "Packet has no valid pts or dts.\n");
                av_packet_unref(packet);
                if (bsf_ctx) av_packet_unref(filtered_packet);
                continue;
            }

            int64_t pts_time = av_rescale_q(pts, time_base, AV_TIME_BASE_Q);
            av_packet_rescale_ts(pkt_to_process, time_base, out_stream->time_base);

            int64_t now = av_gettime() - start_time;
            if (pts_time > now) {
                int64_t sleep_time = pts_time - now;
                if (sleep_time > 0) {
                    av_usleep(sleep_time);
                }
            }

            int64_t push_time_ms = get_current_time_us() / 1000;
            push_timestamps[frame_count] = push_time_ms;
            push_timestamps_after_enc[frame_count] = push_time_ms;

            ret = av_write_frame(output_fmt_ctx, pkt_to_process);
            if (ret < 0) {
                char errbuf[AV_ERROR_MAX_STRING_SIZE];
                av_strerror(ret, errbuf, sizeof(errbuf));
                fprintf(stderr, "[Push Thread] Error writing packet: %s\n", errbuf);
                av_packet_unref(packet);
                if (bsf_ctx) av_packet_unref(filtered_packet);
                break;
            }

            AVPacket* empty_pkt = create_timestamp_packet(pkt_to_process, get_current_time_us());
            if (empty_pkt) {
                av_write_frame(output_fmt_ctx, empty_pkt);
                av_packet_free(&empty_pkt);
            }

            frame_count++;

            if (bsf_ctx) {
                av_packet_unref(filtered_packet);
            }
        }
        av_packet_unref(packet);
    }

    if (bsf_ctx) {
        // Flush BSF
        av_bsf_send_packet(bsf_ctx, NULL);
        while (av_bsf_receive_packet(bsf_ctx, filtered_packet) >= 0) {
            add_frame_index_to_packet(filtered_packet, frame_count++, 4);
            filtered_packet->stream_index = out_stream->index;
            av_packet_rescale_ts(filtered_packet, time_base, out_stream->time_base);
            av_write_frame(output_fmt_ctx, filtered_packet);
            av_packet_unref(filtered_packet);
        }
        av_bsf_free(&bsf_ctx);
    }

    // Write trailer
    ret = av_write_trailer(output_fmt_ctx);
    if (ret < 0) {
        char errbuf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, errbuf, sizeof(errbuf));
        fprintf(stderr, "[Push Thread] Error writing trailer: %s\n", errbuf);
    }

    av_packet_free(&packet);
    av_packet_free(&filtered_packet);
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

static const uint8_t FRAME_INDEX_UUID[16] = {
    0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0xDE, 0xF0,
    0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88
};

std::vector<uint8_t> remove_emulation_prevention(const uint8_t* data, int size) {
    std::vector<uint8_t> output;
    output.reserve(size);
    
    int i = 0;
    while (i < size) {
        // Check for emulation prevention pattern: 0x00 0x00 0x03
        if (i + 2 < size && 
            data[i] == 0x00 && 
            data[i+1] == 0x00 && 
            data[i+2] == 0x03) {
            
            // Check if next byte exists and is 0x00, 0x01, 0x02, or 0x03
            if (i + 3 < size && data[i+3] <= 0x03) {
                // This is an emulation prevention sequence
                output.push_back(0x00);
                output.push_back(0x00);
                i += 3; // Skip the 0x03 emulation prevention byte
            } else {
                // Not emulation prevention, just copy
                output.push_back(data[i]);
                i++;
            }
        } else {
            output.push_back(data[i]);
            i++;
        }
    }
    
    return output;
}

// Function to extract specified number of frame indices from packet and restore original packet
int64_t extract_frame_id_from_packet(AVPacket* pkt, int expected_count) {
    if (!pkt || !pkt->data || pkt->size < 20) {
        std::cerr << "Invalid packet" << std::endl;
        return -1;
    }
    
    uint8_t* data = pkt->data;
    int data_size = pkt->size;
    
    std::vector<int64_t> frame_indices;
    int sei_start = -1;
    int sei_end = -1;
    
    // Find SEI NAL unit
    for (int pos = 0; pos < data_size - 5; pos++) {
        // Look for start code
        if ((pos + 4 < data_size && 
             data[pos] == 0x00 && data[pos+1] == 0x00 && 
             data[pos+2] == 0x00 && data[pos+3] == 0x01 && 
             (data[pos+4] & 0x1F) == 0x06) ||
            (pos + 3 < data_size && 
             data[pos] == 0x00 && data[pos+1] == 0x00 && 
             data[pos+2] == 0x01 && 
             (data[pos+3] & 0x1F) == 0x06)) {
            
            sei_start = pos;
            int start_code_len = (data[pos+2] == 0x01) ? 3 : 4;
            int sei_data_start = pos + start_code_len + 1; // After NAL header
            
            // Find end of SEI NAL unit
            sei_end = data_size; // Default to end of packet
            for (int search_pos = sei_data_start; search_pos < data_size - 3; search_pos++) {
                if (data[search_pos] == 0x00 && data[search_pos+1] == 0x00 &&
                    (data[search_pos+2] == 0x00 || data[search_pos+2] == 0x01)) {
                    sei_end = search_pos;
                    break;
                }
            }
            
            // Extract SEI data and remove emulation prevention
            int sei_raw_size = sei_end - sei_data_start;
            std::vector<uint8_t> sei_data = remove_emulation_prevention(
                data + sei_data_start, sei_raw_size);
            
            // Parse SEI payload
            size_t pos_in_sei = 0;
            while (pos_in_sei < sei_data.size() && pos_in_sei < sei_data.size() - 1) {
                // Check for trailing bits
                if (sei_data[pos_in_sei] == 0x80) {
                    break;
                }
                
                // Read payload type
                int payload_type = 0;
                while (pos_in_sei < sei_data.size() && sei_data[pos_in_sei] == 0xFF) {
                    payload_type += 255;
                    pos_in_sei++;
                }
                if (pos_in_sei < sei_data.size()) {
                    payload_type += sei_data[pos_in_sei++];
                }
                
                // Read payload size
                int payload_size = 0;
                while (pos_in_sei < sei_data.size() && sei_data[pos_in_sei] == 0xFF) {
                    payload_size += 255;
                    pos_in_sei++;
                }
                if (pos_in_sei < sei_data.size()) {
                    payload_size += sei_data[pos_in_sei++];
                }
                
                // Check if this is our custom SEI (type 5)
                if (payload_type == 5 && pos_in_sei + static_cast<size_t>(payload_size) <= sei_data.size()) {
                    // Check UUID
                    bool uuid_match = true;
                    if (payload_size >= 20) {
                        for (int i = 0; i < 16; i++) {
                            if (sei_data[pos_in_sei + i] != FRAME_INDEX_UUID[i]) {
                                uuid_match = false;
                                break;
                            }
                        }
                        
                        if (uuid_match) {
                            // Skip UUID
                            pos_in_sei += 16;
                            
                            // Read count (4 bytes, big endian)
                            uint32_t count = (sei_data[pos_in_sei] << 24) |
                                           (sei_data[pos_in_sei + 1] << 16) |
                                           (sei_data[pos_in_sei + 2] << 8) |
                                           sei_data[pos_in_sei + 3];
                            pos_in_sei += 4;
                            
                            std::cout << "Found matching UUID, count = " << count << std::endl;
                            
                            // Debug: show raw bytes after UUID and count
                            std::cout << "Raw bytes after count: ";
                            for (int k = 0; k < std::min(32, static_cast<int>(sei_data.size() - pos_in_sei)); k++) {
                                std::cout << std::hex << std::setw(2) << std::setfill('0') 
                                         << (int)sei_data[pos_in_sei + k] << " ";
                            }
                            std::cout << std::dec << std::endl;
                            
                            // Sanity check
                            if (count > 100 || count == 0) {
                                std::cerr << "Invalid count: " << count << std::endl;
                                break;
                            }
                            
                            // Read frame indices
                            frame_indices.clear();
                            int remaining_payload = payload_size - 20; // UUID(16) + count(4)
                            int expected_indices_size = count * 8;
                            
                            if (expected_indices_size > remaining_payload) {
                                std::cerr << "Warning: Payload size mismatch. Expected " 
                                         << expected_indices_size << " bytes for indices, but only "
                                         << remaining_payload << " bytes remain in payload" << std::endl;
                            }
                            
                            for (uint32_t i = 0; i < count; i++) {
                                if (pos_in_sei + 8 > sei_data.size()) {
                                    std::cerr << "Reached end of SEI data while reading index " 
                                             << i << std::endl;
                                    break;
                                }
                                
                                if (static_cast<int>(i * 8) >= remaining_payload) {
                                    std::cerr << "Reached end of payload while reading index " 
                                             << i << std::endl;
                                    break;
                                }
                                
                                uint64_t frame_idx = 0;
                                for (int j = 0; j < 8; j++) {
                                    frame_idx = (frame_idx << 8) | sei_data[pos_in_sei++];
                                }
                                frame_indices.push_back(frame_idx);
                            }
                            
                            break; // Found our SEI
                        }
                    }
                    
                    // Skip this payload
                    pos_in_sei += payload_size - 16; // Already read 16 bytes of UUID
                } else {
                    // Skip this payload
                    pos_in_sei += payload_size;
                }
            }
            
            if (!frame_indices.empty()) {
                break; // Found our data
            }
        }
    }
    
    if (frame_indices.empty()) {
        std::cerr << "No frame index SEI found in packet" << std::endl;
        return -1;
    }
    
    // Print extracted frame indices
    std::cout << "Extracted " << frame_indices.size() << " frame indices from SEI: ";
    for (size_t i = 0; i < frame_indices.size() && i < 10; i++) {
        std::cout << frame_indices[i];
        if (i < frame_indices.size() - 1) std::cout << ", ";
    }
    if (frame_indices.size() > 10) std::cout << "...";
    std::cout << std::endl;
    
    // Remove SEI from packet
    if (sei_start >= 0 && sei_end > sei_start) {
        AVPacket* new_pkt = av_packet_alloc();
        if (!new_pkt) {
            std::cerr << "Could not allocate new packet" << std::endl;
            return frame_indices[0];
        }
        
        int before_size = sei_start;
        int after_size = data_size - sei_end;
        int new_size = before_size + after_size;
        
        if (new_size <= 0) {
            std::cerr << "Invalid packet size after SEI removal" << std::endl;
            av_packet_free(&new_pkt);
            return frame_indices[0];
        }
        
        int ret = av_new_packet(new_pkt, new_size + AV_INPUT_BUFFER_PADDING_SIZE);
        if (ret < 0) {
            std::cerr << "Could not allocate packet data" << std::endl;
            av_packet_free(&new_pkt);
            return frame_indices[0];
        }
        
        // Copy data
        if (before_size > 0) {
            memcpy(new_pkt->data, data, before_size);
        }
        if (after_size > 0) {
            memcpy(new_pkt->data + before_size, data + sei_end, after_size);
        }
        
        // Zero padding
        memset(new_pkt->data + new_size, 0, AV_INPUT_BUFFER_PADDING_SIZE);
        new_pkt->size = new_size;
        
        // Copy packet metadata
        new_pkt->pts = pkt->pts;
        new_pkt->dts = pkt->dts;
        new_pkt->stream_index = pkt->stream_index;
        new_pkt->flags = pkt->flags;
        new_pkt->duration = pkt->duration;
        new_pkt->pos = pkt->pos;
        
        // Replace packet
        av_packet_unref(pkt);
        av_packet_move_ref(pkt, new_pkt);
        av_packet_free(&new_pkt);
    }
    
    return frame_indices[0];
}

// Add frame index to packet data (write specified number of consecutive frame indices)
void add_frame_index_to_packet(AVPacket* pkt, uint64_t frame_index, int count) {
    std::vector<uint8_t> sei_payload;
    
    // Add UUID
    sei_payload.insert(sei_payload.end(), FRAME_INDEX_UUID, FRAME_INDEX_UUID + 16);
    
    // Add count (4 bytes, big endian)
    sei_payload.push_back((count >> 24) & 0xFF);
    sei_payload.push_back((count >> 16) & 0xFF);
    sei_payload.push_back((count >> 8) & 0xFF);
    sei_payload.push_back(count & 0xFF);
    
    // Add frame indices (8 bytes each, big endian)
    for (int i = 0; i < count; ++i) {
        uint64_t current_frame = frame_index + i;
        for (int j = 7; j >= 0; j--) {
            sei_payload.push_back((current_frame >> (j * 8)) & 0xFF);
        }
    }
    
    // Build SEI message with emulation prevention
    std::vector<uint8_t> sei_message;
    
    // SEI payload type: user_data_unregistered (5)
    sei_message.push_back(0x05);
    
    // SEI payload size - IMPORTANT: This is the size WITHOUT emulation prevention
    int payload_size = sei_payload.size();
    if (payload_size < 0xFF) {
        sei_message.push_back(payload_size);
    } else {
        // For larger payloads, use 0xFF bytes
        int remaining = payload_size;
        while (remaining >= 0xFF) {
            sei_message.push_back(0xFF);
            remaining -= 0xFF;
        }
        sei_message.push_back(remaining);
    }
    
    // Add the actual payload
    sei_message.insert(sei_message.end(), sei_payload.begin(), sei_payload.end());
    
    // Add rbsp_trailing_bits (0x80)
    sei_message.push_back(0x80);
    
    // Now add emulation prevention to the entire SEI message
    std::vector<uint8_t> sei_with_emulation;
    for (size_t i = 0; i < sei_message.size(); i++) {
        // Check if we need to insert emulation prevention byte
        if (i >= 2 && 
            sei_with_emulation.size() >= 2 &&
            sei_with_emulation[sei_with_emulation.size()-2] == 0x00 &&
            sei_with_emulation[sei_with_emulation.size()-1] == 0x00 &&
            sei_message[i] <= 0x03) {
            // Insert emulation prevention byte 0x03
            sei_with_emulation.push_back(0x03);
        }
        sei_with_emulation.push_back(sei_message[i]);
    }
    
    // Build complete NAL unit
    std::vector<uint8_t> final_sei_nal;
    
    // NAL unit start code (0x00000001)
    final_sei_nal.push_back(0x00);
    final_sei_nal.push_back(0x00);
    final_sei_nal.push_back(0x00);
    final_sei_nal.push_back(0x01);
    
    // NAL unit header for SEI (0x06)
    final_sei_nal.push_back(0x06);
    
    // Add the SEI message with emulation prevention
    final_sei_nal.insert(final_sei_nal.end(), sei_with_emulation.begin(), sei_with_emulation.end());
    
    // Create new packet with SEI + original data
    AVPacket* new_pkt = av_packet_alloc();
    if (!new_pkt) {
        std::cerr << "Could not allocate new packet" << std::endl;
        return;
    }
    
    int total_size = final_sei_nal.size() + pkt->size;
    int ret = av_new_packet(new_pkt, total_size + AV_INPUT_BUFFER_PADDING_SIZE);
    if (ret < 0) {
        std::cerr << "Could not allocate packet data" << std::endl;
        av_packet_free(&new_pkt);
        return;
    }
    
    // Copy SEI data first
    memcpy(new_pkt->data, final_sei_nal.data(), final_sei_nal.size());
    
    // Copy original packet data
    memcpy(new_pkt->data + final_sei_nal.size(), pkt->data, pkt->size);
    
    // Zero out padding
    memset(new_pkt->data + total_size, 0, AV_INPUT_BUFFER_PADDING_SIZE);
    
    // Set packet size (excluding padding)
    new_pkt->size = total_size;
    
    // Copy other packet properties
    new_pkt->pts = pkt->pts;
    new_pkt->dts = pkt->dts;
    new_pkt->stream_index = pkt->stream_index;
    new_pkt->flags = pkt->flags;
    new_pkt->duration = pkt->duration;
    new_pkt->pos = pkt->pos;
    
    // Replace original packet
    av_packet_unref(pkt);
    av_packet_move_ref(pkt, new_pkt);
    av_packet_free(&new_pkt);
}

void send_ping_and_wait_pong_from_url(const char* local_url, const char* remote_url) {
    // Parse local bind address from local_url (e.g. udp://192.168.2.2:10001)
    std::string slocal(local_url);
    std::regex re_local("rtp://([0-9.]+):(\\d+)");
    std::smatch mlocal;
    if (!std::regex_search(slocal, mlocal, re_local)) {
        fprintf(stderr, "Invalid UDP url: %s\n", local_url);
        exit(1);
    }
    std::string local_ip = mlocal[1];
    int local_port = std::stoi(mlocal[2]);
    std::cout << "local_ip: " << local_ip << ", local_port: " << local_port << std::endl;

    // Parse remote ip from remote_url (e.g. rtp://192.168.2.3:10001 or rtp://192.168.2.3:9000)
    std::string sremote(remote_url);
    std::regex re_remote_ip("([0-9.]+)");
    std::smatch mremote;
    if (!std::regex_search(sremote, mremote, re_remote_ip)) {
        fprintf(stderr, "Invalid remote url: %s\n", remote_url);
        exit(1);
    }
    std::string remote_ip = mremote[1];
    int remote_port = 10001; // Always send to transcoding's 10001
    std::cout << "remote_ip: " << remote_ip << ", remote_port: " << remote_port << std::endl;

    // Create UDP socket and bind to local_ip:local_port
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        perror("socket");
        exit(1);
    }
    sockaddr_in local_addr{};
    local_addr.sin_family = AF_INET;
    local_addr.sin_addr.s_addr = inet_addr(local_ip.c_str());
    local_addr.sin_port = htons(local_port);
    if (bind(sock, (sockaddr*)&local_addr, sizeof(local_addr)) < 0) {
        perror("bind");
        close(sock);
        exit(1);
    }
    // Prepare remote transcoding address
    sockaddr_in remote_addr{};
    remote_addr.sin_family = AF_INET;
    remote_addr.sin_addr.s_addr = inet_addr(remote_ip.c_str());
    remote_addr.sin_port = htons(remote_port);
    // Send ping to transcoding's 10001
    sendto(sock, "ping", 4, 0, (sockaddr*)&remote_addr, sizeof(remote_addr));
    char buf[128] = {0};
    struct timeval tv = {3, 0};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ssize_t n = recv(sock, buf, sizeof(buf), 0);
    if (n > 0 && strncmp(buf, "pong", 4) == 0) {
        // ok
    } else {
        fprintf(stderr, "Did not receive pong from transcoding.\n");
        close(sock);
        exit(1);
    }
    close(sock);
}

int main(int argc, char* argv[]) {
    if (argc < 4) {
        fprintf(stderr, "Usage: %s <push_input_file> <push_output_url> <pull_input_url1> [<pull_input_url2> ...]\n", argv[0]);
        fprintf(stderr, "Example: %s snow-scene.mp4 \"rtp://192.168.2.3:9000\" \"rtp://192.168.2.2:5004\"\n", argv[0]);
        return 1;
    }

    char *push_input_file = argv[1];
    char *push_output_url = argv[2];
    int num_pull = argc - 3;

    // Initialize FFmpeg network
    avformat_network_init();

    // UDP handshake: send a ping from the local pull IP:port to transcoding's 10001 port, wait for pong, then proceed.
    send_ping_and_wait_pong_from_url(argv[3], argv[2]);

    // Create pull threads
    std::vector<pthread_t> pull_thread_ids(num_pull);
    std::vector<PullArgs*> pull_args_list(num_pull);

    for (int i = 0; i < num_pull; ++i) {
        pull_args_list[i] = new PullArgs;
        pull_args_list[i]->input_url = argv[3 + i];
        pull_args_list[i]->index = i + 1; // Start indexing from 1
        pull_args_list[i]->num_pull = num_pull;

        int pull_ret = pthread_create(&pull_thread_ids[i], NULL, pull_stream, pull_args_list[i]);
        if (pull_ret != 0) {
            fprintf(stderr, "Failed to create pull thread %d.\n", i + 1);
            exit(1);
        }
    }

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
    sleep(2);

    int ret_create = pthread_create(&push_thread_id, NULL, push_stream_directly, push_args);
    if (ret_create != 0) {
        fprintf(stderr, "Failed to create push thread.\n");
        exit(1);
    }

    // Wait for all pull threads to finish
    for (int i = 0; i < num_pull; ++i) {
        pthread_join(pull_thread_ids[i], NULL);
        delete pull_args_list[i];
    }

    // Wait for push thread to finish
    pthread_join(push_thread_id, NULL);

    // Free push_args
    free(push_args[0]);
    free(push_args[1]);
    free(push_args);

    avformat_network_deinit();

    return 0;
}