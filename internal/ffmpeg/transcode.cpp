#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "transcode.h"
#include "progress.h"

extern "C"
{
#include "transcode_capi.h"
}

static ProgressCBPtr progressCB = nullptr;

static inline char *my_av_err2str(int errnum)
{
    char tmp[AV_ERROR_MAX_STRING_SIZE] = {0};
    return av_make_error_string(tmp, AV_ERROR_MAX_STRING_SIZE, errnum);
}

static enum AVPixelFormat get_format(AVCodecContext *avctx, const enum AVPixelFormat *pix_fmts)
{
    while (*pix_fmts != AV_PIX_FMT_NONE)
    {
        if (*pix_fmts == AV_PIX_FMT_QSV)
        {
            return AV_PIX_FMT_QSV;
        }

        pix_fmts++;
    }

    fprintf(stderr, "The QSV pixel format not offered in get_format()\n");

    return AV_PIX_FMT_NONE;
}

void setTranscodeProgressFunc(ProgressCBPtr f)
{
    progressCB = f;

    return;
}

// Context 统计线程
void *stat_thread(void *arg)
{
    while (1)
    {
        sleep(1);

        int packetQueueSize = 0;
        int frameQueueSize = 0;
        Context *ctx = (Context *)arg;
        {
            std::lock_guard<std::mutex> lck(ctx->packetQueueMutex);
            packetQueueSize = ctx->packetQueue.size();
        }
        {
            std::lock_guard<std::mutex> lck(ctx->frameQueueMutex);
            frameQueueSize = ctx->frameQueue.size();
        }
        int64_t dec_pos = ctx->dec_pos;
        int64_t enc_pos = ctx->enc_pos;
        int64_t duration = ctx->duration;

        printf("Context::stat:: packetQueueSize: %d frameQueueSize: %d %d %d %d %f %f \n", packetQueueSize, frameQueueSize, dec_pos, enc_pos, duration, double(dec_pos) / double(duration), double(enc_pos) / double(duration));

        // if (progressCB)
        // {
        //     // progressCB("TranscodeProgress", ctx->aFilePath, ctx->iFmtCtx->duration, packet->pos);
        // }
    }
}

Context::Context(const char *afn, const char *bfn) : aFilePath(afn), bFilePath(bfn)
{
    isStop = false;
    duration = 0;
    dec_pos = 0;
    enc_pos = 0;

    // 启动统计线程
    if (pthread_create(&thread_stat_id, NULL, stat_thread, this))
    {
        printf("stat_thread err\n");
        exit(-1);
    }
};

Context::~Context()
{
    for (int i = 0; i < ifmt_ctx->nb_streams; i++)
    {
        avcodec_free_context(&stream_ctx[i].decoder_ctx);
        if (ofmt_ctx && ofmt_ctx->nb_streams > i && ofmt_ctx->streams[i] && stream_ctx[i].encoder_ctx)
            avcodec_free_context(&stream_ctx[i].encoder_ctx);
    }

    av_free(stream_ctx);
    avformat_close_input(&ifmt_ctx);
    if (ofmt_ctx && !(ofmt_ctx->oformat->flags & AVFMT_NOFILE))
        avio_closep(&ofmt_ctx->pb);
    avformat_free_context(ofmt_ctx);
}

int Context::init()
{
    int ret = 0;

    ret = av_hwdevice_ctx_create(&hw_device_ctx, AV_HWDEVICE_TYPE_QSV, "auto", NULL, 0);
    if (ret < 0)
    {
        fprintf(stderr, "Failed to create a QSV device. Error code: %s\n", my_av_err2str(ret));
        return ret;
    }

    ret = open_input_file(aFilePath);
    if (ret < 0)
        return ret;

    ret = open_output_file(bFilePath);
    if (ret < 0)
        return ret;

    return 0;
}

