#include <iostream>
#include <thread>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <string>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <sys/time.h>
#include <ctime>
#include <assert.h>
#include <vector>
#include <filesystem>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <edge_server/application_api.h>

// FFmpeg includes
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

// Helper function to convert FFmpeg error codes to std::string
std::string get_error_text(int errnum) {
    char errbuf[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_strerror(errnum, errbuf, sizeof(errbuf));
    return std::string(errbuf);
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

int64_t get_current_time_us() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return ((int64_t)tv.tv_sec * 1000000) + tv.tv_usec;
}

// Structure to hold frame data and timing information
struct FrameData {
    AVFrame* frame;
    std::chrono::steady_clock::time_point decode_start_time;
    std::chrono::steady_clock::time_point decode_end_time;
};

// Thread-safe queue for FrameData
class FrameQueue {
public:
    void push(const FrameData& frame_data) {
        std::unique_lock<std::mutex> lock(mutex_);
        queue_.push(frame_data);
        cond_var_.notify_one();
    }

    bool pop(FrameData& frame_data) {
        std::unique_lock<std::mutex> lock(mutex_);
        while (queue_.empty() && !finished_) {
            cond_var_.wait(lock);
        }
        if (queue_.empty())
            return false;
        frame_data = queue_.front();
        queue_.pop();
        return true;
    }

    size_t size() {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }

    bool is_empty() {
        return queue_.empty();
    }

    void set_finished() {
        std::unique_lock<std::mutex> lock(mutex_);
        finished_ = true;
        cond_var_.notify_all();
    }

    bool is_finished() const {
        return finished_;
    }

private:
    std::queue<FrameData> queue_;
    mutable std::mutex mutex_;
    std::condition_variable cond_var_;
    bool finished_ = false;
};

// Thread-safe queue for AVPacket*
class PacketQueue {
public:
    void push(AVPacket* packet) {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push(packet);
        cond_var_.notify_one();
    }

    bool pop(AVPacket*& packet) {
        std::unique_lock<std::mutex> lock(mutex_);
        while (queue_.empty() && !finished_) {
            cond_var_.wait(lock);
        }
        if (queue_.empty()) return false;
        packet = queue_.front();
        queue_.pop();
        return true;
    }

    bool is_empty() {
        return queue_.empty();
    }

    void set_finished() {
        std::lock_guard<std::mutex> lock(mutex_);
        finished_ = true;
        cond_var_.notify_all();
    }

private:
    std::queue<AVPacket*> queue_;
    std::mutex mutex_;
    std::condition_variable cond_var_;
    bool finished_ = false;
};

// Thread-safe vector to store timing information
class TimingLogger {
public:
    TimingLogger(const std::string& log_filename) : filename_(log_filename) {}

    void add_entry(int frame_number, double decode_time_ms, double encode_time_ms, double interval_ms) {
        std::lock_guard<std::mutex> lock(mutex_);
        log_entries.emplace_back(frame_number, decode_time_ms, encode_time_ms, interval_ms);
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
        ofs << std::fixed << std::setprecision(4);

        // Write header
        ofs << std::left << std::setw(10) << "Frame" 
            << std::left << std::setw(20) << "Decode Time (ms)" 
            << std::left << std::setw(20) << "Encode Time (ms)" 
            << std::left << std::setw(20) << "Transcode Time (ms)" 
            << "\n";

        // Write each entry
        for (const auto& entry : log_entries) {
            ofs << std::left << std::setw(10) << entry.frame_number
                << std::left << std::setw(20) << entry.decode_time_ms
                << std::left << std::setw(20) << entry.encode_time_ms
                << std::left << std::setw(20) << entry.interval_ms
                << "\n";
        }

        ofs.close();
        std::cout << "Timing information written to " << filename_ << std::endl;
    }

private:
    struct LogEntry {
        int frame_number;
        double decode_time_ms;
        double encode_time_ms;
        double interval_ms;

        LogEntry(int fn, double dt, double et, double it)
            : frame_number(fn), decode_time_ms(dt), encode_time_ms(et), interval_ms(it) {}
    };

    std::vector<LogEntry> log_entries;
    std::mutex mutex_;
    std::string filename_;
};

// Structure to hold decoder information
struct DecoderInfo {
    AVFormatContext* input_fmt_ctx;
    AVCodecContext* decoder_ctx;
    int video_stream_idx;
    AVRational input_time_base;
    double input_framerate;
};

// Structure to define encoder configurations
struct EncoderConfig {
    std::string output_url;
    int width;
    int height;
    int bitrate; // in kbps
    std::string log_filename;
    double framerate;
};

// Decoder Initialization and Function
bool initialize_decoder(const char* input_url, DecoderInfo& decoder_info) {
    // Initialize input format context
    decoder_info.input_fmt_ctx = nullptr;
    AVDictionary* format_opts = nullptr;
    av_dict_set(&format_opts, "probesize",       "32768",    0);
    av_dict_set(&format_opts, "analyzeduration", "0",        0); 
    if (avformat_open_input(&decoder_info.input_fmt_ctx, input_url, nullptr, &format_opts) < 0) {
        std::cerr << "Could not open input tcp stream: " << input_url << std::endl;
        av_dict_free(&format_opts);
        return false;
    }

    // Find stream information
    if (avformat_find_stream_info(decoder_info.input_fmt_ctx, nullptr) < 0) {
        std::cerr << "Could not find stream information" << std::endl;
        avformat_close_input(&decoder_info.input_fmt_ctx);
        return false;
    }

    // Find the first video stream
    decoder_info.video_stream_idx = -1;
    AVCodecParameters* codecpar = nullptr;
    for (unsigned int i = 0; i < decoder_info.input_fmt_ctx->nb_streams; ++i) {
        if (decoder_info.input_fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            decoder_info.video_stream_idx = i;
            codecpar = decoder_info.input_fmt_ctx->streams[i]->codecpar;
            break;
        }
    }

    if (decoder_info.video_stream_idx == -1) {
        std::cerr << "Could not find a video stream in the input" << std::endl;
        avformat_close_input(&decoder_info.input_fmt_ctx);
        return false;
    }

    // Find decoder
    const AVCodec* decoder = avcodec_find_decoder(codecpar->codec_id);
    if (!decoder) {
        std::cerr << "Could not find decoder for the video stream" << std::endl;
        avformat_close_input(&decoder_info.input_fmt_ctx);
        return false;
    }

    // Allocate decoder context
    decoder_info.decoder_ctx = avcodec_alloc_context3(decoder);
    if (!decoder_info.decoder_ctx) {
        std::cerr << "Could not allocate decoder context" << std::endl;
        avformat_close_input(&decoder_info.input_fmt_ctx);
        return false;
    }

    // Copy codec parameters to decoder context
    if (avcodec_parameters_to_context(decoder_info.decoder_ctx, codecpar) < 0) {
        std::cerr << "Failed to copy decoder parameters to context" << std::endl;
        avcodec_free_context(&decoder_info.decoder_ctx);
        avformat_close_input(&decoder_info.input_fmt_ctx);
        return false;
    }
    // ctx information must be setting before open2...
    decoder_info.decoder_ctx->thread_count = 0;
    decoder_info.decoder_ctx->thread_type = FF_THREAD_SLICE;
    decoder_info.decoder_ctx->flags2 |= AV_CODEC_FLAG2_FAST;
    decoder_info.decoder_ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;

    // decoder_info.decoder_ctx->thread_count = 4;
    // decoder_info.decoder_ctx->flags2 |= AV_CODEC_FLAG2_FAST;
    // Open decoder
    if (avcodec_open2(decoder_info.decoder_ctx, decoder, nullptr) < 0) {
        std::cerr << "Could not open decoder" << std::endl;
        avcodec_free_context(&decoder_info.decoder_ctx);
        avformat_close_input(&decoder_info.input_fmt_ctx);
        return false;
    }

    // Store input stream's time_base
    decoder_info.input_time_base = decoder_info.input_fmt_ctx->streams[decoder_info.video_stream_idx]->time_base;

    AVRational frame_rate_rational = av_guess_frame_rate(decoder_info.input_fmt_ctx, 
                                                         decoder_info.input_fmt_ctx->streams[decoder_info.video_stream_idx], 
                                                         nullptr);
    if (frame_rate_rational.num == 0 || frame_rate_rational.den == 0) {
        decoder_info.input_framerate = 30.0;
        std::cerr << "Warning: Could not determine input frame rate. Using default 30 FPS." << std::endl;
    } else {
        decoder_info.input_framerate = av_q2d(frame_rate_rational);
    }

    std::cout << "Input frame rate: " << decoder_info.input_framerate << " FPS" << std::endl;
    std::cout << "Decoder initialized successfully." << std::endl;
    return true;
}

uint64_t extract_timestamp(AVPacket *packet) {
    uint8_t *data = packet->data;
    int size = packet->size;
    
    if (size < 8) return AVERROR(EINVAL);
    
    uint32_t first_nal_size = (data[0] << 24) | (data[1] << 16) |
                              (data[2] << 8) | data[3];

    if (first_nal_size + 4 > static_cast<uint32_t>(size)) return AVERROR(EINVAL);

    if (data[4] != 0x06) {
        return AVERROR(EINVAL);
    }
    
    int sei_offset = 5;
    
    if (data[sei_offset] != 0x05) return AVERROR(EINVAL);
    sei_offset += 2;
    
    const uint8_t expected_uuid[16] = {
        0x54, 0x69, 0x6D, 0x65, // "Time"
        0x53, 0x74, 0x61, 0x6D, // "Stam"
        0x70, 0x00, 0x01, 0x02, 
        0x03, 0x04, 0x05, 0x06 
    };
    
    if (memcmp(data + sei_offset, expected_uuid, 16) != 0) {
        return AVERROR(EINVAL);
    }
    sei_offset += 16;
    
    uint64_t timestamp;
    memcpy(&timestamp, data + sei_offset, sizeof(uint64_t));
    
    int original_size = size - (4 + first_nal_size);
    uint8_t *restored_data = (uint8_t*)av_malloc(original_size);
    if (!restored_data) return AVERROR(ENOMEM);
    
    memcpy(restored_data, data + 4 + first_nal_size, original_size);
    
    av_buffer_unref(&packet->buf);
    packet->buf = av_buffer_create(restored_data, original_size, 
                                  av_buffer_default_free, NULL, 0);
    packet->data = restored_data;
    packet->size = original_size;
    
    return timestamp;
}

bool extract_request_header(AVPacket *packet, RequestHeader &header) {
    uint8_t *data = packet->data;
    int size = packet->size;
    
    if (size < 8) return false;
    
    uint32_t first_nal_size = (data[0] << 24) | (data[1] << 16) |
                              (data[2] << 8) | data[3];

    if (first_nal_size + 4 > static_cast<uint32_t>(size)) return false;

    if (data[4] != 0x06) {
        return false;
    }
    
    int sei_offset = 5;
    
    if (data[sei_offset] != 0x05) return false;
    sei_offset += 2;
    
    const uint8_t expected_uuid[16] = {
        0x54, 0x69, 0x6D, 0x65, // "Time"
        0x53, 0x74, 0x61, 0x6D, // "Stam"
        0x70, 0x00, 0x01, 0x02, 
        0x03, 0x04, 0x05, 0x06 
    };
    
    if (memcmp(data + sei_offset, expected_uuid, 16) != 0) {
        return false;
    }
    sei_offset += 16;
    
    memcpy(&header, data + sei_offset, sizeof(RequestHeader));
    
    int original_size = size - (4 + first_nal_size);
    uint8_t *restored_data = (uint8_t*)av_malloc(original_size);
    if (!restored_data) return false;
    
    memcpy(restored_data, data + 4 + first_nal_size, original_size);
    
    av_buffer_unref(&packet->buf);
    packet->buf = av_buffer_create(restored_data, original_size, 
                                  av_buffer_default_free, NULL, 0);
    packet->data = restored_data;
    packet->size = original_size;
    
    return true;
}

// Decoder Function
void packet_reading_thread(AVFormatContext* input_fmt_ctx, int video_stream_idx, PacketQueue& packet_queue) {
    AVPacket* packet = av_packet_alloc();
    if (!packet) {
        std::cerr << "Could not allocate AVPacket" << std::endl;
        packet_queue.set_finished();
        return;
    }
    uint32_t request_count = 0;
    while (true) {
        int ret = av_read_frame(input_fmt_ctx, packet);
        if (ret < 0) {
            if (ret == AVERROR_EOF) break;
            std::cerr << "Error reading frame: " << get_error_text(ret) << std::endl;
            break;
        }
        // uint64_t timestamp = extract_timestamp(packet);
        RequestHeader header;
        extract_request_header(packet, header);
        reportRequest(header.client_key, request_count++, header.send_timestamp);
        if (packet->stream_index == video_stream_idx) {
            // std::cout << "Frame " << request_count - 1 << " at " << get_current_time_us() - header.send_timestamp << " us" << std::endl;
            packet_queue.push(packet);
            packet = av_packet_alloc();
            if (!packet) {
                std::cerr << "Could not allocate AVPacket" << std::endl;
                break;
            }
        } else {
            av_packet_unref(packet);
        }
    }

    if (packet) {
        av_packet_free(&packet);
    }
    packet_queue.set_finished();
}

void decoding_thread(AVCodecContext* decoder_ctx, PacketQueue& packet_queue, const std::vector<FrameQueue*>& encoder_queues) {
    AVPacket* packet = nullptr;
    AVFrame* frame = av_frame_alloc();
    if (!frame) {
        std::cerr << "Could not allocate AVFrame" << std::endl;
        for (auto& q : encoder_queues) q->set_finished();
        return;
    }

    while (packet_queue.pop(packet)) {
        auto decode_start = std::chrono::steady_clock::now();
        int ret = avcodec_send_packet(decoder_ctx, packet);
        av_packet_free(&packet);
        if (ret < 0) {
            std::cerr << "Error sending packet to decoder: " << get_error_text(ret) << std::endl;
            continue;
        }

        while (ret >= 0) {
            ret = avcodec_receive_frame(decoder_ctx, frame);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                break;
            }
            else if (ret < 0) {
                std::cerr << "Error receiving frame from decoder: " << get_error_text(ret) << std::endl;
                break;
            }
            // std::cout << frame->pts << ": " << get_timestamp_with_ms() << std::endl;

            auto decode_end = std::chrono::steady_clock::now();

            for (auto& q : encoder_queues) {
                AVFrame* cloned_frame = av_frame_clone(frame);
                if (!cloned_frame) {
                    std::cerr << "Could not clone frame" << std::endl;
                    continue;
                }
                FrameData frame_data;
                frame_data.frame = cloned_frame;
                frame_data.decode_start_time = decode_start;
                frame_data.decode_end_time = decode_end;
                q->push(frame_data);
            }
            av_frame_unref(frame);
        }
    }

    // Flush decoder
    avcodec_send_packet(decoder_ctx, nullptr);
    while (true) {
        int ret = avcodec_receive_frame(decoder_ctx, frame);
        if (ret == AVERROR_EOF || ret == AVERROR(EAGAIN)) break;
        if (ret < 0) {
            std::cerr << "Error flushing decoder: " << get_error_text(ret) << std::endl;
            break;
        }

        for (auto& q : encoder_queues) {
            AVFrame* cloned_frame = av_frame_clone(frame);
            if (!cloned_frame) {
                std::cerr << "Could not clone frame" << std::endl;
                continue;
            }
            FrameData frame_data;
            frame_data.frame = cloned_frame;
            frame_data.decode_start_time = std::chrono::steady_clock::now();;
            frame_data.decode_end_time = std::chrono::steady_clock::now();;
            q->push(frame_data);
        }
        av_frame_unref(frame);
    }

    for (auto& q : encoder_queues) q->set_finished();
    av_frame_free(&frame);
}

