/*****************************************************************************
 * libav_input.c
 *****************************************************************************
 * Copyright (C) 2012 L-SMASH Works project
 *
 * Authors: Yusuke Nakamura <muken.the.vfrmaniac@gmail.com>
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *****************************************************************************/

/* This file is available under an ISC license.
 * However, when distributing its binary file, it will be under LGPL or GPL.
 * Don't distribute it if its license is GPL. */

/* Libav
 * The binary file will be LGPLed or GPLed. */
#include <libavformat/avformat.h>   /* Demuxer */
#include <libavcodec/avcodec.h>     /* Decoder */
#include <libswscale/swscale.h>     /* Colorspace converter */
#include <libavutil/mathematics.h>  /* Timebase rescaler */

#include "lsmashinput.h"
#include "colorspace.h"

#define DECODER_DELAY( ctx ) (ctx->has_b_frames + ((ctx->active_thread_type & FF_THREAD_FRAME) ? ctx->thread_count - 1 : 0))

#define SEEK_MODE_NORMAL     0
#define SEEK_MODE_UNSAFE     1
#define SEEK_MODE_AGGRESSIVE 2

#define SEEK_DTS_BASED         0x00000001
#define SEEK_PTS_BASED         0x00000002
#define SEEK_FILE_OFFSET_BASED 0x00000004

typedef enum
{
    DECODE_REQUIRE_INITIAL = 0,
    DECODE_INITIALIZING    = 1,
    DECODE_INITIALIZED     = 2
} decode_status_t;

typedef struct
{
    uint8_t  keyframe;
    uint8_t  is_leading;
    uint32_t sample_number;
    int64_t  pts;
    int64_t  dts;
    int64_t  file_offset;
} video_frame_info_t;

typedef struct
{
    int      length;
    uint32_t sample_number;
    int64_t  pts;
    int64_t  dts;
    int64_t  file_offset;
} audio_frame_info_t;

typedef struct libav_handler_tag
{
    char                    *file_name;
    int                      threads;
    /* Video stuff */
    int                      video_index;
    AVFormatContext         *video_format;
    AVCodecContext          *video_ctx;
    struct SwsContext       *sws_ctx;
    uint32_t                 video_frame_count;
    uint32_t                 last_video_frame_number;
    uint32_t                 last_rap_number;
    uint32_t                 delay_count;
    uint32_t                 first_valid_video_frame_number;
    uint32_t                 first_valid_video_frame_size;
    uint8_t                 *first_valid_video_frame_data;
    decode_status_t          decode_status;
    video_frame_info_t      *video_frame_list;      /* stored in presentation order */
    uint8_t                 *keyframe_list;         /* stored in decoding order */
    int                      video_seek_base;
    int                      seek_mode;
    func_convert_colorspace *convert_colorspace;
    /* Audio stuff */
    int                      audio_index;
    AVFormatContext         *audio_format;
    AVCodecContext          *audio_ctx;
    AVCodecParserContext    *audio_parser;
    AVIndexEntry            *audio_index_entries;
    int                      audio_index_entries_count;
    uint8_t                 *audio_output_buffer;
    uint32_t                 audio_frame_count;
    uint32_t                 audio_frame_length;
    audio_frame_info_t      *audio_frame_list;
    int                      audio_seek_base;
    uint32_t                 next_audio_pcm_sample_number;
    uint32_t                 last_audio_frame_number;
    uint32_t                 last_remainder_size;
    int64_t                  av_gap;
} libav_handler_t;

static int lavf_open_file( AVFormatContext **format_ctx, char *file_name )
{
    if( avformat_open_input( format_ctx, file_name, NULL, NULL ) )
    {
        DEBUG_MESSAGE_BOX_DESKTOP( MB_ICONERROR | MB_OK, "Failed to avformat_open_input." );
        return -1;
    }
    if( avformat_find_stream_info( *format_ctx, NULL ) < 0 )
    {
        DEBUG_MESSAGE_BOX_DESKTOP( MB_ICONERROR | MB_OK, "Failed to avformat_find_stream_info." );
        return -1;
    }
    return 0;
}

static int get_first_track_of_type( libav_handler_t *hp, AVFormatContext *format_ctx, uint32_t type )
{
    /* libavformat */
    int index;
    for( index = 0; index < format_ctx->nb_streams && format_ctx->streams[index]->codec->codec_type != type; index++ );
    if( index == format_ctx->nb_streams )
    {
        DEBUG_MESSAGE_BOX_DESKTOP( MB_ICONERROR | MB_OK, "Failed to find stream by libavformat." );
        return 0;
    }
    /* libavcodec */
    AVCodecContext *ctx = format_ctx->streams[index]->codec;
    if( type == AVMEDIA_TYPE_VIDEO )
    {
        hp->video_ctx   = ctx;
        hp->video_index = index;
    }
    else
    {
        hp->audio_ctx   = ctx;
        hp->audio_index = index;
    }
    AVCodec *codec = avcodec_find_decoder( ctx->codec_id );
    if( !codec )
    {
        DEBUG_MESSAGE_BOX_DESKTOP( MB_ICONERROR | MB_OK, "Failed to find %s decoder.", codec->name );
        return 0;
    }
    ctx->thread_count = hp->threads;
    if( avcodec_open2( ctx, codec, NULL ) < 0 )
    {
        DEBUG_MESSAGE_BOX_DESKTOP( MB_ICONERROR | MB_OK, "Failed to avcodec_open2." );
        return 0;
    }
    return 1;
}

static inline int check_frame_reordering( video_frame_info_t *info, uint32_t sample_count )
{
    for( uint32_t i = 1; i < sample_count; i++ )
        if( info[i].pts < info[i - 1].pts )
            return 1;
    return 0;
}

static int compare_pts( const video_frame_info_t *a, const video_frame_info_t *b )
{
    int64_t diff = (int64_t)(a->pts - b->pts);
    return diff > 0 ? 1 : (diff == 0 ? 0 : -1);
}

static inline void sort_presentation_order( video_frame_info_t *info, uint32_t sample_count )
{
    qsort( info, sample_count, sizeof(video_frame_info_t), (int(*)( const void *, const void * ))compare_pts );
}

