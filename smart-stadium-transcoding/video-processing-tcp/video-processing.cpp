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
#include <libavutil/imgutils.h>
#include <libavutil/opt.h> // 添加此行以修复 AV_OPT_SEARCH_CHILDREN 和 av_opt_set_int_list
#include <libswscale/swscale.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
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
    std::chrono::steady_clock::time_point filter_start_time;
    std::chrono::steady_clock::time_point filter_end_time;
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
        std::lock_guard<std::mutex> lock(mutex_);
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
        std::lock_guard<std::mutex> lock(mutex_);
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
    // Constructor that initializes the log filename
    TimingLogger(const std::string& log_filename) : filename_(log_filename) {}

    /**
     * @brief Adds a new log entry with timing information.
     *
     * @param frame_number      The sequential number of the frame.
     * @param decode_time_ms    Time taken to decode the frame in milliseconds.
     * @param filter_time_ms    Time taken to apply filters to the frame in milliseconds.
     * @param encode_time_ms    Time taken to encode the frame in milliseconds.
     * @param interval_ms       Time interval between frames in milliseconds.
     */
    void add_entry(int frame_number, double decode_time_ms, double filter_time_ms, double encode_time_ms, double interval_ms) {
        std::lock_guard<std::mutex> lock(mutex_);
        log_entries.emplace_back(frame_number, decode_time_ms, filter_time_ms, encode_time_ms, interval_ms);
    }

    /**
     * @brief Writes all logged timing information to a file.
     */
    void write_to_file() {
        std::lock_guard<std::mutex> lock(mutex_);
        std::filesystem::path filepath(filename_);
        std::filesystem::path parent_path = filepath.parent_path();

        try {
            // Create directories if they do not exist
            if (!parent_path.empty() && !std::filesystem::exists(parent_path)) {
                std::filesystem::create_directories(parent_path);
                std::cout << "Created directories: " << parent_path << std::endl;
            }
        }
        catch (const std::filesystem::filesystem_error& e) {
            std::cerr << "Filesystem error: " << e.what() << std::endl;
            return;
        }

        // Open the log file for writing
        std::ofstream ofs(filename_);
        if (!ofs.is_open()) {
            std::cerr << "Failed to open " << filename_ << " for writing." << std::endl;
            return;
        }

        // Set formatting options for floating-point numbers
        ofs << std::fixed << std::setprecision(4);

        // Write the header row
        ofs << std::left << std::setw(10) << "Frame"
            << std::left << std::setw(20) << "Decode Time (ms)"
            << std::left << std::setw(20) << "Filter Time (ms)"
            << std::left << std::setw(20) << "Encode Time (ms)"
            << std::left << std::setw(20) << "Interval Time (ms)"
            << "\n";

        // Write each log entry
        for (const auto& entry : log_entries) {
            ofs << std::left << std::setw(10) << entry.frame_number
                << std::left << std::setw(20) << entry.decode_time_ms
                << std::left << std::setw(20) << entry.filter_time_ms
                << std::left << std::setw(20) << entry.encode_time_ms
                << std::left << std::setw(20) << entry.interval_ms
                << "\n";
        }

        // Close the file stream
        ofs.close();
        std::cout << "Timing information written to " << filename_ << std::endl;
    }

private:
    // Structure to hold individual log entries
    struct LogEntry {
        int frame_number;         // Sequential frame number
        double decode_time_ms;    // Decode time in milliseconds
        double filter_time_ms;    // Filter time in milliseconds
        double encode_time_ms;    // Encode time in milliseconds
        double interval_ms;       // Interval time in milliseconds

        // Constructor to initialize all fields
        LogEntry(int fn, double dt, double ft, double et, double it)
            : frame_number(fn), decode_time_ms(dt), filter_time_ms(ft), encode_time_ms(et), interval_ms(it) {}
    };

    std::vector<LogEntry> log_entries; // Vector to store all log entries
    std::mutex mutex_;                  // Mutex to ensure thread safety
    std::string filename_;              // Name of the log file
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

// Structure to hold encoder information
struct EncoderInfo {
    EncoderConfig config;
    AVFormatContext* output_fmt_ctx;
    AVCodecContext* encoder_ctx;
    AVStream* out_stream;
    SwsContext* sws_ctx;
    AVFrame* enc_frame;
    TimingLogger* logger;
};

