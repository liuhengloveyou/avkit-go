#include "metadata.h"

metaData_t *get_metadata(const char *fn)
{
    int ret = 0;
    AVFormatContext *fmt_ctx = NULL;
    // const AVDictionaryEntry *tag = NULL;

    if (!fn)
    {
        return NULL;
    }

    if ((ret = avformat_open_input(&fmt_ctx, fn, NULL, NULL)))
    {
        fprintf(stderr, "avformat_open_input ERR: %s\n", av_err2str(ret));
        return NULL;
    }

    if ((ret = avformat_find_stream_info(fmt_ctx, NULL)) < 0)
    {
        fprintf(stderr, "avformat_find_stream_info ERR: %s\n", av_err2str(ret));
        return NULL;
    }

    // while ((tag = av_dict_iterate(fmt_ctx->metadata, tag)))
    //     printf("%s=%s\n", tag->key, tag->value);

    int stream_index = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (stream_index < 0)
    {
        fprintf(stderr, "Could not find video stream in input file '%s'\n", fn);
        return NULL;
    }

    AVCodecParameters *local_codec_parameters = fmt_ctx->streams[stream_index]->codecpar;
    double totle_seconds = fmt_ctx->duration * av_q2d(AV_TIME_BASE_Q);

    metaData_t *m = (metaData_t *)malloc(sizeof(metaData_t));
    memset(m, 0, sizeof(metaData_t));
    m->width = local_codec_parameters->width;
    m->height = local_codec_parameters->height;
    m->bit_rate = fmt_ctx->bit_rate;
    m->duration = fmt_ctx->duration;
    m->totle_seconds = totle_seconds;
    memcpy(m->fn, fn, strlen(fn));
    // printf("resolution %d x %d\n", local_codec_parameters->width, local_codec_parameters->height);
    // printf("duration=%I64d\n", fmt_ctx->duration);
    // printf("bit_rate=%I64d\n", fmt_ctx->bit_rate);
    // printf("totle_seconds=%f\n", totle_seconds);

    avformat_close_input(&fmt_ctx);
    return m;
}