static void decide_video_seek_method( libav_handler_t *hp, uint32_t sample_count )
{
    hp->video_seek_base = !strcmp( hp->video_format->iformat->name, "mpeg" ) || !strcmp( hp->video_format->iformat->name, "mpegts" )
                        ? SEEK_DTS_BASED | SEEK_PTS_BASED | SEEK_FILE_OFFSET_BASED
                        : SEEK_DTS_BASED | SEEK_PTS_BASED;
    video_frame_info_t *info = hp->video_frame_list;
    for( uint32_t i = 1; i <= sample_count; i++ )
        if( info[i].pts == AV_NOPTS_VALUE )
        {
            hp->video_seek_base &= ~SEEK_PTS_BASED;
            break;
        }
    for( uint32_t i = 1; i <= sample_count; i++ )
        if( info[i].dts == AV_NOPTS_VALUE )
        {
            hp->video_seek_base &= ~SEEK_DTS_BASED;
            break;
        }
    if( hp->video_seek_base & SEEK_FILE_OFFSET_BASED )
    {
        if( hp->video_format->iformat->flags & AVFMT_NO_BYTE_SEEK )
            hp->video_seek_base &= ~SEEK_FILE_OFFSET_BASED;
        else
            for( uint32_t i = 1; i <= sample_count; i++ )
                if( info[i].file_offset == -1 )
                {
                    hp->video_seek_base &= ~SEEK_FILE_OFFSET_BASED;
                    break;
                }
    }
    if( hp->video_seek_base & SEEK_PTS_BASED )
    {
        if( check_frame_reordering( info, sample_count ) )
            /* Consider presentation order for keyframe detection.
             * Note: sample number is 1-origin. */
            sort_presentation_order( &info[1], sample_count );
    }
    else if( hp->video_seek_base & SEEK_DTS_BASED )
        for( uint32_t i = 1; i <= sample_count; i++ )
            info[i].pts = info[i].dts;
}

static void decide_audio_seek_method( libav_handler_t *hp, uint32_t sample_count )
{
    hp->audio_seek_base = !strcmp( hp->audio_format->iformat->name, "mpeg" ) || !strcmp( hp->audio_format->iformat->name, "mpegts" )
                        ? SEEK_DTS_BASED | SEEK_PTS_BASED | SEEK_FILE_OFFSET_BASED
                        : SEEK_DTS_BASED | SEEK_PTS_BASED;
    audio_frame_info_t *info = hp->audio_frame_list;
    for( uint32_t i = 1; i <= sample_count; i++ )
        if( info[i].pts == AV_NOPTS_VALUE )
        {
            hp->audio_seek_base &= ~SEEK_PTS_BASED;
            break;
        }
    for( uint32_t i = 1; i <= sample_count; i++ )
        if( info[i].dts == AV_NOPTS_VALUE )
        {
            hp->audio_seek_base &= ~SEEK_DTS_BASED;
            break;
        }
    if( hp->audio_seek_base & SEEK_FILE_OFFSET_BASED )
    {
        if( hp->audio_format->iformat->flags & AVFMT_NO_BYTE_SEEK )
            hp->audio_seek_base &= ~SEEK_FILE_OFFSET_BASED;
        else
        {
            uint32_t error_count = 0;
            for( uint32_t i = 1; i <= sample_count; i++ )
                error_count += (info[i].file_offset == -1);
            if( error_count == sample_count )
                hp->audio_seek_base &= ~SEEK_FILE_OFFSET_BASED;
        }
    }
    if( !(hp->audio_seek_base & SEEK_PTS_BASED) && (hp->audio_seek_base & SEEK_DTS_BASED) )
        for( uint32_t i = 1; i <= sample_count; i++ )
            info[i].pts = info[i].dts;
}

