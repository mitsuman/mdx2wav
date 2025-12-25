#include "video_encoder.h"
#include <stdio.h>
#include <string.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

VideoEncoder::VideoEncoder()
    : format_ctx_(nullptr)
    , video_codec_ctx_(nullptr)
    , audio_codec_ctx_(nullptr)
    , video_stream_(nullptr)
    , audio_stream_(nullptr)
    , video_frame_(nullptr)
    , audio_frame_(nullptr)
    , packet_(nullptr)
    , sws_ctx_(nullptr)
    , width_(0)
    , height_(0)
    , fps_(0)
    , sample_rate_(0)
    , audio_channels_(0)
    , video_pts_(0)
    , audio_pts_(0)
    , initialized_(false)
    , audio_buffer_(nullptr)
    , audio_buffer_size_(0)
    , audio_buffer_pos_(0)
{
}

VideoEncoder::~VideoEncoder() {
    finalize();
}

void VideoEncoder::setError(const char* message) {
    error_message_ = message;
    fprintf(stderr, "VideoEncoder Error: %s\n", message);
}

bool VideoEncoder::init(const char* filename, int width, int height, int fps,
                       int sample_rate, int audio_channels) {
    if (initialized_) {
        setError("Already initialized");
        return false;
    }
    
    filename_ = filename;
    width_ = width;
    height_ = height;
    fps_ = fps;
    sample_rate_ = sample_rate;
    audio_channels_ = audio_channels;
    video_pts_ = 0;
    audio_pts_ = 0;
    
    // 出力フォーマットコンテキストの作成
    avformat_alloc_output_context2(&format_ctx_, nullptr, nullptr, filename);
    if (!format_ctx_) {
        setError("Could not create output context");
        return false;
    }
    
    // ビデオストリームの初期化
    if (!initVideoStream()) {
        return false;
    }
    
    // オーディオストリームの初期化
    if (!initAudioStream()) {
        return false;
    }
    
    // ファイルを開く
    if (!(format_ctx_->oformat->flags & AVFMT_NOFILE)) {
        if (avio_open(&format_ctx_->pb, filename, AVIO_FLAG_WRITE) < 0) {
            setError("Could not open output file");
            return false;
        }
    }
    
    // ヘッダーを書き込む
    if (avformat_write_header(format_ctx_, nullptr) < 0) {
        setError("Error writing header");
        return false;
    }
    
    initialized_ = true;
    fprintf(stderr, "Video encoder initialized: %s (%dx%d @ %dfps, %dHz)\n",
            filename, width, height, fps, sample_rate);
    return true;
}

bool VideoEncoder::initVideoStream() {
    // H.264コーデックを検索
    const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_H264);
    if (!codec) {
        setError("H.264 codec not found");
        return false;
    }
    
    // ストリームを作成
    video_stream_ = avformat_new_stream(format_ctx_, nullptr);
    if (!video_stream_) {
        setError("Could not create video stream");
        return false;
    }
    video_stream_->id = format_ctx_->nb_streams - 1;
    
    // コーデックコンテキストを作成
    video_codec_ctx_ = avcodec_alloc_context3(codec);
    if (!video_codec_ctx_) {
        setError("Could not allocate video codec context");
        return false;
    }
    
    // コーデックパラメータ設定
    video_codec_ctx_->codec_id = AV_CODEC_ID_H264;
    video_codec_ctx_->bit_rate = 4000000; // 4Mbps
    video_codec_ctx_->width = width_;
    video_codec_ctx_->height = height_;
    video_codec_ctx_->time_base = (AVRational){1, fps_};
    video_codec_ctx_->framerate = (AVRational){fps_, 1};
    video_codec_ctx_->gop_size = fps_; // 1秒ごとにキーフレーム
    video_codec_ctx_->max_b_frames = 0;
    video_codec_ctx_->pix_fmt = AV_PIX_FMT_YUV420P;
    
    // H.264プリセット設定
    av_opt_set(video_codec_ctx_->priv_data, "preset", "medium", 0);
    av_opt_set(video_codec_ctx_->priv_data, "crf", "23", 0);
    
    if (format_ctx_->oformat->flags & AVFMT_GLOBALHEADER) {
        video_codec_ctx_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }
    
    // コーデックを開く
    if (avcodec_open2(video_codec_ctx_, codec, nullptr) < 0) {
        setError("Could not open video codec");
        return false;
    }
    
    // ストリームパラメータをコピー
    avcodec_parameters_from_context(video_stream_->codecpar, video_codec_ctx_);
    video_stream_->time_base = video_codec_ctx_->time_base;
    
    // フレームを作成
    video_frame_ = av_frame_alloc();
    if (!video_frame_) {
        setError("Could not allocate video frame");
        return false;
    }
    video_frame_->format = video_codec_ctx_->pix_fmt;
    video_frame_->width = width_;
    video_frame_->height = height_;
    
    if (av_frame_get_buffer(video_frame_, 0) < 0) {
        setError("Could not allocate video frame buffer");
        return false;
    }
    
    // BGRA to YUV変換コンテキストを作成（32ビットBGRAサーフェス用）
    sws_ctx_ = sws_getContext(
        width_, height_, AV_PIX_FMT_BGRA,
        width_, height_, AV_PIX_FMT_YUV420P,
        SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!sws_ctx_) {
        setError("Could not initialize sws context");
        return false;
    }
    
    // パケットを作成
    packet_ = av_packet_alloc();
    if (!packet_) {
        setError("Could not allocate packet");
        return false;
    }
    
    return true;
}

