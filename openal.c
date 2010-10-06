/*  SSNES - A Super Ninteno Entertainment System (SNES) Emulator frontend for libsnes.
 *  Copyright (C) 2010 - Hans-Kristian Arntzen
 *
 *  Some code herein may be based on code found in BSNES.
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
#include <AL/al.h>
#include <AL/alc.h>
#include <time.h>
#include <string.h>

#define BUFSIZE 128

typedef struct al
{
   ALuint source;
   ALuint *buffers;
   ALuint *res_buf;
   int res_ptr;
   ALenum format;
   int num_buffers;
   int rate;
   int queue;

   uint8_t tmpbuf[BUFSIZE];
   int tmpbuf_ptr;

   ALCdevice *handle;
   ALCcontext *ctx;

   bool nonblock;

} al_t;

static void* __al_init(const char* device, int rate, int latency)
{
   (void)device;
   al_t *al = calloc(1, sizeof(al_t));
   if ( al == NULL )
      return NULL;

   al->handle = alcOpenDevice(NULL);
   if (al->handle == NULL)
      goto error;

   al->ctx = alcCreateContext(al->handle, NULL);
   if (al->ctx == NULL)
      goto error;

   alcMakeContextCurrent(al->ctx);

   al->rate = rate;

   al->num_buffers = (latency * rate * 2 * 2) / (1000 * BUFSIZE);
   al->buffers = calloc(al->num_buffers, sizeof(ALuint));
   al->res_buf = calloc(al->num_buffers, sizeof(ALuint));
   if (al->buffers == NULL || al->res_buf == NULL)
      goto error;

   alGenSources(1, &al->source);
   alGenBuffers(al->num_buffers, al->buffers);

   memcpy(al->res_buf, al->buffers, al->num_buffers * sizeof(ALuint));
   al->res_ptr = al->num_buffers;

   return al;

error:
   if (al)
   {
      alcMakeContextCurrent(NULL);
      if (al->ctx)
         alcDestroyContext(al->ctx);
      if (al->handle)
         alcCloseDevice(al->handle);
      if (al->buffers)
         free(al->buffers);
      if (al->res_buf)
         free(al->res_buf);
      free(al);
   }
   return NULL;
}

static bool al_unqueue_buffers(al_t *al)
{
   ALint val;

   alGetSourcei(al->source, AL_BUFFERS_PROCESSED, &val);

   if (val > 0)
   {
      alSourceUnqueueBuffers(al->source, val, &al->res_buf[al->res_ptr]);
      al->res_ptr += val;
      return true;
   }

   return false;
}

static bool al_get_buffer(al_t *al, ALuint *buffer)
{
   if (al->res_ptr == 0)
   {
#ifndef _WIN32
      struct timespec tv = {
         .tv_sec = 0,
         .tv_nsec = 1000000
      };
#endif

      for(;;)
      {
         if (al_unqueue_buffers(al))
            break;

         if (al->nonblock)
            return false;

#ifdef _WIN32
         Sleep(1);
#else
         nanosleep(&tv, NULL);
#endif
      }
   }

   *buffer = al->res_buf[--al->res_ptr];
   return true;
}

static size_t al_fill_internal_buf(al_t *al, const void* buf, size_t size)
{
   size_t read_size = (BUFSIZE - al->tmpbuf_ptr > size) ? size : (BUFSIZE - al->tmpbuf_ptr);
   memcpy(al->tmpbuf + al->tmpbuf_ptr, buf, read_size);
   al->tmpbuf_ptr += read_size;
   return read_size;
}

static ssize_t __al_write(void* data, const void* buf, size_t size)
{
   al_t *al = data;

   size_t written = 0;
   while (written < size)
   {
      size_t rc = al_fill_internal_buf(al, (const char*)buf + written, size - written);
      written += rc;

      if (al->tmpbuf_ptr != BUFSIZE)
         break;

      ALuint buffer;
      if (!al_get_buffer(al, &buffer))
         return 0;

      alBufferData(buffer, AL_FORMAT_STEREO16, al->tmpbuf, BUFSIZE, al->rate);
      al->tmpbuf_ptr = 0;
      alSourceQueueBuffers(al->source, 1, &buffer);
      if (alGetError() != AL_NO_ERROR)
         return -1;

      ALint val;
      alGetSourcei(al->source, AL_SOURCE_STATE, &val);
      if (val != AL_PLAYING)
         alSourcePlay(al->source);

      if (alGetError() != AL_NO_ERROR)
         return -1;
   }

   return size;
}

static bool __al_stop(void *data)
{
   (void)data;
   return true;
}

static void __al_set_nonblock_state(void *data, bool state)
{
   al_t *al = data;
   al->nonblock = state;
}

static bool __al_start(void *data)
{
   (void)data;
   return true;
}

static void __al_free(void *data)
{
   al_t *al= data;
   if (al)
   {
      alSourceStop(al->source);
      alDeleteSources(1, &al->source);
      if ( al->buffers )
      {
         alDeleteBuffers(al->num_buffers, al->buffers);
         free(al->buffers);
         free(al->res_buf);
      }
   }
   free(al);
}

const audio_driver_t audio_openal = {
   .init = __al_init,
   .write = __al_write,
   .stop = __al_stop,
   .start = __al_start,
   .set_nonblock_state = __al_set_nonblock_state,
   .free = __al_free
};

   