static void create_index( libav_handler_t *hp )
{
    uint32_t video_info_count = 1 << 16;
    uint32_t audio_info_count = 1 << 16;
    video_frame_info_t *video_info = NULL;
    audio_frame_info_t *audio_info = NULL;
    int read_video = (hp->video_index >= 0);
    int read_audio = (hp->audio_index >= 0);
    if( read_video )
    {
        video_info = malloc_zero( video_info_count * sizeof(video_frame_info_t) );
        if( !video_info )
            return;
    }
    if( read_audio )
    {
        audio_info = malloc_zero( audio_info_count * sizeof(audio_frame_info_t) );
        if( !audio_info )
        {
            if( video_info )
                free( video_info );
            return;
        }
        hp->audio_output_buffer = av_mallocz( AVCODEC_MAX_AUDIO_FRAME_SIZE );
        if( !hp->audio_output_buffer )
        {
            DEBUG_AUDIO_MESSAGE_BOX_DESKTOP( MB_ICONERROR | MB_OK, "Failed to allocate memory to the output buffer for audio." );
            if( video_info )
                free( video_info );
            if( audio_info )
                free( audio_info );
            return;
        }
        if( hp->audio_ctx->frame_size == 0 )
        {
            hp->audio_parser = av_parser_init( hp->audio_ctx->codec_id );
            if( hp->audio_parser )
                hp->audio_parser->flags = PARSER_FLAG_COMPLETE_FRAMES;
        }
    }
    AVPacket pkt;
    av_init_packet( &pkt );
    uint32_t video_sample_count    = 0;
    int64_t  last_keyframe_pts     = INT64_MIN;
    uint32_t audio_sample_count    = 0;
    int      constant_frame_length = 1;
    int      frame_length          = 0;
    uint64_t audio_duration        = 0;
    AVFormatContext *format_ctx    = hp->video_format;
    /* av_read_frame() obtains exactly one frame. */
    while( av_read_frame( format_ctx, &pkt ) >= 0 )
    {
        if( read_video && pkt.stream_index == hp->video_index )
        {
            ++video_sample_count;
            video_info[video_sample_count].pts           = pkt.pts;
            video_info[video_sample_count].dts           = pkt.dts;
            video_info[video_sample_count].file_offset   = pkt.pos;
            video_info[video_sample_count].sample_number = video_sample_count;
            if( pkt.pts < last_keyframe_pts )
                video_info[video_sample_count].is_leading = 1;
            if( pkt.flags & AV_PKT_FLAG_KEY )
            {
                video_info[video_sample_count].keyframe = 1;
                last_keyframe_pts = pkt.pts;
            }
            if( video_sample_count == video_info_count )
            {
                video_info_count <<= 1;
                video_frame_info_t *temp = realloc( video_info, video_info_count * sizeof(video_frame_info_t) );
                if( !temp )
                {
                    av_free_packet( &pkt );
                    if( video_info )
                        free( video_info );
                    if( audio_info )
                        free( audio_info );
                    return;
                }
                video_info = temp;
            }
        }
        else if( read_audio && pkt.stream_index == hp->audio_index && audio_duration <= INT32_MAX )
        {
            /* Get frame_length. */
            if( hp->audio_parser )
            {
                uint8_t *out_buffer;
                int out_buffer_size;
                av_parser_parse2( hp->audio_parser, hp->audio_ctx,
                                  &out_buffer, &out_buffer_size,
                                  pkt.data, pkt.size, pkt.pts, pkt.dts, pkt.pos );
            }
            frame_length = hp->audio_ctx->frame_size                        ? hp->audio_ctx->frame_size
                         : (hp->audio_parser && hp->audio_parser->duration) ? hp->audio_parser->duration
                         :                                                    pkt.duration;
            if( frame_length == 0 )
            {
                AVPacket temp = pkt;
                uint8_t *data = pkt.data;
                while( temp.size > 0 )
                {
                    int output_buffer_size = AVCODEC_MAX_AUDIO_FRAME_SIZE;
                    int wasted_data_length = avcodec_decode_audio3( hp->audio_ctx, (int16_t *)hp->audio_output_buffer, &output_buffer_size, &temp );
                    if( wasted_data_length <= 0 )
                        break;
                    temp.size -= wasted_data_length;
                    temp.data += wasted_data_length;
                    if( output_buffer_size > 0 )
                        frame_length += output_buffer_size / (av_get_bytes_per_sample( hp->audio_ctx->sample_fmt ) * hp->audio_ctx->channels);
                }
                pkt      = temp;
                pkt.data = data;
            }
            audio_duration += frame_length;
            if( audio_duration > INT32_MAX )
            {
                av_free_packet( &pkt );
                continue;
            }
            /* Set up audio frame info. */
            ++audio_sample_count;
            audio_info[audio_sample_count].pts         = pkt.pts;
            audio_info[audio_sample_count].dts         = pkt.dts;
            audio_info[audio_sample_count].file_offset = pkt.pos;
            audio_info[audio_sample_count].length      = frame_length;
            if( audio_sample_count > 1 && audio_info[audio_sample_count].length != audio_info[audio_sample_count - 1].length )
                constant_frame_length = 0;
            if( audio_sample_count == audio_info_count )
            {
                audio_info_count <<= 1;
                audio_frame_info_t *temp = realloc( audio_info, audio_info_count * sizeof(audio_frame_info_t) );
                if( !temp )
                {
                    av_free_packet( &pkt );
                    if( video_info )
                        free( video_info );
                    if( audio_info )
                        free( audio_info );
                    return;
                }
                audio_info = temp;
            }
        }
        av_free_packet( &pkt );
    }
    if( read_video )
    {
        hp->keyframe_list = malloc( (video_sample_count + 1) * sizeof(uint8_t) );
        if( !hp->keyframe_list )
        {
            if( video_info )
                free( video_info );
            if( audio_info )
                free( audio_info );
            return;
        }
        for( uint32_t i = 0; i <= video_sample_count; i++ )
            hp->keyframe_list[i] = video_info[i].keyframe;
        hp->video_frame_list = video_info;
        hp->video_frame_count = video_sample_count;
        decide_video_seek_method( hp, video_sample_count );
    }
    if( read_audio )
    {
        hp->audio_frame_length = constant_frame_length ? frame_length : 0;
        hp->audio_frame_list   = audio_info;
        hp->audio_frame_count  = audio_sample_count;
        decide_audio_seek_method( hp, audio_sample_count );
        AVStream *audio_stream = format_ctx->streams[ hp->audio_index ];
        if( audio_stream->nb_index_entries > 0 )
        {
            /* Audio stream in matroska container requires index_entries for seeking.
             * This avoids for re-reading the file to create index_entries since the file will be closed once. */
            hp->audio_index_entries = av_malloc( audio_stream->index_entries_allocated_size );
            if( !hp->audio_index_entries )
            {
                if( video_info )
                    free( video_info );
                if( audio_info )
                    free( audio_info );
                return;
            }
            for( int i = 0; i < audio_stream->nb_index_entries; i++ )
                hp->audio_index_entries[i] = audio_stream->index_entries[i];
            hp->audio_index_entries_count = audio_stream->nb_index_entries;
        }
        if( read_video )
        {
            AVRational video_time_base = format_ctx->streams[ hp->video_index ]->time_base;
            AVRational audio_time_base = audio_stream->time_base;
            if( hp->video_seek_base & SEEK_PTS_BASED )
                hp->av_gap = audio_info[1].pts - av_rescale_q( video_info[1].pts, video_time_base, audio_time_base );
            else if( hp->video_seek_base & SEEK_DTS_BASED )
                hp->av_gap = audio_info[1].dts - av_rescale_q( video_info[1].dts, video_time_base, audio_time_base );
            if( hp->av_gap )
                hp->av_gap = av_rescale_q( hp->av_gap, audio_time_base, (AVRational){ 1, audio_stream->codec->sample_rate } );
        }
    }
}

