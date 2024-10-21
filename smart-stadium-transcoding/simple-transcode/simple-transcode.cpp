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

std::string get_tcpdump_timestamp() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct tm *tm_info;
    tm_info = localtime(&tv.tv_sec);
    char time_buffer[9]; // "HH:MM:SS" + null
    strftime(time_buffer, sizeof(time_buffer), "%H:%M:%S", tm_info);
    long microseconds = tv.tv_usec;
    std::ostringstream oss;
    oss << time_buffer << "." << std::setfill('0') << std::setw(6) << microseconds;
    return oss.str();
}

// Structure to hold frame data and timing information
struct FrameData {
    AVFrame* frame;
    std::chrono::steady_clock::time_point decode_start_time;
    std::chrono::steady_clock::time_point decode_end_time;
    std::chrono::steady_clock::time_point encode_start_time;
    std::chrono::steady_clock::time_point encode_end_time;
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

// Thread-safe vector to store timing information
class TimingLogger {
public:
    void add_entry(int frame_number, double decode_time_ms, double encode_time_ms, double interval_ms) {
        std::lock_guard<std::mutex> lock(mutex_);
        log_entries.emplace_back(frame_number, decode_time_ms, encode_time_ms, interval_ms);
    }

    void write_to_file(const std::string& filename) {
        std::lock_guard<std::mutex> lock(mutex_);
        std::ofstream ofs(filename);
        if (!ofs.is_open()) {
            std::cerr << "Failed to open " << filename << " for writing." << std::endl;
            return;
        }
        ofs << std::fixed << std::setprecision(4);

        // Write header

        // Write each entry
        ofs << std::left << std::setw(10) << "Frame" 
            << std::left << std::setw(20) << "Decode Time (ms)" 
            << std::left << std::setw(20) << "Encode Time (ms)" 
            << std::left << std::setw(20) << "Transcode Time (ms)" 
            << "\n";

        for (const auto& entry : log_entries) {
            ofs << std::left << std::setw(10) << entry.frame_number
                << std::left << std::setw(20) << entry.decode_time_ms
                << std::left << std::setw(20) << entry.encode_time_ms
                << std::left << std::setw(20) << entry.interval_ms
                << "\n";
        }

        ofs.close();
        std::cout << "Timing information written to " << filename << std::endl;
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
};

// Structure to hold decoder information
struct DecoderInfo {
    AVFormatContext* input_fmt_ctx;
    AVCodecContext* decoder_ctx;
    int video_stream_idx;
    AVRational input_time_base;
};

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

    // Open decoder
    if (avcodec_open2(decoder_info.decoder_ctx, decoder, nullptr) < 0) {
        std::cerr << "Could not open decoder" << std::endl;
        avcodec_free_context(&decoder_info.decoder_ctx);
        avformat_close_input(&decoder_info.input_fmt_ctx);
        return false;
    }
    decoder_info.decoder_ctx->thread_count = 4;
    decoder_info.decoder_ctx->thread_type = FF_THREAD_SLICE;
    // decoder_info.decoder_ctx->thread_type = FF_THREAD_FRAME;

    // Store input stream's time_base
    decoder_info.input_time_base = decoder_info.input_fmt_ctx->streams[decoder_info.video_stream_idx]->time_base;

