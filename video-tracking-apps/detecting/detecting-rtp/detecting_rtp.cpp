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
#include <zmq.hpp>
#include <vector>
#include <memory>
#include <fstream>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <semaphore.h>
// Added for TCP server
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <cstring>
#include <algorithm>
#include <netinet/tcp.h>  // Add this for TCP_NODELAY

// FFmpeg includes
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/intreadwrite.h>
#include <libavutil/avutil.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

// Struct to hold YOLO detection result from shared memory
struct YoloDetectionResult {
    int frame_num;
    int64_t timestamp_ms;
    int num_detections;
    
    // New fields for storing detection data
    struct Detection {
        float x1, y1, x2, y2;  // Bounding box coordinates
        float confidence;      // Confidence score
        int class_id;          // Class ID
    };
    
    std::vector<Detection> detections;  // Vector to store all detections
    std::string json_data;     // Keep for backward compatibility
};

// Thread-safe queue for YOLO detection results
class YoloResultQueue {
public:
    void push(const YoloDetectionResult& result) {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push(result);
        cond_var_.notify_one();
    }

    bool pop(YoloDetectionResult& result) {
        std::unique_lock<std::mutex> lock(mutex_);
        while (queue_.empty() && !finished_) {
            cond_var_.wait(lock);
        }
        if (queue_.empty()) return false;
        result = queue_.front();
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
    std::queue<YoloDetectionResult> queue_;
    std::mutex mutex_;
    std::condition_variable cond_var_;
    bool finished_ = false;
};

// Global result queue shared between threads
YoloResultQueue yolo_result_queue;
std::atomic<bool> yolo_result_thread_running(false);
std::atomic<bool> tcp_server_running(false);

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

// Decoder Initialization and Function
bool initialize_decoder(const char* input_url, DecoderInfo& decoder_info) {
    // Initialize input format context
    decoder_info.input_fmt_ctx = nullptr;
    AVDictionary* format_opts = nullptr;
    // av_dict_set(&format_opts, "fflags",          "nobuffer", 0);
    av_dict_set(&format_opts, "probesize",       "327680",    0);
    av_dict_set(&format_opts, "analyzeduration", "0",        0);  
    av_dict_set(&format_opts, "tcp_nodelay", "1", 0);
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

int64_t extract_timestamp(AVPacket *packet) {
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
    
    int64_t timestamp;
    memcpy(&timestamp, data + sei_offset, sizeof(int64_t));
    
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
        int64_t timestamp = extract_timestamp(packet);
        printf("timestamp: %ld\n", timestamp);
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

void decoding_thread(AVCodecContext* decoder_ctx, PacketQueue& packet_queue, FrameQueue* frame_queue) {
    AVPacket* packet = nullptr;
    AVFrame* frame = av_frame_alloc();
    if (!frame) {
        std::cerr << "Could not allocate AVFrame" << std::endl;
        frame_queue->set_finished();
        return;
    }
    uint32_t frame_number = 0;

    while (packet_queue.pop(packet)) {
        auto decode_start = std::chrono::steady_clock::now();
        int ret = avcodec_send_packet(decoder_ctx, packet);
        // std::cout << "Frame number: " << frame_number << " "
        //           << "Decode start: " << std::chrono::duration_cast<std::chrono::milliseconds>(decode_start.time_since_epoch()).count() << std::endl;
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
            frame_number++;
            if (frame_number % 100 == 0) {
                std::cout << "Decoded frame: " << frame_number << " Time used:" << std::chrono::duration_cast<std::chrono::milliseconds>(decode_end - decode_start).count() << "ms" << std::endl;
            }

            AVFrame* cloned_frame = av_frame_clone(frame);
            if (!cloned_frame) {
                std::cerr << "Could not clone frame" << std::endl;
                continue;
            }
            FrameData frame_data;
            frame_data.frame = cloned_frame;
            frame_data.decode_start_time = decode_start;
            frame_data.decode_end_time = decode_end;
            frame_queue->push(frame_data);
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

        AVFrame* cloned_frame = av_frame_clone(frame);
        if (!cloned_frame) {
            std::cerr << "Could not clone frame" << std::endl;
            continue;
        }
        FrameData frame_data;
        frame_data.frame = cloned_frame;
        frame_data.decode_start_time = std::chrono::steady_clock::now();;
        frame_data.decode_end_time = std::chrono::steady_clock::now();;
        frame_queue->push(frame_data);
        av_frame_unref(frame);
    }

    frame_queue->set_finished();
    av_frame_free(&frame);
}

bool decode_frames(DecoderInfo decoder_info, FrameQueue* frame_queue, std::atomic<bool>& decode_finished) {
    PacketQueue packet_queue;

    std::thread reader(packet_reading_thread, decoder_info.input_fmt_ctx, decoder_info.video_stream_idx, std::ref(packet_queue));
    std::thread decoder(decoding_thread, decoder_info.decoder_ctx, std::ref(packet_queue), std::ref(frame_queue));

    reader.join();
    decoder.join();

    decode_finished = true;
    std::cout << "Decoding finished." << std::endl;
    return true;
}

void process_frames_for_yolo_shm(FrameQueue* frame_queue) {
    // Define shared memory parameters
    const std::string SHM_NAME = "/yolo_frame_buffer";
    const std::string SEM_READY_NAME = "/frame_ready";
    const std::string SEM_PROCESSED_NAME = "/frame_processed";
    const size_t MAX_FRAME_SIZE = 4096 * 2160 * 3; // Max frame size (4K resolution)
    const size_t METADATA_SIZE = 256; // Space for metadata
    const size_t BUFFER_SIZE = MAX_FRAME_SIZE + METADATA_SIZE;
    
    // Create shared memory
    int shm_fd = shm_open(SHM_NAME.c_str(), O_CREAT | O_RDWR, 0666);
    if (shm_fd == -1) {
        std::cerr << "Error creating shared memory: " << strerror(errno) << std::endl;
        return;
    }
    
    // Set the size of shared memory
    if (ftruncate(shm_fd, BUFFER_SIZE) == -1) {
        std::cerr << "Error setting shared memory size: " << strerror(errno) << std::endl;
        close(shm_fd);
        shm_unlink(SHM_NAME.c_str());
        return;
    }
    
    // Map shared memory to process address space
    void* shm_ptr = mmap(NULL, BUFFER_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (shm_ptr == MAP_FAILED) {
        std::cerr << "Error mapping shared memory: " << strerror(errno) << std::endl;
        close(shm_fd);
        shm_unlink(SHM_NAME.c_str());
        return;
    }
    
    // Create semaphores for synchronization
    sem_t* sem_ready = sem_open(SEM_READY_NAME.c_str(), O_CREAT, 0666, 0);
    sem_t* sem_processed = sem_open(SEM_PROCESSED_NAME.c_str(), O_CREAT, 0666, 1);
    
    if (sem_ready == SEM_FAILED || sem_processed == SEM_FAILED) {
        std::cerr << "Error creating semaphores: " << strerror(errno) << std::endl;
        munmap(shm_ptr, BUFFER_SIZE);
        close(shm_fd);
        shm_unlink(SHM_NAME.c_str());
        return;
    }
    
    std::cout << "Shared memory and semaphores initialized" << std::endl;
    std::cout << "Frame processing started, waiting for consumer..." << std::endl;

    // Initialize hardware context for FFmpeg
    AVBufferRef* hw_device_ctx = NULL;
    bool hw_accel_available = false;
    
    int err = av_hwdevice_ctx_create(&hw_device_ctx, AV_HWDEVICE_TYPE_CUDA, NULL, NULL, 0);
    if (err >= 0) {
        hw_accel_available = true;
        std::cout << "CUDA hardware acceleration available for FFmpeg conversions" << std::endl;
    } else {
        std::cout << "CUDA hardware acceleration not available, using software conversion: " 
                 << get_error_text(err) << std::endl;
    }

    // Performance tracking
    int frame_counter = 0;
    auto perf_start_time = std::chrono::steady_clock::now();
    int frames_in_this_batch = 0;
    
    // Create a reusable scaling context
    SwsContext* sws_ctx = NULL;
    int prev_width = 0, prev_height = 0;
    AVPixelFormat prev_format = AV_PIX_FMT_NONE;
    
    // Pre-allocate a BGR frame to reuse
    AVFrame* bgr_frame = av_frame_alloc();
    if (!bgr_frame) {
        std::cerr << "Could not allocate BGR frame" << std::endl;
        return;
    }
    
    // Setup pointers to shared memory regions
    uint8_t* metadata_ptr = (uint8_t*)shm_ptr;
    uint8_t* frame_data_ptr = metadata_ptr + METADATA_SIZE;
    
    FrameData frame_data;
    
    while (frame_queue->pop(frame_data)) {
        // Wait for consumer to signal it's done with previous frame
        sem_wait(sem_processed);
        
        auto format_start = std::chrono::steady_clock::now();
        AVFrame* frame = frame_data.frame;
        
        // Set basic frame properties
        bgr_frame->width = frame->width;
        bgr_frame->height = frame->height;
        bgr_frame->format = AV_PIX_FMT_BGR24;
        
        // Write metadata to shared memory
        std::string metadata = std::to_string(frame->width) + "," +
                             std::to_string(frame->height) + "," +
                             std::to_string(frame_counter++);
        
        // Ensure we don't overflow metadata area
        if (metadata.size() >= METADATA_SIZE - 1) {
            metadata.resize(METADATA_SIZE - 1);
        }
        
        // Copy metadata to shared memory and null-terminate
        std::copy(metadata.begin(), metadata.end(), metadata_ptr);
        metadata_ptr[metadata.size()] = '\0';
        
        // If frame dimensions or format changed, recreate scaling context
        if (!sws_ctx || 
            prev_width != frame->width || 
            prev_height != frame->height || 
            prev_format != frame->format) {
            
            // Free old context if it exists
            if (sws_ctx) sws_freeContext(sws_ctx);
            
            // Create new scaling context with hardware acceleration if available
            int flags = SWS_BILINEAR;
            
            if (hw_accel_available) {
                // Hardware acceleration is used internally by FFmpeg
                std::cout << "Setting up hardware-accelerated scaling context for "
                         << frame->width << "x" << frame->height << " frame" << std::endl;
            }
            
            sws_ctx = sws_getContext(
                frame->width, frame->height, (AVPixelFormat)frame->format,
                frame->width, frame->height, AV_PIX_FMT_BGR24,
                flags, NULL, NULL, NULL);
                
            if (!sws_ctx) {
                std::cerr << "Could not initialize conversion context" << std::endl;
                av_frame_free(&frame);
                sem_post(sem_processed);  // Signal we're done even though we failed
                continue;
            }
            
            // Save current dimensions and format
            prev_width = frame->width;
            prev_height = frame->height;
            prev_format = (AVPixelFormat)frame->format;
        }
        
        // Setup BGR frame to use shared memory for output
        bgr_frame->data[0] = frame_data_ptr;
        bgr_frame->linesize[0] = frame->width * 3;
        
        // Perform the conversion directly to shared memory buffer
        int result = sws_scale(sws_ctx, frame->data, frame->linesize, 0, frame->height,
                              bgr_frame->data, bgr_frame->linesize);
        
        if (result <= 0) {
            std::cerr << "Error during frame conversion" << std::endl;
            av_frame_free(&frame);
            sem_post(sem_processed);  // Signal we're done even though we failed
            continue;
        }
        
        auto format_end = std::chrono::steady_clock::now();
        auto format_time = std::chrono::duration_cast<std::chrono::milliseconds>(format_end - format_start).count();
        // std::cout << "Format end: " << std::chrono::duration_cast<std::chrono::milliseconds>(format_end.time_since_epoch()).count() << std::endl;
        
        frames_in_this_batch++;
        
        // Periodically report performance
        if (frame_counter % 100 == 0) {
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - perf_start_time).count();
            double fps = (elapsed > 0) ? (1000.0 * frames_in_this_batch / elapsed) : 0;
            
            std::cout << "Frame " << frame_counter 
                     << " | Format conversion time: " << format_time << "ms"
                     << " | Avg FPS: " << std::fixed << std::setprecision(1) << fps 
                     << (hw_accel_available ? " (HW accelerated)" : " (SW)") << std::endl;
                     
            // Reset batch counters
            perf_start_time = now;
            frames_in_this_batch = 0;
        }
        
        // Clean up frame
        av_frame_free(&frame);
        
        // Signal to consumer that new frame is ready
        sem_post(sem_ready);
    }
    
    // Clean up scaling context
    if (sws_ctx) sws_freeContext(sws_ctx);
    
    // Clean up hardware context
    if (hw_device_ctx) av_buffer_unref(&hw_device_ctx);
    
    // Clean up shared memory and semaphores
    av_frame_free(&bgr_frame);
    munmap(shm_ptr, BUFFER_SIZE);
    close(shm_fd);
    shm_unlink(SHM_NAME.c_str());
    sem_close(sem_ready);
    sem_close(sem_processed);
    sem_unlink(SEM_READY_NAME.c_str());
    sem_unlink(SEM_PROCESSED_NAME.c_str());
    
    std::cout << "Frame processing finished" << std::endl;
}

// Function to read YOLO detection results from shared memory
void yolo_result_reader_thread() {
    std::cout << "Starting YOLO result reader thread..." << std::endl;
    
    // Define shared memory parameters for results
    const std::string RESULT_SHM_NAME = "/yolo_result_buffer";
    const std::string RESULT_SEM_READY_NAME = "/result_ready";
    const std::string RESULT_SEM_PROCESSED_NAME = "/result_processed";
    const size_t RESULT_SIZE = 8192;  // Increased size to accommodate detection results
    
    // First, try to unlink any existing shared memory and semaphores (in case of improper shutdown)
    shm_unlink(RESULT_SHM_NAME.c_str());
    sem_unlink(RESULT_SEM_READY_NAME.c_str());
    sem_unlink(RESULT_SEM_PROCESSED_NAME.c_str());
    
    // Create shared memory
    int shm_fd = shm_open(RESULT_SHM_NAME.c_str(), O_CREAT | O_RDWR, 0666);
    if (shm_fd == -1) {
        std::cerr << "Error creating result shared memory: " << strerror(errno) << std::endl;
        return;
    }
    
    // Set the size of the shared memory
    if (ftruncate(shm_fd, RESULT_SIZE) == -1) {
        std::cerr << "Error setting size for result shared memory: " << strerror(errno) << std::endl;
        close(shm_fd);
        shm_unlink(RESULT_SHM_NAME.c_str());
        return;
    }
    
    // Map shared memory
    void* shm_ptr = mmap(NULL, RESULT_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (shm_ptr == MAP_FAILED) {
        std::cerr << "Error mapping result shared memory: " << strerror(errno) << std::endl;
        close(shm_fd);
        shm_unlink(RESULT_SHM_NAME.c_str());
        return;
    }
    
    // Initialize memory with zeros
    memset(shm_ptr, 0, RESULT_SIZE);
    
    // Create semaphores
    sem_t* sem_ready = sem_open(RESULT_SEM_READY_NAME.c_str(), O_CREAT, 0666, 0);
    sem_t* sem_processed = sem_open(RESULT_SEM_PROCESSED_NAME.c_str(), O_CREAT, 0666, 1);
    
    if (sem_ready == SEM_FAILED || sem_processed == SEM_FAILED) {
        std::cerr << "Error creating result semaphores: " << strerror(errno) << std::endl;
        munmap(shm_ptr, RESULT_SIZE);
        close(shm_fd);
        shm_unlink(RESULT_SHM_NAME.c_str());
        sem_unlink(RESULT_SEM_READY_NAME.c_str());
        sem_unlink(RESULT_SEM_PROCESSED_NAME.c_str());
        return;
    }
    
    std::cout << "Created YOLO result shared memory and semaphores" << std::endl;
    
    int frame_count = 0;
    
    // Set flag to indicate thread is running
    yolo_result_thread_running = true;
    
    while (yolo_result_thread_running) {
        // Wait for new result to be available using sem_timedwait
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += 1; // 1 second timeout
        
        // Use sem_timedwait to allow for graceful shutdown
        int ret = sem_timedwait(sem_ready, &ts);
        if (ret == -1) {
            if (errno == ETIMEDOUT) {
                // Timeout occurred, check if we should exit
                if (!yolo_result_thread_running) {
                    break;
                }
                continue;
            } else {
                std::cerr << "Error waiting for result semaphore: " << strerror(errno) << std::endl;
                break;
            }
        }
        
        // Read frame number and number of detections from shared memory
        char* data_ptr = static_cast<char*>(shm_ptr);
        int* int_ptr = reinterpret_cast<int*>(data_ptr);
        
        int frame_num = int_ptr[0];  // First int is frame number
        int num_detections = int_ptr[1];  // Second int is number of detections
        
        // Create result object
        YoloDetectionResult result;
        result.frame_num = frame_num;
        result.timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        result.num_detections = num_detections;
        
        // Read detection data
        if (num_detections > 0) {
            // Position pointer after the header (2 ints)
            const float* float_ptr = reinterpret_cast<const float*>(int_ptr + 2);
            
            // Reserve space for all detections
            result.detections.reserve(num_detections);
            
            // For each detection, read 4 floats for bbox, 1 float for confidence, and 1 int for class_id
            for (int i = 0; i < num_detections; i++) {
                YoloDetectionResult::Detection det;
                
                // Read bounding box coordinates (4 floats)
                det.x1 = float_ptr[0];
                det.y1 = float_ptr[1];
                det.x2 = float_ptr[2];
                det.y2 = float_ptr[3];
                
                // Read confidence (1 float)
                det.confidence = float_ptr[4];
                
                // Read class ID (1 int)
                det.class_id = *reinterpret_cast<const int*>(float_ptr + 5);
                
                // Add to results
                result.detections.push_back(det);
                
                // Move pointer to next detection (5 floats + 1 int = 6 float-sized elements)
                float_ptr += 6;
            }
        }
        
        // Add to queue for TCP transmission
        yolo_result_queue.push(result);
        
        frame_count++;
        
        // Signal that we've processed the result
        sem_post(sem_processed);
    }
    
    // Clean up
    sem_close(sem_ready);
    sem_close(sem_processed);
    munmap(shm_ptr, RESULT_SIZE);
    close(shm_fd);
    
    // Note: We don't unlink the shared memory and semaphores here to allow for proper shutdown
    // They will be unlinked on the next startup
    
    std::cout << "YOLO result reader thread finished" << std::endl;
}

// Function to start a TCP server to send YOLO results to streaming component
void tcp_result_server_thread(int port) {
    std::cout << "Starting TCP result server on port " << port << "..." << std::endl;
    
    // Create socket
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        std::cerr << "Error creating socket: " << strerror(errno) << std::endl;
        return;
    }
    
    // Set socket options to allow address reuse
    int opt = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        std::cerr << "Error setting socket options: " << strerror(errno) << std::endl;
        close(server_fd);
        return;
    }
    
    // Prepare server address
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(port);
    
    // Bind socket
    if (bind(server_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        std::cerr << "Error binding socket: " << strerror(errno) << std::endl;
        close(server_fd);
        return;
    }
    
    // Listen for connections
    if (listen(server_fd, 5) < 0) {
        std::cerr << "Error listening on socket: " << strerror(errno) << std::endl;
        close(server_fd);
        return;
    }
    
    std::cout << "TCP result server listening on port " << port << std::endl;
    
    // Set flag to indicate server is running
    tcp_server_running = true;
    
    // Set socket to non-blocking mode
    int flags = fcntl(server_fd, F_GETFL, 0);
    fcntl(server_fd, F_SETFL, flags | O_NONBLOCK);
    
    int client_fd = -1;
    bool client_connected = false;
    
    while (tcp_server_running) {
        if (!client_connected) {
            // Accept new connection (non-blocking)
            struct sockaddr_in client_addr;
            socklen_t client_addr_len = sizeof(client_addr);
            client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &client_addr_len);
            
            if (client_fd >= 0) {
                // Connection accepted
                client_connected = true;
                char client_ip[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip));
                std::cout << "Client connected from " << client_ip << ":" << ntohs(client_addr.sin_port) << std::endl;
                
                // Set TCP_NODELAY option to disable Nagle's algorithm
                int flag = 1;
                if (setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, (char*)&flag, sizeof(int)) < 0) {
                    std::cerr << "Warning: Failed to set TCP_NODELAY option: " << strerror(errno) << std::endl;
                } else {
                    std::cout << "TCP_NODELAY option set for client socket" << std::endl;
                }
            } else {
                // No connection or error
                if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    std::cerr << "Error accepting connection: " << strerror(errno) << std::endl;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(100)); // Sleep to avoid busy waiting
                continue;
            }
        }
        
