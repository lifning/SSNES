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
#include "file.h"
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "compat/posix_string.h"

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

static const audio_driver_t *audio_drivers[] = {
#ifdef HAVE_ALSA
   &audio_alsa,
#endif
#if defined(HAVE_OSS) || defined(HAVE_OSS_BSD)
   &audio_oss,
#endif
#ifdef HAVE_RSOUND
   &audio_rsound,
#endif
#ifdef HAVE_COREAUDIO
   &audio_coreaudio,
#endif
#ifdef HAVE_AL
   &audio_openal,
#endif
#ifdef HAVE_ROAR
   &audio_roar,
#endif
#ifdef HAVE_JACK
   &audio_jack,
#endif
#ifdef HAVE_SDL
   &audio_sdl,
#endif
#ifdef HAVE_XAUDIO
   &audio_xa,
#endif
#ifdef HAVE_DSOUND
   &audio_dsound,
#endif
#ifdef HAVE_PULSE
   &audio_pulse,
#endif
#ifdef HAVE_DYLIB
   &audio_ext,
#endif
#ifdef __CELLOS_LV2__
   &audio_ps3,
#endif
#ifdef XENON
   &audio_xenon360,
#endif
#ifdef _XBOX
   &audio_xdk360,
#endif
#ifdef GEKKO
   &audio_wii,
#endif
};

static const video_driver_t *video_drivers[] = {
#ifdef HAVE_OPENGL
   &video_gl,
#endif
#ifdef XENON
   &video_xenon360,
#endif
#ifdef _XBOX
   &video_xdk360,
#endif
#ifdef HAVE_SDL
   &video_sdl,
#endif
#ifdef HAVE_XVIDEO
   &video_xvideo,
#endif
#ifdef HAVE_DYLIB
   &video_ext,
#endif
#ifdef GEKKO
   &video_wii,
#endif
};

static const input_driver_t *input_drivers[] = {
#ifdef __CELLOS_LV2__
   &input_ps3,
#endif
#ifdef HAVE_SDL
   &input_sdl,
#endif
#ifdef HAVE_XVIDEO
   &input_x,
#endif
#ifdef XENON
   &input_xenon360,
#endif
#ifdef _XBOX
   &input_xdk360,
#endif
#ifdef GEKKO
   &input_wii,
#endif
};

static void find_audio_driver(void)
{
   for (unsigned i = 0; i < sizeof(audio_drivers) / sizeof(audio_driver_t*); i++)
   {
      if (strcasecmp(g_settings.audio.driver, audio_drivers[i]->ident) == 0)
      {
         driver.audio = audio_drivers[i];
         return;
      }
   }
   SSNES_ERR("Couldn't find any audio driver named \"%s\"\n", g_settings.audio.driver);
   fprintf(stderr, "Available audio drivers are:\n");
   for (size_t i = 0; i < sizeof(audio_drivers) / sizeof(audio_driver_t*); i++)
      fprintf(stderr, "\t%s\n", audio_drivers[i]->ident);

   ssnes_fail(1, "find_audio_driver()");
}

static void find_video_driver(void)
{
   for (unsigned i = 0; i < sizeof(video_drivers) / sizeof(video_driver_t*); i++)
   {
      if (strcasecmp(g_settings.video.driver, video_drivers[i]->ident) == 0)
      {
         driver.video = video_drivers[i];
         return;
      }
   }
   SSNES_ERR("Couldn't find any video driver named \"%s\"\n", g_settings.video.driver);
   fprintf(stderr, "Available video drivers are:\n");
   for (size_t i = 0; i < sizeof(video_drivers) / sizeof(video_driver_t*); i++)
      fprintf(stderr, "\t%s\n", video_drivers[i]->ident);

   ssnes_fail(1, "find_video_driver()");
}