int Context::set_encoder_hwframe_ctx(AVCodecContext *encoder_ctx, AVBufferRef *hw_device_ctx, AVCodecContext *decoder_ctx)
{
    int err = 0;

    AVBufferRef *hw_frames_ref = av_hwframe_ctx_alloc(hw_device_ctx);
    if (!hw_frames_ref)
    {
        fprintf(stderr, "Failed to create VAAPI frame context.\n");
        return -1;
    }
    AVHWFramesContext *frames_ctx = (AVHWFramesContext *)(hw_frames_ref->data);
    frames_ctx->format = AV_PIX_FMT_QSV;
    frames_ctx->sw_format = AV_PIX_FMT_NV12;
    frames_ctx->width = decoder_ctx->width;
    frames_ctx->height = decoder_ctx->height;
    frames_ctx->initial_pool_size = 20;

    if ((err = av_hwframe_ctx_init(hw_frames_ref)) < 0)
    {
        fprintf(stderr, "Failed to initialize VAAPI frame context.Error code: %s\n", my_av_err2str(err));
        av_buffer_unref(&hw_frames_ref);
        return err;
    }
    encoder_ctx->hw_frames_ctx = av_buffer_ref(hw_frames_ref);
    if (!encoder_ctx->hw_frames_ctx)
        err = AVERROR(ENOMEM);

    /* set AVCodecContext Parameters for encoder, here we keep them stay
     * the same as decoder.
     */
    encoder_ctx->time_base = av_inv_q(decoder_ctx->framerate);
    encoder_ctx->pix_fmt = AV_PIX_FMT_QSV;
    encoder_ctx->width = decoder_ctx->width;
    encoder_ctx->height = decoder_ctx->height;
    encoder_ctx->sample_aspect_ratio = decoder_ctx->sample_aspect_ratio;

    av_buffer_unref(&hw_frames_ref);
    return err;
}