bool decode_frames(DecoderInfo decoder_info, std::vector<FrameQueue*>& encoder_queues, std::atomic<bool>& decode_finished) {
    PacketQueue packet_queue;
    std::atomic<bool> encoding_finished(false);
    std::vector<std::thread> encoder_threads;

    std::thread reader(packet_reading_thread, decoder_info.input_fmt_ctx, decoder_info.video_stream_idx, std::ref(packet_queue));
    std::thread decoder(decoding_thread, decoder_info.decoder_ctx, std::ref(packet_queue), std::ref(encoder_queues));

    reader.join();
    decoder.join();

    decode_finished = true;
    std::cout << "Decoding finished." << std::endl;
    return true;
}

// int embed_response_header(AVPacket *packet, const ResponseInfo& response_info) {
//     uint8_t sei_content[48];
//     int sei_content_size = 0;
    
//     sei_content[sei_content_size++] = 0x06;
//     sei_content[sei_content_size++] = 0x05;
//     int payload_size = 16 + sizeof(ResponseInfo);
//     sei_content[sei_content_size++] = payload_size;
    
//     const uint8_t uuid[16] = {
//         0x54, 0x69, 0x6D, 0x65, // "Time"
//         0x53, 0x74, 0x61, 0x6D, // "Stam"
//         0x70, 0x00, 0x01, 0x02, 
//         0x03, 0x04, 0x05, 0x06 
//     };
//     memcpy(sei_content + sei_content_size, uuid, 16);
//     sei_content_size += 16;
    