bool VideoEncoder::initAudioStream() {
    // AACコーデックを検索
    const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
    if (!codec) {
        setError("AAC codec not found");
        return false;
    }
    
    // ストリームを作成
    audio_stream_ = avformat_new_stream(format_ctx_, nullptr);
    if (!audio_stream_) {
        setError("Could not create audio stream");
        return false;
    }
    audio_stream_->id = format_ctx_->nb_streams - 1;
    
    // コーデックコンテキストを作成
    audio_codec_ctx_ = avcodec_alloc_context3(codec);
    if (!audio_codec_ctx_) {
        setError("Could not allocate audio codec context");
        return false;
    }
    
    // コーデックパラメータ設定
    audio_codec_ctx_->codec_id = AV_CODEC_ID_AAC;
    audio_codec_ctx_->bit_rate = 192000; // 192kbps
    audio_codec_ctx_->sample_rate = sample_rate_;
    
    // チャンネルレイアウト設定（FFmpeg 8.0+）
    if (audio_channels_ == 2) {
        AVChannelLayout stereo_layout = AV_CHANNEL_LAYOUT_STEREO;
        av_channel_layout_copy(&audio_codec_ctx_->ch_layout, &stereo_layout);
    } else {
        AVChannelLayout mono_layout = AV_CHANNEL_LAYOUT_MONO;
        av_channel_layout_copy(&audio_codec_ctx_->ch_layout, &mono_layout);
    }
    
    audio_codec_ctx_->sample_fmt = AV_SAMPLE_FMT_FLTP; // AACはplanar floatを要求
    audio_codec_ctx_->time_base = (AVRational){1, sample_rate_};
    
    if (format_ctx_->oformat->flags & AVFMT_GLOBALHEADER) {
        audio_codec_ctx_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }
    
    // コーデックを開く
    if (avcodec_open2(audio_codec_ctx_, codec, nullptr) < 0) {
        setError("Could not open audio codec");
        return false;
    }
    
    // ストリームパラメータをコピー
    avcodec_parameters_from_context(audio_stream_->codecpar, audio_codec_ctx_);
    audio_stream_->time_base = audio_codec_ctx_->time_base;
    
    // オーディオフレームを作成
    audio_frame_ = av_frame_alloc();
    if (!audio_frame_) {
        setError("Could not allocate audio frame");
        return false;
    }
    audio_frame_->format = audio_codec_ctx_->sample_fmt;
    audio_frame_->ch_layout = audio_codec_ctx_->ch_layout;
    audio_frame_->sample_rate = audio_codec_ctx_->sample_rate;
    audio_frame_->nb_samples = audio_codec_ctx_->frame_size;
    
    if (av_frame_get_buffer(audio_frame_, 0) < 0) {
        setError("Could not allocate audio frame buffer");
        return false;
    }
    
    // 音声バッファを作成（コーデックのフレームサイズに合わせる）
    audio_buffer_size_ = audio_codec_ctx_->frame_size * audio_channels_;
    audio_buffer_ = new short[audio_buffer_size_ * 4]; // 余裕を持たせる
    audio_buffer_pos_ = 0;
    
    return true;
}

bool VideoEncoder::addVideoFrame(const unsigned char* rgb_data) {
    if (!initialized_) {
        setError("Not initialized");
        return false;
    }
    
    // BGRAからYUVに変換（32ビットBGRAサーフェス用）
    const uint8_t* src_data[1] = { rgb_data };
    int src_linesize[1] = { width_ * 4 };
    
    sws_scale(sws_ctx_, src_data, src_linesize, 0, height_,
              video_frame_->data, video_frame_->linesize);
    
    video_frame_->pts = video_pts_++;
    
    return writeVideoFrame(video_frame_);
}

bool VideoEncoder::writeVideoFrame(AVFrame* frame) {
    // フレームをエンコーダに送る
    int ret = avcodec_send_frame(video_codec_ctx_, frame);
    if (ret < 0) {
        setError("Error sending video frame to encoder");
        return false;
    }
    
    // エンコードされたパケットを受け取る
    while (ret >= 0) {
        ret = avcodec_receive_packet(video_codec_ctx_, packet_);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            break;
        } else if (ret < 0) {
            setError("Error receiving video packet from encoder");
            return false;
        }
        
        // パケットのタイムスタンプを調整
        av_packet_rescale_ts(packet_, video_codec_ctx_->time_base, video_stream_->time_base);
        packet_->stream_index = video_stream_->index;
        
        // パケットを書き込む
        ret = av_interleaved_write_frame(format_ctx_, packet_);
        av_packet_unref(packet_);
        
        if (ret < 0) {
            setError("Error writing video frame");
            return false;
        }
    }
    
    return true;
}