static void *open_file( char *file_name, int threads )
{
    libav_handler_t *hp = malloc_zero( sizeof(libav_handler_t) );
    if( !hp )
        return NULL;
    /* Open file and create the index. */
    av_register_all();
    avcodec_register_all();
    AVFormatContext *format_ctx = NULL;
    if( lavf_open_file( &format_ctx, file_name ) )
    {
        if( format_ctx )
            avformat_close_input( &format_ctx );
        free( hp );
        return NULL;
    }
    hp->video_format = format_ctx;
    hp->audio_format = format_ctx;
    hp->file_name    = file_name;
    hp->threads      = threads;
    int video_present = get_first_track_of_type( hp, hp->video_format, AVMEDIA_TYPE_VIDEO );
    int audio_present = get_first_track_of_type( hp, hp->audio_format, AVMEDIA_TYPE_AUDIO );
    if( !video_present && !audio_present )
    {
        if( hp->video_ctx )
            avcodec_close( hp->video_ctx );
        if( hp->audio_ctx )
            avcodec_close( hp->audio_ctx );
        avformat_close_input( &format_ctx );
        free( hp );
        return NULL;
    }
    if( !video_present )
        hp->video_index = -1;
    if( !audio_present )
        hp->audio_index = -1;

    create_index( hp );
    /* Close file.
     * By opening file for video and audio separately, indecent work about frame reading can be avoidable. */
    if( hp->video_ctx )
    {
        avcodec_close( hp->video_ctx );
        hp->video_ctx = NULL;
    }
    if( hp->audio_ctx )
    {
        avcodec_close( hp->audio_ctx );
        hp->audio_ctx = NULL;
    }
    hp->video_format = NULL;
    hp->audio_format = NULL;
    avformat_close_input( &format_ctx );
    return hp;
}

static int get_first_video_track( lsmash_handler_t *h, int seek_mode )
{
    libav_handler_t *hp = (libav_handler_t *)h->video_private;
    if( hp->video_index < 0
     || hp->video_frame_count == 0
     || lavf_open_file( &hp->video_format, hp->file_name )
     || !get_first_track_of_type( hp, hp->video_format, AVMEDIA_TYPE_VIDEO ) )
    {
        if( hp->video_frame_list )
        {
            free( hp->video_frame_list );
            hp->video_frame_list = NULL;
        }
        if( hp->keyframe_list )
        {
            free( hp->keyframe_list );
            hp->keyframe_list = NULL;
        }
        if( hp->video_ctx )
        {
            avcodec_close( hp->video_ctx );
            hp->video_ctx = NULL;
        }
        if( hp->video_format )
        {
            avformat_close_input( &hp->video_format );
            hp->video_format = NULL;
        }
        return -1;
    }
    hp->seek_mode = seek_mode;
    return 0;
}

static int get_first_audio_track( lsmash_handler_t *h )
{
    libav_handler_t *hp = (libav_handler_t *)h->audio_private;
    if( hp->audio_index < 0
     || hp->audio_frame_count == 0
     || lavf_open_file( &hp->audio_format, hp->file_name )
     || !get_first_track_of_type( hp, hp->audio_format, AVMEDIA_TYPE_AUDIO ) )
    {
        if( hp->audio_index_entries )
        {
            av_free( hp->audio_index_entries );
            hp->audio_index_entries = NULL;
        }
        if( hp->audio_output_buffer )
        {
            av_free( hp->audio_output_buffer );
            hp->audio_output_buffer = NULL;
        }
        if( hp->audio_frame_list )
        {
            free( hp->audio_frame_list );
            hp->audio_frame_list = NULL;
        }
        if( hp->audio_ctx )
        {
            avcodec_close( hp->audio_ctx );
            hp->audio_ctx = NULL;
        }
        if( hp->audio_format )
        {
            avformat_close_input( &hp->audio_format );
            hp->audio_format = NULL;
        }
        return -1;
    }
    return 0;
}

static inline uint64_t get_gcd( uint64_t a, uint64_t b )
{
    if( !b )
        return a;
    while( 1 )
    {
        uint64_t c = a % b;
        if( !c )
            return b;
        a = b;
        b = c;
    }
}

static inline uint64_t reduce_fraction( uint64_t *a, uint64_t *b )
{
    uint64_t reduce = get_gcd( *a, *b );
    *a /= reduce;
    *b /= reduce;
    return reduce;
}

static inline double sigexp10( double value, double *exponent )
{
    /* This function separates significand and exp10 from double floating point. */
    *exponent = 1;
    while( value < 1 )
    {
        value *= 10;
        *exponent /= 10;
    }
    while( value >= 10 )
    {
        value /= 10;
        *exponent *= 10;
    }
    return value;
}

static int try_ntsc_framerate( lsmash_handler_t *h, double fps )
{
#define DOUBLE_EPSILON 5e-5
    if( fps == 0 )
        return 0;
    double exponent;
    double fps_sig = sigexp10( fps, &exponent );
    uint64_t fps_den, fps_num;
    int i = 1;
    while( 1 )
    {
        fps_den = i * 1001;
        fps_num = round( fps_den * fps_sig ) * exponent;
        if( fps_num > UINT32_MAX )
            return 0;
        if( fabs( ((double)fps_num / fps_den) / exponent - fps_sig ) < DOUBLE_EPSILON )
            break;
        ++i;
    }
    h->framerate_num = fps_num;
    h->framerate_den = fps_den;
    return 1;
#undef DOUBLE_EPSILON
}

static void setup_timestamp_info( lsmash_handler_t *h )
{
    libav_handler_t *hp = (libav_handler_t *)h->video_private;
    AVStream *video_stream = hp->video_format->streams[ hp->video_index ];
    if( h->video_sample_count == 1 || !(hp->video_seek_base & (SEEK_DTS_BASED | SEEK_PTS_BASED)) )
    {
        h->framerate_num = video_stream->r_frame_rate.num;
        h->framerate_den = video_stream->r_frame_rate.den;
        return;
    }
    video_frame_info_t *info = hp->video_frame_list;
    uint64_t largest_pts           = info[2].pts;
    uint64_t second_largest_pts    = info[1].pts;
    uint64_t presentation_timebase = info[2].pts - info[1].pts;
    for( uint32_t i = 3; i <= h->video_sample_count; i++ )
    {
        if( info[i].pts == info[i - 1].pts )
        {
            MESSAGE_BOX_DESKTOP( MB_OK, "Detected PTS duplication at frame %"PRIu32, i );
            return;
        }
        presentation_timebase = get_gcd( presentation_timebase, info[i].pts - info[i - 1].pts );
        second_largest_pts = largest_pts;
        largest_pts = info[i].pts;
    }
    presentation_timebase *= video_stream->time_base.num;
    uint64_t media_timescale = video_stream->time_base.den;
    uint64_t reduce = reduce_fraction( &media_timescale, &presentation_timebase );
    uint64_t presentation_duration = ((largest_pts - info[1].pts) + (largest_pts - second_largest_pts)) / reduce;
    double presentation_framerate = h->video_sample_count * ((double)media_timescale / presentation_duration);
    if( try_ntsc_framerate( h, presentation_framerate ) )
        return;
    h->framerate_num = presentation_framerate * presentation_timebase + 0.5;
    h->framerate_den = presentation_timebase;
}

