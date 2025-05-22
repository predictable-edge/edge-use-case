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
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

// FFmpeg includes
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

// Global variables for decoder information with synchronization
struct GlobalDecoderInfo {
    AVRational input_time_base;
    double input_framerate;
    bool initialized;
    std::mutex mutex;
    std::condition_variable cv;
};

GlobalDecoderInfo g_decoder_info = {
    {0, 0},   // input_time_base
    0.0,      // input_framerate
    false,    // initialized
};

// Global variables to store client IP and port from handshake
std::string g_client_ip;
int g_client_port = 0;

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
    auto now = std::chrono::system_clock::now();
    auto duration = now.time_since_epoch();
    return std::chrono::duration_cast<std::chrono::microseconds>(duration).count();
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

// Helper function to wait for decoder initialization
void wait_for_decoder_init() {
    std::unique_lock<std::mutex> lock(g_decoder_info.mutex);
    g_decoder_info.cv.wait(lock, []{ return g_decoder_info.initialized; });
}

// Decoder Initialization and Function
bool initialize_decoder(const char* input_url, DecoderInfo& decoder_info) {
    // Initialize input format context
    decoder_info.input_fmt_ctx = nullptr;
    AVDictionary* format_opts = nullptr;
    // Set RTSP specific options
    av_dict_set(&format_opts, "rtsp_transport", "udp", 0);       // Use UDP for RTP transport
    av_dict_set(&format_opts, "rtsp_flags", "prefer_tcp", 0);    // Prefer TCP for RTSP control connection
    av_dict_set(&format_opts, "buffer_size", "8192000", 0);      // Increase buffer size
    av_dict_set(&format_opts, "max_delay", "500000", 0);         // 500ms max delay
    av_dict_set(&format_opts, "reorder_queue_size", "10", 0);    // Reorder queue size
    av_dict_set(&format_opts, "stimeout", "5000000", 0);         // Socket timeout 5 seconds
    av_dict_set(&format_opts, "listen_timeout", "5000000", 0);   // Connection timeout 5 seconds
    av_dict_set(&format_opts, "probesize", "32648", 0);          // Probe size 32648
    av_dict_set(&format_opts, "analyzeduration", "0", 0);        // Analyze duration 0 second
    // Set input format to RTSP
    AVInputFormat* input_format = av_find_input_format("rtsp");
    if (avformat_open_input(&decoder_info.input_fmt_ctx, input_url, input_format, &format_opts) < 0) {
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
    AVRational frame_rate_rational = av_guess_frame_rate(decoder_info.input_fmt_ctx, decoder_info.input_fmt_ctx->streams[decoder_info.video_stream_idx], nullptr);
    if (frame_rate_rational.num == 0 || frame_rate_rational.den == 0) {
        decoder_info.input_framerate = 30.0;
        std::cerr << "Warning: Could not determine input frame rate. Using default 30 FPS." << std::endl;
    } else {
        decoder_info.input_framerate = av_q2d(frame_rate_rational);
    }
    {
        std::lock_guard<std::mutex> lock(g_decoder_info.mutex);
        g_decoder_info.input_time_base = decoder_info.input_time_base;
        g_decoder_info.input_framerate = decoder_info.input_framerate;
        g_decoder_info.initialized = true;
    }
    g_decoder_info.cv.notify_all();
    std::cout << "Input frame rate: " << decoder_info.input_framerate << " FPS" << std::endl;
    std::cout << "Decoder initialized successfully." << std::endl;
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
    while (true) {
        int ret = av_read_frame(input_fmt_ctx, packet);
        if (ret < 0) {
            if (ret == AVERROR_EOF) break;
            std::cerr << "Error reading frame: " << get_error_text(ret) << std::endl;
            break;
        }

        if (packet->stream_index == video_stream_idx) {
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

bool decode_frames(const char* input_url, std::vector<FrameQueue*>& encoder_queues, std::atomic<bool>& decode_finished) {
    DecoderInfo decoder_info;
    if (!initialize_decoder(input_url, decoder_info)) {
        std::cerr << "Decoder initialization failed" << std::endl;
        decode_finished = true;
        for (auto& q : encoder_queues) q->set_finished();
        return false;
    }
    PacketQueue packet_queue;
    std::thread reader(packet_reading_thread, decoder_info.input_fmt_ctx, decoder_info.video_stream_idx, std::ref(packet_queue));
    std::thread decoder(decoding_thread, decoder_info.decoder_ctx, std::ref(packet_queue), std::ref(encoder_queues));
    reader.join();
    decoder.join();
    decode_finished = true;
    std::cout << "Decoding finished." << std::endl;
    return true;
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

// Encoder Function with Initialization and Scaling
bool encode_frames(const EncoderConfig& config, FrameQueue& frame_queue, std::atomic<bool>& encode_finished) {
    AVFormatContext* output_fmt_ctx = nullptr;
    AVStream* out_stream = nullptr;
    // Set UDP options
    AVDictionary* format_opts = nullptr;
    av_dict_set(&format_opts, "buffer_size", "8192000", 0);
    av_dict_set(&format_opts, "pkt_size", "1316", 0);
    av_dict_set(&format_opts, "flush_packets", "1", 0);
    av_dict_set(&format_opts, "muxdelay", "0", 0);
    av_dict_set(&format_opts, "fifo_size", "0", 0);
    av_dict_set(&format_opts, "local_port", "10001", 0);
    std::string output_url = config.output_url;
    std::cout << "output_url: " << output_url << std::endl;
    if (avformat_alloc_output_context2(&output_fmt_ctx, nullptr, "h264", output_url.c_str()) < 0) {
        if (avformat_alloc_output_context2(&output_fmt_ctx, nullptr, "mpegts", output_url.c_str()) < 0) {
            std::cerr << "Could not create output context for " << output_url << std::endl;
            av_dict_free(&format_opts);
            return false;
        }
    }
    const AVCodec* encoder = avcodec_find_encoder(AV_CODEC_ID_H264);
    if (!encoder) {
        std::cerr << "Necessary encoder not found" << std::endl;
        avformat_free_context(output_fmt_ctx);
        av_dict_free(&format_opts);
        return false;
    }
    out_stream = avformat_new_stream(output_fmt_ctx, nullptr);
    if (!out_stream) {
        std::cerr << "Failed allocating output stream for " << output_url << std::endl;
        avformat_free_context(output_fmt_ctx);
        av_dict_free(&format_opts);
        return false;
    }
    AVCodecContext* encoder_ctx = avcodec_alloc_context3(encoder);
    if (!encoder_ctx) {
        std::cerr << "Could not allocate encoder context for " << output_url << std::endl;
        avformat_free_context(output_fmt_ctx);
        av_dict_free(&format_opts);
        return false;
    }
    wait_for_decoder_init();
    AVRational input_time_base;
    double input_framerate;
    {
        std::lock_guard<std::mutex> lock(g_decoder_info.mutex);
        input_time_base = g_decoder_info.input_time_base;
        input_framerate = g_decoder_info.input_framerate;
    }
    encoder_ctx->height = config.height;
    encoder_ctx->width = config.width;
    encoder_ctx->sample_aspect_ratio = AVRational{1, 1};
    encoder_ctx->pix_fmt = AV_PIX_FMT_YUV420P;
    encoder_ctx->time_base = AVRational{1, static_cast<int>(input_framerate)};
    encoder_ctx->framerate = AVRational{static_cast<int>(input_framerate), 1};
    encoder_ctx->bit_rate = static_cast<int>(config.bitrate * 1000 * 30 / input_framerate);
    encoder_ctx->gop_size = static_cast<int>(input_framerate);
    encoder_ctx->max_b_frames = 0;
    encoder_ctx->thread_count = 0;
    AVDictionary* codec_opts = nullptr;
    av_dict_set(&codec_opts, "preset", "ultrafast", 0);
    av_dict_set(&codec_opts, "tune", "zerolatency", 0);
    av_dict_set(&codec_opts, "fflags", "nobuffer+flush_packets", 0);
    av_dict_set(&codec_opts, "delay", "0", 0);
    if (strstr(output_url.c_str(), "udp:") != nullptr) {
        av_dict_set(&format_opts, "local_port", "10001", 0);
        av_dict_set(&format_opts, "ttl", "64", 0);
    }
    if (avcodec_open2(encoder_ctx, encoder, &codec_opts) < 0) {
        std::cerr << "Cannot open video encoder for " << output_url << std::endl;
        avcodec_free_context(&encoder_ctx);
        avformat_free_context(output_fmt_ctx);
        av_dict_free(&codec_opts);
        av_dict_free(&format_opts);
        return false;
    }
    if (avcodec_parameters_from_context(out_stream->codecpar, encoder_ctx) < 0) {
        std::cerr << "Failed to copy encoder parameters to output stream for " << output_url << std::endl;
        avcodec_free_context(&encoder_ctx);
        avformat_free_context(output_fmt_ctx);
        av_dict_free(&codec_opts);
        av_dict_free(&format_opts);
        return false;
    }
    out_stream->time_base = encoder_ctx->time_base;
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
    if (avformat_write_header(output_fmt_ctx, &format_opts) < 0) {
        std::cerr << "Error occurred when writing header to output: " << output_url << std::endl;
        avcodec_free_context(&encoder_ctx);
        avformat_free_context(output_fmt_ctx);
        av_dict_free(&codec_opts);
        av_dict_free(&format_opts);
        return false;
    }
    av_dict_free(&codec_opts);
    av_dict_free(&format_opts);
    SwsContext* sws_ctx = nullptr;
    bool sws_initialized = false;
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
    TimingLogger logger(config.log_filename);
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
            std::cout << "First encoded frame PTS for " << output_url << ": " << first_pts << std::endl;
        }
        enc_frame->pts = frame_count;
        int ret = avcodec_send_frame(encoder_ctx, enc_frame);
        if (ret < 0) {
            std::cerr << "Error sending frame to encoder for " << output_url << ": " << get_error_text(ret) << std::endl;
            av_frame_free(&frame_data.frame);
            break;
        }
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
            av_packet_rescale_ts(enc_pkt, encoder_ctx->time_base, out_stream->time_base);
            enc_pkt->stream_index = out_stream->index;
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
                    std::cerr << "Error writing empty packet to output" << std::endl;
                }
                av_packet_free(&empty_pkt);
            }
            std::cout << "Encoded frame " << frame_count + 1 << " at " << get_current_time_us() << std::endl;
            av_packet_free(&enc_pkt);
        }
        auto encode_end = std::chrono::steady_clock::now();
        double decode_time = std::chrono::duration<double, std::milli>(frame_data.decode_end_time - frame_data.decode_start_time).count();
        double encode_time = std::chrono::duration<double, std::milli>(encode_end - encode_start).count();
        double interval_time = std::chrono::duration<double, std::milli>(encode_end - frame_data.decode_start_time).count();
        frame_count++;
        logger.add_entry(frame_count, decode_time, encode_time, interval_time);
        if (frame_count % 100 == 0) {
            std::cout << "Encoded " << frame_count << " frames for " << output_url << ", queue length: " << frame_queue.size() << std::endl;
        }
        av_frame_free(&frame_data.frame);
    }
    std::cout << "Encoded " << frame_count << " frames for " << output_url << ", current PTS: " << enc_frame->pts << std::endl;
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
        enc_pkt->pts = frame_count;
        enc_pkt->dts = frame_count;
        av_packet_rescale_ts(enc_pkt, encoder_ctx->time_base, out_stream->time_base);
        enc_pkt->stream_index = out_stream->index;
        int write_ret = av_interleaved_write_frame(output_fmt_ctx, enc_pkt);
        if (write_ret < 0) {
            std::cerr << "Error writing flushed packet to output for " << output_url << ": " << get_error_text(write_ret) << std::endl;
            av_packet_free(&enc_pkt);
            break;
        }
        av_packet_free(&enc_pkt);
    }
    av_write_trailer(output_fmt_ctx);
    logger.write_to_file();
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