    std::cout << "Decoder initialized successfully." << std::endl;
    return true;
}

// Decoder Function
bool decode_frames(DecoderInfo decoder_info, FrameQueue& frame_queue, std::atomic<bool>& decode_finished, TimingLogger& logger) {
    AVFormatContext* input_fmt_ctx = decoder_info.input_fmt_ctx;
    AVCodecContext* decoder_ctx = decoder_info.decoder_ctx;
    int video_stream_idx = decoder_info.video_stream_idx;

    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    if (!packet || !frame) {
        std::cerr << "Could not allocate packet or frame in decoder" << std::endl;
        if (packet) av_packet_free(&packet);
        if (frame) av_frame_free(&frame);
        avcodec_free_context(&decoder_ctx);
        avformat_close_input(&input_fmt_ctx);
        return false;
    }

    int64_t first_pts = AV_NOPTS_VALUE;
    int frame_count = 0;
    int64_t last_packet_pts = AV_NOPTS_VALUE; 
    bool should_set_decode_start = false;
    std::chrono::steady_clock::time_point decode_start;
    decoder_ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
    decoder_ctx->max_b_frames = 0;

    while (av_read_frame(input_fmt_ctx, packet) >= 0) {
        if (packet->stream_index == video_stream_idx) {
            bool is_new_frame = false;
            if (packet->pts != AV_NOPTS_VALUE) {
                if (packet->pts != last_packet_pts) {
                    is_new_frame = true;
                    last_packet_pts = packet->pts;
                }
            }
            else if (packet->flags & AV_PKT_FLAG_KEY) {
                is_new_frame = true;
            }
            if (is_new_frame) {
                decode_start = std::chrono::steady_clock::now();
                should_set_decode_start = true;
            }
            int ret = avcodec_send_packet(decoder_ctx, packet);
            if (ret < 0) {
                std::cerr << "Error sending packet to decoder: " << get_error_text(ret) << std::endl;
                av_packet_unref(packet);
                break;
            }

            while (ret >= 0) {
                ret = avcodec_receive_frame(decoder_ctx, frame);
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                    break;
                }
                else if (ret < 0) {
                    std::cerr << "Error during decoding: " << get_error_text(ret) << std::endl;
                    break;
                }

                auto decode_end = std::chrono::steady_clock::now();

                if (first_pts == AV_NOPTS_VALUE) {
                    first_pts = frame->pts;
                    std::cout << "First frame PTS: " << first_pts << std::endl;
                }

                // Clone the frame and push to queue
                AVFrame* cloned_frame = av_frame_clone(frame);
                if (!cloned_frame) {
                    std::cerr << "Could not clone frame" << std::endl;
                    break;
                }

                // Adjust PTS to start from 0
                cloned_frame->pts -= first_pts;

                FrameData frame_data;
                frame_data.frame = cloned_frame;
                if (should_set_decode_start) {
                    frame_data.decode_start_time = decode_start;
                    should_set_decode_start = false;
                } else {
                    frame_data.decode_start_time = std::chrono::steady_clock::time_point();
                }
                frame_data.decode_end_time = decode_end;
                // encode_start_time and encode_end_time will be set in encoder

                frame_queue.push(frame_data);

                frame_count++;
                if (frame_count % 100 == 0) {
                    std::cout << "Decoded " << frame_count << " frames, current PTS: " << cloned_frame->pts << std::endl;
                }
            }
        }
        av_packet_unref(packet);
    }

    // Flush decoder
    avcodec_send_packet(decoder_ctx, nullptr);
    while (avcodec_receive_frame(decoder_ctx, frame) == 0) {
        AVFrame* cloned_frame = av_frame_clone(frame);
        if (cloned_frame) {
            FrameData frame_data;
            frame_data.frame = cloned_frame;
            // No packet received during flushing, so we set decode times to end of decoding
            frame_data.decode_start_time = std::chrono::steady_clock::now();
            frame_data.decode_end_time = frame_data.decode_start_time;
            frame_queue.push(frame_data);
        }
    }

    // Clean up decoder resources
    av_packet_free(&packet);
    av_frame_free(&frame);
    avcodec_free_context(&decoder_ctx);
    avformat_close_input(&input_fmt_ctx);
    decode_finished = true;
    frame_queue.set_finished();

    std::cout << "Decoding finished." << std::endl;
    return true;
}