int Context::open_input_file(const char *filename)
{
    int ret;
    unsigned int i;

    ifmt_ctx = NULL;
    if ((ret = avformat_open_input(&ifmt_ctx, filename, NULL, NULL)) < 0)
    {
        av_log(NULL, AV_LOG_ERROR, "Cannot open input file\n");
        return ret;
    }

    if ((ret = avformat_find_stream_info(ifmt_ctx, NULL)) < 0)
    {
        av_log(NULL, AV_LOG_ERROR, "Cannot find stream information\n");
        return ret;
    }

    stream_ctx = (StreamContext *)av_calloc(ifmt_ctx->nb_streams, sizeof(*stream_ctx));
    if (!stream_ctx)
    {
        return AVERROR(ENOMEM);
    }

    ret = av_find_best_stream(ifmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (ret < 0)
    {
        fprintf(stderr, "Cannot find a video stream in the input file. Error code: %s\n", my_av_err2str(ret));
        return ret;
    }

    for (i = 0; i < ifmt_ctx->nb_streams; i++)
    {
        stream_ctx[i].decoder_ctx = NULL;
        stream_ctx[i].encoder_ctx = NULL;
        stream_ctx[i].encoder = NULL;
        stream_ctx[i].decoder = NULL;
        stream_ctx[i].stream = NULL;
        stream_ctx[i].media_type = AVMEDIA_TYPE_UNKNOWN;

        AVStream *stream = ifmt_ctx->streams[i];
        AVCodecParameters *codecpar = stream->codecpar;

        if (codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
        {
            video_stream_index = i;
            duration = stream->duration;

            stream_ctx[i].stream = stream;
            stream_ctx[i].media_type = AVMEDIA_TYPE_VIDEO;

            switch (codecpar->codec_id)
            {
            case AV_CODEC_ID_H264:
                stream_ctx[i].decoder = avcodec_find_decoder_by_name("h264_qsv");
                stream_ctx[i].encoder = avcodec_find_encoder_by_name("h264_qsv");
                break;
            case AV_CODEC_ID_HEVC:
                stream_ctx[video_stream_index].decoder = avcodec_find_decoder_by_name("hevc_qsv");
                break;
            case AV_CODEC_ID_VP9:
                stream_ctx[video_stream_index].decoder = avcodec_find_decoder_by_name("vp9_qsv");
                break;
            case AV_CODEC_ID_VP8:
                stream_ctx[video_stream_index].decoder = avcodec_find_decoder_by_name("vp8_qsv");
                break;
            case AV_CODEC_ID_AV1:
                stream_ctx[video_stream_index].decoder = avcodec_find_decoder_by_name("av1_qsv");
                break;
            case AV_CODEC_ID_MPEG2VIDEO:
                stream_ctx[video_stream_index].decoder = avcodec_find_decoder_by_name("mpeg2_qsv");
                break;
            case AV_CODEC_ID_MJPEG:
                stream_ctx[video_stream_index].decoder = avcodec_find_decoder_by_name("mjpeg_qsv");
                break;
            default:
                fprintf(stderr, "Codec is not supportted by qsv\n");
                return -1;
            }

            if (!stream_ctx[i].decoder || !stream_ctx[i].encoder)
            {
                av_log(NULL, AV_LOG_ERROR, "Failed to find decoder for stream #%u\n", i);
                return AVERROR_DECODER_NOT_FOUND;
            }

            stream_ctx[i].decoder_ctx = avcodec_alloc_context3(stream_ctx[i].decoder);
            if (!stream_ctx[i].decoder_ctx)
            {
                av_log(NULL, AV_LOG_ERROR, "Failed to allocate the decoder context for stream #%u\n", i);
                return AVERROR(ENOMEM);
            }
            stream_ctx[i].decoder_ctx->codec_id = codecpar->codec_id; // AV_CODEC_ID_H264

            ret = avcodec_parameters_to_context(stream_ctx[i].decoder_ctx, stream->codecpar);
            if (ret < 0)
            {
                av_log(NULL, AV_LOG_ERROR, "Failed to copy decoder parameters to input decoder context for stream #%u\n", i);
                return ret;
            }

            /* Inform the decoder about the timebase for the packet timestamps. This is highly recommended, but not mandatory. */
            stream_ctx[i].decoder_ctx->pkt_timebase = stream->time_base;
            stream_ctx[i].decoder_ctx->framerate = av_guess_frame_rate(ifmt_ctx, stream, NULL);
            stream_ctx[i].decoder_ctx->get_format = get_format;
            stream_ctx[i].decoder_ctx->hw_device_ctx = av_buffer_ref(hw_device_ctx);
            if (!stream_ctx[i].decoder_ctx->hw_device_ctx)
            {
                fprintf(stderr, "A hardware device reference create failed.\n");
                return AVERROR(ENOMEM);
            }

            if (stream->codecpar->extradata_size)
            {
                stream_ctx[i].decoder_ctx->extradata = (unsigned char *)av_mallocz(stream->codecpar->extradata_size + AV_INPUT_BUFFER_PADDING_SIZE);
                if (!stream_ctx[i].decoder_ctx->extradata)
                {
                    ret = AVERROR(ENOMEM);
                    return ret;
                }
                memcpy(stream_ctx[i].decoder_ctx->extradata, stream->codecpar->extradata, stream->codecpar->extradata_size);
                stream_ctx[i].decoder_ctx->extradata_size = stream->codecpar->extradata_size;
            }

            /* Open decoder */
            ret = avcodec_open2(stream_ctx[i].decoder_ctx, stream_ctx[i].decoder, NULL);
            if (ret < 0)
            {
                fprintf(stderr, "Failed to open codec for decoding. Error code: %s\n", my_av_err2str(ret));
                return ret;
            }
        }
        else if (codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
        {
            stream_ctx[i].media_type = AVMEDIA_TYPE_AUDIO;
            stream->discard = AVDISCARD_ALL;
        }
        else if (codecpar->codec_type == AVMEDIA_TYPE_SUBTITLE)
        {
            stream_ctx[i].media_type = AVMEDIA_TYPE_SUBTITLE;
            stream->discard = AVDISCARD_ALL;
        }
    }

    av_dump_format(ifmt_ctx, 0, filename, 0);
    return 0;
}

int Context::open_output_file(const char *filename)
{
    int ret;

    ofmt_ctx = NULL;
    avformat_alloc_output_context2(&ofmt_ctx, NULL, NULL, filename);
    if (!ofmt_ctx)
    {
        av_log(NULL, AV_LOG_ERROR, "Could not create output context\n");
        return AVERROR_UNKNOWN;
    }

    for (int i = 0; i < ifmt_ctx->nb_streams; i++)
    {
        AVStream *in_stream = ifmt_ctx->streams[i];

        if (stream_ctx[i].media_type == AVMEDIA_TYPE_VIDEO)
        {
            const AVCodec *encoder = stream_ctx[i].encoder;
            AVCodecContext *decoder_ctx = stream_ctx[i].decoder_ctx;

            // 新建一个stream
            AVStream *out_stream = avformat_new_stream(ofmt_ctx, encoder);
            if (!out_stream)
            {
                av_log(NULL, AV_LOG_ERROR, "Failed allocating output stream\n");
                return AVERROR_UNKNOWN;
            }

            ret = avcodec_parameters_copy(out_stream->codecpar, in_stream->codecpar);
            if (ret < 0)
            {
                fprintf(stderr, "Failed to copy codec parameters\n");
                return AVERROR_UNKNOWN;
            }
            // out_stream->time_base = stream_ctx[i].encoder_ctx->time_base;
            // out_stream->codecpar->codec_tag = 0;

            // 新建编码器
            stream_ctx[i].encoder_ctx = avcodec_alloc_context3(encoder);
            if (!stream_ctx[i].encoder_ctx)
            {
                av_log(NULL, AV_LOG_FATAL, "Failed to allocate the encoder context\n");
                return AVERROR(ENOMEM);
            }

            int ret = set_encoder_hwframe_ctx(stream_ctx[i].encoder_ctx, hw_device_ctx, decoder_ctx);
            if (ret < 0)
            {
                fprintf(stderr, "Failed to set hwframe context.\n");
                return ret;
            }

            if ((ret = avcodec_open2(stream_ctx[i].encoder_ctx, encoder, NULL)) < 0)
            {
                fprintf(stderr, "Failed to open encode codec. Error code: %s\n", my_av_err2str(ret));
                return ret;
            }

            /* take first format from list of supported formats */
            if (stream_ctx[i].encoder->pix_fmts)
                stream_ctx[i].encoder_ctx->pix_fmt = stream_ctx[i].encoder->pix_fmts[0];
            else
                stream_ctx[i].encoder_ctx->pix_fmt = stream_ctx[i].decoder_ctx->pix_fmt;
            /* video time_base can be set to whatever is handy and supported by encoder */
            stream_ctx[i].encoder_ctx->time_base = av_inv_q(stream_ctx[i].decoder_ctx->framerate);

            if (ofmt_ctx->oformat->flags & AVFMT_GLOBALHEADER)
                stream_ctx[i].encoder_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

            out_stream->time_base = stream_ctx[i].encoder_ctx->time_base;
        }
        else if (stream_ctx[i].media_type == AVMEDIA_TYPE_UNKNOWN)
        {
            av_log(NULL, AV_LOG_FATAL, "Elementary stream #%d is of unknown type, cannot proceed\n", i);
            return AVERROR_INVALIDDATA;
        }
        // else
        // {
        //     /* if this stream must be remuxed */
        //     ret = avcodec_parameters_copy(out_stream->codecpar, in_stream->codecpar);
        //     if (ret < 0)
        //     {
        //         av_log(NULL, AV_LOG_ERROR, "Copying parameters for stream #%u failed\n", i);
        //         return ret;
        //     }
        //     out_stream->time_base = in_stream->time_base;
        // }
    }
    av_dump_format(ofmt_ctx, 0, filename, 1);

    if (!(ofmt_ctx->oformat->flags & AVFMT_NOFILE))
    {
        ret = avio_open(&ofmt_ctx->pb, filename, AVIO_FLAG_WRITE);
        if (ret < 0)
        {
            av_log(NULL, AV_LOG_ERROR, "Could not open output file '%s'", filename);
            return ret;
        }
    }

    /* init muxer, write output file header */
    ret = avformat_write_header(ofmt_ctx, NULL);
    if (ret < 0)
    {
        av_log(NULL, AV_LOG_ERROR, "Error occurred when opening output file\n");
        return ret;
    }

    return 0;
}

///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////
void Demuxer::run()
{
    while (!m_stop)
    {
        if (maxWaitQueue > 0 && ctx->packetQueue.size() > maxWaitQueue)
        {
            usleep(100);
            continue;
        }

        AVPacket *packet = av_packet_alloc();
        int ret = av_read_frame(ctx->ifmt_ctx, packet);
        if (ret < 0)
        {
            fprintf(stderr, "av_read_frame Error: %d [%s]\n", ret, my_av_err2str(ret));
            break;
        }

        unsigned int stream_index = packet->stream_index;
        {
            std::lock_guard<std::mutex> lck(ctx->packetQueueMutex);
            ctx->packetQueue.push(packet);
        }
    }

    // 结束写个空
    AVPacket *packet = av_packet_alloc();
    packet->stream_index = -1;
    {
        std::lock_guard<std::mutex> lck(ctx->packetQueueMutex);
        ctx->packetQueue.push(packet);
    }

    // av_packet_free(&packet);
    printf("\n\n>>>demuxer end %d<<<\n\n", ctx->packetQueue.size());

    return;
}

///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////
void DecodeThread::run()
{
    int ret = -1;
    double pts = 0;

    while (!m_stop)
    {
        AVPacket *packet = nullptr;
        {
            std::lock_guard<std::mutex> lck(ctx->packetQueueMutex);
            if (!ctx->packetQueue.empty() || ctx->packetQueue.size() > 0)
            {
                packet = ctx->packetQueue.front();
                ctx->packetQueue.pop();
            }
        }

        if (!packet)
        {
            usleep(10);
            continue;
        }

        int stream_index = packet->stream_index;
        if (stream_index == -1)
        {
            printf("DecodeThread stream_index -1:: %d %d\n", ctx->packetQueue.size(), stream_index);
            break; // 处理完了
        }

        if (ctx->stream_ctx[stream_index].decoder_ctx)
        {
            StreamContext *streamCtx = &ctx->stream_ctx[stream_index];
            av_log(NULL, AV_LOG_DEBUG, "Going to reencode the frame\n");

            ret = avcodec_send_packet(streamCtx->decoder_ctx, packet);
            if (ret < 0)
            {
                av_log(NULL, AV_LOG_ERROR, "Decoding failed\n");
                break;
            }
            ctx->dec_pos = packet->pts; // 更新进度

            while (ret >= 0)
            {
                AVFrame *frame = av_frame_alloc();
                if (!frame)
                {
                    fprintf(stderr, "av_frame_allo Error\n");
                    exit(-1);
                }

                ret = avcodec_receive_frame(streamCtx->decoder_ctx, frame);
                if (ret == AVERROR_EOF || ret == AVERROR(EAGAIN))
                    continue;
                else if (ret < 0)
                {
                    fprintf(stderr, "Error during decoding\n");
                    goto end;
                }

                frame->pts = frame->best_effort_timestamp;
                // frame->pts = av_rescale_q(frame->pts, streamCtx->decoder_ctx->pkt_timebase, streamCtx->encoder_ctx->time_base);
                int64_t duration = ctx->duration;
                // printf("%f@@@@@@@@@@@@@%d %d width: %d height:%d\n", float(packet->pos) / float(duration), frame->pts, frame->best_effort_timestamp, frame->width, frame->height);
                if (frame->width == 0 || frame->height == 0)
                {
                    continue;
                }

                // frame ready
                if (ret == 0)
                {
                    MyAVFrame *myFrame = new MyAVFrame();
                    myFrame->enc_pkt = packet;
                    myFrame->data = frame;
                    myFrame->stream_index = stream_index;
                    myFrame->packet_pos = packet->pts;

                    std::lock_guard<std::mutex> lck(ctx->frameQueueMutex);
                    ctx->frameQueue.push(myFrame);
                }
                av_frame_unref(frame);
            }
        }
        else
        {
            printf("!!!!!!!!!!!!!!!!!!!!!!!!!!%d\n", stream_index);
            /* remux this frame without reencoding */
            // av_packet_rescale_ts(packet, ctx->ifmt_ctx->streams[stream_index]->time_base, ctx->ofmt_ctx->streams[stream_index]->time_base);
            // ret = av_interleaved_write_frame(ctx->ofmt_ctx, packet);
            // if (ret < 0)
            //     goto end;
        }

        // av_frame_free(&frame); // 下一环节free
        av_packet_free(&packet);
    }

end:
    int64_t pos = ctx->dec_pos;
    ctx->dec_pos = pos;
    printf("\n\n>>>DecodeThread end<<<\n\n");
    return;
}

///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////
int EncodeThread::flush_encoder(unsigned int stream_index)
{
    if (!(ctx->stream_ctx[stream_index].encoder_ctx->codec->capabilities & AV_CODEC_CAP_DELAY))
        return 0;

    av_log(NULL, AV_LOG_INFO, "Flushing stream #%u encoder\n", stream_index);
    /* flush encoder */
    // if ((ret = encode_write(dec_pkt, NULL)) < 0) {
    //     fprintf(stderr, "Failed to flush encoder %s\n", av_err2str(ret));
    //     goto end;
    // }
    return 0;
}

int EncodeThread::encode_write(AVCodecContext *encoder_ctx, AVPacket *enc_pkt, AVFrame *frame)
{
    int ret = 0;

    av_packet_unref(enc_pkt);

    if ((ret = avcodec_send_frame(encoder_ctx, frame)) < 0)
    {
        fprintf(stderr, "Error during encoding. Error code: %d %s\n", ret, my_av_err2str(ret));
        goto end;
    }

    while (1)
    {
        if (ret = avcodec_receive_packet(encoder_ctx, enc_pkt))
            break;
        enc_pkt->stream_index = 0;
        av_packet_rescale_ts(enc_pkt, encoder_ctx->time_base, ctx->ofmt_ctx->streams[0]->time_base);
        if ((ret = av_interleaved_write_frame(ctx->ofmt_ctx, enc_pkt)) < 0)
        {
            fprintf(stderr, "Error during writing data to output file. Error code: %s\n", my_av_err2str(ret));
            return ret;
        }
    }

end:
    if (ret == AVERROR_EOF)
        return 0;
    ret = ((ret == AVERROR(EAGAIN)) ? 0 : -1);
    return ret;
}

void EncodeThread::run()
{
    int ret = 0;

    while (!m_stop)
    {
        MyAVFrame *frame = nullptr;
        {
            std::lock_guard<std::mutex> lck(ctx->frameQueueMutex);
            if (!ctx->frameQueue.empty() || ctx->frameQueue.size() > 0)
            {
                frame = ctx->frameQueue.front();
                ctx->frameQueue.pop();
            }
        }
        if (!frame)
        {
            usleep(100);
            continue;
        }

        printf("EncodeThread: %d\n", ctx->frameQueue.size());

        if ((ret = encode_write(ctx->stream_ctx[ctx->video_stream_index].encoder_ctx, frame->enc_pkt, frame->data)) < 0)
            fprintf(stderr, "Error during encoding and writing.\n");

        ctx->enc_pos = frame->packet_pos; // 更新进度

        // av_frame_free(&frame->data);
        // if (frame->enc_pkt)
        // {
        //     av_packet_free(&frame->enc_pkt);
        //     frame->enc_pkt = NULL;
        // }
        // free(frame);
        // frame = NULL;
    }

end:
    /* flush filters and encoders */
    for (int i = 0; i < ctx->ifmt_ctx->nb_streams; i++)
    {
        /* flush filter */
        // if (!ctx->filter_ctx[i].filter_graph)
        //     continue;
        // ret = filter_encode_write_frame(NULL, i);
        // if (ret < 0)
        // {
        //     av_log(NULL, AV_LOG_ERROR, "Flushing filter failed\n");
        //     break;
        // }

        /* flush encoder */
        ret = flush_encoder(i);
        if (ret < 0)
        {
            av_log(NULL, AV_LOG_ERROR, "Flushing encoder failed\n");
            break;
        }
    }

    /* write the trailer for output stream */
    if ((ret = av_write_trailer(ctx->ofmt_ctx)) < 0)
        fprintf(stderr, "Failed to write trailer %s\n", my_av_err2str(ret));

    printf(">>>EncodeThread end<<<\n\n");

    return;
}

// int EncodeThread::encode_write_frame(unsigned int stream_index, int flush)
// {
//     StreamContext *stream = &ctx->stream_ctx[stream_index];
//     FilteringContext *filter = &ctx->filter_ctx[stream_index];
//     AVFrame *filt_frame = flush ? NULL : filter->filtered_frame;
//     AVPacket *enc_pkt = filter->enc_pkt;
//     int ret;

//     // av_log(NULL, AV_LOG_INFO, "Encoding frame\n");
//     /* encode filtered frame */
//     av_packet_unref(enc_pkt);

//     if (filt_frame && filt_frame->pts != AV_NOPTS_VALUE)
//         filt_frame->pts = av_rescale_q(filt_frame->pts, filt_frame->time_base,
//                                        stream->encoder_ctx->time_base);

//     ret = avcodec_send_frame(stream->encoder_ctx, filt_frame);

//     if (ret < 0)
//         return ret;

//     while (ret >= 0)
//     {
//         ret = avcodec_receive_packet(stream->encoder_ctx, enc_pkt);

//         if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
//             return 0;

//         /* prepare packet for muxing */
//         enc_pkt->stream_index = stream_index;
//         av_packet_rescale_ts(enc_pkt, stream->encoder_ctx->time_base, ctx->ofmt_ctx->streams[stream_index]->time_base);

//         av_log(NULL, AV_LOG_DEBUG, "Muxing frame\n");
//         /* mux encoded frame */
//         ret = av_interleaved_write_frame(ctx->ofmt_ctx, enc_pkt);
//     }

//     return ret;
// }

// int EncodeThread::filter_encode_write_frame(AVFrame *frame, unsigned int stream_index)
// {
//     FilteringContext *filter = &ctx->filter_ctx[stream_index];
//     int ret;

//     // av_log(NULL, AV_LOG_INFO, "Pushing decoded frame to filters\n");
//     /* push the decoded frame into the filtergraph */
//     ret = av_buffersrc_add_frame_flags(filter->buffersrc_ctx, frame, 0);
//     if (ret < 0)
//     {
//         av_log(NULL, AV_LOG_ERROR, "Error while feeding the filtergraph\n");
//         return ret;
//     }

//     /* pull filtered frames from the filtergraph */
//     while (1)
//     {
//         // av_log(NULL, AV_LOG_INFO, "Pulling filtered frame from filters\n");
//         ret = av_buffersink_get_frame(filter->buffersink_ctx,
//                                       filter->filtered_frame);
//         if (ret < 0)
//         {
//             /* if no more frames for output - returns AVERROR(EAGAIN)
//              * if flushed and no more frames for output - returns AVERROR_EOF
//              * rewrite retcode to 0 to show it as normal procedure completion
//              */
//             if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
//                 ret = 0;
//             break;
//         }

//         filter->filtered_frame->time_base = av_buffersink_get_time_base(filter->buffersink_ctx);
//         ;
//         filter->filtered_frame->pict_type = AV_PICTURE_TYPE_NONE;
//         ret = encode_write_frame(stream_index, 0);
//         av_frame_unref(filter->filtered_frame);
//         if (ret < 0)
//             break;
//     }

//     return ret;
// }

///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////

int transcode(const char *input_file, const char *output_file)
{
    printf("transcode %s %s\n", input_file, output_file);

    Context *ctx = new Context(input_file, output_file);
    int ret = ctx->init();
    if (ret)
    {
        return ret;
    }

    Demuxer de(ctx);
    EncodeThread enc(ctx);
    DecodeThread dec(ctx);

    de.start();
    dec.start();
    enc.start();

    while (!ctx->isStop)
    {
        usleep(100);
        continue;
    }

    printf("transcode end.\n");

    enc.stop();
    dec.stop();
    de.stop();

    return 0;
}

/*
g++ -std=c++11  transcode.cpp thread.cpp -I. -ID:/dev/av/ffmpeg-6.1-full_build-shared/include -LD:/dev/av/ffmpeg-6.1-full_build-shared/lib -lavcodec -lavformat -lswscale -lswresample -lavutil -lavfilter -o ../../cmd/a.exe

*/
// int main(int argc, char **argv)
// {
//     transcode("D:\\a.mp4", "D:\\a-1.mp4");
// }