void wait_for_ping_and_reply_pong() {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        perror("socket");
        exit(1);
    }
    sockaddr_in addr{}, peer_addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(10001);
    if (bind(sock, (sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(sock);
        exit(1);
    }
    char buf[128] = {0};
    socklen_t peer_len = sizeof(peer_addr);
    ssize_t n = recvfrom(sock, buf, sizeof(buf), 0, (sockaddr*)&peer_addr, &peer_len);
    if (n > 0 && strncmp(buf, "ping", 4) == 0) {
        char ipbuf[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &peer_addr.sin_addr, ipbuf, sizeof(ipbuf));
        g_client_ip = ipbuf;
        g_client_port = ntohs(peer_addr.sin_port);
        printf("Received ping from client %s:%d\n", g_client_ip.c_str(), g_client_port);
        sendto(sock, "pong", 4, 0, (sockaddr*)&peer_addr, peer_len);
    }
    close(sock);
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " udp://192.168.2.3:9000 [<extra_output_url> ...]" << std::endl;
        std::cerr << "The first output is always the handshake client. Extra outputs (if any) are specified as additional arguments." << std::endl;
        std::cerr << "Supported Resolutions (in order):" << std::endl;
        std::cerr << "1. 3840x2160" << std::endl;
        std::cerr << "2. 2560x1440" << std::endl;
        std::cerr << "3. 1920x1080" << std::endl;
        std::cerr << "4. 1280x720" << std::endl;
        std::cerr << "5. 854x480" << std::endl;
        std::cerr << "6. 640x360" << std::endl;
        return 1;
    }

    // UDP handshake: wait for a ping from the streaming process on port 10001, reply with pong, then proceed.
    wait_for_ping_and_reply_pong();

    const char* input_url = argv[1];
    int num_outputs = argc - 1;
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
    if (num_outputs > (int) resolution_bitrate_log.size()) {
        std::cerr << "Error: Maximum supported output URLs is " << resolution_bitrate_log.size() << "." << std::endl;
        return 1;
    }
    avformat_network_init();
    std::atomic<bool> decode_finished(false);
    {
        std::lock_guard<std::mutex> lock(g_decoder_info.mutex);
        g_decoder_info.initialized = false;
        g_decoder_info.input_framerate = 0.0;
        g_decoder_info.input_time_base = {0, 0};
    }
    std::vector<EncoderConfig> encoder_configs;
    for (int i = 0; i < num_outputs; ++i) {
        EncoderConfig config;
        if (i == 0) {
            // Use the client IP and port from handshake for the first output
            std::ostringstream oss;
            oss << "udp://" << g_client_ip << ":" << g_client_port;
            config.output_url = oss.str();
        } else {
            // Use argv[1 + i] for the rest
            config.output_url = argv[1 + i];
        }
        config.width = resolution_bitrate_log[i].width;
        config.height = resolution_bitrate_log[i].height;
        config.bitrate = resolution_bitrate_log[i].bitrate_kbps;
        config.log_filename = "result/task" + std::to_string(num_outputs) + "/" + get_timestamp_with_ms() + "/"
                              + resolution_bitrate_log[i].log_filename + ".log";
        config.framerate = 30.0;
        encoder_configs.push_back(config);
    }
    std::vector<FrameQueue*> frame_queues;
    for (size_t i = 0; i < encoder_configs.size(); ++i) {
        frame_queues.push_back(new FrameQueue());
    }
    std::thread decoder_thread([&]() {
        if (!decode_frames(input_url, frame_queues, decode_finished)) {
            std::cerr << "Decoding failed" << std::endl;
        }
    });
    std::vector<std::thread> encoder_threads;
    std::vector<std::atomic<bool>> enc_finished_flags(encoder_configs.size());
    for (size_t i = 0; i < encoder_configs.size(); ++i) {
        enc_finished_flags[i] = false;
        encoder_threads.emplace_back([&, i]() {
            if (!encode_frames(encoder_configs[i], *frame_queues[i], enc_finished_flags[i])) {
                std::cerr << "Encoding failed for " << encoder_configs[i].output_url << std::endl;
            }
        });
    }
    decoder_thread.join();
    for (auto& t : encoder_threads) {
        t.join();
    }
    for (auto& q : frame_queues) {
        delete q;
    }
    avformat_network_deinit();
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