static int get_sample( AVFormatContext *format_ctx, int stream_index, AVPacket *pkt )
{
    av_init_packet( pkt );
    while( av_read_frame( format_ctx, pkt ) >= 0 )
    {
        if( pkt->stream_index != stream_index )
        {
            av_free_packet( pkt );
            continue;
        }
        return 0;
    }
    return 1;
}

static int prepare_video_decoding( lsmash_handler_t *h, video_option_t *opt )
{
    libav_handler_t *hp = (libav_handler_t *)h->video_private;
    if( !hp->video_ctx )
        return 0;
    h->video_sample_count = hp->video_frame_count;
    setup_timestamp_info( h );
    /* swscale */
    int output_pixel_format;
    output_colorspace_index index = determine_colorspace_conversion( &hp->video_ctx->pix_fmt, &output_pixel_format );
    static const struct
    {
        func_convert_colorspace *convert_colorspace;
        int                      pixel_size;
        output_colorspace_tag    compression;
    } colorspace_table[3] =
        {
            { to_yuv16le_to_yc48, YC48_SIZE,  OUTPUT_TAG_YC48 },
            { to_rgb24,           RGB24_SIZE, OUTPUT_TAG_RGB  },
            { to_yuy2,            YUY2_SIZE,  OUTPUT_TAG_YUY2 }
        };
    hp->sws_ctx = sws_getCachedContext( NULL,
                                        hp->video_ctx->width, hp->video_ctx->height, hp->video_ctx->pix_fmt,
                                        hp->video_ctx->width, hp->video_ctx->height, output_pixel_format,
                                        SWS_POINT, NULL, NULL, NULL );
    if( !hp->sws_ctx )
    {
        DEBUG_VIDEO_MESSAGE_BOX_DESKTOP( MB_ICONERROR | MB_OK, "Failed to get swscale context." );
        return -1;
    }
    hp->convert_colorspace = colorspace_table[index].convert_colorspace;
    /* BITMAPINFOHEADER */
    h->video_format.biSize        = sizeof( BITMAPINFOHEADER );
    h->video_format.biWidth       = hp->video_ctx->width;
    h->video_format.biHeight      = hp->video_ctx->height;
    h->video_format.biBitCount    = colorspace_table[index].pixel_size * 8;
    h->video_format.biCompression = colorspace_table[index].compression;
    /* Find the first valid video frame. */
    for( uint32_t i = 1; i <= h->video_sample_count; i++ )
    {
        AVPacket pkt;
        if( get_sample( hp->video_format, hp->video_index, &pkt ) == 1 )
        {
            av_free_packet( &pkt );
            break;
        }
        AVFrame picture;
        avcodec_get_frame_defaults( &picture );
        int got_picture;
        if( avcodec_decode_video2( hp->video_ctx, &picture, &got_picture, &pkt ) > 0 && got_picture )
        {
            av_free_packet( &pkt );
            if( i <= DECODER_DELAY( hp->video_ctx ) )
                continue;
            hp->first_valid_video_frame_number = i - DECODER_DELAY( hp->video_ctx );
            if( hp->first_valid_video_frame_number > 1 )
            {
                hp->first_valid_video_frame_size = h->video_format.biWidth * (h->video_format.biBitCount / 8) * h->video_format.biHeight;
                hp->first_valid_video_frame_data = malloc( hp->first_valid_video_frame_size );
                if( !hp->first_valid_video_frame_data )
                    return -1;
                if( hp->first_valid_video_frame_size > hp->convert_colorspace( hp->video_ctx, hp->sws_ctx, &picture, hp->first_valid_video_frame_data ) )
                    continue;
            }
            break;
        }
        av_free_packet( &pkt );
    }
    hp->last_video_frame_number = h->video_sample_count + 1;   /* Force seeking at the first reading. */
    return 0;
}

static int prepare_audio_decoding( lsmash_handler_t *h )
{
    libav_handler_t *hp = (libav_handler_t *)h->audio_private;
    if( !hp->audio_ctx )
        return 0;
    for( uint32_t i = 1; i <= hp->audio_frame_count; i++ )
        h->audio_pcm_sample_count += hp->audio_frame_list[i].length;
    if( h->audio_pcm_sample_count == 0 )
    {
        DEBUG_VIDEO_MESSAGE_BOX_DESKTOP( MB_ICONERROR | MB_OK, "No valid audio frame." );
        return -1;
    }
    if( hp->audio_index_entries )
    {
        AVStream *audio_stream = hp->audio_format->streams[ hp->audio_index ];
        for( int i = 0; i < hp->audio_index_entries_count; i++ )
        {
            AVIndexEntry *ie = &hp->audio_index_entries[i];
            if( av_add_index_entry( audio_stream, ie->pos, ie->timestamp, ie->size, ie->min_distance, ie->flags ) < 0 )
                return -1;
        }
        av_free( hp->audio_index_entries );
        hp->audio_index_entries = NULL;
    }
    if( h->audio_pcm_sample_count * 2 <= hp->audio_frame_count * hp->audio_frame_length )
        /* for HE-AAC upsampling */
        h->audio_pcm_sample_count *= 2;
    hp->next_audio_pcm_sample_number = h->audio_pcm_sample_count + 1;   /* Force seeking at the first reading. */
    /* WAVEFORMATEXTENSIBLE (WAVEFORMATEX) */
    WAVEFORMATEX *Format = &h->audio_format.Format;
    Format->nChannels       = hp->audio_ctx->channels;
    Format->nSamplesPerSec  = hp->audio_ctx->sample_rate;
    Format->wBitsPerSample  = av_get_bytes_per_sample( hp->audio_ctx->sample_fmt ) * 8;
    Format->nBlockAlign     = (Format->nChannels * Format->wBitsPerSample) / 8;
    Format->nAvgBytesPerSec = Format->nSamplesPerSec * Format->nBlockAlign;
    Format->wFormatTag      = Format->wBitsPerSample == 8 || Format->wBitsPerSample == 16 ? WAVE_FORMAT_PCM : WAVE_FORMAT_EXTENSIBLE;
    if( Format->wFormatTag == WAVE_FORMAT_EXTENSIBLE )
    {
        Format->cbSize = 22;
        h->audio_format.Samples.wValidBitsPerSample = hp->audio_ctx->bits_per_raw_sample;
        h->audio_format.SubFormat                   = KSDATAFORMAT_SUBTYPE_PCM;
    }
    else
        Format->cbSize = 0;
    DEBUG_AUDIO_MESSAGE_BOX_DESKTOP( MB_OK, "frame_length = %"PRIu32", channels = %d, sampling_rate = %d, bits_per_sample = %d, block_align = %d, avg_bps = %d",
                                     hp->audio_frame_length, Format->nChannels, Format->nSamplesPerSec,
                                     Format->wBitsPerSample, Format->nBlockAlign, Format->nAvgBytesPerSec );
    return 0;
}