        // Process and send YOLO results from queue
        if (client_connected) {
            YoloDetectionResult result;
            bool got_result = false;
            
            // Try to get a result with a timeout
            {
                std::mutex mtx;
                std::unique_lock<std::mutex> lock(mtx);
                std::condition_variable cv;
                std::atomic<bool> data_ready(false);
                
                // Start a thread to check the queue
                std::thread checker([&]() {
                    got_result = yolo_result_queue.pop(result);
                    data_ready = true;
                    cv.notify_one();
                });
                checker.detach();
                
                // Wait with timeout
                if (!cv.wait_for(lock, std::chrono::seconds(1), [&]() { return data_ready.load(); })) {
                    // Timeout occurred, check if client is still connected
                    int error = 0;
                    socklen_t len = sizeof(error);
                    int ret = getsockopt(client_fd, SOL_SOCKET, SO_ERROR, &error, &len);
                    
                    if (ret < 0 || error != 0) {
                        std::cout << "Client disconnected" << std::endl;
                        close(client_fd);
                        client_connected = false;
                    }
                    continue;
                }
            }
            
            if (got_result) {
                // For compatibility with existing streaming client, we'll encode length-prefixed messages
                
                // Calculate the size of the data to send
                // - 4 bytes for frame number
                // - 4 bytes for number of detections
                // - For each detection: 6*4 bytes (4 floats for bbox, 1 float for confidence, 1 int for class ID)
                size_t data_size = 8 + (result.num_detections * 24);
                
                // Allocate buffer
                std::vector<char> buffer(data_size);
                char* ptr = buffer.data();
                
                // Write frame number
                *reinterpret_cast<int*>(ptr) = result.frame_num;
                ptr += sizeof(int);
                
                // Write number of detections
                *reinterpret_cast<int*>(ptr) = result.num_detections;
                ptr += sizeof(int);
                
                // Write each detection
                for (const auto& det : result.detections) {
                    // Write bounding box
                    *reinterpret_cast<float*>(ptr) = det.x1;
                    ptr += sizeof(float);
                    *reinterpret_cast<float*>(ptr) = det.y1;
                    ptr += sizeof(float);
                    *reinterpret_cast<float*>(ptr) = det.x2;
                    ptr += sizeof(float);
                    *reinterpret_cast<float*>(ptr) = det.y2;
                    ptr += sizeof(float);
                    
                    // Write confidence
                    *reinterpret_cast<float*>(ptr) = det.confidence;
                    ptr += sizeof(float);
                    
                    // Write class ID
                    *reinterpret_cast<int*>(ptr) = det.class_id;
                    ptr += sizeof(int);
                }
                
                // Send the binary data
                ssize_t sent = send(client_fd, buffer.data(), data_size, 0);
                if (sent != static_cast<ssize_t>(data_size)) {
                    std::cerr << "Error sending detection data: " << strerror(errno) << std::endl;
                    close(client_fd);
                    client_connected = false;
                    continue;
                }
                
                if (result.frame_num % 100 == 0) {
                    std::cout << "Sent frame " << result.frame_num << " with " 
                              << result.num_detections << " detections to client" << std::endl;
                }
            }
        }
    }
    
    // Clean up
    if (client_fd >= 0) {
        close(client_fd);
    }
    close(server_fd);
    
    std::cout << "TCP result server thread finished" << std::endl;
}