// Structure to hold filter context
struct FilterContextStruct {
    AVFilterGraph* graph;
    AVFilterContext* buffersrc_ctx;
    AVFilterContext* buffersink_ctx;
};

// Decoder Initialization and Function
bool initialize_decoder(const char* input_url, DecoderInfo& decoder_info) {
    // Initialize input format context
    decoder_info.input_fmt_ctx = nullptr;
    AVDictionary* format_opts = nullptr;
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
    AVCodec* decoder = avcodec_find_decoder(codecpar->codec_id);
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
    }
    else {
        decoder_info.input_framerate = av_q2d(frame_rate_rational);
    }

    std::cout << "Input frame rate: " << decoder_info.input_framerate << " FPS" << std::endl;
    std::cout << "Decoder initialized successfully." << std::endl;
    return true;
}

// Encoder Initialization Function
bool initialize_encoder(const EncoderConfig& config, EncoderInfo& encoder_info, AVRational input_time_base) {
    encoder_info.config = config;
    encoder_info.output_fmt_ctx = nullptr;
    encoder_info.encoder_ctx = nullptr;
    encoder_info.out_stream = nullptr;
    encoder_info.sws_ctx = nullptr;
    encoder_info.enc_frame = nullptr;
    encoder_info.logger = new TimingLogger(config.log_filename);

    AVDictionary* format_opts = nullptr;

    // Allocate output format context with FLV over TCP
    if (avformat_alloc_output_context2(&encoder_info.output_fmt_ctx, nullptr, "flv", config.output_url.c_str()) < 0) {
        std::cerr << "Could not create output context for " << config.output_url << std::endl;
        av_dict_free(&format_opts);
        return false;
    }

    // Find encoder for H.264
    AVCodec* encoder = avcodec_find_encoder(AV_CODEC_ID_H264);
    if (!encoder) {
        std::cerr << "Necessary encoder not found" << std::endl;
        avformat_free_context(encoder_info.output_fmt_ctx);
        av_dict_free(&format_opts);
        return false;
    }

    // Create new stream for encoder
    encoder_info.out_stream = avformat_new_stream(encoder_info.output_fmt_ctx, nullptr);
    if (!encoder_info.out_stream) {
        std::cerr << "Failed allocating output stream for " << config.output_url << std::endl;
        avformat_free_context(encoder_info.output_fmt_ctx);
        av_dict_free(&format_opts);
        return false;
    }

    // Allocate and configure encoder context
    encoder_info.encoder_ctx = avcodec_alloc_context3(encoder);
    if (!encoder_info.encoder_ctx) {
        std::cerr << "Could not allocate encoder context for " << config.output_url << std::endl;
        avformat_free_context(encoder_info.output_fmt_ctx);
        av_dict_free(&format_opts);
        return false;
    }

    // Set encoder parameters based on configuration
    encoder_info.encoder_ctx->height = config.height;
    encoder_info.encoder_ctx->width = config.width;
    encoder_info.encoder_ctx->sample_aspect_ratio = AVRational{1, 1}; // Square pixels
    encoder_info.encoder_ctx->pix_fmt = AV_PIX_FMT_YUV420P;
    encoder_info.encoder_ctx->time_base = AVRational{1, static_cast<int>(config.framerate)}; // e.g., 30 fps
    encoder_info.encoder_ctx->framerate = AVRational{static_cast<int>(config.framerate), 1};
    encoder_info.encoder_ctx->bit_rate = static_cast<int>(config.bitrate * 1000); // Convert kbps to bps
    encoder_info.encoder_ctx->gop_size = static_cast<int>(config.framerate); // One GOP per second
    encoder_info.encoder_ctx->max_b_frames = 0;
    encoder_info.encoder_ctx->thread_count = 0;

    // Set preset and tune options for low latency
    AVDictionary* codec_opts = nullptr;
    av_dict_set(&codec_opts, "preset", "ultrafast", 0);
    av_dict_set(&codec_opts, "tune", "zerolatency", 0);
    av_dict_set(&codec_opts, "delay", "0", 0);

    // Open encoder with codec options
    if (avcodec_open2(encoder_info.encoder_ctx, encoder, &codec_opts) < 0) {
        std::cerr << "Cannot open video encoder for " << config.output_url << std::endl;
        avcodec_free_context(&encoder_info.encoder_ctx);
        avformat_free_context(encoder_info.output_fmt_ctx);
        av_dict_free(&codec_opts);
        av_dict_free(&format_opts);
        return false;
    }

    // Copy encoder parameters to output stream
    if (avcodec_parameters_from_context(encoder_info.out_stream->codecpar, encoder_info.encoder_ctx) < 0) {
        std::cerr << "Failed to copy encoder parameters to output stream for " << config.output_url << std::endl;
        avcodec_free_context(&encoder_info.encoder_ctx);
        avformat_free_context(encoder_info.output_fmt_ctx);
        av_dict_free(&codec_opts);
        av_dict_free(&format_opts);
        return false;
    }

    encoder_info.out_stream->time_base = encoder_info.encoder_ctx->time_base;

    // Open output URL with format options
    if (!(encoder_info.output_fmt_ctx->oformat->flags & AVFMT_NOFILE)) {
        if (avio_open2(&encoder_info.output_fmt_ctx->pb, config.output_url.c_str(), AVIO_FLAG_WRITE, nullptr, &format_opts) < 0) {
            std::cerr << "Could not open output URL: " << config.output_url << std::endl;
            avcodec_free_context(&encoder_info.encoder_ctx);
            avformat_free_context(encoder_info.output_fmt_ctx);
            av_dict_free(&codec_opts);
            av_dict_free(&format_opts);
            return false;
        }
    }

    // Write header to output
    if (avformat_write_header(encoder_info.output_fmt_ctx, &codec_opts) < 0) {
        std::cerr << "Error occurred when writing header to output: " << config.output_url << std::endl;
        avcodec_free_context(&encoder_info.encoder_ctx);
        avformat_free_context(encoder_info.output_fmt_ctx);
        av_dict_free(&codec_opts);
        av_dict_free(&format_opts);
        return false;
    }

    // Free codec options as they are no longer needed
    av_dict_free(&codec_opts);
    av_dict_free(&format_opts);

    // Initialize scaler
    encoder_info.sws_ctx = sws_getContext(
        config.width, config.height, AV_PIX_FMT_YUV420P,
        encoder_info.encoder_ctx->width, encoder_info.encoder_ctx->height, encoder_info.encoder_ctx->pix_fmt,
        SWS_BILINEAR, nullptr, nullptr, nullptr
    );

    if (!encoder_info.sws_ctx) {
        std::cerr << "Could not initialize the conversion context for " << config.output_url << std::endl;
        avcodec_free_context(&encoder_info.encoder_ctx);
        avformat_free_context(encoder_info.output_fmt_ctx);
        return false;
    }

    // Allocate frame for encoder
    encoder_info.enc_frame = av_frame_alloc();
    if (!encoder_info.enc_frame) {
        std::cerr << "Could not allocate encoding frame for " << config.output_url << std::endl;
        sws_freeContext(encoder_info.sws_ctx);
        avcodec_free_context(&encoder_info.encoder_ctx);
        avformat_free_context(encoder_info.output_fmt_ctx);
        return false;
    }
    encoder_info.enc_frame->format = encoder_info.encoder_ctx->pix_fmt;
    encoder_info.enc_frame->width  = encoder_info.encoder_ctx->width;
    encoder_info.enc_frame->height = encoder_info.encoder_ctx->height;

    if (av_frame_get_buffer(encoder_info.enc_frame, 32) < 0) {
        std::cerr << "Could not allocate the video frame data for " << config.output_url << std::endl;
        av_frame_free(&encoder_info.enc_frame);
        sws_freeContext(encoder_info.sws_ctx);
        avcodec_free_context(&encoder_info.encoder_ctx);
        avformat_free_context(encoder_info.output_fmt_ctx);
        return false;
    }

    return true;
}