static void find_input_driver(void)
{
   for (unsigned i = 0; i < sizeof(input_drivers) / sizeof(input_driver_t*); i++)
   {
      if (strcasecmp(g_settings.input.driver, input_drivers[i]->ident) == 0)
      {
         driver.input = input_drivers[i];
         return;
      }
   }
   SSNES_ERR("Couldn't find any input driver named \"%s\"\n", g_settings.input.driver);
   fprintf(stderr, "Available input drivers are:\n");
   for (size_t i = 0; i < sizeof(input_drivers) / sizeof(input_driver_t*); i++)
      fprintf(stderr, "\t%s\n", input_drivers[i]->ident);

   ssnes_fail(1, "find_input_driver()");
}

void init_drivers_pre(void)
{
   find_audio_driver();
   find_video_driver();
   find_input_driver();
}

void init_drivers(void)
{
   init_video_input();
   init_audio();
}

void uninit_drivers(void)
{
   uninit_audio();
   uninit_video_input();
}

#ifdef HAVE_DYLIB
static void init_dsp_plugin(void)
{
   if (!(*g_settings.audio.dsp_plugin))
      return;
   ssnes_dsp_info_t info = {0};

   g_extern.audio_data.dsp_lib = dylib_load(g_settings.audio.dsp_plugin);
   if (!g_extern.audio_data.dsp_lib)
   {
      SSNES_ERR("Failed to open DSP plugin: \"%s\" ...\n", g_settings.audio.dsp_plugin);
      return;
   }

   const ssnes_dsp_plugin_t* (SSNES_API_CALLTYPE *plugin_init)(void) = 
      (const ssnes_dsp_plugin_t *(SSNES_API_CALLTYPE*)(void))dylib_proc(g_extern.audio_data.dsp_lib, "ssnes_dsp_plugin_init");
   if (!plugin_init)
   {
      SSNES_ERR("Failed to find symbol \"ssnes_dsp_plugin_init\" in DSP plugin.\n");
      goto error;
   }

   g_extern.audio_data.dsp_plugin = plugin_init();
   if (!g_extern.audio_data.dsp_plugin)
   {
      SSNES_ERR("Failed to get a valid DSP plugin.\n");
      goto error;
   }

   if (g_extern.audio_data.dsp_plugin->api_version != SSNES_DSP_API_VERSION)
   {
      SSNES_ERR("DSP plugin API mismatch. SSNES: %d, Plugin: %d\n", SSNES_DSP_API_VERSION, g_extern.audio_data.dsp_plugin->api_version);
      goto error;
   }

   SSNES_LOG("Loaded DSP plugin: \"%s\"\n", g_extern.audio_data.dsp_plugin->ident ? g_extern.audio_data.dsp_plugin->ident : "Unknown");

   info.input_rate = g_settings.audio.in_rate;
   info.output_rate = g_settings.audio.out_rate;

   g_extern.audio_data.dsp_handle = g_extern.audio_data.dsp_plugin->init(&info);
   if (!g_extern.audio_data.dsp_handle)
   {
      SSNES_ERR("Failed to init DSP plugin.\n");
      goto error;
   }

   return;

error:
   if (g_extern.audio_data.dsp_lib)
      dylib_close(g_extern.audio_data.dsp_lib);
   g_extern.audio_data.dsp_plugin = NULL;
   g_extern.audio_data.dsp_lib = NULL;
}

static void deinit_dsp_plugin(void)
{
   if (g_extern.audio_data.dsp_lib && g_extern.audio_data.dsp_plugin)
   {
      g_extern.audio_data.dsp_plugin->free(g_extern.audio_data.dsp_handle);
      dylib_close(g_extern.audio_data.dsp_lib);
   }
}
#endif