//     memcpy(sei_content + sei_content_size, &response_info, sizeof(ResponseInfo));
//     sei_content_size += sizeof(ResponseInfo);
//     sei_content[sei_content_size++] = 0x80;
    
//     int new_size = packet->size + sei_content_size + 4;
//     uint8_t *new_data = (uint8_t*)av_malloc(new_size);
//     if (!new_data) return AVERROR(ENOMEM);
    
//     new_data[0] = (sei_content_size >> 24) & 0xFF;
//     new_data[1] = (sei_content_size >> 16) & 0xFF;
//     new_data[2] = (sei_content_size >> 8) & 0xFF;
//     new_data[3] = sei_content_size & 0xFF;
    
//     memcpy(new_data + 4, sei_content, sei_content_size);

//     memcpy(new_data + 4 + sei_content_size, packet->data, packet->size);
//     av_buffer_unref(&packet->buf);
//     packet->buf = av_buffer_create(new_data, new_size, 
//                                   av_buffer_default_free, NULL, 0);
//     packet->data = new_data;
//     packet->size = new_size;
//     return 0;
// }

// Encoder Function with Initialization and Scaling
// bool encode_frames(const EncoderConfig& config, FrameQueue& frame_queue, AVRational input_time_base, std::atomic<bool>& encode_finished) {
//     AVFormatContext* output_fmt_ctx = nullptr;
//     AVStream* out_stream = nullptr;

//     // Set TCP options with increased latency and specified packet size
//     AVDictionary* format_opts = nullptr;

//     // Allocate output format context with FLV over TCP
//     if (avformat_alloc_output_context2(&output_fmt_ctx, nullptr, "flv", config.output_url.c_str()) < 0) {
//         std::cerr << "Could not create output context for " << config.output_url << std::endl;
//         av_dict_free(&format_opts);
//         return false;
//     }

