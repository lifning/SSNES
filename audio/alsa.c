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


#include "../driver.h"
#include <stdlib.h>
#include <asoundlib.h>
#include "../general.h"

#define TRY_ALSA(x) if (x < 0) { \
                  goto error; \
               }

typedef struct alsa
{
   snd_pcm_t *pcm;
   bool nonblock;
   bool has_float;

   size_t buffer_size;
} alsa_t;

static bool alsa_use_float(void *data)
{
   alsa_t *alsa = (alsa_t*)data;
   return alsa->has_float;
}

static bool find_float_format(snd_pcm_t *pcm, snd_pcm_hw_params_t *params)
{
   if (snd_pcm_hw_params_test_format(pcm, params, SND_PCM_FORMAT_FLOAT) == 0)
   {
      SSNES_LOG("ALSA: Using floating point format.\n");
      return true;
   }
   SSNES_LOG("ALSA: Using signed 16-bit format.\n");
   return false;
}

static void *alsa_init(const char *device, unsigned rate, unsigned latency)
{
   alsa_t *alsa = (alsa_t*)calloc(1, sizeof(alsa_t));
   if (!alsa)
      return NULL;

   snd_pcm_hw_params_t *params = NULL;
   snd_pcm_sw_params_t *sw_params = NULL;

   const char *alsa_dev = "default";
   if (device)
      alsa_dev = device;

   unsigned latency_usec = latency * 1000;
   unsigned channels = 2;
   unsigned periods = 4;
   snd_pcm_uframes_t buffer_size;
   snd_pcm_format_t format;

   TRY_ALSA(snd_pcm_open(&alsa->pcm, alsa_dev, SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK));

   TRY_ALSA(snd_pcm_hw_params_malloc(&params));
   alsa->has_float = find_float_format(alsa->pcm, params);
   format = alsa->has_float ? SND_PCM_FORMAT_FLOAT : SND_PCM_FORMAT_S16;

   TRY_ALSA(snd_pcm_hw_params_any(alsa->pcm, params));
   TRY_ALSA(snd_pcm_hw_params_set_access(alsa->pcm, params, SND_PCM_ACCESS_RW_INTERLEAVED));
   TRY_ALSA(snd_pcm_hw_params_set_format(alsa->pcm, params, format));
   TRY_ALSA(snd_pcm_hw_params_set_channels(alsa->pcm, params, channels));
   TRY_ALSA(snd_pcm_hw_params_set_rate(alsa->pcm, params, rate, 0));

   TRY_ALSA(snd_pcm_hw_params_set_buffer_time_near(alsa->pcm, params, &latency_usec, NULL));
   TRY_ALSA(snd_pcm_hw_params_set_periods_near(alsa->pcm, params, &periods, NULL));

   TRY_ALSA(snd_pcm_hw_params(alsa->pcm, params));

   snd_pcm_hw_params_get_period_size(params, &buffer_size, NULL);
   SSNES_LOG("ALSA: Period size: %d frames\n", (int)buffer_size);
   snd_pcm_hw_params_get_buffer_size(params, &buffer_size);
   SSNES_LOG("ALSA: Buffer size: %d frames\n", (int)buffer_size);
   alsa->buffer_size = snd_pcm_frames_to_bytes(alsa->pcm, buffer_size);

   TRY_ALSA(snd_pcm_sw_params_malloc(&sw_params));
   TRY_ALSA(snd_pcm_sw_params_current(alsa->pcm, sw_params));
   TRY_ALSA(snd_pcm_sw_params_set_start_threshold(alsa->pcm, sw_params, buffer_size / 2));
   TRY_ALSA(snd_pcm_sw_params(alsa->pcm, sw_params));

   snd_pcm_hw_params_free(params);
   snd_pcm_sw_params_free(sw_params);

   return alsa;

error:
   SSNES_ERR("ALSA: Failed to initialize...\n");
   if (params)
      snd_pcm_hw_params_free(params);

   if (sw_params)
      snd_pcm_sw_params_free(sw_params);

   if (alsa)
   {
      if (alsa->pcm)
         snd_pcm_close(alsa->pcm);

      free(alsa);
   }
   return NULL;
}

static ssize_t alsa_write(void *data, const void *buf, size_t size)
{
   alsa_t *alsa = (alsa_t*)data;

   snd_pcm_sframes_t frames;
   snd_pcm_sframes_t written = 0;
   int rc;
   size = snd_pcm_bytes_to_frames(alsa->pcm, size); // Frames to write

   while (written < (snd_pcm_sframes_t)size)
   {
      if (!alsa->nonblock)
      {
         rc = snd_pcm_wait(alsa->pcm, -1);
         if (rc == -EPIPE || rc == -ESTRPIPE)
         {
            if (snd_pcm_recover(alsa->pcm, rc, 1) < 0)
               return -1;
            continue;
         }
      }

      frames = snd_pcm_writei(alsa->pcm, (const char*)buf + written * 2 * (alsa->has_float ? sizeof(float) : sizeof(int16_t)), size - written);

      if (frames == -EPIPE || frames == -EINTR || frames == -ESTRPIPE)
      {
         if (snd_pcm_recover(alsa->pcm, frames, 1) < 0)
            return -1;

         return 0;
      }
      else if (frames == -EAGAIN && alsa->nonblock)
         return 0;
      else if (frames < 0)
         return -1;

      written += frames;
   }

   return snd_pcm_frames_to_bytes(alsa->pcm, size);
}

static bool alsa_stop(void *data)
{
   return true;
}

static void alsa_set_nonblock_state(void *data, bool state)
{
   alsa_t *alsa = (alsa_t*)data;
   alsa->nonblock = state;
}

static bool alsa_start(void *data)
{
   return true;
}

static void alsa_free(void *data)
{
   alsa_t *alsa = (alsa_t*)data;
   if (alsa)
   {
      if (alsa->pcm)
      {
         snd_pcm_drop(alsa->pcm);
         snd_pcm_close(alsa->pcm);
      }
      free(alsa);
   }
}

static size_t alsa_write_avail(void *data)
{
   alsa_t *alsa = (alsa_t*)data;
   return snd_pcm_frames_to_bytes(alsa->pcm, snd_pcm_avail_update(alsa->pcm));
}

static size_t alsa_buffer_size(void *data)
{
   alsa_t *alsa = (alsa_t*)data;
   return alsa->buffer_size;
}

const audio_driver_t audio_alsa = {
   alsa_init,
   alsa_write,
   alsa_stop,
   alsa_start,
   alsa_set_nonblock_state,
   alsa_free,
   alsa_use_float,
   "alsa",
   alsa_write_avail,
   alsa_buffer_size,
};