static void adjust_audio_input_rate(void)
{
   if (g_extern.system.timing_set)
   {
      float timing_skew = fabs(1.0f - g_extern.system.timing.fps / g_settings.video.refresh_rate);
      if (timing_skew > 0.05f) // We don't want to adjust pitch too much. If we have extreme cases, just don't readjust at all.
      {
         SSNES_LOG("Timings deviate too much. Will not adjust. (Display = %.2f Hz, Game = %.2f Hz)\n",
               g_settings.video.refresh_rate,
               (float)g_extern.system.timing.fps);

         g_settings.video.refresh_rate = g_extern.system.timing.fps;
      }
   }

   if (g_extern.system.timing_set)
   {
      g_settings.audio.in_rate = g_extern.system.timing.sample_rate *
         (g_settings.video.refresh_rate / g_extern.system.timing.fps);
   }
   else
   {
      g_settings.audio.in_rate = 32040.5 *
         (g_settings.video.refresh_rate / (21477272.0 / 357366.0)); // SNES metrics.
   }

   SSNES_LOG("Set audio input rate to: %.2f Hz.\n", g_settings.audio.in_rate);
}

void init_audio(void)
{
   // Accomodate rewind since at some point we might have two full buffers.
   size_t max_bufsamples = AUDIO_CHUNK_SIZE_NONBLOCKING * 2;
   size_t outsamples_max = max_bufsamples * AUDIO_MAX_RATIO * g_settings.slowmotion_ratio;

   // Used for recording even if audio isn't enabled.
   ssnes_assert(g_extern.audio_data.conv_outsamples = (int16_t*)malloc(outsamples_max * sizeof(int16_t)));

   g_extern.audio_data.block_chunk_size = AUDIO_CHUNK_SIZE_BLOCKING;
   g_extern.audio_data.nonblock_chunk_size = AUDIO_CHUNK_SIZE_NONBLOCKING;
   g_extern.audio_data.chunk_size = g_extern.audio_data.block_chunk_size;

   // Needs to be able to hold full content of a full max_bufsamples in addition to its own.
   ssnes_assert(g_extern.audio_data.rewind_buf = (int16_t*)malloc(max_bufsamples * sizeof(int16_t)));
   g_extern.audio_data.rewind_size = max_bufsamples;

   if (!g_settings.audio.enable)
   {
      g_extern.audio_active = false;
      return;
   }

   adjust_audio_input_rate();

   driver.audio_data = audio_init_func(*g_settings.audio.device ? g_settings.audio.device : NULL,
         g_settings.audio.out_rate, g_settings.audio.latency);

   if (!driver.audio_data)
      g_extern.audio_active = false;

   if (g_extern.audio_active && driver.audio->use_float && audio_use_float_func())
      g_extern.audio_data.use_float = true;

   if (!g_settings.audio.sync && g_extern.audio_active)
   {
      audio_set_nonblock_state_func(true);
      g_extern.audio_data.chunk_size = g_extern.audio_data.nonblock_chunk_size;
   }

   g_extern.audio_data.source = resampler_new();
   if (!g_extern.audio_data.source)
      g_extern.audio_active = false;

   ssnes_assert(g_extern.audio_data.data = (float*)malloc(max_bufsamples * sizeof(float)));
   g_extern.audio_data.data_ptr = 0;

   ssnes_assert(g_settings.audio.out_rate < g_settings.audio.in_rate * AUDIO_MAX_RATIO);
   ssnes_assert(g_extern.audio_data.outsamples = (float*)malloc(outsamples_max * sizeof(float)));

   g_extern.audio_data.orig_src_ratio =
      g_extern.audio_data.src_ratio =
      (double)g_settings.audio.out_rate / g_settings.audio.in_rate;

   if (g_settings.audio.rate_control)
   {
      if (driver.audio->buffer_size && driver.audio->write_avail)
      {
         g_extern.audio_data.driver_buffer_size = audio_buffer_size_func();
         g_extern.audio_data.rate_control = true;
      }
      else
         SSNES_WARN("Audio rate control was desired, but driver does not support needed features.\n");
   }

#ifdef HAVE_DYLIB
   init_dsp_plugin();
#endif
}

