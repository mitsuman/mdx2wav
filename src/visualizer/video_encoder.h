#ifndef VIDEO_ENCODER_H
#define VIDEO_ENCODER_H

#include <string>

// FFmpeg forward declarations
struct AVFormatContext;
struct AVCodecContext;
struct AVStream;
struct AVFrame;
struct AVPacket;
struct SwsContext;

class VideoEncoder {
public:
    VideoEncoder();
    ~VideoEncoder();
    
    // 初期化
    // @param filename 出力ファイル名（例: output.mp4）
    // @param width 動画の幅
    // @param height 動画の高さ
    // @param fps フレームレート
    // @param sample_rate 音声サンプルレート
    // @param audio_channels 音声チャンネル数（2=ステレオ）
    bool init(const char* filename, int width, int height, int fps, 
              int sample_rate, int audio_channels);
    
    // ビデオフレームを追加
    // @param rgb_data RGBピクセルデータ（width * height * 3バイト）
    bool addVideoFrame(const unsigned char* rgb_data);
    
    // オーディオサンプルを追加
    // @param samples 16bit符号付きステレオサンプル
    // @param sample_count サンプル数（フレーム数、ステレオなので実際のshort配列は sample_count * 2）
    bool addAudioSamples(const short* samples, int sample_count);
    
    // エンコード終了
    void finalize();
    
    // エラーメッセージ取得
    const char* getError() const { return error_message_.c_str(); }
    
    // 初期化済みかどうか
    bool isInitialized() const { return initialized_; }
    
private:
    // ビデオストリームの初期化
    bool initVideoStream();
    
    // オーディオストリームの初期化
    bool initAudioStream();
    
    // フレームデータを書き込み
    bool writeVideoFrame(AVFrame* frame);
    bool writeAudioFrame(AVFrame* frame);
    
    // エラーメッセージ設定
    void setError(const char* message);
    
    AVFormatContext* format_ctx_;
    AVCodecContext* video_codec_ctx_;
    AVCodecContext* audio_codec_ctx_;
    AVStream* video_stream_;
    AVStream* audio_stream_;
    AVFrame* video_frame_;
    AVFrame* audio_frame_;
    AVPacket* packet_;
    SwsContext* sws_ctx_;
    
    int width_;
    int height_;
    int fps_;
    int sample_rate_;
    int audio_channels_;
    
    int64_t video_pts_;
    int64_t audio_pts_;
    
    bool initialized_;
    std::string error_message_;
    std::string filename_;
    
    // 音声バッファ（サンプルを蓄積してフレーム単位で書き込む）
    short* audio_buffer_;
    int audio_buffer_size_;
    int audio_buffer_pos_;
};

#endif // VIDEO_ENCODER_H