// Encoder Function with Initialization and Scaling
bool encode_frames(const char* output_url, FrameQueue& frame_queue, std::atomic<bool>& encode_finished, AVRational input_time_base, TimingLogger& logger) {
    AVFormatContext* output_fmt_ctx = nullptr;
    AVStream* out_stream = nullptr;

    // Set SRT options with increased latency and specified packet size
    AVDictionary* format_opts = nullptr;
    av_dict_set(&format_opts, "latency", "0", 0);     // Latency in ms
    av_dict_set(&format_opts, "buffer_size", "1000000", 0);
    // av_dict_set(&format_opts, "maxbw", "100000000", 0);

    // Allocate output format context with MPEG-TS over SRT
    if (avformat_alloc_output_context2(&output_fmt_ctx, nullptr, "flv", output_url) < 0) {
        std::cerr << "Could not create output context" << std::endl;
        av_dict_free(&format_opts);
        return false;
    }

    // Find encoder for H.264
    AVCodec* encoder = avcodec_find_encoder(AV_CODEC_ID_H264);
    if (!encoder) {
        std::cerr << "Necessary encoder not found" << std::endl;
        avformat_free_context(output_fmt_ctx);
        av_dict_free(&format_opts);
        return false;
    }

    // Create new stream for encoder
    out_stream = avformat_new_stream(output_fmt_ctx, nullptr);
    if (!out_stream) {
        std::cerr << "Failed allocating output stream" << std::endl;
        avformat_free_context(output_fmt_ctx);
        av_dict_free(&format_opts);
        return false;
    }

    // Allocate and configure encoder context
    AVCodecContext* encoder_ctx = avcodec_alloc_context3(encoder);
    if (!encoder_ctx) {
        std::cerr << "Could not allocate encoder context" << std::endl;
        avformat_free_context(output_fmt_ctx);
        av_dict_free(&format_opts);
        return false;
    }

    // Set encoder parameters with reduced bitrate
    encoder_ctx->height = 1440;
    encoder_ctx->width = 2560;
    encoder_ctx->sample_aspect_ratio = AVRational{1, 1}; // Square pixels
    encoder_ctx->pix_fmt = AV_PIX_FMT_YUV420P;
    encoder_ctx->time_base = AVRational{1, 30};          // 30 fps
    encoder_ctx->framerate = AVRational{30, 1};
    encoder_ctx->bit_rate = 10000 * 1000;
    encoder_ctx->gop_size = 30;
    encoder_ctx->max_b_frames = 0;
    encoder_ctx->thread_count = 8;
    // encoder_ctx->thread_type = FF_THREAD_SLICE;

    // Set preset and tune options for low latency
    AVDictionary* codec_opts = nullptr;
    av_dict_set(&codec_opts, "preset", "ultrafast", 0);
    av_dict_set(&codec_opts, "tune", "zerolatency", 0);

    // Open encoder with codec options
    if (avcodec_open2(encoder_ctx, encoder, &codec_opts) < 0) {
        std::cerr << "Cannot open video encoder" << std::endl;
        avcodec_free_context(&encoder_ctx);
        avformat_free_context(output_fmt_ctx);
        av_dict_free(&codec_opts);
        av_dict_free(&format_opts);
        return false;
    }

    // Copy encoder parameters to output stream
    if (avcodec_parameters_from_context(out_stream->codecpar, encoder_ctx) < 0) {
        std::cerr << "Failed to copy encoder parameters to output stream" << std::endl;
        avcodec_free_context(&encoder_ctx);
        avformat_free_context(output_fmt_ctx);
        av_dict_free(&codec_opts);
        av_dict_free(&format_opts);
        return false;
    }

    out_stream->time_base = encoder_ctx->time_base;

    // Open output URL with format options
    if (!(output_fmt_ctx->oformat->flags & AVFMT_NOFILE)) {
        if (avio_open2(&output_fmt_ctx->pb, output_url, AVIO_FLAG_WRITE, nullptr, &format_opts) < 0) {
            std::cerr << "Could not open output URL: " << output_url << std::endl;
            avcodec_free_context(&encoder_ctx);
            avformat_free_context(output_fmt_ctx);
            av_dict_free(&codec_opts);
            av_dict_free(&format_opts);
            return false;
        }
    }

    // Write header to output
    if (avformat_write_header(output_fmt_ctx, &codec_opts) < 0) {
        std::cerr << "Error occurred when writing header to output" << std::endl;
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
        std::cerr << "Could not allocate encoding frame" << std::endl;
        avformat_free_context(output_fmt_ctx);
        avcodec_free_context(&encoder_ctx);
        return false;
    }
    enc_frame->format = encoder_ctx->pix_fmt;
    enc_frame->width  = encoder_ctx->width;
    enc_frame->height = encoder_ctx->height;

    if (av_frame_get_buffer(enc_frame, 32) < 0) {
        std::cerr << "Could not allocate the video frame data" << std::endl;
        av_frame_free(&enc_frame);
        avcodec_free_context(&encoder_ctx);
        avformat_free_context(output_fmt_ctx);
        return false;
    }

    int64_t first_pts = AV_NOPTS_VALUE;
    int frame_count = 0;

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
        frame_data.encode_start_time = encode_start;

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
                std::cerr << "Could not initialize the conversion context" << std::endl;
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
            std::cerr << "Could not convert frame" << std::endl;
            av_frame_free(&frame_data.frame);
            continue;
        }

        if (first_pts == AV_NOPTS_VALUE) {
            first_pts = frame_data.frame->pts;
            std::cout << "First encoded frame PTS: " << first_pts << std::endl;
        }

        // Set PTS based on frame counter
        enc_frame->pts = av_rescale_q(frame_data.frame->pts, input_time_base, encoder_ctx->time_base);

        // Send frame to encoder
        int ret = avcodec_send_frame(encoder_ctx, enc_frame);
        if (ret < 0) {
            std::cerr << "Error sending frame to encoder: " << get_error_text(ret) << std::endl;
            av_frame_free(&frame_data.frame);
            break;
        }

        // Receive packets from encoder
        while (ret >= 0) {
            AVPacket* enc_pkt = av_packet_alloc();
            if (!enc_pkt) {
                std::cerr << "Could not allocate encoding packet" << std::endl;
                break;
            }

            ret = avcodec_receive_packet(encoder_ctx, enc_pkt);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                av_packet_free(&enc_pkt);
                break;
            } else if (ret < 0) {
                std::cerr << "Error during encoding: " << get_error_text(ret) << std::endl;
                av_packet_free(&enc_pkt);
                break;
            }

            // Rescale packet timestamp
            av_packet_rescale_ts(enc_pkt, encoder_ctx->time_base, out_stream->time_base);
            enc_pkt->stream_index = out_stream->index;

            // Write packet to output
            int write_ret = av_interleaved_write_frame(output_fmt_ctx, enc_pkt);
            if (write_ret < 0) {
                std::cerr << "Error writing packet to output: " << get_error_text(write_ret) << std::endl;
                av_packet_free(&enc_pkt);
                break;
            }

            av_packet_free(&enc_pkt);
        }

        auto encode_end = std::chrono::steady_clock::now();
        frame_data.encode_end_time = encode_end;

        // Calculate timings
        double decode_time = std::chrono::duration<double, std::milli>(frame_data.decode_end_time - frame_data.decode_start_time).count();
        double encode_time = std::chrono::duration<double, std::milli>(frame_data.encode_end_time - frame_data.encode_start_time).count();
        double interval_time = std::chrono::duration<double, std::milli>(frame_data.encode_end_time - frame_data.decode_start_time).count();

        frame_count++;
        logger.add_entry(frame_count, decode_time, encode_time, interval_time);

        if (frame_count % 100 == 0) {
            std::cout << "Encoded " << frame_count << " frames, current PTS: " << enc_frame->pts << std::endl;
        }

        av_frame_free(&frame_data.frame);
    }
    std::cout << "Encoded " << frame_count << " frames, current PTS: " << enc_frame->pts << std::endl;

    // Flush encoder to ensure all frames are processed
    avcodec_send_frame(encoder_ctx, nullptr);
    while (true) {
        AVPacket* enc_pkt = av_packet_alloc();
        if (!enc_pkt) {
            std::cerr << "Could not allocate encoding packet during flush" << std::endl;
            break;
        }

        int ret = avcodec_receive_packet(encoder_ctx, enc_pkt);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            av_packet_free(&enc_pkt);
            break;
        } else if (ret < 0) {
            std::cerr << "Error during encoding flush: " << get_error_text(ret) << std::endl;
            av_packet_free(&enc_pkt);
            break;
        }

        // Rescale packet timestamp
        av_packet_rescale_ts(enc_pkt, encoder_ctx->time_base, out_stream->time_base);
        enc_pkt->stream_index = out_stream->index;

        // Write flushed packet to output
        int write_ret = av_interleaved_write_frame(output_fmt_ctx, enc_pkt);
        if (write_ret < 0) {
            std::cerr << "Error writing flushed packet to output: " << get_error_text(write_ret) << std::endl;
            av_packet_free(&enc_pkt);
            break;
        }

        av_packet_free(&enc_pkt);
    }

    // Write trailer to finalize the stream
    av_write_trailer(output_fmt_ctx);

    // Clean up encoder resources
    av_frame_free(&enc_frame);
    sws_freeContext(sws_ctx);
    avcodec_free_context(&encoder_ctx);
    if (!(output_fmt_ctx->oformat->flags & AVFMT_NOFILE))
        avio_closep(&output_fmt_ctx->pb);
    avformat_free_context(output_fmt_ctx);

    encode_finished = true;
    std::cout << "Encoding finished." << std::endl;
    return true;
}