void uninit_audio(void)
{
   free(g_extern.audio_data.conv_outsamples);
   g_extern.audio_data.conv_outsamples = NULL;
   g_extern.audio_data.data_ptr = 0;

   free(g_extern.audio_data.rewind_buf);
   g_extern.audio_data.rewind_buf = NULL;

   if (!g_settings.audio.enable)
   {
      g_extern.audio_active = false;
      return;
   }

   if (driver.audio_data && driver.audio)
      driver.audio->free(driver.audio_data);

   if (g_extern.audio_data.source)
      resampler_free(g_extern.audio_data.source);

   free(g_extern.audio_data.data);
   g_extern.audio_data.data = NULL;

   free(g_extern.audio_data.outsamples);
   g_extern.audio_data.outsamples = NULL;

#ifdef HAVE_DYLIB
   deinit_dsp_plugin();
#endif
}

#ifdef HAVE_DYLIB
static void init_filter(void)
{
   if (g_extern.filter.active)
      return;
   if (*g_settings.video.filter_path == '\0')
      return;

   SSNES_LOG("Loading bSNES filter from \"%s\"\n", g_settings.video.filter_path);
   g_extern.filter.lib = dylib_load(g_settings.video.filter_path);
   if (!g_extern.filter.lib)
   {
      SSNES_ERR("Failed to load filter \"%s\"\n", g_settings.video.filter_path);
      return;
   }

   g_extern.filter.psize = 
      (void (*)(unsigned*, unsigned*))dylib_proc(g_extern.filter.lib, "filter_size");
   g_extern.filter.prender = 
      (void (*)(uint32_t*, uint32_t*, 
                unsigned, const uint16_t*, 
                unsigned, unsigned, unsigned))dylib_proc(g_extern.filter.lib, "filter_render");
   if (!g_extern.filter.psize || !g_extern.filter.prender)
   {
      SSNES_ERR("Failed to find functions in filter...\n");
      dylib_close(g_extern.filter.lib);
      g_extern.filter.lib = NULL;
      return;
   }

   g_extern.filter.active = true;

   unsigned width = g_extern.system.geom.max_width;
   unsigned height = g_extern.system.geom.max_height;
   g_extern.filter.psize(&width, &height);

   unsigned pow2_x = next_pow2(width);
   unsigned pow2_y = next_pow2(height);
   unsigned maxsize = pow2_x > pow2_y ? pow2_x : pow2_y; 
   g_extern.filter.scale = maxsize / SSNES_SCALE_BASE;

   g_extern.filter.buffer = (uint32_t*)malloc(SSNES_SCALE_BASE * SSNES_SCALE_BASE * g_extern.filter.scale * g_extern.filter.scale * sizeof(uint32_t));
   g_extern.filter.pitch = SSNES_SCALE_BASE * g_extern.filter.scale * sizeof(uint32_t);
   ssnes_assert(g_extern.filter.buffer);

   g_extern.filter.colormap = (uint32_t*)malloc(32768 * sizeof(uint32_t));
   ssnes_assert(g_extern.filter.colormap);

   // Set up conversion map from 16-bit XRGB1555 to 32-bit ARGB.
   for (unsigned i = 0; i < 32768; i++)
   {
      unsigned r = (i >> 10) & 31;
      unsigned g = (i >>  5) & 31;
      unsigned b = (i >>  0) & 31;

      r = (r << 3) | (r >> 2);
      g = (g << 3) | (g >> 2);
      b = (b << 3) | (b >> 2);
      g_extern.filter.colormap[i] = (r << 16) | (g << 8) | (b << 0);
   }
}

static void deinit_filter(void)
{
   if (!g_extern.filter.active)
      return;

   g_extern.filter.active = false;
   dylib_close(g_extern.filter.lib);
   g_extern.filter.lib = NULL;
   free(g_extern.filter.buffer);
   free(g_extern.filter.colormap);
}
#endif