// Initialize Filter Graph
bool init_filter_graph(FilterContextStruct& filter_ctx, AVCodecContext* dec_ctx, DecoderInfo& decoder_info) {
    char args[512];
    int ret = 0;
    filter_ctx.graph = avfilter_graph_alloc();
    if (!filter_ctx.graph) {
        std::cerr << "Unable to create filter graph." << std::endl;
        return false;
    }

    // Get buffer source and buffer sink
    const AVFilter* buffersrc  = avfilter_get_by_name("buffer");
    const AVFilter* buffersink = avfilter_get_by_name("buffersink");
    AVFilterContext* buffersrc_ctx = nullptr;
    AVFilterContext* buffersink_ctx = nullptr;

    const char* pix_fmt_str = av_get_pix_fmt_name(dec_ctx->pix_fmt);
    if (!pix_fmt_str) {
        std::cerr << "Unknown pixel format" << std::endl;
        avfilter_graph_free(&filter_ctx.graph);
        return false;
    }

    int time_base_num = dec_ctx->time_base.num;
    int time_base_den = dec_ctx->time_base.den;
    if (time_base_num == 0 || time_base_den == 0) {
        time_base_num = 1;
        time_base_den = static_cast<int>(decoder_info.input_framerate);
        std::cerr << "Warning: Invalid time_base. Setting time_base to " << time_base_num << "/" << time_base_den << "." << std::endl;
    }

    int sar_num = dec_ctx->sample_aspect_ratio.num;
    int sar_den = dec_ctx->sample_aspect_ratio.den;
    if (sar_num == 0 || sar_den == 0) {
        sar_num = 1;
        sar_den = 1;
        std::cerr << "Warning: Invalid sample_aspect_ratio. Setting pixel_aspect to 1/1." << std::endl;
    }

    std::cout << "Decoder Context:" << std::endl;
    std::cout << "Width: " << dec_ctx->width << std::endl;
    std::cout << "Height: " << dec_ctx->height << std::endl;
    std::cout << "Pixel Format: " << pix_fmt_str << std::endl;
    std::cout << "Time Base: " << dec_ctx->time_base.num << "/" << dec_ctx->time_base.den << std::endl;
    std::cout << "Sample Aspect Ratio: " << sar_num << "/" << sar_den << std::endl;

    // Define input arguments
    snprintf(args, sizeof(args),
             "video_size=%dx%d:pix_fmt=%s:time_base=%d/%d:pixel_aspect=%d/%d",
             dec_ctx->width, dec_ctx->height, pix_fmt_str,
             time_base_num, time_base_den,
             sar_num, sar_den);

    std::cout << "Buffer source args: " << args << std::endl;

    // Create buffer source
    ret = avfilter_graph_create_filter(&buffersrc_ctx, buffersrc, "in",
                                       args, nullptr, filter_ctx.graph);
    if (ret < 0) {
        std::cerr << "Cannot create buffer source: " << get_error_text(ret) << std::endl;
        avfilter_graph_free(&filter_ctx.graph);
        return false;
    }

    // Create buffer sink
    ret = avfilter_graph_create_filter(&buffersink_ctx, buffersink, "out",
                                       nullptr, nullptr, filter_ctx.graph);
    if (ret < 0) {
        std::cerr << "Cannot create buffer sink: " << get_error_text(ret) << std::endl;
        avfilter_graph_free(&filter_ctx.graph);
        return false;
    }

    // Set buffer sink pixel formats
    enum AVPixelFormat pix_fmts[] = { AV_PIX_FMT_YUV420P, AV_PIX_FMT_NONE };
    ret = av_opt_set_int_list(buffersink_ctx, "pix_fmts", pix_fmts,
                              AV_PIX_FMT_NONE, AV_OPT_SEARCH_CHILDREN);
    if (ret < 0) {
        std::cerr << "Cannot set output pixel format: " << get_error_text(ret) << std::endl;
        avfilter_graph_free(&filter_ctx.graph);
        return false;
    }

    // Define filter chain: denoise, sharpen, color correction
    // const char* filter_descr = "hqdn3d=1.5:1.5:6:6,unsharp=5:5:1.0:5:5:0.0,eq=contrast=1.2:brightness=0.05:saturation=1.3";
    // const char* filter_descr = "atadenoise=0a=0.2:0b=0.2,unsharp=3:3:1.0:3:3:0.0,eq=contrast=1.2:brightness=0.05:saturation=1.3";
    // sharp large size: more cpu
    const char* filter_descr = "unsharp=3:3:0.05:3:3:0.0, eq=contrast=1.2:brightness=0.05:saturation=1.3";

    AVFilterInOut* outputs = avfilter_inout_alloc();
    AVFilterInOut* inputs  = avfilter_inout_alloc();

    if (!outputs || !inputs) {
        std::cerr << "Cannot allocate filter inputs/outputs." << std::endl;
        avfilter_inout_free(&outputs);
        avfilter_inout_free(&inputs);
        avfilter_graph_free(&filter_ctx.graph);
        return false;
    }

    // Set outputs
    outputs->name       = av_strdup("in");
    outputs->filter_ctx = buffersrc_ctx;
    outputs->pad_idx    = 0;
    outputs->next       = nullptr;

    // Set inputs
    inputs->name       = av_strdup("out");
    inputs->filter_ctx = buffersink_ctx;
    inputs->pad_idx    = 0;
    inputs->next       = nullptr;

    // Parse filter graph
    ret = avfilter_graph_parse_ptr(filter_ctx.graph, filter_descr,
                                   &inputs, &outputs, nullptr);
    if (ret < 0) {
        std::cerr << "Error parsing filter graph: " << get_error_text(ret) << std::endl;
        avfilter_inout_free(&outputs);
        avfilter_inout_free(&inputs);
        avfilter_graph_free(&filter_ctx.graph);
        return false;
    }

    // Configure filter graph
    ret = avfilter_graph_config(filter_ctx.graph, nullptr);
    if (ret < 0) {
        std::cerr << "Error configuring filter graph: " << get_error_text(ret) << std::endl;
        avfilter_inout_free(&outputs);
        avfilter_inout_free(&inputs);
        avfilter_graph_free(&filter_ctx.graph);
        return false;
    }
    filter_ctx.graph->thread_type = AVFILTER_THREAD_SLICE;
    filter_ctx.graph->nb_threads = 0;

    // Free in/out structures
    avfilter_inout_free(&outputs);
    avfilter_inout_free(&inputs);

    // Assign filter contexts
    filter_ctx.buffersrc_ctx = buffersrc_ctx;
    filter_ctx.buffersink_ctx = buffersink_ctx;

    return true;
}