//     // Find encoder for H.264
//     const AVCodec* encoder = avcodec_find_encoder(AV_CODEC_ID_H264);
//     if (!encoder) {
//         std::cerr << "Necessary encoder not found" << std::endl;
//         avformat_free_context(output_fmt_ctx);
//         av_dict_free(&format_opts);
//         return false;
//     }

//     // Create new stream for encoder
//     out_stream = avformat_new_stream(output_fmt_ctx, nullptr);
//     if (!out_stream) {
//         std::cerr << "Failed allocating output stream for " << config.output_url << std::endl;
//         avformat_free_context(output_fmt_ctx);
//         av_dict_free(&format_opts);
//         return false;
//     }

//     // Allocate and configure encoder context
//     AVCodecContext* encoder_ctx = avcodec_alloc_context3(encoder);
//     if (!encoder_ctx) {
//         std::cerr << "Could not allocate encoder context for " << config.output_url << std::endl;
//         avformat_free_context(output_fmt_ctx);
//         av_dict_free(&format_opts);
//         return false;
//     }

//     // Set encoder parameters based on configuration
//     encoder_ctx->height = config.height;
//     encoder_ctx->width = config.width;
//     encoder_ctx->sample_aspect_ratio = AVRational{1, 1}; // Square pixels
//     encoder_ctx->pix_fmt = AV_PIX_FMT_YUV420P;
//     encoder_ctx->time_base = AVRational{1, static_cast<int>(config.framerate)};          // 30 fps
//     encoder_ctx->framerate = AVRational{static_cast<int>(config.framerate), 1};
//     encoder_ctx->bit_rate = static_cast<int>(config.bitrate * 1000 * 30 / config.framerate);       // Convert kbps to bps
//     encoder_ctx->gop_size = static_cast<int>(config.framerate);
//     encoder_ctx->max_b_frames = 0;
//     encoder_ctx->thread_count = 0;

//     // Set preset and tune options for low latency
//     AVDictionary* codec_opts = nullptr;
//     av_dict_set(&codec_opts, "preset", "ultrafast", 0);
//     av_dict_set(&codec_opts, "tune", "zerolatency", 0);
//     av_dict_set(&codec_opts, "delay", "0", 0);

//     // Open encoder with codec options
//     if (avcodec_open2(encoder_ctx, encoder, &codec_opts) < 0) {
//         std::cerr << "Cannot open video encoder for " << config.output_url << std::endl;
//         avcodec_free_context(&encoder_ctx);
//         avformat_free_context(output_fmt_ctx);
//         av_dict_free(&codec_opts);
//         av_dict_free(&format_opts);
//         return false;
//     }

//     // Copy encoder parameters to output stream
//     if (avcodec_parameters_from_context(out_stream->codecpar, encoder_ctx) < 0) {
//         std::cerr << "Failed to copy encoder parameters to output stream for " << config.output_url << std::endl;
//         avcodec_free_context(&encoder_ctx);
//         avformat_free_context(output_fmt_ctx);
//         av_dict_free(&codec_opts);
//         av_dict_free(&format_opts);
//         return false;
//     }

//     out_stream->time_base = encoder_ctx->time_base;

//     // Open output URL with format options
//     if (!(output_fmt_ctx->oformat->flags & AVFMT_NOFILE)) {
//         if (avio_open2(&output_fmt_ctx->pb, config.output_url.c_str(), AVIO_FLAG_WRITE, nullptr, &format_opts) < 0) {
//             std::cerr << "Could not open output URL: " << config.output_url << std::endl;
//             avcodec_free_context(&encoder_ctx);
//             avformat_free_context(output_fmt_ctx);
//             av_dict_free(&codec_opts);
//             av_dict_free(&format_opts);
//             return false;
//         }
//     }

//     // Write header to output
//     if (avformat_write_header(output_fmt_ctx, &codec_opts) < 0) {
//         std::cerr << "Error occurred when writing header to output: " << config.output_url << std::endl;
//         avcodec_free_context(&encoder_ctx);
//         avformat_free_context(output_fmt_ctx);
//         av_dict_free(&codec_opts);
//         av_dict_free(&format_opts);
//         return false;
//     }

//     // Free codec options as they are no longer needed
//     av_dict_free(&codec_opts);
//     av_dict_free(&format_opts);

//     // Initialize scaler (will set up based on first frame)
//     SwsContext* sws_ctx = nullptr;
//     bool sws_initialized = false;

//     // Allocate frame for encoder
//     AVFrame* enc_frame = av_frame_alloc();
//     if (!enc_frame) {
//         std::cerr << "Could not allocate encoding frame for " << config.output_url << std::endl;
//         avformat_free_context(output_fmt_ctx);
//         avcodec_free_context(&encoder_ctx);
//         return false;
//     }
//     enc_frame->format = encoder_ctx->pix_fmt;
//     enc_frame->width  = encoder_ctx->width;
//     enc_frame->height = encoder_ctx->height;

//     if (av_frame_get_buffer(enc_frame, 32) < 0) {
//         std::cerr << "Could not allocate the video frame data for " << config.output_url << std::endl;
//         av_frame_free(&enc_frame);
//         avcodec_free_context(&encoder_ctx);
//         avformat_free_context(output_fmt_ctx);
//         return false;
//     }

//     int64_t first_pts = AV_NOPTS_VALUE;
//     int frame_count = 0;

//     // Initialize TimingLogger
//     TimingLogger logger(config.log_filename);
//     uint32_t response_id = 0;

//     // Read frames from queue, encode, and write to output
//     while (true) {
//         FrameData frame_data;
//         bool has_frame = frame_queue.pop(frame_data);
//         if (!has_frame) {
//             if (frame_queue.is_finished())
//                 break;
//             else
//                 continue;
//         }

//         auto encode_start = std::chrono::steady_clock::now();

//         if (!sws_initialized) {
//             // Initialize sws_ctx based on input frame's format and resolution
//             sws_ctx = sws_getContext(
//                 frame_data.frame->width,
//                 frame_data.frame->height,
//                 static_cast<AVPixelFormat>(frame_data.frame->format),
//                 encoder_ctx->width,
//                 encoder_ctx->height,
//                 encoder_ctx->pix_fmt,
//                 SWS_BILINEAR,
//                 nullptr,
//                 nullptr,
//                 nullptr
//             );

//             if (!sws_ctx) {
//                 std::cerr << "Could not initialize the conversion context for " << config.output_url << std::endl;
//                 av_frame_free(&frame_data.frame);
//                 continue;
//             }

//             sws_initialized = true;
//         }

