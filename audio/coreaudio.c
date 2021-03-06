/*  SSNES - A frontend for libretro.
 *  Copyright (C) 2010-2012 - Hans-Kristian Arntzen
 *  Copyright (C) 2011-2012 - Chris Moeller
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
#include "fifo_buffer.h"
#include <stdlib.h>
#include "../boolean.h"
#include <pthread.h>

#include <CoreAudio/CoreAudio.h>
#include <CoreServices/CoreServices.h>
#include <AudioUnit/AudioUnit.h>
#include <AudioUnit/AUComponent.h>

typedef struct coreaudio
{
   pthread_mutex_t lock;
   pthread_cond_t cond;

   ComponentInstance dev;
   bool dev_alive;

   fifo_buffer_t *buffer;
   bool nonblock;
   size_t buffer_size;
} coreaudio_t;

static void coreaudio_free(void *data)
{
   coreaudio_t *dev = (coreaudio_t*)data;
   if (!dev)
      return;

   if (dev->dev_alive)
   {
      AudioOutputUnitStop(dev->dev);
      CloseComponent(dev->dev);
   }

   if (dev->buffer)
      fifo_free(dev->buffer);

   pthread_mutex_destroy(&dev->lock);
   pthread_cond_destroy(&dev->cond);

   free(dev);
}

static OSStatus audio_cb(void *userdata, AudioUnitRenderActionFlags *action_flags,
      const AudioTimeStamp *time_stamp, UInt32 bus_number,
      UInt32 number_frames, AudioBufferList *io_data)
{
   coreaudio_t *dev = (coreaudio_t*)userdata;
   (void)time_stamp;
   (void)bus_number;
   (void)number_frames;

   if (!io_data)
      return noErr;
   if (io_data->mNumberBuffers != 1)
      return noErr;

   unsigned write_avail = io_data->mBuffers[0].mDataByteSize;
   void *outbuf = io_data->mBuffers[0].mData;

   pthread_mutex_lock(&dev->lock);
   if (fifo_read_avail(dev->buffer) < write_avail)
   {
      *action_flags = kAudioUnitRenderAction_OutputIsSilence;
      memset(outbuf, 0, write_avail); // Seems to be needed.
      pthread_mutex_unlock(&dev->lock);
      pthread_cond_signal(&dev->cond); // Technically possible to deadlock without.
      return noErr;
   }

   fifo_read(dev->buffer, outbuf, write_avail);
   pthread_mutex_unlock(&dev->lock);
   pthread_cond_signal(&dev->cond);
   return noErr;
}

static void *coreaudio_init(const char *device, unsigned rate, unsigned latency)
{
   (void)device;

   coreaudio_t *dev = (coreaudio_t*)calloc(1, sizeof(*dev));
   if (!dev)
      return NULL;

   CFRunLoopRef run_loop = NULL;
   AudioObjectPropertyAddress addr = {
      kAudioHardwarePropertyRunLoop,
      kAudioObjectPropertyScopeGlobal,
      kAudioObjectPropertyElementMaster
   };

   pthread_mutex_init(&dev->lock, NULL);
   pthread_cond_init(&dev->cond, NULL);
   AudioObjectSetPropertyData(kAudioObjectSystemObject, &addr, 0, NULL,
         sizeof(CFRunLoopRef), &run_loop);

   ComponentDescription desc = {0};
   desc.componentType = kAudioUnitType_Output;
   desc.componentSubType = kAudioUnitSubType_HALOutput;
   desc.componentManufacturer = kAudioUnitManufacturer_Apple;

   AudioStreamBasicDescription stream_desc = {0};
   AudioStreamBasicDescription real_desc;
   AudioChannelLayout layout = {0};
   AURenderCallbackStruct cb = {0};

   Component comp = NULL;
   OSStatus res = noErr;
   UInt32 i_size = 0;
   size_t fifo_size;

   comp = FindNextComponent(NULL, &desc);
   if (comp == NULL)
      goto error;

   res = OpenAComponent(comp, &dev->dev);
   if (res != noErr)
      goto error;

   dev->dev_alive = true;

   stream_desc.mSampleRate = rate;
   stream_desc.mBitsPerChannel = sizeof(float) * CHAR_BIT;
   stream_desc.mChannelsPerFrame = 2;
   stream_desc.mBytesPerPacket = 2 * sizeof(float);
   stream_desc.mBytesPerFrame = 2 * sizeof(float);
   stream_desc.mFramesPerPacket = 1;
   stream_desc.mFormatID = kAudioFormatLinearPCM;
   stream_desc.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked | (is_little_endian() ? 0 : kAudioFormatFlagIsBigEndian);

   res = AudioUnitSetProperty(dev->dev, kAudioUnitProperty_StreamFormat,
         kAudioUnitScope_Input, 0, &stream_desc, sizeof(stream_desc));
   if (res != noErr)
      goto error;

   i_size = sizeof(real_desc);
   res = AudioUnitGetProperty(dev->dev, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &real_desc, &i_size);
   if (res != noErr)
      goto error;

   SSNES_LOG("[CoreAudio]: Using output sample rate of %.1f Hz\n", (float)real_desc.mSampleRate);
   g_settings.audio.out_rate = real_desc.mSampleRate;

   if (real_desc.mChannelsPerFrame != stream_desc.mChannelsPerFrame)
      goto error;
   if (real_desc.mBitsPerChannel != stream_desc.mBitsPerChannel)
      goto error;
   if (real_desc.mFormatFlags != stream_desc.mFormatFlags)
      goto error;
   if (real_desc.mFormatID != stream_desc.mFormatID)
      goto error;

   layout.mChannelLayoutTag = kAudioChannelLayoutTag_UseChannelBitmap;
   layout.mChannelBitmap = 0x03;

   res = AudioUnitSetProperty(dev->dev, kAudioUnitProperty_AudioChannelLayout,
         kAudioUnitScope_Input, 0, &layout, sizeof(layout));
   if (res != noErr)
      goto error;

   cb.inputProc = audio_cb;
   cb.inputProcRefCon = dev;

   res = AudioUnitSetProperty(dev->dev, kAudioUnitProperty_SetRenderCallback,
         kAudioUnitScope_Input, 0, &cb, sizeof(cb));
   if (res != noErr)
      goto error;

   res = AudioUnitInitialize(dev->dev);
   if (res != noErr)
      goto error;

   fifo_size = (latency * g_settings.audio.out_rate) / 1000;
   fifo_size *= 2 * sizeof(float);
   dev->buffer_size = fifo_size;

   dev->buffer = fifo_new(fifo_size);
   if (!dev->buffer)
      goto error;

   SSNES_LOG("[CoreAudio]: Using buffer size of %u bytes: (latency = %u ms)\n", (unsigned)fifo_size, latency);

   res = AudioOutputUnitStart(dev->dev);
   if (res != noErr)
      goto error;

   return dev;

error:
   SSNES_ERR("[CoreAudio]: Failed to initialize driver ...\n");
   coreaudio_free(dev);
   return NULL;
}

static ssize_t coreaudio_write(void *data, const void *buf_, size_t size)
{
   coreaudio_t *dev = (coreaudio_t*)data;

   const uint8_t *buf = (const uint8_t*)buf_;
   size_t written = 0;

   while (size > 0)
   {
      pthread_mutex_lock(&dev->lock);

      size_t write_avail = fifo_write_avail(dev->buffer);
      if (write_avail > size)
         write_avail = size;

      fifo_write(dev->buffer, buf, write_avail);
      buf += write_avail;
      written += write_avail;
      size -= write_avail;

      if (dev->nonblock)
      {
         pthread_mutex_unlock(&dev->lock);
         break;
      }

      if (write_avail == 0)
         pthread_cond_wait(&dev->cond, &dev->lock);
      pthread_mutex_unlock(&dev->lock);
   }

   return written;
}

static bool coreaudio_stop(void *data)
{
   coreaudio_t *dev = (coreaudio_t*)data;
   return AudioOutputUnitStop(dev->dev) == noErr;
}

static void coreaudio_set_nonblock_state(void *data, bool state)
{
   coreaudio_t *dev = (coreaudio_t*)data;
   dev->nonblock = state;
}

static bool coreaudio_start(void *data)
{
   coreaudio_t *dev = (coreaudio_t*)data;
   return AudioOutputUnitStart(dev->dev) == noErr;
}

static bool coreaudio_use_float(void *data)
{
   (void)data;
   return true;
}

static size_t coreaudio_write_avail(void *data)
{
   coreaudio_t *dev = (coreaudio_t*)data;
   pthread_mutex_lock(&dev->lock);
   size_t avail = fifo_write_avail(dev->buffer);
   pthread_mutex_unlock(&dev->lock);
   return avail;
}

static size_t coreaudio_buffer_size(void *data)
{
   coreaudio_t *dev = (coreaudio_t*)data;
   return dev->buffer_size;
}

const audio_driver_t audio_coreaudio = {
   coreaudio_init,
   coreaudio_write,
   coreaudio_stop,
   coreaudio_start,
   coreaudio_set_nonblock_state,
   coreaudio_free,
   coreaudio_use_float,
   "coreaudio",
   coreaudio_write_avail,
   coreaudio_buffer_size,
};