static int decode_video_sample( libav_handler_t *hp, AVFrame *picture, int *got_picture, uint32_t sample_number )
{
    AVPacket pkt;
    if( get_sample( hp->video_format, hp->video_index, &pkt ) )
        return 1;
    if( pkt.flags == AV_PKT_FLAG_KEY )
        hp->last_rap_number = sample_number;
    avcodec_get_frame_defaults( picture );
    if( avcodec_decode_video2( hp->video_ctx, picture, got_picture, &pkt ) < 0 )
    {
        av_free_packet( &pkt );
        DEBUG_VIDEO_MESSAGE_BOX_DESKTOP( MB_OK, "Failed to decode a video frame." );
        return -1;
    }
    av_free_packet( &pkt );
    return 0;
}

static int64_t find_random_accessible_point( lsmash_handler_t *h, uint32_t presentation_sample_number, uint32_t decoding_sample_number, uint32_t *rap_number )
{
    libav_handler_t *hp = (libav_handler_t *)h->video_private;
    uint8_t is_leading = hp->video_frame_list[presentation_sample_number].is_leading;
    if( decoding_sample_number == 0 )
        decoding_sample_number = hp->video_frame_list[presentation_sample_number].sample_number;
    *rap_number = decoding_sample_number;
    while( *rap_number )
    {
        if( hp->keyframe_list[ *rap_number ] )
        {
            if( !is_leading )
                break;
            /* Shall be decoded from more past random access point. */
            is_leading = 0;
        }
        --(*rap_number);
    }
    if( *rap_number == 0 )
        *rap_number = 1;
    hp->last_rap_number = *rap_number;
    int64_t rap_pos = INT64_MIN;
    for( uint32_t i = 1; i <= h->video_sample_count; i++ )
        if( *rap_number == hp->video_frame_list[i].sample_number )
        {
            rap_pos = (hp->video_seek_base & SEEK_FILE_OFFSET_BASED) ? hp->video_frame_list[i].file_offset
                    : (hp->video_seek_base & SEEK_PTS_BASED)         ? hp->video_frame_list[i].pts
                    : (hp->video_seek_base & SEEK_DTS_BASED)         ? hp->video_frame_list[i].dts
                    :                                                  hp->video_frame_list[i].sample_number;
            break;
        }
    return rap_pos;
}

static void flush_buffers( AVCodecContext *ctx )
{
    /* Close and reopen the decoder even if the decoder implements avcodec_flush_buffers().
     * It seems this brings about more stable composition when seeking. */
    AVCodec *codec = ctx->codec;
    avcodec_close( ctx );
    if( avcodec_open2( ctx, codec, NULL ) < 0 )
        MESSAGE_BOX_DESKTOP( MB_ICONERROR | MB_OK, "Failed to flush buffers.\nIt is recommended you reopen the file." );
}

static uint32_t seek_video( libav_handler_t *hp, AVFrame *picture,
                            uint32_t presentation_sample_number, uint32_t rap_number,
                            int64_t rap_pos, int error_ignorance )
{
    /* Prepare to decode from random accessible sample. */
    int flags = (hp->video_seek_base & SEEK_FILE_OFFSET_BASED) ? AVSEEK_FLAG_BYTE : hp->video_seek_base == 0 ? AVSEEK_FLAG_FRAME : 0;
    if( av_seek_frame( hp->video_format, hp->video_index, rap_pos, flags | AVSEEK_FLAG_BACKWARD ) < 0 )
        return 0;
    flush_buffers( hp->video_ctx );
    hp->delay_count   = 0;
    hp->decode_status = DECODE_REQUIRE_INITIAL;
    if( rap_number + DECODER_DELAY( hp->video_ctx ) < presentation_sample_number )
        hp->video_ctx->skip_frame = AVDISCARD_NONREF;
    int dummy;
    uint32_t i;
    for( i = rap_number; i < presentation_sample_number + DECODER_DELAY( hp->video_ctx ); i++ )
    {
        if( i + DECODER_DELAY( hp->video_ctx ) == presentation_sample_number )
            hp->video_ctx->skip_frame = AVDISCARD_DEFAULT;
        int ret = decode_video_sample( hp, picture, &dummy, i );
        if( ret == -1 && !error_ignorance )
        {
            DEBUG_VIDEO_MESSAGE_BOX_DESKTOP( MB_OK, "Failed to decode a video frame." );
            return 0;
        }
        else if( ret == 1 )
            break;      /* Sample doesn't exist. */
    }
    hp->video_ctx->skip_frame = AVDISCARD_DEFAULT;
    hp->delay_count = DECODER_DELAY( hp->video_ctx );
    DEBUG_VIDEO_MESSAGE_BOX_DESKTOP( MB_OK, "rap_number = %d, seek_position = %d", rap_number, i );
    return i;
}