//         // Convert frame to encoder's format and resolution
//         int converted = sws_scale(
//             sws_ctx,
//             (const uint8_t* const*)frame_data.frame->data,
//             frame_data.frame->linesize,
//             0,
//             frame_data.frame->height,
//             enc_frame->data,
//             enc_frame->linesize
//         );

//         if (converted <= 0) {
//             std::cerr << "Could not convert frame for " << config.output_url << std::endl;
//             av_frame_free(&frame_data.frame);
//             continue;
//         }

//         if (first_pts == AV_NOPTS_VALUE) {
//             first_pts = frame_data.frame->pts;
//             std::cout << "First encoded frame PTS for " << config.output_url << ": " << first_pts << std::endl;
//         }

//         // Set PTS based on frame counter
//         enc_frame->pts = av_rescale_q(frame_data.frame->pts, input_time_base, encoder_ctx->time_base);

//         // Send frame to encoder
//         int ret = avcodec_send_frame(encoder_ctx, enc_frame);
//         if (ret < 0) {
//             std::cerr << "Error sending frame to encoder for " << config.output_url << ": " << get_error_text(ret) << std::endl;
//             av_frame_free(&frame_data.frame);
//             break;
//         }

//         // Receive packets from encoder
//         while (ret >= 0) {
//             AVPacket* enc_pkt = av_packet_alloc();
//             if (!enc_pkt) {
//                 std::cerr << "Could not allocate encoding packet for " << config.output_url << std::endl;
//                 break;
//             }

//             ret = avcodec_receive_packet(encoder_ctx, enc_pkt);
//             if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
//                 av_packet_free(&enc_pkt);
//                 break;
//             } else if (ret < 0) {
//                 std::cerr << "Error during encoding for " << config.output_url << ": " << get_error_text(ret) << std::endl;
//                 av_packet_free(&enc_pkt);
//                 break;
//             }

//             // Rescale packet timestamp
//             av_packet_rescale_ts(enc_pkt, encoder_ctx->time_base, out_stream->time_base);
//             enc_pkt->stream_index = out_stream->index;

//             // Write packet to output
//             ResponseInfo response_info = getResponseHeader(response_id++);
//             embed_response_header(enc_pkt, response_info);
//             int write_ret = av_interleaved_write_frame(output_fmt_ctx, enc_pkt);
//             if (write_ret < 0) {
//                 std::cerr << "Error writing packet to output for " << config.output_url << ": " << get_error_text(write_ret) << std::endl;
//                 av_packet_free(&enc_pkt);
//                 break;
//             }

//             av_packet_free(&enc_pkt);
//         }

//         auto encode_end = std::chrono::steady_clock::now();

//         // Calculate timings
//         double decode_time = std::chrono::duration<double, std::milli>(frame_data.decode_end_time - frame_data.decode_start_time).count();
//         double encode_time = std::chrono::duration<double, std::milli>(encode_end - encode_start).count();
//         double interval_time = std::chrono::duration<double, std::milli>(encode_end - frame_data.decode_start_time).count();

//         frame_count++;
//         logger.add_entry(frame_count, decode_time, encode_time, interval_time);

//         if (frame_count % 100 == 0) {
//             std::cout << "Encoded " << frame_count << " frames for " << config.output_url << ", queue length: " << frame_queue.size() << std::endl;
//         }
//         // std::cout << "Decoded Time: " << decode_time << std::endl;

//         av_frame_free(&frame_data.frame);
//     }
//     std::cout << "Encoded " << frame_count << " frames for " << config.output_url << ", current PTS: " << enc_frame->pts << std::endl;

//     // Flush encoder to ensure all frames are processed
//     avcodec_send_frame(encoder_ctx, nullptr);
//     while (true) {
//         AVPacket* enc_pkt = av_packet_alloc();
//         if (!enc_pkt) {
//             std::cerr << "Could not allocate encoding packet during flush for " << config.output_url << std::endl;
//             break;
//         }

//         int ret = avcodec_receive_packet(encoder_ctx, enc_pkt);
//         if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
//             av_packet_free(&enc_pkt);
//             break;
//         } else if (ret < 0) {
//             std::cerr << "Error during encoding flush for " << config.output_url << ": " << get_error_text(ret) << std::endl;
//             av_packet_free(&enc_pkt);
//             break;
//         }

//         // Rescale packet timestamp
//         av_packet_rescale_ts(enc_pkt, encoder_ctx->time_base, out_stream->time_base);
//         enc_pkt->stream_index = out_stream->index;

//         // Write flushed packet to output
//         int write_ret = av_interleaved_write_frame(output_fmt_ctx, enc_pkt);
//         if (write_ret < 0) {
//             std::cerr << "Error writing flushed packet to output for " << config.output_url << ": " << get_error_text(write_ret) << std::endl;
//             av_packet_free(&enc_pkt);
//             break;
//         }

//         av_packet_free(&enc_pkt);
//     }

//     // Write trailer to finalize the stream
//     av_write_trailer(output_fmt_ctx);

//     // Write timing information to log file
//     logger.write_to_file();

//     // Clean up encoder resources
//     av_frame_free(&enc_frame);
//     sws_freeContext(sws_ctx);
//     avcodec_free_context(&encoder_ctx);
//     if (!(output_fmt_ctx->oformat->flags & AVFMT_NOFILE))
//         avio_closep(&output_fmt_ctx->pb);
//     avformat_free_context(output_fmt_ctx);

//     encode_finished = true;
//     std::cout << "Encoding finished for " << config.output_url << "." << std::endl;
//     return true;
// }

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

    pkt->pts   = base->pts;
    pkt->dts   = base->dts;
    pkt->flags = 0;
    pkt->stream_index = base->stream_index;
    return pkt;
}

/**
 * @brief Add response information to a packet
 * @param pkt The packet to modify
 * @param response_info The response information to add
 */
