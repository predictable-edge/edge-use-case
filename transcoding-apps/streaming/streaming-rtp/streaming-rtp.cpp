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
    std::strftime(buffer, sizeof(buffer), "%Y%m%d-%H%M%S", now_tm);
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
    int num_pull = pull_args->num_pull;

    // Create a TimingLogger instance for this pull thread
    std::stringstream ss;
    ss <<  "result/task" << num_pull << "/" << get_timestamp_with_ms() << "/"
       << "frame-" << index << ".log"; 
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
    // av_dict_set(&options, "reorder_queue_size", "0", 0);     // Disable reordering for lower latency
    // av_dict_set(&options, "max_delay", "0", 0);              // Minimize buffering delay
    av_dict_set(&options, "flags", "low_delay", 0);          // Enable low delay
    av_dict_set(&options, "protocol_whitelist", "file,rtp,udp", 0);

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
    while (av_read_frame(input_fmt_ctx, packet) >= 0) {
        if (packet->stream_index == video_stream_index) {
            int64_t pull_time_ms_before_dec = get_current_time_us() / 1000;
            int64_t pull_time_ms = get_current_time_us() / 1000;
            std::cout << "Frame " << frame_count + 1 << " pulled at " << get_current_time_us() << std::endl;

            // Add entry to TimingLogger
            logger.add_entry(
                static_cast<int>(frame_count + 1),
                push_timestamps[frame_count],
                pull_time_ms,
                push_timestamps_after_enc[frame_count],
                pull_time_ms_before_dec
            );
            frame_count++;
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
    ret = avformat_alloc_output_context2(&output_fmt_ctx, NULL, "rtsp", output_url);
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
    AVDictionary* rtsp_options = NULL;
    av_dict_set(&rtsp_options, "rtsp_transport", "udp", 0);        // Use UDP for RTP transport
    av_dict_set(&rtsp_options, "listen_timeout", "5000000", 0);    // Listen timeout 5 seconds
    av_dict_set(&rtsp_options, "max_delay", "500000", 0);          // Max delay 500ms
    av_dict_set(&rtsp_options, "reorder_queue_size", "10", 0);     // Reorder queue size
    av_dict_set(&rtsp_options, "buffer_size", "1048576", 0);       // 1MB buffer
    av_dict_set(&rtsp_options, "pkt_size", "1316", 0);             // Optimal packet size
    av_dict_set(&rtsp_options, "flush_packets", "1", 0);           // Flush packets immediately

    // Open output URL with UDP options
    // ret = avio_open2(&output_fmt_ctx->pb, output_url, AVIO_FLAG_WRITE, NULL, &rtsp_options);
    // CHECK_ERR(ret, "Could not open output URL");

    // Set the maximum interleave delta to a very low value for decreased latency
    output_fmt_ctx->max_interleave_delta = 0;

    // Write header
    ret = avformat_write_header(output_fmt_ctx, &rtsp_options);
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
            int64_t push_time_ms = get_current_time_us() / 1000;
            push_timestamps[frame_count] = push_time_ms;
            push_timestamps_after_enc[frame_count] = push_time_ms;
            ret = av_write_frame(output_fmt_ctx, packet);
            if (ret < 0) {
                pthread_mutex_lock(&cout_mutex);
                char errbuf[AV_ERROR_MAX_STRING_SIZE];
                av_strerror(ret, errbuf, sizeof(errbuf));
                fprintf(stderr, "[Push Thread] Error writing packet: %s\n", errbuf);
                pthread_mutex_unlock(&cout_mutex);
                break;
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

int main(int argc, char* argv[]) {
    if (argc < 4) {
        fprintf(stderr, "Usage: %s <push_input_file> <push_output_url> <pull_input_url1> [<pull_input_url2> ...]\n", argv[0]);
        fprintf(stderr, "Example: %s snow-scene.mp4 \"rtsp://192.168.2.3:9000/stream\" \"rtp://192.168.2.2:5004\"\n", argv[0]);
        return 1;
    }

    char *push_input_file = argv[1];
    char *push_output_url = argv[2];
    int num_pull = argc - 3;

    // Initialize FFmpeg network
    avformat_network_init();

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