#ifndef __FFEMU_H
#define __FFEMU_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Available video codecs
typedef enum ffemu_video_codec
{
   FFEMU_VIDEO_NONE,
   FFEMU_VIDEO_H264,
   FFEMU_VIDEO_MPEG4,
} ffemu_video_codec;

// Available audio codecs
typedef enum ffemu_audio_codec
{
   FFEMU_AUDIO_NONE,
   FFEMU_AUDIO_VORBIS,
   FFEMU_AUDIO_MP3,
   FFEMU_AUDIO_AAC,
} ffemu_audio_codec;

// Available pixel formats
typedef enum ffemu_pixel_format
{
   FFEMU_FMT_XBGR1555,
   FFEMU_FMT_RGB888,
} ffemu_pixel_format;

typedef enum ffemu_rescaler
{
   FFEMU_RESCALER_LANCZOS,
   FFEMU_RESCALER_POINT
} ffemu_rescaler;

struct ffemu_rational
{
   unsigned num;
   unsigned den;
};

// Parameters passed to ffemu_new()
struct ffemu_params
{
   // Video codec to use. If not recording video, select FFEMU_VIDEO_NONE.
   ffemu_video_codec vcodec;

   // Desired output resolution.
   unsigned out_width;
   unsigned out_height;
   float aspect_ratio;

   // Rescaler for video.
   ffemu_rescaler rescaler;

   // Pixel format for video input.
   ffemu_pixel_format format;

   // FPS of video input.
   struct ffemu_rational fps;

   // Relative video quality. 0 is lossless (if available), 10 is very low quality. 
   // A value over 10 is codec defined if it will give even worse quality.
   unsigned videoq;

   // Define some video codec dependent option. (E.g. h264 profiles)
   uint64_t video_opt;

   // Audio codec. If not recording audio, select FFEMU_AUDIO_NONE.
   ffemu_audio_codec acodec;

   // Audio sample rate.
   unsigned samplerate;

   // Audio channels.
   unsigned channels;

   // Audio bits. Sample format is always signed PCM in native byte order.
   //unsigned bits;

   // Relative audio quality. 0 is lossless (if available), 10 is very low quality.
   // A value over 10 is codec defined if it will give even worse quality.
   // Some codecs might ignore this (lossless codecs such as FLAC).
   unsigned audioq;

   // Define some audio codec dependent option.
   uint64_t audio_opt;

   // Filename to dump to.
   const char *filename;
};

struct ffemu_video_data
{
   const void *data;
   unsigned width;
   unsigned height;
   unsigned pitch;
};

struct ffemu_audio_data
{
   const int16_t *data;
   size_t frames;
};

typedef struct ffemu ffemu_t;

ffemu_t *ffemu_new(const struct ffemu_params *params);
void ffemu_free(ffemu_t* handle);

int ffemu_push_video(ffemu_t *handle, const struct ffemu_video_data *data);
int ffemu_push_audio(ffemu_t *handle, const struct ffemu_audio_data *data);
int ffemu_finalize(ffemu_t *handle);



#ifdef __cplusplus
}
#endif

#endif