void add_response_info_to_packet(AVPacket* pkt, const ResponseInfo& response_info) {
    // Create a new packet
    AVPacket* new_pkt = av_packet_alloc();
    if (!new_pkt) {
        std::cerr << "Could not allocate new packet" << std::endl;
        return;
    }
    
    // Ensure the new packet has enough buffer to hold original data plus response info
    int new_size = pkt->size + sizeof(ResponseInfo);
    int ret = av_new_packet(new_pkt, new_size);
    if (ret < 0) {
        std::cerr << "Could not allocate packet data" << std::endl;
        av_packet_free(&new_pkt);
        return;
    }
    
    // Copy original data
    memcpy(new_pkt->data, pkt->data, pkt->size);
    
    // Append response info to the end of data
    memcpy(new_pkt->data + pkt->size, &response_info, sizeof(ResponseInfo));
    
    // Copy other packet properties
    new_pkt->pts = pkt->pts;
    new_pkt->dts = pkt->dts;
    new_pkt->stream_index = pkt->stream_index;
    new_pkt->flags = pkt->flags;
    new_pkt->duration = pkt->duration;
    new_pkt->pos = pkt->pos;
    
    // Release original packet
    av_packet_unref(pkt);
    
    // Move new packet content to original packet
    av_packet_move_ref(pkt, new_pkt);
    
    // Free the new packet structure (data has been moved to original packet)
    av_packet_free(&new_pkt);
}