// Decoder Thread Function
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
        }
        else {
            av_packet_unref(packet);
        }
    }

    if (packet) {
        av_packet_free(&packet);
    }
    packet_queue.set_finished();
}

// Decoder Function
void decoding_thread(AVCodecContext* decoder_ctx, PacketQueue& packet_queue, FrameQueue& decode_queue) {
    AVPacket* packet = nullptr;
    AVFrame* frame = av_frame_alloc();
    if (!frame) {
        std::cerr << "Could not allocate AVFrame" << std::endl;
        decode_queue.set_finished();
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

            FrameData frame_data;
            frame_data.frame = av_frame_clone(frame);
            frame_data.decode_start_time = decode_start;
            frame_data.decode_end_time = decode_end;
            decode_queue.push(frame_data);

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

        FrameData frame_data;
        frame_data.frame = av_frame_clone(frame);
        frame_data.decode_start_time = std::chrono::steady_clock::now();
        frame_data.decode_end_time = std::chrono::steady_clock::now();
        decode_queue.push(frame_data);

        av_frame_unref(frame);
    }

    decode_queue.set_finished();
    av_frame_free(&frame);
}

// Filter Thread Function
int frame_count = 0;
void filter_thread_func(FrameQueue& decode_queue, std::vector<FrameQueue*>& encoder_queues, FilterContextStruct& filter_ctx) {
    FrameData frame_data;
    while (decode_queue.pop(frame_data)) {
        auto filter_start = std::chrono::steady_clock::now();
        // Push frame to filter graph
        int ret = av_buffersrc_add_frame_flags(filter_ctx.buffersrc_ctx, frame_data.frame, AV_BUFFERSRC_FLAG_KEEP_REF);
        if (ret < 0) {
            std::cerr << "Error while feeding the filter graph: " << get_error_text(ret) << std::endl;
            av_frame_free(&frame_data.frame);
            continue;
        }

        av_frame_free(&frame_data.frame);

        // Pull filtered frames from filter graph
        while (true) {
            AVFrame* filt_frame = av_frame_alloc();
            if (!filt_frame) {
                std::cerr << "Could not allocate filtered frame." << std::endl;
                break;
            }

            ret = av_buffersink_get_frame(filter_ctx.buffersink_ctx, filt_frame);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                av_frame_free(&filt_frame);
                break;
            }
            if (ret < 0) {
                std::cerr << "Error while getting filtered frame: " << get_error_text(ret) << std::endl;
                av_frame_free(&filt_frame);
                break;
            }
            auto filter_end = std::chrono::steady_clock::now();

            // Distribute the filtered frame to all encoder queues
            FrameData filt_frame_data;
            filt_frame_data.frame = filt_frame;
            filt_frame_data.decode_start_time = frame_data.decode_start_time;
            filt_frame_data.decode_end_time = frame_data.decode_end_time;
            filt_frame_data.filter_start_time = filter_start;
            filt_frame_data.filter_end_time = filter_end;
            if (frame_count % 100 == 0) {
                std::cout << "Filter " << frame_count << ", queue length: " << decode_queue.size() << std::endl;
            }
            frame_count++;
            // Timing information can be updated here if needed
            for (auto& q : encoder_queues) {
                q->push(filt_frame_data);
            }
        }
    }

    // Flush filter graph
    int ret = av_buffersrc_add_frame_flags(filter_ctx.buffersrc_ctx, nullptr, AV_BUFFERSRC_FLAG_KEEP_REF);
    if (ret < 0) {
        std::cerr << "Error while flushing the filter graph: " << get_error_text(ret) << std::endl;
    }

    while (true) {
        AVFrame* filt_frame = av_frame_alloc();
        if (!filt_frame) {
            std::cerr << "Could not allocate filtered frame during flush." << std::endl;
            break;
        }

        ret = av_buffersink_get_frame(filter_ctx.buffersink_ctx, filt_frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            av_frame_free(&filt_frame);
            break;
        }
        if (ret < 0) {
            std::cerr << "Error while getting filtered frame during flush: " << get_error_text(ret) << std::endl;
            av_frame_free(&filt_frame);
            break;
        }

        // Distribute the filtered frame to all encoder queues
        FrameData filt_frame_data;
        filt_frame_data.frame = filt_frame;
        for (auto& q : encoder_queues) {
            q->push(filt_frame_data);
        }
    }

    // Notify all encoder queues that no more frames will come
    for (auto& q : encoder_queues) {
        q->set_finished();
    }
}