int main(int argc, char* argv[]) {
    if (argc != 3) {
        std::cerr << "Usage: " << argv[0] << " <input_srt_url> <output_srt_url>" << std::endl;
        return 1;
    }

    const char* input_url = argv[1];
    const char* output_url = argv[2];

    // Set FFmpeg log level to show more information (optional, useful for debugging)
    // av_log_set_level(AV_LOG_DEBUG); // Change to AV_LOG_INFO for less verbosity

    // Initialize FFmpeg
    avformat_network_init();

    // Prepare frame queue
    FrameQueue frame_queue;

    // Initialize TimingLogger
    TimingLogger logger;

    std::atomic<bool> decode_finished(false);
    std::atomic<bool> encode_finished(false);

    // Initialize decoder
    DecoderInfo decoder_info;
    if (!initialize_decoder(input_url, decoder_info)) {
        std::cerr << "Decoder initialization failed" << std::endl;
        avformat_network_deinit();
        return 1;
    }

    // Start decoder thread
    std::thread decoder_thread([&]() {
        if (!decode_frames(decoder_info, frame_queue, decode_finished, logger)) {
            std::cerr << "Decoding failed" << std::endl;
        }
    });

    // Start encoder thread, passing input_time_base
    std::thread encoder_thread_main([&]() {
        if (!encode_frames(output_url, frame_queue, encode_finished, decoder_info.input_time_base, logger)) {
            std::cerr << "Encoding failed" << std::endl;
        }
    });

    // Wait for threads to finish
    decoder_thread.join();
    encoder_thread_main.join();

    // Clean up FFmpeg
    avformat_network_deinit();

    // Write timing information to frame.log
    logger.write_to_file("frame.log");

    if (encode_finished) {
        std::cout << "Transcoding completed successfully." << std::endl;
    } else {
        std::cerr << "Transcoding encountered errors." << std::endl;
    }

    return 0;
}