bool encode_frames(const EncoderConfig& config, FrameQueue& frame_queue, AVRational input_time_base, std::atomic<bool>& encode_finished) {
    AVFormatContext* output_fmt_ctx = nullptr;
    AVStream* out_stream = nullptr;

    // Set UDP options
    AVDictionary* format_opts = nullptr;
    av_dict_set(&format_opts, "buffer_size", "8192000", 0);      // Increase buffer size
    av_dict_set(&format_opts, "pkt_size", "1316", 0);            // UDP packet size
    av_dict_set(&format_opts, "flush_packets", "1", 0);          // Flush packets immediately
    av_dict_set(&format_opts, "muxdelay", "0", 0);               // No muxing delay
    av_dict_set(&format_opts, "fifo_size", "0", 0);              // No FIFO for immediate sending

    // Parse output URL to ensure it's properly formatted for UDP
    // UDP URL format: udp://ip:port
    std::string output_url = config.output_url;
    
    // Create output context for UDP
    if (avformat_alloc_output_context2(&output_fmt_ctx, nullptr, "h264", output_url.c_str()) < 0) {
        // If h264 doesn't work, try with mpegts format which works well over UDP
        if (avformat_alloc_output_context2(&output_fmt_ctx, nullptr, "mpegts", output_url.c_str()) < 0) {
            std::cerr << "Could not create output context for " << output_url << std::endl;
            av_dict_free(&format_opts);
            return false;
        }
    }

    // Find encoder for H.264
    const AVCodec* encoder = avcodec_find_encoder(AV_CODEC_ID_H264);
    if (!encoder) {
        std::cerr << "Necessary encoder not found" << std::endl;
        avformat_free_context(output_fmt_ctx);
        av_dict_free(&format_opts);
        return false;
    }

    // Create new stream for encoder
    out_stream = avformat_new_stream(output_fmt_ctx, nullptr);
    if (!out_stream) {
        std::cerr << "Failed allocating output stream for " << output_url << std::endl;
        avformat_free_context(output_fmt_ctx);
        av_dict_free(&format_opts);
        return false;
    }

    // Allocate and configure encoder context
    AVCodecContext* encoder_ctx = avcodec_alloc_context3(encoder);
    if (!encoder_ctx) {
        std::cerr << "Could not allocate encoder context for " << output_url << std::endl;
        avformat_free_context(output_fmt_ctx);
        av_dict_free(&format_opts);
        return false;
    }

    // Set encoder parameters based on configuration
    encoder_ctx->height = config.height;
    encoder_ctx->width = config.width;
    encoder_ctx->sample_aspect_ratio = AVRational{1, 1}; // Square pixels
    encoder_ctx->pix_fmt = AV_PIX_FMT_YUV420P;
    encoder_ctx->time_base = AVRational{1, static_cast<int>(config.framerate)};          // 30 fps
    encoder_ctx->framerate = AVRational{static_cast<int>(config.framerate), 1};
    encoder_ctx->bit_rate = static_cast<int>(config.bitrate * 1000 * 30 / config.framerate);       // Convert kbps to bps
    encoder_ctx->gop_size = static_cast<int>(config.framerate);
    encoder_ctx->max_b_frames = 0;
    encoder_ctx->thread_count = 0;

    // Set preset and tune options for low latency
    AVDictionary* codec_opts = nullptr;
    av_dict_set(&codec_opts, "preset", "ultrafast", 0);
    av_dict_set(&codec_opts, "tune", "zerolatency", 0);
    av_dict_set(&codec_opts, "fflags", "nobuffer+flush_packets", 0);
    av_dict_set(&codec_opts, "delay", "0", 0);
    
    // UDP specific options
    if (strstr(output_url.c_str(), "udp:") != nullptr) {
        av_dict_set(&format_opts, "local_port", "0", 0);         // Let the OS choose a local port
        av_dict_set(&format_opts, "ttl", "64", 0);              // Time-to-live for UDP packets
    }

    // Open encoder with codec options
    if (avcodec_open2(encoder_ctx, encoder, &codec_opts) < 0) {
        std::cerr << "Cannot open video encoder for " << output_url << std::endl;
        avcodec_free_context(&encoder_ctx);
        avformat_free_context(output_fmt_ctx);
        av_dict_free(&codec_opts);
        av_dict_free(&format_opts);
        return false;
    }

    // Copy encoder parameters to output stream
    if (avcodec_parameters_from_context(out_stream->codecpar, encoder_ctx) < 0) {
        std::cerr << "Failed to copy encoder parameters to output stream for " << output_url << std::endl;
        avcodec_free_context(&encoder_ctx);
        avformat_free_context(output_fmt_ctx);
        av_dict_free(&codec_opts);
        av_dict_free(&format_opts);
        return false;
    }

    out_stream->time_base = encoder_ctx->time_base;

    // Open output URL with format options for UDP
    if (!(output_fmt_ctx->oformat->flags & AVFMT_NOFILE)) {
        if (avio_open2(&output_fmt_ctx->pb, output_url.c_str(), AVIO_FLAG_WRITE, nullptr, &format_opts) < 0) {
            std::cerr << "Could not open output URL: " << output_url << std::endl;
            avcodec_free_context(&encoder_ctx);
            avformat_free_context(output_fmt_ctx);
            av_dict_free(&codec_opts);
            av_dict_free(&format_opts);
            return false;
        }
    }

    // Write header to output
    if (avformat_write_header(output_fmt_ctx, &format_opts) < 0) {
        std::cerr << "Error occurred when writing header to output: " << output_url << std::endl;
        avcodec_free_context(&encoder_ctx);
        avformat_free_context(output_fmt_ctx);
        av_dict_free(&codec_opts);
        av_dict_free(&format_opts);
        return false;
    }

    // Free codec options as they are no longer needed
    av_dict_free(&codec_opts);
    av_dict_free(&format_opts);

    // Initialize scaler (will set up based on first frame)
    SwsContext* sws_ctx = nullptr;
    bool sws_initialized = false;

    // Allocate frame for encoder
    AVFrame* enc_frame = av_frame_alloc();
    if (!enc_frame) {
        std::cerr << "Could not allocate encoding frame for " << output_url << std::endl;
        avformat_free_context(output_fmt_ctx);
        avcodec_free_context(&encoder_ctx);
        return false;
    }
    enc_frame->format = encoder_ctx->pix_fmt;
    enc_frame->width  = encoder_ctx->width;
    enc_frame->height = encoder_ctx->height;

    if (av_frame_get_buffer(enc_frame, 32) < 0) {
        std::cerr << "Could not allocate the video frame data for " << output_url << std::endl;
        av_frame_free(&enc_frame);
        avcodec_free_context(&encoder_ctx);
        avformat_free_context(output_fmt_ctx);
        return false;
    }

    int64_t first_pts = AV_NOPTS_VALUE;
    int frame_count = 0;

    // Initialize TimingLogger
    TimingLogger logger(config.log_filename);
    uint32_t response_id = 0;
    // Read frames from queue, encode, and write to output
    while (true) {
        FrameData frame_data;
        bool has_frame = frame_queue.pop(frame_data);
        if (!has_frame) {
            if (frame_queue.is_finished())
                break;
            else
                continue;
        }

        auto encode_start = std::chrono::steady_clock::now();

        if (!sws_initialized) {
            // Initialize sws_ctx based on input frame's format and resolution
            sws_ctx = sws_getContext(
                frame_data.frame->width,
                frame_data.frame->height,
                static_cast<AVPixelFormat>(frame_data.frame->format),
                encoder_ctx->width,
                encoder_ctx->height,
                encoder_ctx->pix_fmt,
                SWS_BILINEAR,
                nullptr,
                nullptr,
                nullptr
            );

            if (!sws_ctx) {
                std::cerr << "Could not initialize the conversion context for " << output_url << std::endl;
                av_frame_free(&frame_data.frame);
                continue;
            }

            sws_initialized = true;
        }

        // Convert frame to encoder's format and resolution
        int converted = sws_scale(
            sws_ctx,
            (const uint8_t* const*)frame_data.frame->data,
            frame_data.frame->linesize,
            0,
            frame_data.frame->height,
            enc_frame->data,
            enc_frame->linesize
        );

        if (converted <= 0) {
            std::cerr << "Could not convert frame for " << output_url << std::endl;
            av_frame_free(&frame_data.frame);
            continue;
        }

        if (first_pts == AV_NOPTS_VALUE) {
            first_pts = frame_data.frame->pts;
        }

        // Generate monotonically increasing PTS based on frame counter
        // This ensures we don't have timestamp ordering issues
        enc_frame->pts = frame_count;

        // Send frame to encoder
        int ret = avcodec_send_frame(encoder_ctx, enc_frame);
        if (ret < 0) {
            std::cerr << "Error sending frame to encoder for " << output_url << ": " << get_error_text(ret) << std::endl;
            av_frame_free(&frame_data.frame);
            break;
        }

        // Receive packets from encoder
        while (ret >= 0) {
            AVPacket* enc_pkt = av_packet_alloc();
            if (!enc_pkt) {
                std::cerr << "Could not allocate encoding packet for " << output_url << std::endl;
                break;
            }

            ret = avcodec_receive_packet(encoder_ctx, enc_pkt);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                av_packet_free(&enc_pkt);
                break;
            } else if (ret < 0) {
                std::cerr << "Error during encoding for " << output_url << ": " << get_error_text(ret) << std::endl;
                av_packet_free(&enc_pkt);
                break;
            }
            
            // Rescale packet timestamp to output stream's time base
            av_packet_rescale_ts(enc_pkt, encoder_ctx->time_base, out_stream->time_base);
            enc_pkt->stream_index = out_stream->index;
            // Write packet to output
            ResponseInfo response_info = getResponseHeader(response_id++);
            add_response_info_to_packet(enc_pkt, response_info);

            // Write packet to output
            int write_ret = av_interleaved_write_frame(output_fmt_ctx, enc_pkt);
            if (write_ret < 0) {
                std::cerr << "Error writing packet to output for " << output_url << ": " << get_error_text(write_ret) << std::endl;
                av_packet_free(&enc_pkt);
                break;
            }

            AVPacket* empty_pkt = create_timestamp_packet(enc_pkt, get_current_time_us());
            if (empty_pkt) {
                int empty_write_ret = av_write_frame(output_fmt_ctx, empty_pkt);
                if (empty_write_ret < 0) {
                    std::cerr << "Error writing empty packet to output: " << get_error_text(empty_write_ret) << std::endl;
                }
                av_packet_free(&empty_pkt);
            }
            // std::cout << "Encoded frame " << frame_count + 1 << " at " << get_current_time_us() << std::endl;

            av_packet_free(&enc_pkt);
        }

        auto encode_end = std::chrono::steady_clock::now();

        // Calculate timings
        double decode_time = std::chrono::duration<double, std::milli>(frame_data.decode_end_time - frame_data.decode_start_time).count();
        double encode_time = std::chrono::duration<double, std::milli>(encode_end - encode_start).count();
        double interval_time = std::chrono::duration<double, std::milli>(encode_end - frame_data.decode_start_time).count();

        frame_count += 2;
        logger.add_entry(frame_count / 2, decode_time, encode_time, interval_time);

        if (frame_count % 100 == 0) {
            std::cout << "Encoded " << frame_count << " frames for " << output_url << ", queue length: " << frame_queue.size() << std::endl;
        }

        av_frame_free(&frame_data.frame);
    }
    std::cout << "Encoded " << frame_count << " frames for " << output_url << ", current PTS: " << enc_frame->pts << std::endl;

    // Flush encoder to ensure all frames are processed
    avcodec_send_frame(encoder_ctx, nullptr);
    while (true) {
        AVPacket* enc_pkt = av_packet_alloc();
        if (!enc_pkt) {
            std::cerr << "Could not allocate encoding packet during flush for " << output_url << std::endl;
            break;
        }

        int ret = avcodec_receive_packet(encoder_ctx, enc_pkt);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            av_packet_free(&enc_pkt);
            break;
        } else if (ret < 0) {
            std::cerr << "Error during encoding flush for " << output_url << ": " << get_error_text(ret) << std::endl;
            av_packet_free(&enc_pkt);
            break;
        }

        // Set packet timestamps to ensure monotonically increasing values
        enc_pkt->pts = frame_count;
        enc_pkt->dts = frame_count;
        
        // Rescale packet timestamp to output stream's time base
        av_packet_rescale_ts(enc_pkt, encoder_ctx->time_base, out_stream->time_base);
        enc_pkt->stream_index = out_stream->index;

        // Write flushed packet to output
        int write_ret = av_interleaved_write_frame(output_fmt_ctx, enc_pkt);
        if (write_ret < 0) {
            std::cerr << "Error writing flushed packet to output for " << output_url << ": " << get_error_text(write_ret) << std::endl;
            av_packet_free(&enc_pkt);
            break;
        }

        av_packet_free(&enc_pkt);
    }

    // Write trailer to finalize the stream
    av_write_trailer(output_fmt_ctx);

    // Write timing information to log file
    logger.write_to_file();

    // Clean up encoder resources
    av_frame_free(&enc_frame);
    sws_freeContext(sws_ctx);
    avcodec_free_context(&encoder_ctx);
    if (!(output_fmt_ctx->oformat->flags & AVFMT_NOFILE))
        avio_closep(&output_fmt_ctx->pb);
    avformat_free_context(output_fmt_ctx);

    encode_finished = true;
    std::cout << "Encoding finished for " << output_url << "." << std::endl;
    return true;
}

