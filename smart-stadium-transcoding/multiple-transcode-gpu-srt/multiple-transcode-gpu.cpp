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

// FFmpeg includes
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
#include <libavutil/opt.h> 
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersrc.h>
#include <libavfilter/buffersink.h>
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
    int bit_depth;
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

// Global hardware device context
AVBufferRef* hw_device_ctx = nullptr;

// Function to select the hardware pixel format
static enum AVPixelFormat get_hw_format(AVCodecContext* ctx, const enum AVPixelFormat* pix_fmts) {
    for (const enum AVPixelFormat* p = pix_fmts; *p != AV_PIX_FMT_NONE; p++) {
        if (*p == AV_PIX_FMT_CUDA || *p == AV_PIX_FMT_D3D11VA_VLD || *p == AV_PIX_FMT_D3D11) {
            return *p;
        }
    }
    std::cerr << "Failed to get HW surface format." << std::endl;
    return AV_PIX_FMT_NONE;
}

// Decoder Initialization and Function
bool initialize_decoder(const char* input_url, DecoderInfo& decoder_info) {
    // Initialize input format context
    decoder_info.input_fmt_ctx = nullptr;
    AVDictionary* format_opts = nullptr;
    av_dict_set(&format_opts, "latency", "0", 0);         // Latency in ms 
    av_dict_set(&format_opts, "buffer_size", "1000000", 0);
    if (avformat_open_input(&decoder_info.input_fmt_ctx, input_url, nullptr, &format_opts) < 0) {
        std::cerr << "Could not open input SRT stream: " << input_url << std::endl;
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

    // Get bit depth
    const AVPixFmtDescriptor* pix_desc = av_pix_fmt_desc_get((AVPixelFormat)codecpar->format);
    if (!pix_desc) {
        std::cerr << "Failed to get pixel format descriptor." << std::endl;
        avformat_close_input(&decoder_info.input_fmt_ctx);
        return false;
    }
    decoder_info.bit_depth = pix_desc->comp[0].depth;

    // Choose decoder based on bit depth
    AVCodec* decoder = nullptr;
    if (decoder_info.bit_depth <= 8 && codecpar->codec_id == AV_CODEC_ID_H264) {
        decoder = avcodec_find_decoder_by_name("h264_cuvid");
        if (!decoder) {
            std::cerr << "Failed to find the hardware-accelerated decoder for h264. Falling back to software decoder." << std::endl;
            decoder = avcodec_find_decoder(codecpar->codec_id);
            if (!decoder) {
                std::cerr << "Failed to find the software decoder for h264" << std::endl;
                avformat_close_input(&decoder_info.input_fmt_ctx);
                return false;
            }
        }
    } else {
        // Use software decoder for higher bit depths or other codecs
        decoder = avcodec_find_decoder(codecpar->codec_id);
        if (!decoder) {
            std::cerr << "Failed to find decoder for the video stream" << std::endl;
            avformat_close_input(&decoder_info.input_fmt_ctx);
            return false;
        }
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

    // If hardware decoder, set hw_device_ctx and get_format callback
    if (decoder_info.bit_depth <= 8 && decoder->capabilities & AV_CODEC_CAP_HARDWARE) {
        decoder_info.decoder_ctx->get_format = get_hw_format;
        decoder_info.decoder_ctx->hw_device_ctx = av_buffer_ref(hw_device_ctx);
        if (!decoder_info.decoder_ctx->hw_device_ctx) {
            std::cerr << "Failed to reference hw_device_ctx." << std::endl;
            avcodec_free_context(&decoder_info.decoder_ctx);
            avformat_close_input(&decoder_info.input_fmt_ctx);
            return false;
        }
    }

    AVDictionary* decoder_opts = nullptr;
    // these settings do not work
    av_dict_set(&decoder_opts, "delay", "0", 0);
    av_dict_set(&decoder_opts, "max_delay", "0", 0);
    av_dict_set(&decoder_opts, "reorder", "0", 0);

    // valid when enable low delay flag, important when reducing decoding buffer latency
    decoder_info.decoder_ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;

    // Open decoder
    if (avcodec_open2(decoder_info.decoder_ctx, decoder, &decoder_opts) < 0) {
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
        // int64_t decode_s = get_current_time_us();
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
            // int64_t decode_e = get_current_time_us();

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
            // std::cout << frame->pts << ": " << (decode_e - decode_s) << std::endl;
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
            frame_data.decode_start_time = std::chrono::steady_clock::now();
            frame_data.decode_end_time = std::chrono::steady_clock::now();
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

// Encoder Function with Initialization and Scaling
bool encode_frames(const EncoderConfig& config, FrameQueue& frame_queue, AVRational input_time_base, std::atomic<bool>& encode_finished, int input_width, int input_height) {
    AVFormatContext* output_fmt_ctx = nullptr;
    AVStream* out_stream = nullptr;

    // Set SRT options with increased latency and specified packet size
    AVDictionary* format_opts = nullptr;
    av_dict_set(&format_opts, "latency", "0", 0);     // Latency in ms
    av_dict_set(&format_opts, "buffer_size", "1000000", 0);

    // Allocate output format context with FLV over SRT
    if (avformat_alloc_output_context2(&output_fmt_ctx, nullptr, "flv", config.output_url.c_str()) < 0) {
        std::cerr << "Could not create output context for " << config.output_url << std::endl;
        av_dict_free(&format_opts);
        return false;
    }

    // Find the NVENC encoder
    AVCodec* encoder = avcodec_find_encoder_by_name("h264_nvenc");
    if (!encoder) {
        std::cerr << "Necessary encoder not found" << std::endl;
        avformat_free_context(output_fmt_ctx);
        av_dict_free(&format_opts);
        return false;
    }

    // Create new stream for encoder
    out_stream = avformat_new_stream(output_fmt_ctx, nullptr);
    if (!out_stream) {
        std::cerr << "Failed allocating output stream for " << config.output_url << std::endl;
        avformat_free_context(output_fmt_ctx);
        av_dict_free(&format_opts);
        return false;
    }

    // Allocate and configure encoder context
    AVCodecContext* encoder_ctx = avcodec_alloc_context3(encoder);
    if (!encoder_ctx) {
        std::cerr << "Could not allocate encoder context for " << config.output_url << std::endl;
        avformat_free_context(output_fmt_ctx);
        av_dict_free(&format_opts);
        return false;
    }

    // Set encoder parameters based on configuration
    encoder_ctx->height = config.height;
    encoder_ctx->width = config.width;
    encoder_ctx->sample_aspect_ratio = AVRational{1, 1}; // Square pixels
    encoder_ctx->pix_fmt = AV_PIX_FMT_CUDA; // Use hardware pixel format
    encoder_ctx->time_base = AVRational{1, static_cast<int>(config.framerate)};          // Set frame rate
    encoder_ctx->framerate = AVRational{static_cast<int>(config.framerate), 1};
    encoder_ctx->bit_rate = static_cast<int>(config.bitrate * 1000);       // Convert kbps to bps
    encoder_ctx->gop_size = static_cast<int>(config.framerate);
    encoder_ctx->max_b_frames = 0;
    encoder_ctx->thread_count = 0;

    // Set hardware device context
    encoder_ctx->hw_device_ctx = av_buffer_ref(hw_device_ctx);
    if (!encoder_ctx->hw_device_ctx) {
        std::cerr << "Failed to set hw_device_ctx on encoder." << std::endl;
        avcodec_free_context(&encoder_ctx);
        avformat_free_context(output_fmt_ctx);
        av_dict_free(&format_opts);
        return false;
    }

    // Initialize hardware frames context
    AVBufferRef* hw_frames_ref = av_hwframe_ctx_alloc(hw_device_ctx);
    if (!hw_frames_ref) {
        std::cerr << "Failed to create hardware frame context." << std::endl;
        avcodec_free_context(&encoder_ctx);
        avformat_free_context(output_fmt_ctx);
        av_dict_free(&format_opts);
        return false;
    }

    AVHWFramesContext* frames_ctx = (AVHWFramesContext*)(hw_frames_ref->data);
    frames_ctx->format = AV_PIX_FMT_CUDA; // Set format to CUDA hardware
    frames_ctx->sw_format = AV_PIX_FMT_NV12; // Set software format as NV12
    frames_ctx->width = config.width;
    frames_ctx->height = config.height;
    frames_ctx->initial_pool_size = 20;

    // Initialize the hardware frames context
    if (av_hwframe_ctx_init(hw_frames_ref) < 0) {
        std::cerr << "Failed to initialize hardware frames context." << std::endl;
        av_buffer_unref(&hw_frames_ref);
        avcodec_free_context(&encoder_ctx);
        avformat_free_context(output_fmt_ctx);
        av_dict_free(&format_opts);
        return false;
    }

    // Set the encoder context's hw_frames_ctx
    encoder_ctx->hw_frames_ctx = av_buffer_ref(hw_frames_ref);
    if (!encoder_ctx->hw_frames_ctx) {
        std::cerr << "Failed to set hw_frames_ctx on encoder." << std::endl;
        av_buffer_unref(&hw_frames_ref);
        avcodec_free_context(&encoder_ctx);
        avformat_free_context(output_fmt_ctx);
        av_dict_free(&format_opts);
        return false;
    }

    // Set preset and tune options for low latency
    AVDictionary* codec_opts = nullptr;
    av_dict_set(&codec_opts, "preset", "p1", 0); // p7 is equivalent to "slow" preset
    av_dict_set(&codec_opts, "tune", "ull", 0);  // Ultra low latency
    av_dict_set_int(&codec_opts, "async_depth", 1, 0);

    // valid when reducing latency
    av_dict_set(&codec_opts, "delay", "0", 0);

    // Open encoder with codec options
    if (avcodec_open2(encoder_ctx, encoder, &codec_opts) < 0) {
        std::cerr << "Cannot open video encoder for " << config.output_url << std::endl;
        av_buffer_unref(&hw_frames_ref);
        avcodec_free_context(&encoder_ctx);
        avformat_free_context(output_fmt_ctx);
        av_dict_free(&codec_opts);
        av_dict_free(&format_opts);
        return false;
    }

    // Copy encoder parameters to output stream
    if (avcodec_parameters_from_context(out_stream->codecpar, encoder_ctx) < 0) {
        std::cerr << "Failed to copy encoder parameters to output stream for " << config.output_url << std::endl;
        av_buffer_unref(&hw_frames_ref);
        avcodec_free_context(&encoder_ctx);
        avformat_free_context(output_fmt_ctx);
        av_dict_free(&codec_opts);
        av_dict_free(&format_opts);
        return false;
    }

    out_stream->time_base = encoder_ctx->time_base;

    // Open output URL with format options
    if (!(output_fmt_ctx->oformat->flags & AVFMT_NOFILE)) {
        if (avio_open2(&output_fmt_ctx->pb, config.output_url.c_str(), AVIO_FLAG_WRITE, nullptr, &format_opts) < 0) {
            std::cerr << "Could not open output URL: " << config.output_url << std::endl;
            av_buffer_unref(&hw_frames_ref);
            avcodec_free_context(&encoder_ctx);
            avformat_free_context(output_fmt_ctx);
            av_dict_free(&codec_opts);
            av_dict_free(&format_opts);
            return false;
        }
    }

    // Write header to output
    if (avformat_write_header(output_fmt_ctx, &codec_opts) < 0) {
        std::cerr << "Error occurred when writing header to output: " << config.output_url << std::endl;
        av_buffer_unref(&hw_frames_ref);
        avcodec_free_context(&encoder_ctx);
        avformat_free_context(output_fmt_ctx);
        av_dict_free(&codec_opts);
        av_dict_free(&format_opts);
        return false;
    }

    // Free codec options as they are no longer needed
    av_dict_free(&codec_opts);
    av_dict_free(&format_opts);

    // Initialize filter graph for scaling on GPU
    AVFilterGraph* filter_graph = avfilter_graph_alloc();
    if (!filter_graph) {
        std::cerr << "Unable to create filter graph." << std::endl;
        av_buffer_unref(&hw_frames_ref);
        avcodec_free_context(&encoder_ctx);
        avformat_free_context(output_fmt_ctx);
        return false;
    }

    AVFilterContext* buffersrc_ctx = nullptr;
    AVFilterContext* buffersink_ctx = nullptr;

    // Create buffer source
    const AVFilter* buffersrc = avfilter_get_by_name("buffer");
    // Create buffer sink
    const AVFilter* buffersink = avfilter_get_by_name("buffersink");

    // Create filter inputs and outputs
    AVFilterInOut* inputs = avfilter_inout_alloc();
    AVFilterInOut* outputs = avfilter_inout_alloc();
    char args[512];
    snprintf(args, sizeof(args),
            "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d",
            input_width, input_height, AV_PIX_FMT_CUDA,
            encoder_ctx->time_base.num, encoder_ctx->time_base.den);

    if (avfilter_graph_create_filter(&buffersrc_ctx, buffersrc, "in", args, nullptr, filter_graph) < 0) {
        std::cerr << "Cannot create buffer source." << std::endl;
        avfilter_inout_free(&inputs);
        avfilter_inout_free(&outputs);
        avfilter_graph_free(&filter_graph);
        av_buffer_unref(&hw_frames_ref);
        avcodec_free_context(&encoder_ctx);
        avformat_free_context(output_fmt_ctx);
        return false;
    }

    if (avfilter_graph_create_filter(&buffersink_ctx, buffersink, "out", nullptr, nullptr, filter_graph) < 0) {
        std::cerr << "Cannot create buffer sink." << std::endl;
        avfilter_inout_free(&inputs);
        avfilter_inout_free(&outputs);
        avfilter_graph_free(&filter_graph);
        av_buffer_unref(&hw_frames_ref);
        avcodec_free_context(&encoder_ctx);
        avformat_free_context(output_fmt_ctx);
        return false;
    }

    // Set the hardware frames context for buffer source
    AVBufferSrcParameters *src_params = av_buffersrc_parameters_alloc();
    if (!src_params) {
        std::cerr << "Cannot allocate AVBufferSrcParameters" << std::endl;
        avfilter_inout_free(&inputs);
        avfilter_inout_free(&outputs);
        avfilter_graph_free(&filter_graph);
        av_buffer_unref(&hw_frames_ref);
        avcodec_free_context(&encoder_ctx);
        avformat_free_context(output_fmt_ctx);
        return false;
    }

    src_params->format = AV_PIX_FMT_CUDA;
    src_params->hw_frames_ctx = av_buffer_ref(hw_frames_ref);
    src_params->width = input_width;
    src_params->height = input_height;
    src_params->time_base = encoder_ctx->time_base;

    if (av_buffersrc_parameters_set(buffersrc_ctx, src_params) < 0) {
        std::cerr << "Cannot set parameters for buffer source." << std::endl;
        av_freep(&src_params);
        avfilter_inout_free(&inputs);
        avfilter_inout_free(&outputs);
        avfilter_graph_free(&filter_graph);
        av_buffer_unref(&hw_frames_ref);
        avcodec_free_context(&encoder_ctx);
        avformat_free_context(output_fmt_ctx);
        return false;
    }
    av_freep(&src_params);

    enum AVPixelFormat pix_fmts[] = { AV_PIX_FMT_CUDA, AV_PIX_FMT_NONE };
    if (av_opt_set_int_list(buffersink_ctx, "pix_fmts", pix_fmts, AV_PIX_FMT_NONE, AV_OPT_SEARCH_CHILDREN) < 0) {
        std::cerr << "Cannot set output pixel format to AV_PIX_FMT_CUDA." << std::endl;
        avfilter_inout_free(&inputs);
        avfilter_inout_free(&outputs);
        avfilter_graph_free(&filter_graph);
        av_buffer_unref(&hw_frames_ref);
        avcodec_free_context(&encoder_ctx);
        avformat_free_context(output_fmt_ctx);
        return false;
    }

    // Set the endpoints for the filter graph
    outputs->name = av_strdup("in");
    outputs->filter_ctx = buffersrc_ctx;
    outputs->pad_idx = 0;
    outputs->next = nullptr;

    inputs->name = av_strdup("out");
    inputs->filter_ctx = buffersink_ctx;
    inputs->pad_idx = 0;
    inputs->next = nullptr;

    // Create the filter chain
    std::string filter_desc = "scale_cuda=w=" + std::to_string(config.width) + ":h=" + std::to_string(config.height);
    if (avfilter_graph_parse_ptr(filter_graph, filter_desc.c_str(), &inputs, &outputs, nullptr) < 0) {
        std::cerr << "Error parsing filter graph." << std::endl;
        avfilter_inout_free(&inputs);
        avfilter_inout_free(&outputs);
        avfilter_graph_free(&filter_graph);
        av_buffer_unref(&hw_frames_ref);
        avcodec_free_context(&encoder_ctx);
        avformat_free_context(output_fmt_ctx);
        return false;
    }

    if (avfilter_graph_config(filter_graph, nullptr) < 0) {
        std::cerr << "Error configuring the filter graph." << std::endl;
        avfilter_inout_free(&inputs);
        avfilter_inout_free(&outputs);
        avfilter_graph_free(&filter_graph);
        av_buffer_unref(&hw_frames_ref);
        avcodec_free_context(&encoder_ctx);
        avformat_free_context(output_fmt_ctx);
        return false;
    }

    AVFilterLink *outlink = buffersink_ctx->inputs[0];
    if (!outlink) {
        std::cerr << "Error: buffersink_ctx->inputs[0] is null." << std::endl;
        avfilter_inout_free(&inputs);
        avfilter_inout_free(&outputs);
        avfilter_graph_free(&filter_graph);
        av_buffer_unref(&hw_frames_ref);
        avcodec_free_context(&encoder_ctx);
        avformat_free_context(output_fmt_ctx);
        return false;
    }

    outlink->hw_frames_ctx = av_buffer_ref(hw_frames_ref);
    if (!outlink->hw_frames_ctx) {
        std::cerr << "Cannot set hw_frames_ctx on buffersink input link." << std::endl;
        avfilter_inout_free(&inputs);
        avfilter_inout_free(&outputs);
        avfilter_graph_free(&filter_graph);
        av_buffer_unref(&hw_frames_ref);
        avcodec_free_context(&encoder_ctx);
        avformat_free_context(output_fmt_ctx);
        return false;
    }

    avfilter_inout_free(&inputs);
    avfilter_inout_free(&outputs);

    int64_t first_pts = AV_NOPTS_VALUE;
    int frame_count = 0;

    // Initialize TimingLogger
    TimingLogger logger(config.log_filename);
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

        // Push the frame into the filter graph
        if (av_buffersrc_add_frame_flags(buffersrc_ctx, frame_data.frame, AV_BUFFERSRC_FLAG_KEEP_REF) < 0) {
            std::cerr << "Error while feeding the filter graph." << std::endl;
            av_frame_free(&frame_data.frame);
            continue;
        }

        // Pull filtered frames from the filter graph
        AVFrame* filt_frame = av_frame_alloc();
        while (true) {
            int ret = av_buffersink_get_frame(buffersink_ctx, filt_frame);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                av_frame_unref(filt_frame);
                break;
            }
            if (ret < 0) {
                av_frame_unref(filt_frame);
                break;
            }

            if (first_pts == AV_NOPTS_VALUE) {
                first_pts = filt_frame->pts;
                std::cout << "First encoded frame PTS for " << config.output_url << ": " << first_pts << std::endl;
            }

            // Set PTS
            filt_frame->pts = av_rescale_q(filt_frame->pts, input_time_base, encoder_ctx->time_base);

            // Send the filtered frame to the encoder
            ret = avcodec_send_frame(encoder_ctx, filt_frame);
            if (ret < 0) {
                std::cerr << "Error sending a frame for encoding." << std::endl;
                av_frame_unref(filt_frame);
                break;
            }

            // Receive packets from encoder
            while (ret >= 0) {
                AVPacket* enc_pkt = av_packet_alloc();
                if (!enc_pkt) {
                    std::cerr << "Could not allocate encoding packet for " << config.output_url << std::endl;
                    break;
                }

                ret = avcodec_receive_packet(encoder_ctx, enc_pkt);
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                    av_packet_free(&enc_pkt);
                    break;
                } else if (ret < 0) {
                    std::cerr << "Error during encoding for " << config.output_url << ": " << get_error_text(ret) << std::endl;
                    av_packet_free(&enc_pkt);
                    break;
                }

                // Rescale packet timestamp
                av_packet_rescale_ts(enc_pkt, encoder_ctx->time_base, out_stream->time_base);
                enc_pkt->stream_index = out_stream->index;
                // std::cout << filt_frame->pts << ": " << get_timestamp_with_ms() << std::endl;

                // Write packet to output
                int write_ret = av_interleaved_write_frame(output_fmt_ctx, enc_pkt);
                if (write_ret < 0) {
                    std::cerr << "Error writing packet to output for " << config.output_url << ": " << get_error_text(write_ret) << std::endl;
                    av_packet_free(&enc_pkt);
                    break;
                }

                av_packet_free(&enc_pkt);
            }

            av_frame_unref(filt_frame);
        }
        av_frame_free(&filt_frame);
        av_frame_free(&frame_data.frame);

        auto encode_end = std::chrono::steady_clock::now();

        // Calculate timings
        double decode_time = std::chrono::duration<double, std::milli>(frame_data.decode_end_time - frame_data.decode_start_time).count();
        double encode_time = std::chrono::duration<double, std::milli>(encode_end - encode_start).count();
        double interval_time = std::chrono::duration<double, std::milli>(encode_end - frame_data.decode_start_time).count();

        frame_count++;
        logger.add_entry(frame_count, decode_time, encode_time, interval_time);

        if (frame_count % 100 == 0) {
            std::cout << "Encoded " << frame_count << " frames for " << config.output_url << ", queue length: " << frame_queue.size() << std::endl;
            std::cout << "Decoded Time: " << decode_time << std::endl;
        }
    }
    std::cout << "Encoded " << frame_count << " frames for " << config.output_url << std::endl;

    // Flush encoder to ensure all frames are processed
    avcodec_send_frame(encoder_ctx, nullptr);
    while (true) {
        AVPacket* enc_pkt = av_packet_alloc();
        if (!enc_pkt) {
            std::cerr << "Could not allocate encoding packet during flush for " << config.output_url << std::endl;
            break;
        }

        int ret = avcodec_receive_packet(encoder_ctx, enc_pkt);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            av_packet_free(&enc_pkt);
            break;
        } else if (ret < 0) {
            std::cerr << "Error during encoding flush for " << config.output_url << ": " << get_error_text(ret) << std::endl;
            av_packet_free(&enc_pkt);
            break;
        }

        // Rescale packet timestamp
        av_packet_rescale_ts(enc_pkt, encoder_ctx->time_base, out_stream->time_base);
        enc_pkt->stream_index = out_stream->index;

        // Write flushed packet to output
        int write_ret = av_interleaved_write_frame(output_fmt_ctx, enc_pkt);
        if (write_ret < 0) {
            std::cerr << "Error writing flushed packet to output for " << config.output_url << ": " << get_error_text(write_ret) << std::endl;
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
    avfilter_graph_free(&filter_graph);
    av_buffer_unref(&hw_frames_ref);
    avcodec_free_context(&encoder_ctx);
    if (!(output_fmt_ctx->oformat->flags & AVFMT_NOFILE))
        avio_closep(&output_fmt_ctx->pb);
    avformat_free_context(output_fmt_ctx);

    encode_finished = true;
    std::cout << "Encoding finished for " << config.output_url << "." << std::endl;
    return true;
}

int main(int argc, char* argv[]) {
    // Expecting at least 2 arguments: program, input_url, and at least 1 output_url
    if (argc < 3) {
        std::cerr << "Usage: " << argv[0] 
                  << " <input_srt_url> <output1_srt_url> [<output2_srt_url> ... <output6_srt_url>]" 
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

    // std::vector<ResolutionBitrateLog> resolution_bitrate_log = {
    //     {3840, 2160, 16000,  "frame-1"},
    //     {2560, 1440, 10000,  "frame-2"},
    //     {1920, 1080, 5000,   "frame-3"},
    //     {1280, 720,  2500,   "frame-4"},
    //     {854,  480,  1000,   "frame-5"},
    //     {640,  360,  600,    "frame-6"}
    // };

    // std::vector<ResolutionBitrateLog> resolution_bitrate_log = {
    //     {3840, 2160, 16000,  "frame-1"},
    //     {3840, 2160, 16000,  "frame-2"},
    //     {3840, 2160, 16000,   "frame-3"},
    //     {3840, 2160, 16000,   "frame-4"},
    //     {3840, 2160, 16000,   "frame-5"},
    //     {3840, 2160, 16000,    "frame-6"},
    //     {3840, 2160, 16000,    "frame-7"},
    //     {3840, 2160, 16000,    "frame-8"},
    // };

    std::vector<ResolutionBitrateLog> resolution_bitrate_log = {
        {2560, 1440, 4000,  "frame-1"},
        {2560, 1440, 4000,  "frame-2"},
        {2560, 1440, 4000,  "frame-3"},
        {2560, 1440, 4000,  "frame-4"},
        {2560, 1440, 4000,  "frame-5"},
        {2560, 1440, 4000,  "frame-6"}
    };

    if (num_outputs > (int) resolution_bitrate_log.size()) {
        std::cerr << "Error: Maximum supported output URLs is " << resolution_bitrate_log.size() << "." << std::endl;
        return 1;
    }

    // Initialize FFmpeg
    avformat_network_init();

    // Initialize hardware device context
    if (av_hwdevice_ctx_create(&hw_device_ctx, AV_HWDEVICE_TYPE_CUDA, nullptr, nullptr, 0) < 0) {
        std::cerr << "Failed to create CUDA device." << std::endl;
        avformat_network_deinit();
        return -1;
    }

    std::atomic<bool> decode_finished(false);

    // Initialize decoder
    DecoderInfo decoder_info;
    if (!initialize_decoder(input_url, decoder_info)) {
        std::cerr << "Decoder initialization failed" << std::endl;
        av_buffer_unref(&hw_device_ctx);
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
        config.log_filename = "/home/zx/edge-use-case/smart-stadium-transcoding/result/multiple-transcode-gpu/task" + std::to_string(num_outputs) + "/" + get_timestamp_with_ms() + "/"
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
            if (!encode_frames(encoder_configs[i], *frame_queues[i], decoder_info.input_time_base, enc_finished_flags[i], decoder_info.decoder_ctx->width, decoder_info.decoder_ctx->height)) {
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

    // Clean up hardware device context
    if (hw_device_ctx) {
        av_buffer_unref(&hw_device_ctx);
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