#ifdef HAVE_XML
static void init_shader_dir(void)
{
   if (!*g_settings.video.shader_dir)
      return;

   g_extern.shader_dir.elems = dir_list_new(g_settings.video.shader_dir, ".shader");
   g_extern.shader_dir.size = 0;
   g_extern.shader_dir.ptr = 0;
   if (g_extern.shader_dir.elems)
   {
      while (g_extern.shader_dir.elems[g_extern.shader_dir.size])
      {
         SSNES_LOG("Found shader \"%s\"\n", g_extern.shader_dir.elems[g_extern.shader_dir.size]);
         g_extern.shader_dir.size++;
      }
   }
}

static void deinit_shader_dir(void)
{
   // It handles NULL, no worries :D
   dir_list_free(g_extern.shader_dir.elems);
   g_extern.shader_dir.elems = NULL;
   g_extern.shader_dir.size = 0;
   g_extern.shader_dir.ptr = 0;
}
#endif

void init_video_input(void)
{
#ifdef HAVE_DYLIB
   init_filter();
#endif

#ifdef HAVE_XML
   init_shader_dir();
#endif

   unsigned max_dim = max(g_extern.system.geom.max_width, g_extern.system.geom.max_height);
   unsigned scale = max_dim / SSNES_SCALE_BASE;
   scale = max(scale, 1);

   if (g_extern.filter.active)
      scale = g_extern.filter.scale;

   unsigned width;
   unsigned height;
   if (g_settings.video.fullscreen)
   {
      width = g_settings.video.fullscreen_x;
      height = g_settings.video.fullscreen_y;
   }
   else
   {
      if (g_settings.video.force_aspect && (g_settings.video.aspect_ratio > 0.0f))
      {
         width = roundf(g_extern.system.geom.base_height * g_settings.video.xscale * g_settings.video.aspect_ratio);
         height = roundf(g_extern.system.geom.base_height * g_settings.video.yscale);
      }
      else
      {
         width = roundf(g_extern.system.geom.base_width * g_settings.video.xscale);
         height = roundf(g_extern.system.geom.base_height * g_settings.video.yscale);
      }
   }

   if (g_settings.video.aspect_ratio < 0.0f)
   {
      g_settings.video.aspect_ratio = (float)g_extern.system.geom.base_width / g_extern.system.geom.base_height;
      SSNES_LOG("Adjusting aspect ratio to %.2f\n", g_settings.video.aspect_ratio);
   }

   SSNES_LOG("Video @ %ux%u\n", width, height);

   video_info_t video = {0};
   video.width = width;
   video.height = height;
   video.fullscreen = g_settings.video.fullscreen;
   video.vsync = g_settings.video.vsync;
   video.force_aspect = g_settings.video.force_aspect;
   video.smooth = g_settings.video.smooth;
   video.input_scale = scale;
   video.rgb32 = g_extern.filter.active;

   const input_driver_t *tmp = driver.input;
   driver.video_data = video_init_func(&video, &driver.input, &driver.input_data);

   if (driver.video_data == NULL)
   {
      SSNES_ERR("Cannot open video driver ... Exiting ...\n");
      ssnes_fail(1, "init_video_input()");
   }

   if (driver.video->set_rotation && g_extern.system.rotation)
      video_set_rotation_func(g_extern.system.rotation);

   // Video driver didn't provide an input driver so we use configured one.
   if (driver.input == NULL)
   {
      driver.input = tmp;
      if (driver.input != NULL)
      {
         driver.input_data = input_init_func();
         if (driver.input_data == NULL)
         {
            SSNES_ERR("Cannot init input driver. Exiting ...\n");
            ssnes_fail(1, "init_video_input()");
         }
      }
      else
      {
         SSNES_ERR("Cannot find input driver. Exiting ...\n");
         ssnes_fail(1, "init_video_input()");
      }
   }
}

void uninit_video_input(void)
{
   if (driver.input_data != driver.video_data && driver.input)
      input_free_func();

   if (driver.video_data && driver.video)
      video_free_func();

#ifdef HAVE_DYLIB
   deinit_filter();
#endif

#ifdef HAVE_XML
   deinit_shader_dir();
#endif
}

driver_t driver;