void cleanup() {
    yolo_result_thread_running = false;
    tcp_server_running = false;

    sem_unlink("/frame_ready");
    sem_unlink("/frame_processed");
    sem_unlink("/result_ready");
    sem_unlink("/result_processed");
    sem_unlink("/sem_slots_empty");
    sem_unlink("/sem_slots_filled");
    sem_unlink("/sem_mutex");
    
    shm_unlink("/yolo_frame_buffer");
    shm_unlink("/yolo_result_buffer");
    shm_unlink("/yolo_frame_ringbuf");
    
    std::cout << "Cleanup completed" << std::endl;
    _exit(0);
}

int main(int argc, char* argv[]) {
    // Expecting at least 1 argument: input_url (optionally tcp_result_port)
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] 
                  << " tcp://192.168.2.3:9000?listen=1 [tcp_result_port]" 
                  << std::endl;
        return 1;
    }

    const char* input_url = argv[1];
    int tcp_result_port = 9876;  // Default port for results
    
    // Check if a custom port is provided
    if (argc > 2) {
        tcp_result_port = std::atoi(argv[2]);
    }
    
    // Initialize FFmpeg
    avformat_network_init();

    std::atomic<bool> decode_finished(false);

    FrameQueue* frame_queue = new FrameQueue();
    
    // Start YOLO result reader thread
    std::thread yolo_result_thread(yolo_result_reader_thread);
    
    // Start TCP result server thread
    std::thread tcp_server_thread(tcp_result_server_thread, tcp_result_port);
    
    // Start frame processing thread
    std::thread processing_thread(process_frames_for_yolo_shm, frame_queue);

    // Initialize decoder
    DecoderInfo decoder_info;
    if (!initialize_decoder(input_url, decoder_info)) {
        std::cerr << "Decoder initialization failed" << std::endl;
        // Signal threads to exit
        yolo_result_thread_running = false;
        tcp_server_running = false;
        avformat_network_deinit();
        return 1;
    }

    // Start decoder thread
    std::thread decoder_thread([&]() {
        if (!decode_frames(decoder_info, frame_queue, decode_finished)) {
            std::cerr << "Decoding failed" << std::endl;
        }
    });

    // Wait for decoder and processing threads to finish
    decoder_thread.join();
    processing_thread.join();
    
    // Signal other threads to exit and wait for them
    yolo_result_thread_running = false;
    tcp_server_running = false;
    
    yolo_result_thread.join();
    tcp_server_thread.join();

    // Clean up frame queues
    delete frame_queue;

    // Clean up FFmpeg
    cleanup();
    avcodec_free_context(&decoder_info.decoder_ctx);
    avformat_close_input(&decoder_info.input_fmt_ctx);
    avformat_network_deinit();
    return 0;
}