// Encoder Thread Function
bool encode_frames(const EncoderInfo& encoder_info, FrameQueue& frame_queue, AVRational input_time_base, std::atomic<bool>& encode_finished) {
    AVFormatContext* output_fmt_ctx = encoder_info.output_fmt_ctx;
    AVCodecContext* encoder_ctx = encoder_info.encoder_ctx;
    AVStream* out_stream = encoder_info.out_stream;
    SwsContext* sws_ctx = encoder_info.sws_ctx;
    AVFrame* enc_frame = encoder_info.enc_frame;
    TimingLogger* logger = encoder_info.logger;

    int frame_count = 0;

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
            std::cerr << "Could not convert frame for " << encoder_info.config.output_url << std::endl;
            av_frame_free(&frame_data.frame);
            continue;
        }

        // Set PTS
        enc_frame->pts = frame_data.frame->pts;

        // Send frame to encoder
        int ret = avcodec_send_frame(encoder_ctx, enc_frame);
        if (ret < 0) {
            std::cerr << "Error sending frame to encoder for " << encoder_info.config.output_url << ": " << get_error_text(ret) << std::endl;
            av_frame_free(&frame_data.frame);
            continue;
        }

        // Receive packets from encoder
        while (ret >= 0) {
            AVPacket* enc_pkt = av_packet_alloc();
            if (!enc_pkt) {
                std::cerr << "Could not allocate encoding packet for " << encoder_info.config.output_url << std::endl;
                break;
            }

            ret = avcodec_receive_packet(encoder_ctx, enc_pkt);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                av_packet_free(&enc_pkt);
                break;
            }
            else if (ret < 0) {
                std::cerr << "Error during encoding for " << encoder_info.config.output_url << ": " << get_error_text(ret) << std::endl;
                av_packet_free(&enc_pkt);
                break;
            }

            // Rescale packet timestamp
            av_packet_rescale_ts(enc_pkt, encoder_ctx->time_base, out_stream->time_base);
            enc_pkt->stream_index = out_stream->index;

            // Write packet to output
            int write_ret = av_interleaved_write_frame(output_fmt_ctx, enc_pkt);
            if (write_ret < 0) {
                std::cerr << "Error writing packet to output for " << encoder_info.config.output_url << ": " << get_error_text(write_ret) << std::endl;
                av_packet_free(&enc_pkt);
                break;
            }

            av_packet_free(&enc_pkt);
        }
        auto encode_end = std::chrono::steady_clock::now();

        // Calculate timings
        double decode_time = std::chrono::duration<double, std::milli>(frame_data.decode_end_time - frame_data.decode_start_time).count();
        double filter_time = std::chrono::duration<double, std::milli>(frame_data.filter_end_time - frame_data.filter_start_time).count();
        double encode_time = std::chrono::duration<double, std::milli>(encode_end - encode_start).count();
        double interval_time = std::chrono::duration<double, std::milli>(encode_end - frame_data.decode_start_time).count();

        frame_count++;
        logger->add_entry(frame_count, decode_time, filter_time, encode_time, interval_time);

        if (frame_count % 100 == 0) {
            std::cout << "Encoded " << frame_count << " frames for " << encoder_info.config.output_url << ", queue length: " << frame_queue.size() << std::endl;
        }

        av_frame_free(&frame_data.frame);
    }

    // Flush encoder
    avcodec_send_frame(encoder_ctx, nullptr);
    while (true) {
        AVPacket* enc_pkt = av_packet_alloc();
        if (!enc_pkt) {
            std::cerr << "Could not allocate encoding packet during flush for " << encoder_info.config.output_url << std::endl;
            break;
        }

        int ret = avcodec_receive_packet(encoder_ctx, enc_pkt);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            av_packet_free(&enc_pkt);
            break;
        }
        else if (ret < 0) {
            std::cerr << "Error during encoding flush for " << encoder_info.config.output_url << ": " << get_error_text(ret) << std::endl;
            av_packet_free(&enc_pkt);
            break;
        }

        // Rescale packet timestamp
        av_packet_rescale_ts(enc_pkt, encoder_ctx->time_base, out_stream->time_base);
        enc_pkt->stream_index = out_stream->index;

        // Write packet to output
        int write_ret = av_interleaved_write_frame(output_fmt_ctx, enc_pkt);
        if (write_ret < 0) {
            std::cerr << "Error writing flushed packet to output for " << encoder_info.config.output_url << ": " << get_error_text(write_ret) << std::endl;
            av_packet_free(&enc_pkt);
            break;
        }

        av_packet_free(&enc_pkt);
    }

    // Write trailer to finalize the stream
    av_write_trailer(output_fmt_ctx);

    // Write timing information to log file
    logger->write_to_file();

    // Clean up encoder resources
    av_frame_free(&enc_frame);
    sws_freeContext(sws_ctx);
    avcodec_free_context(&encoder_ctx);
    if (!(output_fmt_ctx->oformat->flags & AVFMT_NOFILE))
        avio_closep(&output_fmt_ctx->pb);
    avformat_free_context(output_fmt_ctx);
    delete logger;

    encode_finished = true;
    std::cout << "Encoding finished for " << encoder_info.config.output_url << "." << std::endl;
    return true;
}