int main(int argc, char* argv[]) {
    // Expecting at least 2 arguments: program, input_url, and at least 1 output_url
    // Maximum of 6 output URLs supported
    if (argc < 3) {
        std::cerr << "Usage: " << argv[0] 
                  << " tcp://192.168.2.3:9000?listen=1 udp://192.168.2.2:10000 [<output2_tcp_url> ... <output6_tcp_url>]" 
                  << std::endl;
        std::cerr << "Supported Resolutions (in order):" << std::endl;
        std::cerr << "1. 3840x2160" << std::endl;
        std::cerr << "2. 2560x1440" << std::endl;
        std::cerr << "3. 1920x1080" << std::endl;
        std::cerr << "4. 1280x720" << std::endl;
        std::cerr << "5. 854x480" << std::endl;
        std::cerr << "6. 640x360" << std::endl;
        return 1;
    }

    const char* input_url = argv[1];
    int num_outputs = argc - 2;

    // Define possible resolutions in order
    struct ResolutionBitrateLog {
        int width;
        int height;
        int bitrate_kbps;
        std::string log_filename;
    };

    std::vector<ResolutionBitrateLog> resolution_bitrate_log = {
        {3840, 2160, 16000,  "frame-1"},
        {2560, 1440, 10000,  "frame-2"},
        {1920, 1080, 5000,  "frame-3"},
        {1280, 720,  2500,  "frame-4"},
        {854,  480,  1000,  "frame-5"},
        {640,  360,  600,   "frame-6"}
    };
    // std::vector<ResolutionBitrateLog> resolution_bitrate_log = {
    //     {2560, 1440, 4000,  "frame-1"},
    //     {2560, 1440, 4000,  "frame-2"},
    //     {2560, 1440, 4000,  "frame-3"},
    //     {2560, 1440, 4000,  "frame-4"},
    //     {2560, 1440, 4000,  "frame-5"},
    //     {2560, 1440, 4000,  "frame-6"}
    // };

    if (num_outputs > (int) resolution_bitrate_log.size()) {
        std::cerr << "Error: Maximum supported output URLs is " << resolution_bitrate_log.size() << "." << std::endl;
        return 1;
    }

    // Define encoder configurations based on the number of output URLs

    // Initialize FFmpeg
    avformat_network_init();

    std::atomic<bool> decode_finished(false);

    // Initialize decoder
    DecoderInfo decoder_info;
    if (!initialize_decoder(input_url, decoder_info)) {
        std::cerr << "Decoder initialization failed" << std::endl;
        avformat_network_deinit();
        return 1;
    }

    std::vector<EncoderConfig> encoder_configs;
    for (int i = 0; i < num_outputs; ++i) {
        EncoderConfig config;
        config.output_url = argv[2 + i];
        config.width = resolution_bitrate_log[i].width;
        config.height = resolution_bitrate_log[i].height;
        config.bitrate = resolution_bitrate_log[i].bitrate_kbps;
        config.log_filename = "result/task" + std::to_string(num_outputs) + "/" + get_timestamp_with_ms() + "/"
                              + resolution_bitrate_log[i].log_filename + ".log";
        config.framerate = decoder_info.input_framerate;
        encoder_configs.push_back(config);
    }

    // Prepare frame queues for each encoder
    std::vector<FrameQueue*> frame_queues;
    for (size_t i = 0; i < encoder_configs.size(); ++i) {
        frame_queues.push_back(new FrameQueue());
    }

    // Start decoder thread
    std::thread decoder_thread([&]() {
        if (!decode_frames(decoder_info, frame_queues, decode_finished)) {
            std::cerr << "Decoding failed" << std::endl;
        }
    });

    // Start encoder threads
    std::vector<std::thread> encoder_threads;
    std::vector<std::atomic<bool>> enc_finished_flags(encoder_configs.size());
    for (size_t i = 0; i < encoder_configs.size(); ++i) {
        enc_finished_flags[i] = false;
        encoder_threads.emplace_back([&, i]() {
            if (!encode_frames(encoder_configs[i], *frame_queues[i], decoder_info.input_time_base, enc_finished_flags[i])) {
                std::cerr << "Encoding failed for " << encoder_configs[i].output_url << std::endl;
            }
        });
    }

    // Wait for threads to finish
    decoder_thread.join();
    for (auto& t : encoder_threads) {
        t.join();
    }

    // Clean up frame queues
    for (auto& q : frame_queues) {
        delete q;
    }

    // Clean up FFmpeg
    avformat_network_deinit();

    // Check if all encoders finished successfully
    bool all_success = true;
    for (size_t i = 0; i < enc_finished_flags.size(); ++i) {
        if (!enc_finished_flags[i]) {
            all_success = false;
            std::cerr << "Encoder for " << encoder_configs[i].output_url << " did not finish successfully." << std::endl;
        }
    }

    if (all_success) {
        std::cout << "Transcoding completed successfully for all resolutions." << std::endl;
    } else {
        std::cerr << "Transcoding encountered errors." << std::endl;
    }

    return 0;
}