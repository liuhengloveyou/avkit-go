/**
gcc remux.c -I. -ID:/dev/av/ffmpeg-6.1-full_build-shared/include -LD:/dev/av/ffmpeg-6.1-full_build-shared/lib -lavcodec -lavformat -lswscale -lswresample -lavutil -lavfilter -o ../../cmd/remux.exe

 */

#include <libavutil/timestamp.h>
#include <libavformat/avformat.h>
#include <libavfilter/avfilter.h>
#include <libavcodec/bsf.h>
#include "libswscale/swscale.h"

#include <time.h>
#include "unistd.h"

#include "progress.h"
#include "remux.h"

static time_t t1 = 0;

static AVBSFContext *bsf_ctx = NULL;

struct buffer_data
{
    uint8_t *ptr;
    size_t size; ///< size left in the buffer
};

Remuxer *newRemuxer(const char *in, const char *out, ProgressCBPtr cb)
{
    Remuxer *remuxer = (Remuxer *)malloc(sizeof(Remuxer));
    memset(remuxer, 0, sizeof(Remuxer));
    strcpy(remuxer->in_file, in);
    strcpy(remuxer->out_file, out);
    remuxer->progressCB = cb;

    return remuxer;
}

static void log_packet(const AVFormatContext *fmt_ctx, const AVPacket *pkt, const char *tag)
{
    AVRational *time_base = &fmt_ctx->streams[pkt->stream_index]->time_base;

    printf("%s: pts:%s pts_time:%s dts:%s dts_time:%s duration:%s duration_time:%s stream_index:%d\n",
           tag,
           av_ts2str(pkt->pts), av_ts2timestr(pkt->pts, time_base),
           av_ts2str(pkt->dts), av_ts2timestr(pkt->dts, time_base),
           av_ts2str(pkt->duration), av_ts2timestr(pkt->duration, time_base),
           pkt->stream_index);
}

static int read_packet(void *opaque, uint8_t *buf, int buf_size)
{
    struct buffer_data *bd = (struct buffer_data *)opaque;
    buf_size = FFMIN(buf_size, bd->size);

    if (!buf_size)
        return AVERROR_EOF;
    printf("ptr:%p size:%zu\n", bd->ptr, bd->size);

    /* copy internal buffer data to buf */
    memcpy(buf, bd->ptr, buf_size);
    bd->ptr += buf_size;
    bd->size -= buf_size;

    return buf_size;
}

int open_bitstream_filter(AVStream *stream, AVBSFContext **bsf_ctx, const char *name)
{
    int ret = 0;

    const AVBitStreamFilter *filter = av_bsf_get_by_name(name);
    if (!filter)
    {
        ret = -1;
        fprintf(stderr, "Unknow bitstream filter.\n");
    }
    if ((ret = av_bsf_alloc(filter, bsf_ctx) < 0))
    {
        fprintf(stderr, "av_bsf_alloc failed\n");
        return ret;
    }
    if ((ret = avcodec_parameters_copy((*bsf_ctx)->par_in, stream->codecpar)) < 0)
    {
        fprintf(stderr, "avcodec_parameters_copy failed, ret=%d\n", ret);
        return ret;
    }
    if ((ret = av_bsf_init(*bsf_ctx)) < 0)
    {
        fprintf(stderr, "av_bsf_init failed, ret=%d\n", ret);
        return ret;
    }

    return ret;
}

int64_t pts = 0;
int64_t dts = 0;

int filter_stream(AVBSFContext *bsf_ctx, AVFormatContext *ofmt_ctx, AVStream *in_stream, AVStream *out_stream, AVPacket *pkt)
{
    int ret = 0;
    if (ret = av_bsf_send_packet(bsf_ctx, pkt) < 0)
    {
        fprintf(stderr, "av_bsf_send_packet failed, ret=%d\n", ret);
        return ret;
    }

    while ((ret = av_bsf_receive_packet(bsf_ctx, pkt) == 0))
    {
        ret = av_interleaved_write_frame(ofmt_ctx, pkt);
        if (ret < 0)
            return ret;

        av_packet_unref(pkt);
    }

    if (ret == AVERROR(EAGAIN))
        return 0;

    return ret;
}