static int get_picture( libav_handler_t *hp, AVFrame *picture, uint32_t current, uint32_t goal, uint32_t video_sample_count )
{
    if( hp->decode_status == DECODE_INITIALIZING )
    {
        if( hp->delay_count > DECODER_DELAY( hp->video_ctx ) )
            -- hp->delay_count;
        else
            hp->decode_status = DECODE_INITIALIZED;
    }
    int got_picture = 0;
    do
    {
        int ret = decode_video_sample( hp, picture, &got_picture, current );
        if( ret == -1 )
            return -1;
        else if( ret == 1 )
            break;      /* Sample doesn't exist. */
        ++current;
        if( !got_picture )
            ++ hp->delay_count;
        DEBUG_VIDEO_MESSAGE_BOX_DESKTOP( MB_OK, "current frame = %d, decoded frame = %d, delay_count = %d",
                                         goal, current - 1, hp->delay_count );
        if( hp->delay_count > DECODER_DELAY( hp->video_ctx ) && hp->decode_status == DECODE_INITIALIZED )
            break;
    } while( current <= goal );
    /* Flush the last frames. */
    if( current > video_sample_count && !got_picture && DECODER_DELAY( hp->video_ctx ) )
        do
        {
            AVPacket pkt;
            av_init_packet( &pkt );
            pkt.data = NULL;
            pkt.size = 0;
            avcodec_get_frame_defaults( picture );
            if( avcodec_decode_video2( hp->video_ctx, picture, &got_picture, &pkt ) < 0 )
            {
                DEBUG_VIDEO_MESSAGE_BOX_DESKTOP( MB_OK, "Failed to decode and flush a video frame." );
                return -1;
            }
            ++current;
            if( !got_picture )
                ++ hp->delay_count;
        } while( current <= goal );
    if( hp->decode_status == DECODE_REQUIRE_INITIAL )
        hp->decode_status = DECODE_INITIALIZING;
    return got_picture ? 0 : -1;
}

static int read_video( lsmash_handler_t *h, int sample_number, void *buf )
{
#define MAX_ERROR_COUNT 3       /* arbitrary */
    libav_handler_t *hp = (libav_handler_t *)h->video_private;
    ++sample_number;            /* sample_number is 1-origin. */
    if( sample_number < hp->first_valid_video_frame_number )
    {
        /* Copy the first valid video frame data. */
        memcpy( buf, hp->first_valid_video_frame_data, hp->first_valid_video_frame_size );
        hp->last_video_frame_number = h->video_sample_count + 1;   /* Force seeking at the next access for valid frame. */
        return hp->first_valid_video_frame_size;
    }
    AVFrame picture;            /* Decoded video data will be stored here. */
    uint32_t start_number;      /* number of sample, for normal decoding, where decoding starts excluding decoding delay */
    uint32_t rap_number;        /* number of sample, for seeking, where decoding starts excluding decoding delay */
    int seek_mode = hp->seek_mode;
    int64_t rap_pos = INT64_MIN;
    if( sample_number == hp->last_video_frame_number + 1 )
    {
        start_number = sample_number + hp->delay_count;
        rap_number = hp->last_rap_number;
    }
    else
    {
        /* Require starting to decode from random accessible sample. */
        rap_pos = find_random_accessible_point( h, sample_number, 0, &rap_number );
        start_number = seek_video( hp, &picture, sample_number, rap_number, rap_pos, seek_mode != SEEK_MODE_NORMAL );
    }
    /* Get desired picture. */
    int error_count = 0;
    while( start_number == 0 || get_picture( hp, &picture, start_number, sample_number + hp->delay_count, h->video_sample_count ) )
    {
        /* Failed to get desired picture. */
        if( seek_mode == SEEK_MODE_AGGRESSIVE )
            goto video_fail;
        if( ++error_count > MAX_ERROR_COUNT || rap_number <= 1 )
        {
            if( seek_mode == SEEK_MODE_UNSAFE )
                goto video_fail;
            /* Retry to decode from the same random accessible sample with error ignorance. */
            seek_mode = SEEK_MODE_AGGRESSIVE;
        }
        else
            /* Retry to decode from more past random accessible sample. */
            rap_pos = find_random_accessible_point( h, sample_number, rap_number - 1, &rap_number );
        start_number = seek_video( hp, &picture, sample_number, rap_number, rap_pos, seek_mode != SEEK_MODE_NORMAL );
    }
    hp->last_video_frame_number = sample_number;
    DEBUG_VIDEO_MESSAGE_BOX_DESKTOP( MB_OK, "src_linesize[0] = %d, src_linesize[1] = %d, src_linesize[2] = %d, src_linesize[3] = %d",
                                     picture.linesize[0], picture.linesize[1], picture.linesize[2], picture.linesize[3] );
    return hp->convert_colorspace( hp->video_ctx, hp->sws_ctx, &picture, buf );
video_fail:
    /* fatal error of decoding */
    DEBUG_VIDEO_MESSAGE_BOX_DESKTOP( MB_ICONERROR | MB_OK, "Couldn't read video frame." );
    return 0;
#undef MAX_ERROR_COUNT
}