// Main Function
int main(int argc, char* argv[]) {
    // Expecting at least 2 arguments: program, input_url, and at least 1 output_url
    // Maximum of 6 output URLs supported
    if (argc < 3) {
        std::cerr << "Usage: " << argv[0]
                  << " <input_tcp_url> <output1_tcp_url> [<output2_tcp_url> ... <output6_tcp_url>]"
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
        {3840, 2160, 16000, "frame-1"},
        {2560, 1440, 10000, "frame-2"},
        {1920, 1080, 5000,  "frame-3"},
        {1280, 720,  2500,  "frame-4"},
        {854,  480,  1000,  "frame-5"},
        {640,  360,  600,   "frame-6"}
    };

    if (num_outputs > (int)resolution_bitrate_log.size()) {
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
        config.log_filename = "/home/zx/edge-use-case/smart-stadium-transcoding/result/video-processing-tcp/task" + std::to_string(num_outputs) + "/" + get_timestamp_with_ms() + "/"
                              + resolution_bitrate_log[i].log_filename + ".log";
        config.framerate = decoder_info.input_framerate;
        encoder_configs.push_back(config);
    }

    // Initialize encoders
    std::vector<EncoderInfo> encoder_infos;
    for (size_t i = 0; i < encoder_configs.size(); ++i) {
        EncoderInfo encoder_info;
        if (!initialize_encoder(encoder_configs[i], encoder_info, decoder_info.input_time_base)) {
            std::cerr << "Failed to initialize encoder for " << encoder_configs[i].output_url << std::endl;
            // Handle error: cleanup and exit or skip this encoder
            continue;
        }
        encoder_infos.push_back(encoder_info);
    }

    if (encoder_infos.empty()) {
        std::cerr << "No encoders initialized successfully." << std::endl;
        avformat_close_input(&decoder_info.input_fmt_ctx);
        avcodec_free_context(&decoder_info.decoder_ctx);
        avformat_network_deinit();
        return 1;
    }

    // Prepare queues
    PacketQueue packet_queue;
    FrameQueue decode_queue;
    std::vector<FrameQueue*> encoder_queues;
    for (size_t i = 0; i < encoder_infos.size(); ++i) {
        encoder_queues.push_back(new FrameQueue());
    }

    // Initialize filter graph
    FilterContextStruct filter_ctx;
    if (!init_filter_graph(filter_ctx, decoder_info.decoder_ctx, decoder_info)) {
        std::cerr << "Failed to initialize filter graph." << std::endl;
        // Handle error: cleanup and exit
        for (auto& q : encoder_queues) delete q;
        avformat_close_input(&decoder_info.input_fmt_ctx);
        avcodec_free_context(&decoder_info.decoder_ctx);
        avformat_network_deinit();
        return 1;
    }

    // Start decoder thread
    std::thread reader_thread(packet_reading_thread, decoder_info.input_fmt_ctx, decoder_info.video_stream_idx, std::ref(packet_queue));
    std::thread decoder_thread_obj(decoding_thread, decoder_info.decoder_ctx, std::ref(packet_queue), std::ref(decode_queue));

    // Start filter thread
    std::thread filter_thread(filter_thread_func, std::ref(decode_queue), std::ref(encoder_queues), std::ref(filter_ctx));

    // Start encoder threads
    std::vector<std::thread> encoder_threads;
    std::vector<std::atomic<bool>> enc_finished_flags(encoder_infos.size());
    for (size_t i = 0; i < encoder_infos.size(); ++i) {
        enc_finished_flags[i] = false;
        encoder_threads.emplace_back([&, i]() {
            if (!encode_frames(encoder_infos[i], *encoder_queues[i], decoder_info.input_time_base, enc_finished_flags[i])) {
                std::cerr << "Encoding failed for " << encoder_infos[i].config.output_url << std::endl;
            }
        });
    }

    // Wait for threads to finish
    reader_thread.join();
    decoder_thread_obj.join();
    filter_thread.join();
    for (auto& t : encoder_threads) {
        t.join();
    }

    // Clean up frame queues
    for (auto& q : encoder_queues) {
        delete q;
    }

    // Free filter graph
    avfilter_graph_free(&filter_ctx.graph);

    // Clean up decoder
    avcodec_free_context(&decoder_info.decoder_ctx);
    avformat_close_input(&decoder_info.input_fmt_ctx);

    // Clean up FFmpeg
    avformat_network_deinit();

    // Check if all encoders finished successfully
    bool all_success = true;
    for (size_t i = 0; i < enc_finished_flags.size(); ++i) {
        if (!enc_finished_flags[i]) {
            all_success = false;
            std::cerr << "Encoder for " << encoder_infos[i].config.output_url << " did not finish successfully." << std::endl;
        }
    }

    if (all_success) {
        std::cout << "Transcoding completed successfully for all resolutions." << std::endl;
    }
    else {
        std::cerr << "Transcoding encountered errors." << std::endl;
    }

    return 0;
}