int remux(Remuxer *remuxer)
{
    const AVOutputFormat *ofmt = NULL;
    AVFormatContext *ifmt_ctx = NULL, *ofmt_ctx = NULL;
    AVDictionary *options = NULL;

    int ret, i;
    int stream_index = 0;
    int *stream_mapping = NULL;
    int stream_mapping_size = 0;
    size_t buffer_size, avio_ctx_buffer_size = 4096;

    int is_annexb = 1;
    int video_stream_index = -1;

    const char *in = remuxer->in_file;
    const char *out = remuxer->out_file;

    AVPacket *pkt = av_packet_alloc();
    if (!pkt)
    {
        fprintf(stderr, "Could not allocate AVPacket\n");
        return 1;
    }

    // 打开输入流
    AVDictionary *avformat_open_input_options = NULL;
    av_dict_set(&avformat_open_input_options, "rw_timeout", "2000000", 0); // 设置网络超时，当输入源为文件时可注释此行。
    av_dict_set(&avformat_open_input_options, "sdp_flags", "custom_io", 0);
    av_dict_set_int(&avformat_open_input_options, "reorder_queue_size", 0, 0);
    av_dict_set(&avformat_open_input_options, "rtsp_transport", "tcp", 0); // 如果以tcp方式打开将udp替换为tcp

    if ((ret = avformat_open_input(&ifmt_ctx, in, 0, &avformat_open_input_options)) < 0)
    {
        fprintf(stderr, "Could not open input file '%s'", in);
        goto end;
    }

    if ((ret = avformat_find_stream_info(ifmt_ctx, 0)) < 0)
    {
        fprintf(stderr, "Failed to retrieve input stream information");
        goto end;
    }

    av_dump_format(ifmt_ctx, 0, in, 0);

    if (remuxer->duration == 0)
    {
        remuxer->duration = ifmt_ctx->duration * av_q2d(AV_TIME_BASE_Q);
    }

    time_t t2 = time(NULL);
    printf("t2 = %ld\n", t2 - t1);

    // 打开输出流
    avformat_alloc_output_context2(&ofmt_ctx, NULL, NULL, out);
    if (!ofmt_ctx)
    {
        fprintf(stderr, "Could not create output context\n");
        ret = AVERROR_UNKNOWN;
        goto end;
    }
    ofmt = ofmt_ctx->oformat;

    stream_mapping_size = ifmt_ctx->nb_streams;
    stream_mapping = av_calloc(stream_mapping_size, sizeof(*stream_mapping));
    if (!stream_mapping)
    {
        ret = AVERROR(ENOMEM);
        goto end;
    }

    for (i = 0; i < ifmt_ctx->nb_streams; i++)
    {
        AVStream *out_stream;
        AVStream *in_stream = ifmt_ctx->streams[i];
        AVCodecParameters *in_codecpar = in_stream->codecpar;

        if (in_codecpar->codec_type != AVMEDIA_TYPE_AUDIO &&
            in_codecpar->codec_type != AVMEDIA_TYPE_VIDEO &&
            in_codecpar->codec_type != AVMEDIA_TYPE_SUBTITLE)
        {
            stream_mapping[i] = -1;
            continue;
        }

        stream_mapping[i] = stream_index++;

        out_stream = avformat_new_stream(ofmt_ctx, NULL);
        if (!out_stream)
        {
            fprintf(stderr, "Failed allocating output stream\n");
            ret = AVERROR_UNKNOWN;
            goto end;
        }

        ret = avcodec_parameters_copy(out_stream->codecpar, in_codecpar);
        if (ret < 0)
        {
            fprintf(stderr, "Failed to copy codec parameters\n");
            goto end;
        }
        out_stream->codecpar->codec_tag = 0;

        if (ifmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
        {
            video_stream_index = i;
            is_annexb = strcmp(av_fourcc2str(ifmt_ctx->streams[i]->codecpar->codec_tag), "avc1") >= 0 ? 1 : 0;
        }
    }
    av_dump_format(ofmt_ctx, 0, out, 1);

    if (!(ofmt->flags & AVFMT_NOFILE))
    {
        if ((ret = avio_open2(&ofmt_ctx->pb, out, AVIO_FLAG_WRITE, NULL, NULL)) < 0)
        {
            fprintf(stderr, "Could not open output file '%s'", out);
            goto end;
        }
    }

    if (video_stream_index == -1)
    {
        fprintf(stderr, "no video stream found.\n");
        goto end;
    }

    if (is_annexb)
    {
        if (ret = open_bitstream_filter(ifmt_ctx->streams[video_stream_index], &bsf_ctx, "h264_mp4toannexb") < 0)
        {
            fprintf(stderr, "open_bitstream_filter failed, ret=%d\n", ret);
            goto end;
        }
    }

    // if ((ret = av_dict_set(&options, "hls_time", "100", 0)) < 0)
    // {
    //     fprintf(stderr, "Failed to set listen mode for server: %s\n", av_err2str(ret));
    //     return ret;
    // }
    // if ((ret = av_dict_set(&options, "hls_list_size", "0", 0)) < 0)
    // {
    //     fprintf(stderr, "Failed to set listen mode for server: %s\n", av_err2str(ret));
    //     return ret;
    // }
    // if ((ret = av_dict_set(&options, "hls_wrap", "0", 0)) < 0)
    // {
    //     fprintf(stderr, "Failed to set listen mode for server: %s\n", av_err2str(ret));
    //     return ret;
    // }

    ret = avformat_write_header(ofmt_ctx, &options);
    if (ret < 0)
    {
        fprintf(stderr, "Error occurred when opening output file\n");
        goto end;
    }

    while (!remuxer->isStop)
    {
        AVStream *in_stream, *out_stream;

        ret = av_read_frame(ifmt_ctx, pkt);
        if (ret < 0)
            break;

        // 进度
        remuxer->pos = (float)pkt->pts * av_q2d(ifmt_ctx->streams[pkt->stream_index]->time_base);
        if (remuxer->progressCB)
        {
            remuxer->progressCB("remuxProgress", remuxer->in_file, remuxer->duration, remuxer->pos);
        }

        in_stream = ifmt_ctx->streams[pkt->stream_index];
        if (pkt->stream_index >= stream_mapping_size || stream_mapping[pkt->stream_index] < 0)
        {
            av_packet_unref(pkt);
            continue;
        }

        pkt->stream_index = stream_mapping[pkt->stream_index];
        out_stream = ofmt_ctx->streams[pkt->stream_index];
        // log_packet(ifmt_ctx, pkt, "in");

        time_t t4 = time(NULL);
         // 转换PTS/DTS
        pkt->pts = av_rescale_q_rnd(pkt->pts, in_stream->time_base, out_stream->time_base, (enum AVRounding) (AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX));
        pkt->dts = av_rescale_q_rnd(pkt->dts, in_stream->time_base, out_stream->time_base, (enum AVRounding) (AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX));
        pkt->duration = av_rescale_q(pkt->duration, in_stream->time_base, out_stream->time_base);

        if (video_stream_index == pkt->stream_index && is_annexb)
        {
            ret = filter_stream(bsf_ctx, ofmt_ctx, in_stream, out_stream, pkt);
        }
        else
        {
            // /* copy packet */
            // av_packet_rescale_ts(pkt, in_stream->time_base, out_stream->time_base);
            // pkt->pos = -1;
            // log_packet(ofmt_ctx, pkt, "out");
            ret = av_interleaved_write_frame(ofmt_ctx, pkt);
        }

        // ret = av_interleaved_write_frame(ofmt_ctx, pkt);
        /* pkt is now blank (av_interleaved_write_frame() takes ownership of
         * its contents and resets pkt), so that no unreferencing is necessary.
         * This would be different if one used av_write_frame(). */
        if (ret < 0)
        {
            fprintf(stderr, "Error muxing packet\n");
            break;
        }
    }

    av_write_trailer(ofmt_ctx);

end:
    av_packet_free(&pkt);
    avformat_close_input(&ifmt_ctx);

    /* close output */
    if (ofmt_ctx && !(ofmt->flags & AVFMT_NOFILE))
    {
        avio_closep(&ofmt_ctx->pb);
    }
    avformat_free_context(ofmt_ctx);
    av_freep(&stream_mapping);

    if (ret < 0 && ret != AVERROR_EOF)
    {
        fprintf(stderr, "Error occurred: %s\n", av_err2str(ret));
        return -1;
    }

    return 0;
}

// int main(int argc, char **argv)
// {
//     // av_log_set_level(AV_LOG_DEBUG);

//     t1 = time(NULL);
//     printf("start = %ld\n", t1);

//     // rtsp://admin:HuaWei123@192.168.100.237/LiveMedia/ch1/Media1
//     remux("rtsp://admin:qwer1234@172.29.251.10:554/h264/ch33/main/av_stream", "D:\\aaa\\a.m3u8");

//     // remux("D:\\a.mp4", "D:\\a.mkv");
// }