static int read_audio( lsmash_handler_t *h, int start, int wanted_length, void *buf )
{
    DEBUG_AUDIO_MESSAGE_BOX_DESKTOP( MB_OK, "start = %d, wanted_length = %d", start, wanted_length );
    libav_handler_t *hp = (libav_handler_t *)h->audio_private;
    if( start < hp->av_gap )
    {
        if( start + wanted_length < hp->av_gap )
        {
            hp->last_remainder_size          = 0;
            hp->last_audio_frame_number      = 0;
            hp->next_audio_pcm_sample_number = h->audio_pcm_sample_count + 1;
            return 0;
        }
        start = hp->av_gap - start;
    }
    else
        start -= hp->av_gap;
    uint32_t frame_number;
    uint64_t data_offset;
    int      copy_size;
    int      output_length = 0;
    int      block_align = h->audio_format.Format.nBlockAlign;
    if( start == hp->next_audio_pcm_sample_number )
    {
        frame_number = hp->last_audio_frame_number;
        if( hp->last_remainder_size )
        {
            copy_size = min( hp->last_remainder_size, wanted_length * block_align );
            memcpy( buf, hp->audio_output_buffer, copy_size );
            buf                     += copy_size;
            hp->last_remainder_size -= copy_size;
            int copied_length = copy_size / block_align;
            output_length += copied_length;
            wanted_length -= copied_length;
            if( wanted_length <= 0 )
                goto audio_out;
        }
        ++frame_number;
        data_offset = 0;
    }
    else
    {
        /* Seek audio stream. */
        hp->last_remainder_size          = 0;
        hp->next_audio_pcm_sample_number = 0;
        hp->last_audio_frame_number      = 0;
        frame_number = 1;
        uint64_t next_frame_pos = 0;
        uint32_t frame_length   = 0;
        do
        {
            frame_length = hp->audio_frame_list[frame_number].length;
            next_frame_pos += (uint64_t)frame_length;
            if( start < next_frame_pos )
                break;
            ++frame_number;
        } while( frame_number <= hp->audio_frame_count );
        data_offset = (start + frame_length - next_frame_pos) * block_align;
        uint32_t rap_number = frame_number;
        int64_t rap_pos;
        if( hp->audio_seek_base & SEEK_FILE_OFFSET_BASED )
        {
            while( hp->audio_frame_list[rap_number].file_offset == -1 )
                --rap_number;
            rap_pos = hp->audio_frame_list[rap_number].file_offset;
        }
        else
            rap_pos = (hp->audio_seek_base & SEEK_PTS_BASED) ? hp->audio_frame_list[rap_number].pts
                    : (hp->audio_seek_base & SEEK_DTS_BASED) ? hp->audio_frame_list[rap_number].dts
                    :                                          hp->audio_frame_list[rap_number].sample_number;
        int flags = (hp->audio_seek_base & SEEK_FILE_OFFSET_BASED) ? AVSEEK_FLAG_BYTE : hp->audio_seek_base == 0 ? AVSEEK_FLAG_FRAME : 0;
        if( av_seek_frame( hp->audio_format, hp->audio_index, rap_pos, flags | AVSEEK_FLAG_BACKWARD ) < 0 )
            av_seek_frame( hp->audio_format, hp->audio_index, rap_pos, flags | AVSEEK_FLAG_BACKWARD | AVSEEK_FLAG_ANY );
        flush_buffers( hp->audio_ctx );
        AVPacket pkt;
        while( rap_number < frame_number )
        {
            if( get_sample( hp->audio_format, hp->audio_index, &pkt ) )
                break;
            ++rap_number;
        }
    }
    do
    {
        copy_size = 0;
        AVPacket pkt;
        if( get_sample( hp->audio_format, hp->audio_index, &pkt ) )
            goto audio_out;
        while( pkt.size > 0 )
        {
            int output_buffer_size = AVCODEC_MAX_AUDIO_FRAME_SIZE;
            int wasted_data_length = avcodec_decode_audio3( hp->audio_ctx, (int16_t *)hp->audio_output_buffer, &output_buffer_size, &pkt );
            if( wasted_data_length < 0 )
            {
                DEBUG_AUDIO_MESSAGE_BOX_DESKTOP( MB_ICONERROR | MB_OK, "Failed to decode a audio frame." );
                goto audio_out;
            }
            pkt.size -= wasted_data_length;
            pkt.data += wasted_data_length;
            if( output_buffer_size > data_offset )
            {
                copy_size = min( output_buffer_size - data_offset, wanted_length * block_align );
                memcpy( buf, hp->audio_output_buffer + data_offset, copy_size );
                int copied_length = copy_size / block_align;
                output_length += copied_length;
                wanted_length -= copied_length;
                buf           += copy_size;
                data_offset = 0;
            }
            else
            {
                copy_size = 0;
                data_offset -= output_buffer_size;
            }
            DEBUG_AUDIO_MESSAGE_BOX_DESKTOP( MB_OK, "frame_number = %d, decoded_length = %d, copied_length = %d, output_length = %d",
                                             frame_number, output_buffer_size / h->audio_format.nBlockAlign, copy_size / block_align, output_length );
            if( wanted_length <= 0 )
            {
                hp->last_remainder_size = output_buffer_size - copy_size;
                goto audio_out;
            }
        }
        ++frame_number;
    } while( 1 );
audio_out:
    DEBUG_AUDIO_MESSAGE_BOX_DESKTOP( MB_OK, "output_length = %d, remainder = %d", output_length, hp->last_remainder_size );
    if( hp->last_remainder_size && copy_size != 0 )
        /* Move unused decoded data to the head of output buffer for the next access. */
        memmove( hp->audio_output_buffer, hp->audio_output_buffer + copy_size, hp->last_remainder_size );
    hp->next_audio_pcm_sample_number = start + output_length;
    hp->last_audio_frame_number = frame_number;
    return output_length;
}

static int is_keyframe( lsmash_handler_t *h, int sample_number )
{
    libav_handler_t *hp = (libav_handler_t *)h->video_private;
    return hp->video_frame_list[sample_number + 1].keyframe;
}

static void video_cleanup( lsmash_handler_t *h )
{
    libav_handler_t *hp = (libav_handler_t *)h->video_private;
    if( !hp )
        return;
    if( hp->first_valid_video_frame_data )
        free( hp->first_valid_video_frame_data );
    if( hp->video_frame_list )
        free( hp->video_frame_list );
    if( hp->keyframe_list )
        free( hp->keyframe_list );
    if( hp->sws_ctx )
        sws_freeContext( hp->sws_ctx );
    if( hp->video_ctx )
        avcodec_close( hp->video_ctx );
    if( hp->video_format )
        avformat_close_input( &hp->video_format );
}

static void audio_cleanup( lsmash_handler_t *h )
{
    libav_handler_t *hp = (libav_handler_t *)h->audio_private;
    if( !hp )
        return;
    if( hp->audio_index_entries )
        av_free( hp->audio_index_entries );
    if( hp->audio_output_buffer )
        av_free( hp->audio_output_buffer );
    if( hp->audio_ctx )
        avcodec_close( hp->audio_ctx );
    if( hp->audio_format )
        avformat_close_input( &hp->audio_format );
}

static void close_file( void *private_stuff )
{
    if( private_stuff )
        free( private_stuff );
}

lsmash_reader_t libav_reader =
{
    LIBAV_READER,
    open_file,
    get_first_video_track,
    get_first_audio_track,
    NULL,
    prepare_video_decoding,
    prepare_audio_decoding,
    read_video,
    read_audio,
    is_keyframe,
    video_cleanup,
    audio_cleanup,
    close_file
};
