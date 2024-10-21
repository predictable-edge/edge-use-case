#include <iostream>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/avutil.h>
#include <libswscale/swscale.h>
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <input_file>" << std::endl;
        return 1;
    }

    const char* input_file = argv[1];

    // Initialize FFmpeg (no need to call av_register_all)
    av_log_set_level(AV_LOG_ERROR); // Optional: Only show error logs

    // Open input file and allocate format context
    AVFormatContext* format_ctx = nullptr;
    if (avformat_open_input(&format_ctx, input_file, nullptr, nullptr) < 0) {
        std::cerr << "Could not open input file: " << input_file << std::endl;
        return 2;
    }

    // Retrieve stream information
    if (avformat_find_stream_info(format_ctx, nullptr) < 0) {
        std::cerr << "Could not find stream information" << std::endl;
        avformat_close_input(&format_ctx);
        return 3;
    }

    // Find the first video stream
    int video_stream_idx = -1;
    for (unsigned int i = 0; i < format_ctx->nb_streams; ++i) {
        if (format_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            video_stream_idx = i;
            break;
        }
    }

    if (video_stream_idx == -1) {
        std::cerr << "Could not find a video stream" << std::endl;
        avformat_close_input(&format_ctx);
        return 4;
    }

    // Get codec parameters and find the decoder
    AVCodecParameters* codecpar = format_ctx->streams[video_stream_idx]->codecpar;
    AVCodec* codec = avcodec_find_decoder(codecpar->codec_id);
    if (!codec) {
        std::cerr << "Could not find the decoder" << std::endl;
        avformat_close_input(&format_ctx);
        return 5;
    }

    // Allocate codec context
    AVCodecContext* codec_ctx = avcodec_alloc_context3(codec);
    if (!codec_ctx) {
        std::cerr << "Could not allocate codec context" << std::endl;
        avformat_close_input(&format_ctx);
        return 6;
    }

    // Copy codec parameters to context
    if (avcodec_parameters_to_context(codec_ctx, codecpar) < 0) {
        std::cerr << "Failed to copy codec parameters to codec context" << std::endl;
        avcodec_free_context(&codec_ctx);
        avformat_close_input(&format_ctx);
        return 7;
    }

    // Open codec
    if (avcodec_open2(codec_ctx, codec, nullptr) < 0) {
        std::cerr << "Could not open codec" << std::endl;
        avcodec_free_context(&codec_ctx);
        avformat_close_input(&format_ctx);
        return 8;
    }

    // Allocate frame and packet
    AVFrame* frame = av_frame_alloc();
    if (!frame) {
        std::cerr << "Could not allocate frame" << std::endl;
        avcodec_free_context(&codec_ctx);
        avformat_close_input(&format_ctx);
        return 9;
    }

    AVPacket* packet = av_packet_alloc();
    if (!packet) {
        std::cerr << "Could not allocate packet" << std::endl;
        av_frame_free(&frame);
        avcodec_free_context(&codec_ctx);
        avformat_close_input(&format_ctx);
        return 10;
    }

    // Read frames from the video stream
    while (av_read_frame(format_ctx, packet) >= 0) {
        if (packet->stream_index == video_stream_idx) {
            // Send packet to decoder
            int ret = avcodec_send_packet(codec_ctx, packet);
            if (ret < 0) {
                std::cerr << "Error sending packet for decoding" << std::endl;
                break;
            }

            // Receive frame from decoder
            while (ret >= 0) {
                ret = avcodec_receive_frame(codec_ctx, frame);
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                    break;
                } else if (ret < 0) {
                    std::cerr << "Error during decoding" << std::endl;
                    break;
                }

                // Print basic frame properties
                std::cout << "Frame " << codec_ctx->frame_number
                          << " (type=" << av_get_picture_type_char(frame->pict_type)
                          << ", size=" << frame->pkt_size
                          << " bytes) pts " << frame->pts
                          << " key_frame " << frame->key_frame
                          << std::endl;

                // Optional: Process frame data (e.g., convert format, save image, etc.)
            }
        }

        // Unreference the packet to free its data
        av_packet_unref(packet);
    }

    // Flush the decoder
    avcodec_send_packet(codec_ctx, nullptr);
    while (avcodec_receive_frame(codec_ctx, frame) == 0) {
        std::cout << "Frame " << codec_ctx->frame_number
                  << " (type=" << av_get_picture_type_char(frame->pict_type)
                  << ", size=" << frame->pkt_size
                  << " bytes) pts " << frame->pts
                  << " key_frame " << frame->key_frame
                  << std::endl;
    }

    // Free allocated resources
    av_packet_free(&packet);
    av_frame_free(&frame);
    avcodec_free_context(&codec_ctx);
    avformat_close_input(&format_ctx);

    return 0;
}