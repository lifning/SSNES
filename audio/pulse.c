/*  SSNES - A frontend for libretro.
 *  Copyright (C) 2010-2012 - Hans-Kristian Arntzen
 *

 * 
 *  SSNES is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 *
 *  SSNES is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with SSNES.
 *  If not, see <http://www.gnu.org/licenses/>.
 */


#include "driver.h"
#include "general.h"
#include <pulse/pulseaudio.h>
#include "../boolean.h"
#include <string.h>

#include <stdio.h>

typedef struct
{
   pa_threaded_mainloop *mainloop;
   pa_context *context;
   pa_stream *stream;
   bool nonblock;
   size_t buffer_size;
} pa_t;

static void pulse_free(void *data)
{
   pa_t *pa = (pa_t*)data;
   if (pa)
   {
      if (pa->mainloop)
         pa_threaded_mainloop_stop(pa->mainloop);

      if (pa->stream)
      {
         pa_stream_disconnect(pa->stream);
         pa_stream_unref(pa->stream);
      }

      if (pa->context)
      {
         pa_context_disconnect(pa->context);
         pa_context_unref(pa->context);
      }

      if (pa->mainloop)
         pa_threaded_mainloop_free(pa->mainloop);

      free(pa);
   }
}

static void context_state_cb(pa_context *c, void *data)
{
   pa_t *pa = (pa_t*)data;
   switch (pa_context_get_state(c))
   {
      case PA_CONTEXT_READY:
      case PA_CONTEXT_TERMINATED:
      case PA_CONTEXT_FAILED:
         pa_threaded_mainloop_signal(pa->mainloop, 0);
         break;
      default:
         break;
   }
}

static void stream_state_cb(pa_stream *s, void *data) 
{
   pa_t *pa = (pa_t*)data;
   switch (pa_stream_get_state(s)) {
      case PA_STREAM_READY:
      case PA_STREAM_FAILED:
      case PA_STREAM_TERMINATED:
         pa_threaded_mainloop_signal(pa->mainloop, 0);
         break;
      default:
         break;
   }
}

static void stream_request_cb(pa_stream *s, size_t length, void *data) 
{
   (void)length;
   (void)s;
   pa_t *pa = (pa_t*)data;
   pa_threaded_mainloop_signal(pa->mainloop, 0);
}

static void stream_latency_update_cb(pa_stream *s, void *data) 
{
   (void)s;
   pa_t *pa = (pa_t*)data;
   pa_threaded_mainloop_signal(pa->mainloop, 0);
}

static void *pulse_init(const char *device, unsigned rate, unsigned latency)
{
   pa_sample_spec spec;
   memset(&spec, 0, sizeof(spec));
   pa_buffer_attr buffer_attr = {0};

   pa_t *pa = (pa_t*)calloc(1, sizeof(*pa));
   if (!pa)
      goto error;

   pa->mainloop = pa_threaded_mainloop_new();
   if (!pa->mainloop)
      goto error;

   pa->context = pa_context_new(pa_threaded_mainloop_get_api(pa->mainloop), "SSNES");
   if (!pa->context)
      goto error;

   pa_context_set_state_callback(pa->context, context_state_cb, pa);

   if (pa_context_connect(pa->context, device, PA_CONTEXT_NOFLAGS, NULL) < 0)
      goto error;

   pa_threaded_mainloop_lock(pa->mainloop);
   if (pa_threaded_mainloop_start(pa->mainloop) < 0)
      goto error;

   pa_threaded_mainloop_wait(pa->mainloop);

   if (pa_context_get_state(pa->context) != PA_CONTEXT_READY)
      goto unlock_error;

   spec.format = is_little_endian() ? PA_SAMPLE_FLOAT32LE : PA_SAMPLE_FLOAT32BE;
   spec.channels = 2;
   spec.rate = rate;

   pa->stream = pa_stream_new(pa->context, "audio", &spec, NULL);
   if (!pa->stream)
      goto unlock_error;

   pa_stream_set_state_callback(pa->stream, stream_state_cb, pa);
   pa_stream_set_write_callback(pa->stream, stream_request_cb, pa);
   pa_stream_set_latency_update_callback(pa->stream, stream_latency_update_cb, pa);

   buffer_attr.maxlength = -1;
   buffer_attr.tlength = pa_usec_to_bytes(latency * PA_USEC_PER_MSEC, &spec);
   buffer_attr.prebuf = -1;
   buffer_attr.minreq = -1;
   buffer_attr.fragsize = -1;

   pa->buffer_size = buffer_attr.tlength;

   if (pa_stream_connect_playback(pa->stream, NULL, &buffer_attr, PA_STREAM_ADJUST_LATENCY, NULL, NULL) < 0)
      goto error;

   pa_threaded_mainloop_wait(pa->mainloop);

   if (pa_stream_get_state(pa->stream) != PA_STREAM_READY)
      goto unlock_error;

   pa_threaded_mainloop_unlock(pa->mainloop);

   return pa;

unlock_error:
   pa_threaded_mainloop_unlock(pa->mainloop);
error:
   pulse_free(pa); 
   return NULL;
}

static ssize_t pulse_write(void *data, const void *buf, size_t size)
{
   pa_t *pa = (pa_t*)data;

   pa_threaded_mainloop_lock(pa->mainloop);
   size_t length = pa_stream_writable_size(pa->stream);
   pa_threaded_mainloop_unlock(pa->mainloop);

   while (length < size)
   {
      pa_threaded_mainloop_wait(pa->mainloop);
      pa_threaded_mainloop_lock(pa->mainloop);
      length = pa_stream_writable_size(pa->stream);
      pa_threaded_mainloop_unlock(pa->mainloop);

      if (pa->nonblock)
         break;
   }

   size_t write_size = length < size ? length : size;

   pa_threaded_mainloop_lock(pa->mainloop);
   pa_stream_write(pa->stream, buf, write_size, NULL, 0LL, PA_SEEK_RELATIVE);
   pa_threaded_mainloop_unlock(pa->mainloop);
   return write_size;
}

static bool pulse_stop(void *data)
{
   (void)data;
   return true;
}

static bool pulse_start(void *data)
{
   (void)data;
   return true;
}

static void pulse_set_nonblock_state(void *data, bool state)
{
   pa_t *pa = (pa_t*)data;
   pa->nonblock = state;
}

static bool pulse_use_float(void *data)
{
   (void)data;
   return true;
}

static size_t pulse_write_avail(void *data)
{
   pa_t *pa = (pa_t*)data;
   pa_threaded_mainloop_lock(pa->mainloop);
   size_t length = pa_stream_writable_size(pa->stream);
   pa_threaded_mainloop_unlock(pa->mainloop);
   return length;
}

static size_t pulse_buffer_size(void *data)
{
   pa_t *pa = (pa_t*)data;
   return pa->buffer_size;
}

const audio_driver_t audio_pulse = {
   pulse_init,
   pulse_write,
   pulse_stop,
   pulse_start,
   pulse_set_nonblock_state,
   pulse_free,
   pulse_use_float,
   "pulse",
   pulse_write_avail,
   pulse_buffer_size,
};

