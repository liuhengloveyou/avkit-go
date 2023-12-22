#pragma once

#include <atomic>
#include <queue>
#include <mutex>

#ifdef __cplusplus
extern "C"
{
#endif

#include <libavutil/avassert.h>
#include <libavutil/mathematics.h>
#include <libavutil/timestamp.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavutil/time.h>
#include <libavutil/imgutils.h>
#include <libavutil/dict.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>

#ifdef __cplusplus
}
#endif

#include "thread.h"

#define MAX_AUDIO_FRAME_SIZE 192000

typedef struct StreamContext
{
    AVCodecContext *decoder_ctx;
    AVCodecContext *encoder_ctx;

    const AVCodec *decoder = NULL;
    const AVCodec *encoder = NULL;

    AVStream *stream = NULL;
    AVMediaType media_type;
} StreamContext;

//  AVBufferRef *hw_device_ctx = NULL;

typedef struct FilteringContext
{
    AVFilterContext *buffersink_ctx;
    AVFilterContext *buffersrc_ctx;
    AVFilterGraph *filter_graph;

    AVPacket *enc_pkt;
    AVFrame *filtered_frame;
} FilteringContext;

typedef struct MyAVFrame
{
    AVFrame *data;
    AVPacket *enc_pkt;
    uint64_t stream_index;
    int64_t packet_pos = 0;
} MyAVFrame;

class Context
{
public:
    Context() = delete;
    Context(const char *afn, const char *bfn);
    ~Context();

    int init();

public:
    AVBufferRef *hw_device_ctx = NULL;
    AVFormatContext *ifmt_ctx = NULL;
    AVFormatContext *ofmt_ctx = NULL;
    StreamContext *stream_ctx = NULL;
    SwrContext *swr_ctx = NULL;

    int video_stream_index = -1;

    // 进度
    std::atomic<int64_t> duration;
    std::atomic<int64_t> dec_pos;
    std::atomic<int64_t> enc_pos;

    std::queue<AVPacket *> packetQueue;
    std::mutex packetQueueMutex;
    std::queue<MyAVFrame *> frameQueue;
    std::mutex frameQueueMutex;

    // for audio
    uint8_t audio_buf[(MAX_AUDIO_FRAME_SIZE * 3) / 2];
    unsigned int audio_buf_size = 0;
    unsigned int audio_buf_index = 0;
    AVFrame *audio_frame = nullptr;
    AVPacket *audio_pkt = nullptr;
    uint8_t *audio_pkt_data = nullptr;
    int audio_pkt_size = 0;

    std::atomic<bool> isStop; // = false;

    const char *aFilePath;
    const char *bFilePath;

private:
    int set_encoder_hwframe_ctx(AVCodecContext *encoder_ctx, AVBufferRef *hw_device_ctx,  AVCodecContext *decoder_ctx);
    int open_input_file(const char *filename);
    int open_output_file(const char *filename);

private:
    pthread_t thread_stat_id;
};

class Demuxer : public ThreadBase
{
public:
    Demuxer(Context *c) : ctx(c)
    {
        maxWaitQueue = 0;
    }
    ~Demuxer(){};

    void run();

public:
    std::atomic<int> maxWaitQueue;

private:
    Context *ctx = nullptr;
};

class DecodeThread : public ThreadBase
{
public:
    DecodeThread(Context *c) : ctx(c) {}
    ~DecodeThread(){};

    void run();

private:
    Context *ctx = nullptr;
};

class EncodeThread : public ThreadBase
{
public:
    EncodeThread(Context *c) : ctx(c) {}
    ~EncodeThread(){};

    void run();

private:
    int encode_write(AVCodecContext *encoder_ctx, AVPacket *enc_pkt, AVFrame *frame);
    // int encode_write_frame(unsigned int stream_index, int flush);
    int flush_encoder(unsigned int stream_index);

private:
    Context *ctx = nullptr;
};