bool VideoEncoder::addAudioSamples(const short* samples, int sample_count) {
    if (!initialized_) {
        setError("Not initialized");
        return false;
    }
    
    // サンプルをバッファに追加
    int samples_to_copy = sample_count * audio_channels_;
    memcpy(audio_buffer_ + audio_buffer_pos_, samples, samples_to_copy * sizeof(short));
    audio_buffer_pos_ += samples_to_copy;
    
    // フレームサイズ分のデータが溜まったら書き込む
    int frame_size_bytes = audio_codec_ctx_->frame_size * audio_channels_;
    
    while (audio_buffer_pos_ >= frame_size_bytes) {
        // S16からFLTPに変換（planar float形式）
        float* left_channel = (float*)audio_frame_->data[0];
        float* right_channel = (float*)audio_frame_->data[1];
        
        for (int i = 0; i < audio_codec_ctx_->frame_size; i++) {
            left_channel[i] = audio_buffer_[i * 2] / 32768.0f;
            if (audio_channels_ == 2) {
                right_channel[i] = audio_buffer_[i * 2 + 1] / 32768.0f;
            }
        }
        
        audio_frame_->pts = audio_pts_;
        audio_pts_ += audio_codec_ctx_->frame_size;
        
        if (!writeAudioFrame(audio_frame_)) {
            return false;
        }
        
        // バッファの残りを前に詰める
        int remaining = audio_buffer_pos_ - frame_size_bytes;
        if (remaining > 0) {
            memmove(audio_buffer_, audio_buffer_ + frame_size_bytes, remaining * sizeof(short));
        }
        audio_buffer_pos_ = remaining;
    }
    
    return true;
}

bool VideoEncoder::writeAudioFrame(AVFrame* frame) {
    // フレームをエンコーダに送る
    int ret = avcodec_send_frame(audio_codec_ctx_, frame);
    if (ret < 0) {
        setError("Error sending audio frame to encoder");
        return false;
    }
    
    // エンコードされたパケットを受け取る
    while (ret >= 0) {
        ret = avcodec_receive_packet(audio_codec_ctx_, packet_);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            break;
        } else if (ret < 0) {
            setError("Error receiving audio packet from encoder");
            return false;
        }
        
        // パケットのタイムスタンプを調整
        av_packet_rescale_ts(packet_, audio_codec_ctx_->time_base, audio_stream_->time_base);
        packet_->stream_index = audio_stream_->index;
        
        // パケットを書き込む
        ret = av_interleaved_write_frame(format_ctx_, packet_);
        av_packet_unref(packet_);
        
        if (ret < 0) {
            setError("Error writing audio frame");
            return false;
        }
    }
    
    return true;
}

void VideoEncoder::finalize() {
    if (!initialized_) {
        return;
    }
    
    // 残りのフレームをフラッシュ
    if (video_codec_ctx_) {
        avcodec_send_frame(video_codec_ctx_, nullptr);
        while (avcodec_receive_packet(video_codec_ctx_, packet_) == 0) {
            av_packet_rescale_ts(packet_, video_codec_ctx_->time_base, video_stream_->time_base);
            packet_->stream_index = video_stream_->index;
            av_interleaved_write_frame(format_ctx_, packet_);
            av_packet_unref(packet_);
        }
    }
    
    if (audio_codec_ctx_) {
        avcodec_send_frame(audio_codec_ctx_, nullptr);
        while (avcodec_receive_packet(audio_codec_ctx_, packet_) == 0) {
            av_packet_rescale_ts(packet_, audio_codec_ctx_->time_base, audio_stream_->time_base);
            packet_->stream_index = audio_stream_->index;
            av_interleaved_write_frame(format_ctx_, packet_);
            av_packet_unref(packet_);
        }
    }
    
    // トレーラーを書き込む
    if (format_ctx_) {
        av_write_trailer(format_ctx_);
    }
    
    // リソースを解放
    if (sws_ctx_) {
        sws_freeContext(sws_ctx_);
        sws_ctx_ = nullptr;
    }
    
    if (video_frame_) {
        av_frame_free(&video_frame_);
    }
    
    if (audio_frame_) {
        av_frame_free(&audio_frame_);
    }
    
    if (packet_) {
        av_packet_free(&packet_);
    }
    
    if (video_codec_ctx_) {
        avcodec_free_context(&video_codec_ctx_);
    }
    
    if (audio_codec_ctx_) {
        avcodec_free_context(&audio_codec_ctx_);
    }
    
    if (format_ctx_) {
        if (!(format_ctx_->oformat->flags & AVFMT_NOFILE)) {
            avio_closep(&format_ctx_->pb);
        }
        avformat_free_context(format_ctx_);
        format_ctx_ = nullptr;
    }
    
    if (audio_buffer_) {
        delete[] audio_buffer_;
        audio_buffer_ = nullptr;
    }
    
    initialized_ = false;
    fprintf(stderr, "Video encoder finalized: %s\n", filename_.c_str());
}
