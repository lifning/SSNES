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

#include "boolean.h"
#include "libsnes.hpp"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include "driver.h"
#include "file.h"
#include "general.h"
#include "dynamic.h"
#include "audio/utils.h"
#include "record/ffemu.h"
#include "rewind.h"
#include "movie.h"
#include "compat/strl.h"
#include "screenshot.h"
#include "cheats.h"
#include "compat/getopt_ssnes.h"

#if defined(_WIN32) && !defined(_XBOX)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#elif defined(_XBOX)
#include <xtl.h>
#endif

#ifdef _WIN32
#include "msvc/msvc_compat.h"
#endif

#ifdef __APPLE__
#include "SDL.h" 
// OSX seems to really need -lSDLmain, 
// so we include SDL.h here so it can hack our main.
// We want to use -mconsole in Win32, so we need main().
#endif

// To avoid continous switching if we hold the button down, we require that the button must go from pressed, unpressed back to pressed to be able to toggle between then.
static void set_fast_forward_button(bool new_button_state, bool new_hold_button_state)
{
   bool update_sync = false;
   static bool old_button_state = false;
   static bool old_hold_button_state = false;
   static bool syncing_state = false;

   if (new_button_state && !old_button_state)
   {
      syncing_state = !syncing_state;
      update_sync = true;
   }
   else if (old_hold_button_state != new_hold_button_state)
   {
      syncing_state = new_hold_button_state;
      update_sync = true;
   }

   if (update_sync)
   {
      if (g_extern.video_active)
         video_set_nonblock_state_func(syncing_state);
      if (g_extern.audio_active)
         audio_set_nonblock_state_func(g_settings.audio.sync ? syncing_state : true);

      if (syncing_state)
         g_extern.audio_data.chunk_size =
            syncing_state ? g_extern.audio_data.nonblock_chunk_size : g_extern.audio_data.block_chunk_size;
   }

   old_button_state = new_button_state;
   old_hold_button_state = new_hold_button_state;
}

static inline unsigned lines_to_pitch(unsigned height)
{
   if (g_extern.system.pitch == 0) // SNES semantics
      return ((height == 448) || (height == 478)) ? 1024 : 2048;
   else
      return g_extern.system.pitch;
}

#ifdef HAVE_SCREENSHOTS
static void take_screenshot(void)
{
   if (!(*g_settings.screenshot_directory))
      return;

   bool ret = false;
   if (g_extern.frame_cache.data)
   {
      const uint16_t *data = g_extern.frame_cache.data;
      unsigned width = g_extern.frame_cache.width;
      unsigned height = g_extern.frame_cache.height;
      ret = screenshot_dump(g_settings.screenshot_directory,
            data, 
            width, height,
            lines_to_pitch(height));
   }

   const char *msg = NULL;
   if (ret)
   {
      SSNES_LOG("Taking screenshot.\n");
      msg = "Taking screenshot.";
   }
   else
   {
      SSNES_WARN("Failed to take screenshot ...\n");
      msg = "Failed to take screenshot.";
   }

   msg_queue_clear(g_extern.msg_queue);

   if (g_extern.is_paused)
   {
      msg_queue_push(g_extern.msg_queue, msg, 1, 1);
      ssnes_render_cached_frame();
   }
   else
      msg_queue_push(g_extern.msg_queue, msg, 1, 180);
}
#endif

#ifndef SSNES_CONSOLE
static inline void adjust_crop(const uint16_t **data, unsigned *height)
{
   // Rather SNES specific.
   unsigned pixel_pitch = lines_to_pitch(*height) >> 1;
   if (g_settings.video.crop_overscan)
   {
      if (*height == 239)
      {
         *data += 7 * pixel_pitch; // Skip 7 top scanlines.
         *height = 224;
      }
      else if (*height == 478)
      {
         *data += 15 * pixel_pitch; // Skip 15 top scanlines.
         *height = 448;
      }
   }
}
#endif

static void readjust_audio_input_rate(void)
{
   int avail = audio_write_avail_func();
   //fprintf(stderr, "Audio buffer is %u%% full\n",
   //      (unsigned)(100 - (avail * 100) / g_extern.audio_data.driver_buffer_size));

   int half_size = g_extern.audio_data.driver_buffer_size / 2;
   int delta_mid = avail - half_size;
   double direction = (double)delta_mid / half_size;

   double adjust = 1.0 + g_settings.audio.rate_control_delta * direction;

   g_extern.audio_data.src_ratio = g_extern.audio_data.orig_src_ratio * adjust;

   //fprintf(stderr, "New rate: %lf, Orig rate: %lf\n",
   //      g_extern.audio_data.src_ratio, g_extern.audio_data.orig_src_ratio);
}

// libsnes: 0.065
// Format received is 16-bit 0RRRRRGGGGGBBBBB
static void video_frame(const uint16_t *data, unsigned width, unsigned height)
{
#ifndef SSNES_CONSOLE
   if (!g_extern.video_active)
      return;

   adjust_crop(&data, &height);
#endif

   // Slightly messy code,
   // but we really need to do processing before blocking on VSync for best possible scheduling.
#ifdef HAVE_FFMPEG
   bool is_dupe = !data;

   if (g_extern.recording && (!g_extern.filter.active || !g_settings.video.post_filter_record || is_dupe))
   {
      struct ffemu_video_data ffemu_data = {0};
      ffemu_data.data = data;
      ffemu_data.pitch = lines_to_pitch(height);
      ffemu_data.width = width;
      ffemu_data.height = height;
      ffemu_data.is_dupe = is_dupe;
      ffemu_push_video(g_extern.rec, &ffemu_data);
   }

   if (is_dupe)
      return;
#endif

   const char *msg = msg_queue_pull(g_extern.msg_queue);

#ifdef HAVE_DYLIB
   if (g_extern.filter.active)
   {
      unsigned owidth = width;
      unsigned oheight = height;
      g_extern.filter.psize(&owidth, &oheight);
      g_extern.filter.prender(g_extern.filter.colormap, g_extern.filter.buffer, 
            g_extern.filter.pitch, data, lines_to_pitch(height), width, height);

#ifdef HAVE_FFMPEG
      if (g_extern.recording && g_settings.video.post_filter_record)
      {
         struct ffemu_video_data ffemu_data = {0};
         ffemu_data.data = g_extern.filter.buffer;
         ffemu_data.pitch = g_extern.filter.pitch;
         ffemu_data.width = owidth;
         ffemu_data.height = oheight;
         ffemu_push_video(g_extern.rec, &ffemu_data);
      }
#endif

      if (!video_frame_func(g_extern.filter.buffer, owidth, oheight, g_extern.filter.pitch, msg))
         g_extern.video_active = false;
   }
   else if (!video_frame_func(data, width, height, lines_to_pitch(height), msg))
      g_extern.video_active = false;
#else
   if (!video_frame_func(data, width, height, lines_to_pitch(height), msg))
      g_extern.video_active = false;
#endif

   g_extern.frame_cache.data = data;
   g_extern.frame_cache.width = width;
   g_extern.frame_cache.height = height;
}

#ifdef HAVE_GRIFFIN
#include "console/griffin/ssnes_func_hooks.h"
#endif

#ifndef HAVE_GRIFFIN_OVERRIDE_VIDEO_FRAME_FUNC
void ssnes_render_cached_frame(void)
{
#ifdef HAVE_FFMPEG
   // Cannot allow FFmpeg recording when pushing duped frames.
   bool recording = g_extern.recording;
   g_extern.recording = false;
#endif

   // Not 100% safe, since the library might have
   // freed the memory, but no known implementations do this :D
   // It would be really stupid at any rate ...
   if (g_extern.frame_cache.data)
   {
      video_frame(g_extern.frame_cache.data,
            g_extern.frame_cache.width,
            g_extern.frame_cache.height);
   }

#ifdef HAVE_FFMPEG
   g_extern.recording = recording;
#endif
}
#endif

#ifndef HAVE_GRIFFIN_OVERRIDE_AUDIO_FLUSH_FUNC
static bool audio_flush(const int16_t *data, size_t samples)
{
#ifdef HAVE_FFMPEG
   if (g_extern.recording)
   {
      struct ffemu_audio_data ffemu_data = {0};
      ffemu_data.data = data;
      ffemu_data.frames = samples / 2;
      ffemu_push_audio(g_extern.rec, &ffemu_data);
   }
#endif

   if (g_extern.is_paused)
      return true;
   if (!g_extern.audio_active)
      return false;

   const float *output_data = NULL;
   unsigned output_frames = 0;

   audio_convert_s16_to_float(g_extern.audio_data.data, data, samples);

   ssnes_dsp_output_t dsp_output = {0};
   dsp_output.should_resample = SSNES_TRUE;

#ifdef HAVE_DYLIB
   ssnes_dsp_input_t dsp_input = {0};
   dsp_input.samples = g_extern.audio_data.data;
   dsp_input.frames = samples / 2;

   if (g_extern.audio_data.dsp_plugin)
      g_extern.audio_data.dsp_plugin->process(g_extern.audio_data.dsp_handle, &dsp_output, &dsp_input);
#endif

   if (dsp_output.should_resample)
   {
      struct resampler_data src_data = {0};
      src_data.data_in = dsp_output.samples ? dsp_output.samples : g_extern.audio_data.data;
      src_data.data_out = g_extern.audio_data.outsamples;
      src_data.input_frames = dsp_output.samples ? dsp_output.frames : (samples / 2);

      if (g_extern.audio_data.rate_control)
         readjust_audio_input_rate();

      src_data.ratio = g_extern.audio_data.src_ratio;
      if (g_extern.is_slowmotion)
         src_data.ratio *= g_settings.slowmotion_ratio;

      resampler_process(g_extern.audio_data.source, &src_data);

      output_data = g_extern.audio_data.outsamples;
      output_frames = src_data.output_frames;
   }
#ifdef HAVE_DYLIB
   else
   {
      output_data = dsp_output.samples;
      output_frames = dsp_output.frames;
   }
#endif

   union
   {
      float f[0x10000];
      int16_t i[0x10000 * sizeof(float) / sizeof(int16_t)];
   } static empty_buf; // Const here would require us to statically initialize it, bloating the binary.

   if (g_extern.audio_data.use_float)
   {
      if (audio_write_func(g_extern.audio_data.mute ? empty_buf.f : output_data,
               output_frames * sizeof(float) * 2) < 0)
      {
         fprintf(stderr, "SSNES [ERROR]: Audio backend failed to write. Will continue without sound.\n");
         return false;
      }
   }
   else
   {
      if (!g_extern.audio_data.mute)
      {
         audio_convert_float_to_s16(g_extern.audio_data.conv_outsamples,
               output_data, output_frames * 2);
      }

      if (audio_write_func(g_extern.audio_data.mute ? empty_buf.i : g_extern.audio_data.conv_outsamples,
               output_frames * sizeof(int16_t) * 2) < 0)
      {
         fprintf(stderr, "SSNES [ERROR]: Audio backend failed to write. Will continue without sound.\n");
         return false;
      }
   }

   return true;
}
#endif

static void audio_sample_rewind(uint16_t left, uint16_t right)
{
   g_extern.audio_data.rewind_buf[--g_extern.audio_data.rewind_ptr] = right;
   g_extern.audio_data.rewind_buf[--g_extern.audio_data.rewind_ptr] = left;
}

static void audio_sample(uint16_t left, uint16_t right)
{
   g_extern.audio_data.conv_outsamples[g_extern.audio_data.data_ptr++] = left;
   g_extern.audio_data.conv_outsamples[g_extern.audio_data.data_ptr++] = right;

   if (g_extern.audio_data.data_ptr < g_extern.audio_data.chunk_size)
      return;

   g_extern.audio_active = audio_flush(g_extern.audio_data.conv_outsamples,
         g_extern.audio_data.data_ptr) && g_extern.audio_active;

   g_extern.audio_data.data_ptr = 0;
}

// Non-standard, alternative callback better suited for systems that process audio in batch.
// Avoids tons of calls to audio_sample() ...
unsigned audio_sample_batch(const int16_t *data, unsigned frames)
{
   if (frames > (AUDIO_CHUNK_SIZE_NONBLOCKING >> 1))
      frames = AUDIO_CHUNK_SIZE_NONBLOCKING >> 1;

   g_extern.audio_active = audio_flush(data, frames << 1) && g_extern.audio_active;
   return frames;
}

static void input_poll(void)
{
   input_poll_func();
}

static int16_t input_state(bool port, unsigned device, unsigned index, unsigned id)
{
#ifdef HAVE_BSV_MOVIE
   if (g_extern.bsv.movie && g_extern.bsv.movie_playback)
   {
      int16_t ret;
      if (bsv_movie_get_input(g_extern.bsv.movie, &ret))
         return ret;
      else
         g_extern.bsv.movie_end = true;
   }
#endif

   static const struct snes_keybind *binds[MAX_PLAYERS] = {
      g_settings.input.binds[0],
      g_settings.input.binds[1],
      g_settings.input.binds[2],
      g_settings.input.binds[3],
      g_settings.input.binds[4],
      g_settings.input.binds[5],
      g_settings.input.binds[6],
      g_settings.input.binds[7],
   };

   int16_t res = 0;
   if (id < SSNES_FIRST_META_KEY)
      res = input_input_state_func(binds, port, device, index, id);

#ifdef HAVE_BSV_MOVIE
   if (g_extern.bsv.movie && !g_extern.bsv.movie_playback)
      bsv_movie_set_input(g_extern.bsv.movie, res);
#endif

   return res;
}

#ifdef _WIN32
#define SSNES_DEFAULT_CONF_PATH_STR "\n\t\tDefaults to ssnes.cfg in same directory as ssnes.exe."
#elif defined(__APPLE__)
#define SSNES_DEFAULT_CONF_PATH_STR " Defaults to $HOME/.ssnes.cfg."
#else
#define SSNES_DEFAULT_CONF_PATH_STR " Defaults to $XDG_CONFIG_HOME/ssnes/ssnes.cfg,\n\t\tor $HOME/.ssnes.cfg, if $XDG_CONFIG_HOME is not defined."
#endif

#include "config.features.h"

#define _PSUPP(var, name, desc) printf("\t%s:\n\t\t%s: %s\n", name, desc, _##var##_supp ? "yes" : "no")
static void print_features(void)
{
   puts("");
   puts("Features:");
   _PSUPP(sdl, "SDL", "SDL drivers");
   _PSUPP(thread, "Threads", "Threading support");
   _PSUPP(opengl, "OpenGL", "OpenGL driver");
   _PSUPP(xvideo, "XVideo", "XVideo output");
   _PSUPP(alsa, "ALSA", "audio driver");
   _PSUPP(oss, "OSS", "audio driver");
   _PSUPP(jack, "Jack", "audio driver");
   _PSUPP(rsound, "RSound", "audio driver");
   _PSUPP(roar, "RoarAudio", "audio driver");
   _PSUPP(pulse, "PulseAudio", "audio driver");
   _PSUPP(dsound, "DirectSound", "audio driver");
   _PSUPP(xaudio, "XAudio2", "audio driver");
   _PSUPP(al, "OpenAL", "audio driver");
   _PSUPP(dylib, "External", "External filter and driver support");
   _PSUPP(cg, "Cg", "Cg pixel shaders");
   _PSUPP(xml, "XML", "bSNES XML pixel shaders");
   _PSUPP(sdl_image, "SDL_image", "SDL_image image loading");
   _PSUPP(fbo, "FBO", "OpenGL render-to-texture (multi-pass shaders)");
   _PSUPP(dynamic, "Dynamic", "Dynamic run-time loading of libsnes library");
   _PSUPP(ffmpeg, "FFmpeg", "On-the-fly recording of gameplay with libavcodec");
   _PSUPP(x264rgb, "x264 RGB", "x264 lossless RGB recording for FFmpeg");
   _PSUPP(configfile, "Config file", "Configuration file support");
   _PSUPP(freetype, "FreeType", "TTF font rendering with FreeType");
   _PSUPP(netplay, "Netplay", "Peer-to-peer netplay");
   _PSUPP(python, "Python", "Script support in shaders");
}
#undef _PSUPP

static void print_compiler(FILE *file)
{
   fprintf(file, "\nCompiler: ");
#if defined(_MSC_VER)
   fprintf(file, "MSVC (%d) %u-bit\n", _MSC_VER, (unsigned)(CHAR_BIT * sizeof(size_t)));
#elif defined(__SNC__)
   fprintf(file, "SNC (%d) %u-bit\n",
      __SN_VER__, (unsigned)(CHAR_BIT * sizeof(size_t)));
#elif defined(_WIN32) && defined(__GNUC__)
   fprintf(file, "MinGW (%d.%d.%d) %u-bit\n",
      __GNUC__, __GNUC_MINOR__, __GNUC_PATCHLEVEL__, (unsigned)(CHAR_BIT * sizeof(size_t)));
#elif defined(__clang__)
   fprintf(file, "Clang/LLVM (%s) %u-bit\n",
      __VERSION__, (unsigned)(CHAR_BIT * sizeof(size_t)));
#elif defined(__GNUC__)
   fprintf(file, "GCC (%d.%d.%d) %u-bit\n",
      __GNUC__, __GNUC_MINOR__, __GNUC_PATCHLEVEL__, (unsigned)(CHAR_BIT * sizeof(size_t)));
#else
   fprintf(file, "Unknown compiler %u-bit\n",
      (unsigned)(CHAR_BIT * sizeof(size_t)));
#endif
   fprintf(file, "Built: %s\n", __DATE__);
}

static void print_help(void)
{
   puts("===================================================================");
   puts("ssnes: Simple Super Nintendo Emulator (libsnes) -- v" PACKAGE_VERSION " --");
   print_compiler(stdout);
   puts("===================================================================");
   puts("Usage: ssnes [rom file] [options...]");
   puts("\t-h/--help: Show this help message.");
   puts("\t--features: Prints available features compiled into SSNES.");
   puts("\t-s/--save: Path for save file (*.srm). Required when rom is input from stdin.");
   puts("\t-f/--fullscreen: Start SSNES in fullscreen regardless of config settings.");
   puts("\t-S/--savestate: Path to use for save states. If not selected, *.state will be assumed.");
#ifdef HAVE_CONFIGFILE
   puts("\t-c/--config: Path for config file." SSNES_DEFAULT_CONF_PATH_STR);
#endif
#ifdef HAVE_DYNAMIC
   puts("\t-L/--libsnes: Path to libsnes implementation. Overrides any config setting.");
#endif
   puts("\t-g/--gameboy: Path to Gameboy ROM. Load SuperGameBoy as the regular rom.");
   puts("\t-b/--bsx: Path to BSX rom. Load BSX BIOS as the regular rom.");
   puts("\t-B/--bsxslot: Path to BSX slotted rom. Load BSX BIOS as the regular rom.");
   puts("\t--sufamiA: Path to A slot of Sufami Turbo. Load Sufami base cart as regular rom.");
   puts("\t--sufamiB: Path to B slot of Sufami Turbo.");
   puts("\t-m/--mouse: Connect a virtual mouse into designated port of the SNES (1 or 2)."); 
   puts("\t\tThis argument can be specified several times to connect more mice.");
   puts("\t-N/--nodevice: Disconnects the controller device connected to the emulated SNES (1 or 2).");
   puts("\t-p/--scope: Connect a virtual SuperScope into port 2 of the SNES.");
   puts("\t-j/--justifier: Connect a virtual Konami Justifier into port 2 of the SNES.");
   puts("\t-J/--justifiers: Daisy chain two virtual Konami Justifiers into port 2 of the SNES.");
   puts("\t-4/--multitap: Connect a multitap to port 2 of the SNES.");
#ifdef HAVE_BSV_MOVIE
   puts("\t-P/--bsvplay: Playback a BSV movie file.");
   puts("\t-R/--bsvrecord: Start recording a BSV movie file from the beginning.");
   puts("\t-M/--sram-mode: Takes an argument telling how SRAM should be handled in the session.");
#endif
   puts("\t\t{no,}load-{no,}save describes if SRAM should be loaded, and if SRAM should be saved.");
   puts("\t\tDo note that noload-save implies that save files will be deleted and overwritten.");

#ifdef HAVE_NETPLAY
   puts("\t-H/--host: Host netplay as player 1.");
   puts("\t-C/--connect: Connect to netplay as player 2.");
   puts("\t--port: Port used to netplay. Default is 55435.");
   puts("\t-F/--frames: Sync frames when using netplay.");
   puts("\t--spectate: Netplay will become spectating mode.");
   puts("\t\tHost can live stream the game content to players that connect.");
   puts("\t\tHowever, the client will not be able to play. Multiple clients can connect to the host.");
   puts("\t--nick: Picks a nickname for use with netplay. Not mandatory.");
#endif

#ifdef HAVE_FFMPEG
   puts("\t-r/--record: Path to record video file.\n\t\tUsing .mkv extension is recommended.");
   puts("\t--size: Overrides output video size when recording with FFmpeg (format: WIDTHxHEIGHT).");
#endif
   puts("\t-v/--verbose: Verbose logging.");
   puts("\t-U/--ups: Specifies path for UPS patch that will be applied to ROM.");
   puts("\t--bps: Specifies path for BPS patch that will be applied to ROM.");
   puts("\t--ips: Specifies path for IPS patch that will be applied to ROM.");
   puts("\t--no-patch: Disables all forms of rom patching.");
   puts("\t-X/--xml: Specifies path to XML memory map.");
   puts("\t-D/--detach: Detach SSNES from the running console. Not relevant for all platforms.\n");
}

static void set_basename(const char *path)
{
   strlcpy(g_extern.system.fullpath, path, sizeof(g_extern.system.fullpath));

   strlcpy(g_extern.basename, path, sizeof(g_extern.basename));
   char *dst = strrchr(g_extern.basename, '.');
   if (dst)
      *dst = '\0';
}

static void set_paths(const char *path)
{
   set_basename(path);

   SSNES_LOG("Opening file: \"%s\"\n", path);
   g_extern.rom_file = fopen(path, "rb");
   if (g_extern.rom_file == NULL)
   {
      SSNES_ERR("Could not open file: \"%s\"\n", path);
      ssnes_fail(1, "set_paths()");
   }

   if (!g_extern.has_set_save_path)
      fill_pathname_noext(g_extern.savefile_name_srm, g_extern.basename, ".srm", sizeof(g_extern.savefile_name_srm));
   if (!g_extern.has_set_state_path)
      fill_pathname_noext(g_extern.savestate_name, g_extern.basename, ".state", sizeof(g_extern.savestate_name));

   if (path_is_directory(g_extern.savefile_name_srm))
   {
      fill_pathname_dir(g_extern.savefile_name_srm, g_extern.basename, ".srm", sizeof(g_extern.savefile_name_srm));
      SSNES_LOG("Redirecting save file to \"%s\".\n", g_extern.savefile_name_srm);
   }
   if (path_is_directory(g_extern.savestate_name))
   {
      fill_pathname_dir(g_extern.savestate_name, g_extern.basename, ".state", sizeof(g_extern.savestate_name));
      SSNES_LOG("Redirecting save state to \"%s\".\n", g_extern.savestate_name);
   }

#ifdef HAVE_CONFIGFILE
   if (*g_extern.config_path && path_is_directory(g_extern.config_path))
   {
      fill_pathname_dir(g_extern.config_path, g_extern.basename, ".cfg", sizeof(g_extern.config_path));
      SSNES_LOG("Redirecting config file to \"%s\".\n", g_extern.config_path);
      if (!path_file_exists(g_extern.config_path))
      {
         *g_extern.config_path = '\0';
         SSNES_LOG("Did not find config file. Using system default.\n");
      }
   }
#endif
}

static void verify_stdin_paths(void)
{
   if (strlen(g_extern.savefile_name_srm) == 0)
   {
      SSNES_ERR("Need savefile path argument (--save) when reading rom from stdin.\n");
      print_help();
      ssnes_fail(1, "verify_stdin_paths()");
   }
   else if (strlen(g_extern.savestate_name) == 0)
   {
      SSNES_ERR("Need savestate path argument (--savestate) when reading rom from stdin.\n");
      print_help();
      ssnes_fail(1, "verify_stdin_paths()");
   }

   if (path_is_directory(g_extern.savefile_name_srm))
   {
      SSNES_ERR("Cannot specify directory for path argument (--save) when reading from stdin.\n");
      print_help();
      ssnes_fail(1, "verify_stdin_paths()");
   }
   else if (path_is_directory(g_extern.savestate_name))
   {
      SSNES_ERR("Cannot specify directory for path argument (--savestate) when reading from stdin.\n");
      print_help();
      ssnes_fail(1, "verify_stdin_paths()");
   }

#ifdef HAVE_CONFIGFILE
   else if (path_is_directory(g_extern.config_path))
   {
      SSNES_ERR("Cannot specify directory for config file (--config) when reading from stdin.\n");
      print_help();
      ssnes_fail(1, "verify_stdin_paths()");
   }
#endif
}

static void parse_input(int argc, char *argv[])
{
   if (argc < 2)
   {
      print_help();
      ssnes_fail(1, "parse_input()");
   }

   // Make sure we can call parse_input several times ...
   optind = 1;

   int val = 0;

   const struct option opts[] = {
#ifdef HAVE_DYNAMIC
      { "libsnes", 1, NULL, 'L' },
#endif
      { "help", 0, NULL, 'h' },
      { "save", 1, NULL, 's' },
      { "fullscreen", 0, NULL, 'f' },
#ifdef HAVE_FFMPEG
      { "record", 1, NULL, 'r' },
      { "size", 1, &val, 's' },
#endif
      { "verbose", 0, NULL, 'v' },
      { "gameboy", 1, NULL, 'g' },
#ifdef HAVE_CONFIGFILE
      { "config", 0, NULL, 'c' },
#endif
      { "mouse", 1, NULL, 'm' },
      { "nodevice", 1, NULL, 'N' },
      { "scope", 0, NULL, 'p' },
      { "savestate", 1, NULL, 'S' },
      { "bsx", 1, NULL, 'b' },
      { "bsxslot", 1, NULL, 'B' },
      { "justifier", 0, NULL, 'j' },
      { "justifiers", 0, NULL, 'J' },
      { "multitap", 0, NULL, '4' },
      { "sufamiA", 1, NULL, 'Y' },
      { "sufamiB", 1, NULL, 'Z' },
#ifdef HAVE_BSV_MOVIE
      { "bsvplay", 1, NULL, 'P' },
      { "bsvrecord", 1, NULL, 'R' },
      { "sram-mode", 1, NULL, 'M' },
#endif
#ifdef HAVE_NETPLAY
      { "host", 0, NULL, 'H' },
      { "connect", 1, NULL, 'C' },
      { "frames", 1, NULL, 'F' },
      { "port", 1, &val, 'p' },
      { "spectate", 0, &val, 'S' },
      { "nick", 1, &val, 'N' },
#endif
      { "ups", 1, NULL, 'U' },
      { "bps", 1, &val, 'B' },
      { "ips", 1, &val, 'I' },
      { "no-patch", 0, &val, 'n' },
      { "xml", 1, NULL, 'X' },
      { "detach", 0, NULL, 'D' },
      { "features", 0, &val, 'f' },
      { NULL, 0, NULL, 0 }
   };

#ifdef HAVE_FFMPEG
#define FFMPEG_RECORD_ARG "r:"
#else
#define FFMPEG_RECORD_ARG
#endif

#ifdef HAVE_CONFIGFILE
#define CONFIG_FILE_ARG "c:"
#else
#define CONFIG_FILE_ARG
#endif

#ifdef HAVE_DYNAMIC
#define DYNAMIC_ARG "L:"
#else
#define DYNAMIC_ARG
#endif

#ifdef HAVE_NETPLAY
#define NETPLAY_ARG "HC:F:"
#else
#define NETPLAY_ARG
#endif

#ifdef HAVE_BSV_MOVIE
#define BSV_MOVIE_ARG "P:R:M:"
#else
#define BSV_MOVIE_ARG
#endif

   const char *optstring = "hs:fvS:m:p4jJg:b:B:Y:Z:U:DN:X:" BSV_MOVIE_ARG NETPLAY_ARG DYNAMIC_ARG FFMPEG_RECORD_ARG CONFIG_FILE_ARG;
   for (;;)
   {
      val = 0;
      int c = getopt_long(argc, argv, optstring, opts, NULL);
      int port;

      if (c == -1)
         break;

      switch (c)
      {
         case 'h':
            print_help();
            exit(0);

         case '4':
            g_extern.has_multitap = true;
            break;

         case 'j':
            g_extern.has_justifier = true;
            break;

         case 'J':
            g_extern.has_justifiers = true;
            break;

         case 's':
            strlcpy(g_extern.savefile_name_srm, optarg, sizeof(g_extern.savefile_name_srm));
            g_extern.has_set_save_path = true;
            break;

         case 'f':
            g_extern.force_fullscreen = true;
            break;

         case 'g':
            strlcpy(g_extern.gb_rom_path, optarg, sizeof(g_extern.gb_rom_path));
            g_extern.game_type = SSNES_CART_SGB;
            break;

         case 'b':
            strlcpy(g_extern.bsx_rom_path, optarg, sizeof(g_extern.bsx_rom_path));
            g_extern.game_type = SSNES_CART_BSX;
            break;

         case 'B':
            strlcpy(g_extern.bsx_rom_path, optarg, sizeof(g_extern.bsx_rom_path));
            g_extern.game_type = SSNES_CART_BSX_SLOTTED;
            break;

         case 'Y':
            strlcpy(g_extern.sufami_rom_path[0], optarg, sizeof(g_extern.sufami_rom_path[0]));
            g_extern.game_type = SSNES_CART_SUFAMI;
            break;

         case 'Z':
            strlcpy(g_extern.sufami_rom_path[1], optarg, sizeof(g_extern.sufami_rom_path[1]));
            g_extern.game_type = SSNES_CART_SUFAMI;
            break;

         case 'S':
            strlcpy(g_extern.savestate_name, optarg, sizeof(g_extern.savestate_name));
            g_extern.has_set_state_path = true;
            break;

         case 'v':
            g_extern.verbose = true;
            break;

         case 'm':
            port = strtol(optarg, NULL, 0);
            if (port < 1 || port > 2)
            {
               SSNES_ERR("Connect mouse to port 1 or 2.\n");
               print_help();
               ssnes_fail(1, "parse_input()");
            }
            g_extern.has_mouse[port - 1] = true;
            break;

         case 'N':
            port = strtol(optarg, NULL, 0);
            if (port < 1 || port > 2)
            {
               SSNES_ERR("Disconnected device from port 1 or 2.\n");
               print_help();
               ssnes_fail(1, "parse_input()");
            }
            g_extern.disconnect_device[port - 1] = true;
            break;

         case 'p':
            g_extern.has_scope[1] = true;
            break;

#ifdef HAVE_CONFIGFILE
         case 'c':
            strlcpy(g_extern.config_path, optarg, sizeof(g_extern.config_path));
            break;
#endif

#ifdef HAVE_FFMPEG
         case 'r':
            strlcpy(g_extern.record_path, optarg, sizeof(g_extern.record_path));
            g_extern.recording = true;
            break;
#endif

#ifdef HAVE_DYNAMIC
         case 'L':
            strlcpy(g_settings.libsnes, optarg, sizeof(g_settings.libsnes));
            break;
#endif

#ifdef HAVE_BSV_MOVIE
         case 'P':
         case 'R':
            strlcpy(g_extern.bsv.movie_start_path, optarg,
                  sizeof(g_extern.bsv.movie_start_path));
            g_extern.bsv.movie_start_playback = c == 'P';
            g_extern.bsv.movie_start_recording = c == 'R';
            break;

         case 'M':
            if (strcmp(optarg, "noload-nosave") == 0)
            {
               g_extern.sram_load_disable = true;
               g_extern.sram_save_disable = true;
            }
            else if (strcmp(optarg, "noload-save") == 0)
               g_extern.sram_load_disable = true;
            else if (strcmp(optarg, "load-nosave") == 0)
               g_extern.sram_save_disable = true;
            else if (strcmp(optarg, "load-save") != 0)
            {
               SSNES_ERR("Invalid argument in --sram-mode.\n");
               print_help();
               ssnes_fail(1, "parse_input()");
            }
            break;
#endif

#ifdef HAVE_NETPLAY
         case 'H':
            g_extern.netplay_enable = true;
            break;

         case 'C':
            g_extern.netplay_enable = true;
            strlcpy(g_extern.netplay_server, optarg, sizeof(g_extern.netplay_server));
            break;

         case 'F':
            g_extern.netplay_sync_frames = strtol(optarg, NULL, 0);
            if (g_extern.netplay_sync_frames > 16)
               g_extern.netplay_sync_frames = 16;
            break;
#endif

         case 'U':
            strlcpy(g_extern.ups_name, optarg, sizeof(g_extern.ups_name));
            g_extern.ups_pref = true;
            break;

         case 'X':
            strlcpy(g_extern.xml_name, optarg, sizeof(g_extern.xml_name));
            break;

         case 'D':
#if defined(_WIN32) && !defined(_XBOX)
            FreeConsole();
#endif
            break;

         case 0:
            switch (val)
            {
#ifdef HAVE_NETPLAY
               case 'p':
                  g_extern.netplay_port = strtoul(optarg, NULL, 0);
                  break;

               case 'S':
                  g_extern.netplay_is_spectate = true;
                  break;

               case 'N':
                  strlcpy(g_extern.netplay_nick, optarg, sizeof(g_extern.netplay_nick));
                  break;
#endif

               case 'B':
                  strlcpy(g_extern.bps_name, optarg, sizeof(g_extern.bps_name));
                  g_extern.bps_pref = true;
                  break;

               case 'I':
                  strlcpy(g_extern.ips_name, optarg, sizeof(g_extern.ips_name));
                  g_extern.ips_pref = true;
                  break;

               case 'n':
                  g_extern.block_patch = true;
                  break;

#ifdef HAVE_FFMPEG
               case 's':
               {
                  errno = 0;
                  char *ptr;
                  g_extern.record_width = strtoul(optarg, &ptr, 0);
                  if ((*ptr != 'x') || errno)
                  {
                     SSNES_ERR("Wrong format for --size.\n");
                     print_help();
                     ssnes_fail(1, "parse_input()");
                  }

                  ptr++;
                  g_extern.record_height = strtoul(ptr, &ptr, 0);
                  if ((*ptr != '\0') || errno)
                  {
                     SSNES_ERR("Wrong format for --size.\n");
                     print_help();
                     ssnes_fail(1, "parse_input()");
                  }
                  break;
               }
#endif
               case 'f':
                  print_features();
                  exit(0);

               default:
                  break;
            }
            break;

         case '?':
            print_help();
            ssnes_fail(1, "parse_input()");

         default:
            SSNES_ERR("Error parsing arguments.\n");
            ssnes_fail(1, "parse_input()");
      }
   }

   if (optind < argc)
      set_paths(argv[optind]);
   else
      verify_stdin_paths();
}

// TODO: Add rest of the controllers.
static void init_controllers(void)
{
   if (g_extern.has_justifier)
   {
      SSNES_LOG("Connecting Justifier to port 2.\n");
      psnes_set_controller_port_device(SNES_PORT_2, SNES_DEVICE_JUSTIFIER);
   }
   else if (g_extern.has_justifiers)
   {
      SSNES_LOG("Connecting Justifiers to port 2.\n");
      psnes_set_controller_port_device(SNES_PORT_2, SNES_DEVICE_JUSTIFIERS);
   }
   else if (g_extern.has_multitap)
   {
      SSNES_LOG("Connecting Multitap to port 2.\n");
      psnes_set_controller_port_device(SNES_PORT_2, SNES_DEVICE_MULTITAP);
   }
   else
   {
      for (unsigned i = 0; i < 2; i++)
      {
         if (g_extern.disconnect_device[i])
         {
            SSNES_LOG("Disconnecting device from port %u.\n", i + 1);
            psnes_set_controller_port_device(i, SNES_DEVICE_NONE);
         }
         else if (g_extern.has_mouse[i])
         {
            SSNES_LOG("Connecting mouse to port %u.\n", i + 1);
            psnes_set_controller_port_device(i, SNES_DEVICE_MOUSE);
         }
         else if (g_extern.has_scope[i])
         {
            SSNES_LOG("Connecting scope to port %u.\n", i + 1);
            psnes_set_controller_port_device(i, SNES_DEVICE_SUPER_SCOPE);
         }
      }
   }
}

static inline void load_save_files(void)
{
   switch (g_extern.game_type)
   {
      case SSNES_CART_NORMAL:
         load_ram_file(g_extern.savefile_name_srm, SNES_MEMORY_CARTRIDGE_RAM);
         load_ram_file(g_extern.savefile_name_rtc, SNES_MEMORY_CARTRIDGE_RTC);
         break;

      case SSNES_CART_SGB:
         save_ram_file(g_extern.savefile_name_srm, SNES_MEMORY_GAME_BOY_RAM);
         save_ram_file(g_extern.savefile_name_rtc, SNES_MEMORY_GAME_BOY_RTC);
         break;

      case SSNES_CART_BSX:
      case SSNES_CART_BSX_SLOTTED:
         load_ram_file(g_extern.savefile_name_srm, SNES_MEMORY_BSX_RAM);
         load_ram_file(g_extern.savefile_name_psrm, SNES_MEMORY_BSX_PRAM);
         break;

      case SSNES_CART_SUFAMI:
         load_ram_file(g_extern.savefile_name_asrm, SNES_MEMORY_SUFAMI_TURBO_A_RAM);
         load_ram_file(g_extern.savefile_name_bsrm, SNES_MEMORY_SUFAMI_TURBO_B_RAM);
         break;

      default:
         break;
   }
}

static inline void save_files(void)
{
   switch (g_extern.game_type)
   {
      case SSNES_CART_NORMAL:
         SSNES_LOG("Saving regular SRAM.\n");
         save_ram_file(g_extern.savefile_name_srm, SNES_MEMORY_CARTRIDGE_RAM);
         save_ram_file(g_extern.savefile_name_rtc, SNES_MEMORY_CARTRIDGE_RTC);
         break;

      case SSNES_CART_SGB:
         SSNES_LOG("Saving Gameboy SRAM.\n");
         save_ram_file(g_extern.savefile_name_srm, SNES_MEMORY_GAME_BOY_RAM);
         save_ram_file(g_extern.savefile_name_rtc, SNES_MEMORY_GAME_BOY_RTC);
         break;

      case SSNES_CART_BSX:
      case SSNES_CART_BSX_SLOTTED:
         SSNES_LOG("Saving BSX (P)RAM.\n");
         save_ram_file(g_extern.savefile_name_srm, SNES_MEMORY_BSX_RAM);
         save_ram_file(g_extern.savefile_name_psrm, SNES_MEMORY_BSX_PRAM);
         break;

      case SSNES_CART_SUFAMI:
         SSNES_LOG("Saving Sufami turbo A/B RAM.\n");
         save_ram_file(g_extern.savefile_name_asrm, SNES_MEMORY_SUFAMI_TURBO_A_RAM);
         save_ram_file(g_extern.savefile_name_bsrm, SNES_MEMORY_SUFAMI_TURBO_B_RAM);
         break;

      default:
         break;
   }
}


#ifdef HAVE_FFMPEG
static void init_recording(void)
{
   if (!g_extern.recording)
      return;

   // Canonical values.
   double fps = psnes_get_region() == SNES_REGION_NTSC ? 60.00 : 50.00;
   double samplerate = 32000.0;
   if (g_extern.system.timing_set)
   {
      fps = g_extern.system.timing.fps;
      samplerate = g_extern.system.timing.sample_rate;
      SSNES_LOG("Custom timing given: FPS: %.4f, Sample rate: %.4f\n", (float)fps, (float)samplerate);
   }

   struct ffemu_params params = {0};
   params.out_width = g_extern.system.geom.base_width;
   params.out_height = g_extern.system.geom.base_height;
   params.fb_width = g_extern.system.geom.max_width;
   params.fb_height = g_extern.system.geom.max_height;
   params.channels = 2;
   params.filename = g_extern.record_path;
   params.fps = fps;
   params.samplerate = samplerate;
   params.rgb32 = false;

   if (g_extern.record_width || g_extern.record_height)
   {
      params.out_width = g_extern.record_width;
      params.out_height = g_extern.record_height;
   }
   else if (g_settings.video.hires_record)
   {
      params.out_width *= 2;
      params.out_height *= 2;
   }

   if (g_settings.video.force_aspect && (g_settings.video.aspect_ratio > 0.0f))
      params.aspect_ratio = g_settings.video.aspect_ratio;
   else
      params.aspect_ratio = (float)params.out_width / params.out_height;

   if (g_settings.video.post_filter_record && g_extern.filter.active)
   {
      g_extern.filter.psize(&params.out_width, &params.out_height);
      params.rgb32 = true;

      unsigned max_width = params.fb_width;
      unsigned max_height = params.fb_height;
      g_extern.filter.psize(&max_width, &max_height);
      params.fb_width = next_pow2(max_width);
      params.fb_height = next_pow2(max_height);
   }

   SSNES_LOG("Recording with FFmpeg to %s @ %ux%u. (FB size: %ux%u 32-bit: %s)\n", g_extern.record_path, params.out_width, params.out_height, params.fb_width, params.fb_height, params.rgb32 ? "yes" : "no");
   g_extern.rec = ffemu_new(&params);
   if (!g_extern.rec)
   {
      SSNES_ERR("Failed to start FFmpeg recording.\n");
      g_extern.recording = false;
   }
}

static void deinit_recording(void)
{
   if (g_extern.recording)
   {
      ffemu_finalize(g_extern.rec);
      ffemu_free(g_extern.rec);
   }
}
#endif

static void init_msg_queue(void)
{
   ssnes_assert(g_extern.msg_queue = msg_queue_new(8));
}

static void deinit_msg_queue(void)
{
   if (g_extern.msg_queue)
      msg_queue_free(g_extern.msg_queue);
}

#ifdef HAVE_XML
static void init_cheats(void)
{
   if (*g_settings.cheat_database)
      g_extern.cheat = cheat_manager_new(g_settings.cheat_database);
}

static void deinit_cheats(void)
{
   if (g_extern.cheat)
      cheat_manager_free(g_extern.cheat);
}
#endif

static void init_rewind(void)
{
   if (!g_settings.rewind_enable)
      return;

   g_extern.state_size = psnes_serialize_size();

   // Make sure we allocate at least 4-byte multiple.
   size_t aligned_state_size = (g_extern.state_size + 3) & ~3;
   g_extern.state_buf = calloc(1, aligned_state_size);

   if (!g_extern.state_buf)
   {
      SSNES_ERR("Failed to allocate memory for rewind buffer.\n");
      return;
   }

   if (!psnes_serialize((uint8_t*)g_extern.state_buf, g_extern.state_size))
   {
      SSNES_ERR("Failed to perform initial serialization for rewind.\n");
      free(g_extern.state_buf);
      g_extern.state_buf = NULL;
      return;
   }

   SSNES_LOG("Initing rewind buffer with size: %u MB\n", (unsigned)(g_settings.rewind_buffer_size / 1000000));
   g_extern.state_manager = state_manager_new(aligned_state_size, g_settings.rewind_buffer_size, g_extern.state_buf);

   if (!g_extern.state_manager)
      SSNES_WARN("Failed to init rewind buffer. Rewinding will be disabled.\n");
}

static void deinit_rewind(void)
{
   if (g_extern.state_manager)
      state_manager_free(g_extern.state_manager);
   if (g_extern.state_buf)
      free(g_extern.state_buf);
}

#ifdef HAVE_BSV_MOVIE
static void init_movie(void)
{
   if (g_extern.bsv.movie_start_playback)
   {
      g_extern.bsv.movie = bsv_movie_init(g_extern.bsv.movie_start_path, SSNES_MOVIE_PLAYBACK);
      if (!g_extern.bsv.movie)
      {
         SSNES_ERR("Failed to load movie file: \"%s\".\n", g_extern.bsv.movie_start_path);
         ssnes_fail(1, "init_movie()");
      }

      g_extern.bsv.movie_playback = true;
      msg_queue_push(g_extern.msg_queue, "Starting movie playback.", 2, 180);
      SSNES_LOG("Starting movie playback.\n");
      g_settings.rewind_granularity = 1;
   }
   else if (g_extern.bsv.movie_start_recording)
   {
      char msg[PATH_MAX];
      snprintf(msg, sizeof(msg), "Starting movie record to \"%s\".",
            g_extern.bsv.movie_start_path);

      g_extern.bsv.movie = bsv_movie_init(g_extern.bsv.movie_start_path, SSNES_MOVIE_RECORD);
      msg_queue_clear(g_extern.msg_queue);
      msg_queue_push(g_extern.msg_queue,
            g_extern.bsv.movie ? msg : "Failed to start movie record.", 1, 180);

      if (g_extern.bsv.movie)
      {
         SSNES_LOG("Starting movie record to \"%s\".\n", g_extern.bsv.movie_start_path);
         g_settings.rewind_granularity = 1;
      }
      else
         SSNES_ERR("Failed to start movie record.\n");
   }
}

static void deinit_movie(void)
{
   if (g_extern.bsv.movie)
      bsv_movie_free(g_extern.bsv.movie);
}
#endif

#define SSNES_DEFAULT_PORT 55435

#ifdef HAVE_NETPLAY
static void init_netplay(void)
{
   if (!g_extern.netplay_enable)
      return;

   struct snes_callbacks cbs = {0};
   cbs.frame_cb = video_frame;
   cbs.sample_cb = audio_sample;
   cbs.state_cb = input_state;

   if (*g_extern.netplay_server)
   {
      SSNES_LOG("Connecting to netplay host...\n");
      g_extern.netplay_is_client = true;
   }
   else
      SSNES_LOG("Waiting for client...\n");

   g_extern.netplay = netplay_new(g_extern.netplay_is_client ? g_extern.netplay_server : NULL,
         g_extern.netplay_port ? g_extern.netplay_port : SSNES_DEFAULT_PORT,
         g_extern.netplay_sync_frames, &cbs, g_extern.netplay_is_spectate,
         g_extern.netplay_nick);

   if (!g_extern.netplay)
   {
      g_extern.netplay_is_client = false;
      SSNES_WARN("Failed to init netplay ...\n");

      if (g_extern.msg_queue)
      {
         msg_queue_push(g_extern.msg_queue,
               "Failed to init netplay ...",
               0, 180);
      }
   }
}

static void deinit_netplay(void)
{
   if (g_extern.netplay)
      netplay_free(g_extern.netplay);
}
#endif

static void init_libsnes_cbs(void)
{
#ifdef HAVE_NETPLAY
   if (g_extern.netplay)
   {
      psnes_set_video_refresh(g_extern.netplay_is_spectate ?
            video_frame : video_frame_net);
      psnes_set_audio_sample(g_extern.netplay_is_spectate ?
            audio_sample : audio_sample_net);

      psnes_set_input_state(g_extern.netplay_is_spectate ?
            (g_extern.netplay_is_client ? input_state_spectate_client : input_state_spectate)
            : input_state_net);
   }
   else
   {
      psnes_set_video_refresh(video_frame);
      psnes_set_audio_sample(audio_sample);
      psnes_set_input_state(input_state);
   }
#else
   psnes_set_video_refresh(video_frame);
   psnes_set_audio_sample(audio_sample);
   psnes_set_input_state(input_state);
#endif
   psnes_set_input_poll(input_poll);
}

#ifdef HAVE_THREADS
static void init_autosave(void)
{
   int ram_types[2] = {-1, -1};
   const char *ram_paths[2] = {NULL, NULL};

   switch (g_extern.game_type)
   {
      case SSNES_CART_BSX:
      case SSNES_CART_BSX_SLOTTED:
         ram_types[0] = SNES_MEMORY_BSX_RAM;
         ram_types[1] = SNES_MEMORY_BSX_PRAM;
         ram_paths[0] = g_extern.savefile_name_srm;
         ram_paths[1] = g_extern.savefile_name_psrm;
         break;

      case SSNES_CART_SUFAMI:
         ram_types[0] = SNES_MEMORY_SUFAMI_TURBO_A_RAM;
         ram_types[1] = SNES_MEMORY_SUFAMI_TURBO_B_RAM;
         ram_paths[0] = g_extern.savefile_name_asrm;
         ram_paths[1] = g_extern.savefile_name_bsrm;
         break;

      case SSNES_CART_SGB:
         ram_types[0] = SNES_MEMORY_GAME_BOY_RAM;
         ram_types[1] = SNES_MEMORY_GAME_BOY_RTC;
         ram_paths[0] = g_extern.savefile_name_srm;
         ram_paths[1] = g_extern.savefile_name_rtc;
         break;

      default:
         ram_types[0] = SNES_MEMORY_CARTRIDGE_RAM;
         ram_types[1] = SNES_MEMORY_CARTRIDGE_RTC;
         ram_paths[0] = g_extern.savefile_name_srm;
         ram_paths[1] = g_extern.savefile_name_rtc;
   }

   if (g_settings.autosave_interval > 0)
   {
      for (unsigned i = 0; i < sizeof(g_extern.autosave)/sizeof(g_extern.autosave[0]); i++)
      {
         if (ram_paths[i] && strlen(ram_paths[i]) > 0 && psnes_get_memory_size(ram_types[i]) > 0)
         {
            g_extern.autosave[i] = autosave_new(ram_paths[i], 
                  psnes_get_memory_data(ram_types[i]), 
                  psnes_get_memory_size(ram_types[i]), 
                  g_settings.autosave_interval);
            if (!g_extern.autosave[i])
               SSNES_WARN("Could not initialize autosave.\n");
         }
      }
   }
}

static void deinit_autosave(void)
{
   for (unsigned i = 0; i < sizeof(g_extern.autosave)/sizeof(g_extern.autosave[0]); i++)
   {
      if (g_extern.autosave[i])
         autosave_free(g_extern.autosave[i]);
   }
}
#endif

static void set_savestate_auto_index(void)
{
   if (!g_settings.savestate_auto_index)
      return;

   char state_path[PATH_MAX];
   strlcpy(state_path, g_extern.savestate_name, sizeof(state_path));

   char *split = strrchr(state_path, '/');
   if (!split)
      split = strrchr(state_path, '\\');

   const char *base = state_path;
   const char *dir = state_path;
   if (split)
   {
      *split = '\0';
      base = split + 1;
   }

   unsigned max_index = 0;

   char **dir_list = dir_list_new(dir, NULL);
   if (!dir_list)
      return;

   unsigned index = 0;
   const char *dir_elem;
   while ((dir_elem = dir_list[index++]))
   {
      if (!strstr(dir_elem, base))
         continue;

      const char *end = dir_elem + strlen(dir_elem);
      while ((end != dir_elem) && isdigit(end[-1])) end--;

      unsigned index = strtoul(end, NULL, 0);
      if (index > max_index)
         max_index = index;
   }

   dir_list_free(dir_list);

   g_extern.state_slot = max_index;
   SSNES_LOG("Found last state slot: #%u\n", g_extern.state_slot);
}

static void fill_pathnames(void)
{
   switch (g_extern.game_type)
   {
      case SSNES_CART_BSX:
      case SSNES_CART_BSX_SLOTTED:
         // BSX PSRM
         if (!g_extern.has_set_save_path)
         {
            fill_pathname(g_extern.savefile_name_srm,
                  g_extern.bsx_rom_path, ".srm", sizeof(g_extern.savefile_name_srm));
         }

         fill_pathname(g_extern.savefile_name_psrm,
               g_extern.savefile_name_srm, ".psrm", sizeof(g_extern.savefile_name_psrm));

         if (!g_extern.has_set_state_path)
         {
            fill_pathname(g_extern.savestate_name,
                  g_extern.bsx_rom_path, ".state", sizeof(g_extern.savestate_name));
         }
         break;

      case SSNES_CART_SUFAMI:
         if (g_extern.has_set_save_path && *g_extern.sufami_rom_path[0] && *g_extern.sufami_rom_path[1])
            SSNES_WARN("Sufami Turbo SRAM paths will be inferred from their respective paths to avoid conflicts.\n");

         // SUFAMI ARAM
         fill_pathname(g_extern.savefile_name_asrm,
               g_extern.sufami_rom_path[0], ".srm", sizeof(g_extern.savefile_name_asrm));

         // SUFAMI BRAM
         fill_pathname(g_extern.savefile_name_bsrm,
               g_extern.sufami_rom_path[1], ".srm", sizeof(g_extern.savefile_name_bsrm));

         if (!g_extern.has_set_state_path)
         {
            fill_pathname(g_extern.savestate_name,
                  *g_extern.sufami_rom_path[0] ?
                     g_extern.sufami_rom_path[0] : g_extern.sufami_rom_path[1],
                     ".state", sizeof(g_extern.savestate_name));
         }
         break;

      case SSNES_CART_SGB:
         if (!g_extern.has_set_save_path)
         {
            fill_pathname(g_extern.savefile_name_srm,
                  g_extern.gb_rom_path, ".srm", sizeof(g_extern.savefile_name_srm));
         }

         if (!g_extern.has_set_state_path)
         {
            fill_pathname(g_extern.savestate_name,
                  g_extern.gb_rom_path, ".state", sizeof(g_extern.savestate_name));
         }

         fill_pathname(g_extern.savefile_name_rtc,
               g_extern.savefile_name_srm, ".rtc", sizeof(g_extern.savefile_name_rtc));
         break;

      default:
         // Infer .rtc save path from save ram path.
         fill_pathname(g_extern.savefile_name_rtc,
               g_extern.savefile_name_srm, ".rtc", sizeof(g_extern.savefile_name_rtc));
   }

#ifdef HAVE_BSV_MOVIE
   fill_pathname(g_extern.bsv.movie_path, g_extern.savefile_name_srm, "", sizeof(g_extern.bsv.movie_path));
#endif

   if (*g_extern.basename)
   {
      if (!(*g_extern.ups_name))
         fill_pathname_noext(g_extern.ups_name, g_extern.basename, ".ups", sizeof(g_extern.ups_name));

      if (!(*g_extern.bps_name))
         fill_pathname_noext(g_extern.bps_name, g_extern.basename, ".bps", sizeof(g_extern.bps_name));

      if (!(*g_extern.ips_name))
         fill_pathname_noext(g_extern.ips_name, g_extern.basename, ".ips", sizeof(g_extern.ips_name));

      if (!(*g_extern.xml_name))
         fill_pathname_noext(g_extern.xml_name, g_extern.basename, ".xml", sizeof(g_extern.xml_name));

#ifdef HAVE_SCREENSHOTS
      if (!(*g_settings.screenshot_directory))
      {
         strlcpy(g_settings.screenshot_directory, g_extern.basename, sizeof(g_settings.screenshot_directory));
         char *dir = strrchr(g_settings.screenshot_directory, '/');
         if (!dir)
            dir = strrchr(g_settings.screenshot_directory, '\\');
         if (dir)
            *dir = '\0';
      }
#endif
   }
}

static void load_auto_state(void)
{
   char savestate_name_auto[PATH_MAX];
   fill_pathname_noext(savestate_name_auto, g_extern.savestate_name,
         ".auto", sizeof(savestate_name_auto));

   if (path_file_exists(savestate_name_auto))
   {
      SSNES_LOG("Found auto savestate in: %s\n", savestate_name_auto);
      load_state(savestate_name_auto);

      char msg[PATH_MAX];
      snprintf(msg, sizeof(msg), "Auto-loaded savestate from: \"%s\"", savestate_name_auto);
      msg_queue_push(g_extern.msg_queue, msg, 1, 180);
   }
}

void ssnes_load_state(void)
{
   char load_path[PATH_MAX];

   if (g_extern.state_slot > 0)
      snprintf(load_path, sizeof(load_path), "%s%u", g_extern.savestate_name, g_extern.state_slot);
   else
      snprintf(load_path, sizeof(load_path), "%s", g_extern.savestate_name);

   char msg[512];
   if (load_state(load_path))
      snprintf(msg, sizeof(msg), "Loaded state from slot #%u.", g_extern.state_slot);
   else
      snprintf(msg, sizeof(msg), "Failed to load state from \"%s\".", load_path);

   msg_queue_clear(g_extern.msg_queue);
   msg_queue_push(g_extern.msg_queue, msg, 2, 180);
}

void ssnes_save_state(void)
{
   if (g_settings.savestate_auto_index)
      g_extern.state_slot++;

   char save_path[PATH_MAX];

   if (g_extern.state_slot > 0)
      snprintf(save_path, sizeof(save_path), "%s%u", g_extern.savestate_name, g_extern.state_slot);
   else
      snprintf(save_path, sizeof(save_path), "%s", g_extern.savestate_name);

   char msg[512];
   if (save_state(save_path))
      snprintf(msg, sizeof(msg), "Saved state to slot #%u.", g_extern.state_slot);
   else
      snprintf(msg, sizeof(msg), "Failed to save state to \"%s\".", save_path);

   msg_queue_clear(g_extern.msg_queue);
   msg_queue_push(g_extern.msg_queue, msg, 2, 180);
}

// Save or load state here.
static void check_savestates(bool immutable)
{
   static bool old_should_savestate = false;
   bool should_savestate = input_key_pressed_func(SSNES_SAVE_STATE_KEY);

   if (should_savestate && !old_should_savestate)
      ssnes_save_state();
   old_should_savestate = should_savestate;

   if (!immutable)
   {
      static bool old_should_loadstate = false;
      bool should_loadstate = input_key_pressed_func(SSNES_LOAD_STATE_KEY);

      if (!should_savestate && should_loadstate && !old_should_loadstate)
         ssnes_load_state();
      old_should_loadstate = should_loadstate;
   }
}

#ifndef SSNES_CONSOLE
static bool check_fullscreen(void)
{
   // If we go fullscreen we drop all drivers and reinit to be safe.
   static bool was_pressed = false;
   bool pressed = input_key_pressed_func(SSNES_FULLSCREEN_TOGGLE_KEY);
   bool toggle = pressed && !was_pressed;
   if (toggle)
   {
      g_settings.video.fullscreen = !g_settings.video.fullscreen;
      uninit_drivers();
      init_drivers();

      // Poll input to avoid possibly stale data to corrupt things.
      if (driver.input)
         input_poll_func();
   }

   was_pressed = pressed;
   return toggle;
}
#endif

void ssnes_state_slot_increase(void)
{
   g_extern.state_slot++;

   if (g_extern.msg_queue)
      msg_queue_clear(g_extern.msg_queue);
   char msg[256];

   snprintf(msg, sizeof(msg), "Save state/movie slot: %u", g_extern.state_slot);

   if (g_extern.msg_queue)
      msg_queue_push(g_extern.msg_queue, msg, 1, 180);

   SSNES_LOG("%s\n", msg);
}

void ssnes_state_slot_decrease(void)
{
   if (g_extern.state_slot > 0)
      g_extern.state_slot--;

   if (g_extern.msg_queue)
      msg_queue_clear(g_extern.msg_queue);

   char msg[256];

   snprintf(msg, sizeof(msg), "Save state/movie slot: %u", g_extern.state_slot);

   if (g_extern.msg_queue)
      msg_queue_push(g_extern.msg_queue, msg, 1, 180);

   SSNES_LOG("%s\n", msg);
}

static void check_stateslots(void)
{
   // Save state slots
   static bool old_should_slot_increase = false;
   bool should_slot_increase = input_key_pressed_func(SSNES_STATE_SLOT_PLUS);
   if (should_slot_increase && !old_should_slot_increase)
      ssnes_state_slot_increase();
   old_should_slot_increase = should_slot_increase;

   static bool old_should_slot_decrease = false;
   bool should_slot_decrease = input_key_pressed_func(SSNES_STATE_SLOT_MINUS);
   if (should_slot_decrease && !old_should_slot_decrease)
      ssnes_state_slot_decrease();
   old_should_slot_decrease = should_slot_decrease;
}

static void check_input_rate(void)
{
   bool display = false;
   if (input_key_pressed_func(SSNES_AUDIO_INPUT_RATE_PLUS))
   {
      g_settings.audio.in_rate += g_settings.audio.rate_step;
      display = true;
   }
   else if (input_key_pressed_func(SSNES_AUDIO_INPUT_RATE_MINUS))
   {
      g_settings.audio.in_rate -= g_settings.audio.rate_step;
      display = true;
   }

   if (display)
   {
      char msg[256];
      snprintf(msg, sizeof(msg), "Audio input rate: %.2f Hz", g_settings.audio.in_rate);

      msg_queue_clear(g_extern.msg_queue);
      msg_queue_push(g_extern.msg_queue, msg, 1, 180);
      SSNES_LOG("%s\n", msg);

      g_extern.audio_data.src_ratio =
         (double)g_settings.audio.out_rate / g_settings.audio.in_rate;
   }
}

static inline void flush_rewind_audio(void)
{
   if (g_extern.frame_is_reverse) // We just rewound. Flush rewind audio buffer.
   {
      g_extern.audio_active = audio_flush(g_extern.audio_data.rewind_buf + g_extern.audio_data.rewind_ptr,
            g_extern.audio_data.rewind_size - g_extern.audio_data.rewind_ptr) && g_extern.audio_active;
   }
}

static inline void setup_rewind_audio(void)
{
   // Push audio ready to be played.
   g_extern.audio_data.rewind_ptr = g_extern.audio_data.rewind_size;
   for (unsigned i = 0; i < g_extern.audio_data.data_ptr; i += 2)
   {
      g_extern.audio_data.rewind_buf[--g_extern.audio_data.rewind_ptr] =
         g_extern.audio_data.conv_outsamples[i + 1];

      g_extern.audio_data.rewind_buf[--g_extern.audio_data.rewind_ptr] =
         g_extern.audio_data.conv_outsamples[i + 0];
   }

   g_extern.audio_data.data_ptr = 0;
}

static void check_rewind(void)
{
   flush_rewind_audio();
   g_extern.frame_is_reverse = false;

   static bool first = true;
   if (first)
   {
      first = false;
      return;
   }

   if (!g_extern.state_manager)
      return;

   if (input_key_pressed_func(SSNES_REWIND))
   {
      msg_queue_clear(g_extern.msg_queue);
      void *buf;
      if (state_manager_pop(g_extern.state_manager, &buf))
      {
         g_extern.frame_is_reverse = true;
         setup_rewind_audio();

         msg_queue_push(g_extern.msg_queue, "Rewinding.", 0, g_extern.is_paused ? 1 : 30);
         psnes_unserialize((uint8_t*)buf, g_extern.state_size);

#ifdef HAVE_BSV_MOVIE
         if (g_extern.bsv.movie)
            bsv_movie_frame_rewind(g_extern.bsv.movie);
#endif
      }
      else
         msg_queue_push(g_extern.msg_queue, "Reached end of rewind buffer.", 0, 30);
   }
   else
   {
      static unsigned cnt = 0;
      cnt = (cnt + 1) % (g_settings.rewind_granularity ? g_settings.rewind_granularity : 1); // Avoid possible SIGFPE.
#ifdef HAVE_BSV_MOVIE
      if (cnt == 0 || g_extern.bsv.movie)
#else
      if (cnt == 0)
#endif
      {
         psnes_serialize((uint8_t*)g_extern.state_buf, g_extern.state_size);
         state_manager_push(g_extern.state_manager, g_extern.state_buf);
      }
   }

   psnes_set_audio_sample(g_extern.frame_is_reverse ? audio_sample_rewind : audio_sample);
}

static void check_slowmotion(void)
{
   g_extern.is_slowmotion = input_key_pressed_func(SSNES_SLOWMOTION);
   if (g_extern.is_slowmotion)
   {
      msg_queue_clear(g_extern.msg_queue);
      msg_queue_push(g_extern.msg_queue, g_extern.frame_is_reverse ? "Slow motion rewind." : "Slow motion.", 0, 30);
   }
}

#ifdef HAVE_BSV_MOVIE
static void movie_record_toggle(void)
{
   if (g_extern.bsv.movie)
   {
      msg_queue_clear(g_extern.msg_queue);
      msg_queue_push(g_extern.msg_queue, "Stopping movie record.", 2, 180);
      SSNES_LOG("Stopping movie record.\n");
      bsv_movie_free(g_extern.bsv.movie);
      g_extern.bsv.movie = NULL;
   }
   else
   {
      g_settings.rewind_granularity = 1;

      char path[PATH_MAX];
      if (g_extern.state_slot > 0)
      {
         snprintf(path, sizeof(path), "%s%u.bsv",
               g_extern.bsv.movie_path, g_extern.state_slot);
      }
      else
      {
         snprintf(path, sizeof(path), "%s.bsv",
               g_extern.bsv.movie_path);
      }

      char msg[PATH_MAX];
      snprintf(msg, sizeof(msg), "Starting movie record to \"%s\".", path);

      g_extern.bsv.movie = bsv_movie_init(path, SSNES_MOVIE_RECORD);
      msg_queue_clear(g_extern.msg_queue);
      msg_queue_push(g_extern.msg_queue, g_extern.bsv.movie ? msg : "Failed to start movie record.", 1, 180);

      if (g_extern.bsv.movie)
         SSNES_LOG("Starting movie record to \"%s\".\n", path);
      else
         SSNES_ERR("Failed to start movie record.\n");
   }
}

static void check_movie_record(bool pressed)
{
   if (pressed)
      movie_record_toggle();
}

static void check_movie_playback(bool pressed)
{
   if (g_extern.bsv.movie_end || pressed)
   {
      msg_queue_push(g_extern.msg_queue, "Movie playback ended.", 1, 180);
      SSNES_LOG("Movie playback ended.\n");

      bsv_movie_free(g_extern.bsv.movie);
      g_extern.bsv.movie = NULL;
      g_extern.bsv.movie_end = false;
      g_extern.bsv.movie_playback = false;
   }
}

static void check_movie(void)
{
   static bool old_button = false;
   bool new_button = input_key_pressed_func(SSNES_MOVIE_RECORD_TOGGLE);
   bool pressed = new_button && !old_button;

   if (g_extern.bsv.movie_playback)
      check_movie_playback(pressed);
   else
      check_movie_record(pressed);

   old_button = new_button;
}
#endif

#ifndef SSNES_CONSOLE
static void check_pause(void)
{
   static bool old_state = false;
   bool new_state = input_key_pressed_func(SSNES_PAUSE_TOGGLE);

   // FRAMEADVANCE will set us into pause mode.
   new_state |= !g_extern.is_paused && input_key_pressed_func(SSNES_FRAMEADVANCE);

   static bool old_focus = true;
   bool focus = true;

   if (g_settings.pause_nonactive)
      focus = video_focus_func();

   if (focus && new_state && !old_state)
   {
      g_extern.is_paused = !g_extern.is_paused;

      if (g_extern.is_paused)
      {
         SSNES_LOG("Paused.\n");
         if (driver.audio_data)
            audio_stop_func();
      }
      else 
      {
         SSNES_LOG("Unpaused.\n");
         if (driver.audio_data)
         {
            if (!audio_start_func())
            {
               SSNES_ERR("Failed to resume audio driver. Will continue without audio.\n");
               g_extern.audio_active = false;
            }
         }
      }
   }
   else if (focus && !old_focus)
   {
      SSNES_LOG("Unpaused.\n");
      g_extern.is_paused = false;
      if (driver.audio_data && !audio_start_func())
      {
         SSNES_ERR("Failed to resume audio driver. Will continue without audio.\n");
         g_extern.audio_active = false;
      }
   }
   else if (!focus && old_focus)
   {
      SSNES_LOG("Paused.\n");
      g_extern.is_paused = true;
      if (driver.audio_data)
         audio_stop_func();
   }

   old_focus = focus;
   old_state = new_state;
}
#endif

static void check_oneshot(void)
{
   static bool old_state = false;
   bool new_state = input_key_pressed_func(SSNES_FRAMEADVANCE);
   g_extern.is_oneshot = (new_state && !old_state);
   old_state = new_state;

   // Rewind buttons works like FRAMEREWIND when paused. We will one-shot in that case.
   static bool old_rewind_state = false;
   bool new_rewind_state = input_key_pressed_func(SSNES_REWIND);
   g_extern.is_oneshot |= new_rewind_state && !old_rewind_state;
   old_rewind_state = new_rewind_state;
}

void ssnes_game_reset(void)
{
   SSNES_LOG("Resetting game.\n");
   msg_queue_clear(g_extern.msg_queue);
   msg_queue_push(g_extern.msg_queue, "Reset.", 1, 120);
   psnes_reset();
   init_controllers(); // bSNES since v073r01 resets controllers to JOYPAD after a reset, so just enforce it here.
}

static void check_reset(void)
{
   static bool old_state = false;
   bool new_state = input_key_pressed_func(SSNES_RESET);
   if (new_state && !old_state)
      ssnes_game_reset();

   old_state = new_state;
}

#ifdef HAVE_XML
static void check_shader_dir(void)
{
   static bool old_pressed_next = false;
   static bool old_pressed_prev = false;

   if (!g_extern.shader_dir.elems || !driver.video->xml_shader)
      return;

   bool should_apply = false;
   bool pressed_next = input_key_pressed_func(SSNES_SHADER_NEXT);
   bool pressed_prev = input_key_pressed_func(SSNES_SHADER_PREV);
   if (pressed_next && !old_pressed_next)
   {
      should_apply = true;
      g_extern.shader_dir.ptr = (g_extern.shader_dir.ptr + 1) % g_extern.shader_dir.size;
   }
   else if (pressed_prev && !old_pressed_prev)
   {
      should_apply = true;
      if (g_extern.shader_dir.ptr == 0)
         g_extern.shader_dir.ptr = g_extern.shader_dir.size - 1;
      else
         g_extern.shader_dir.ptr--;
   }

   if (should_apply)
   {
      const char *shader = g_extern.shader_dir.elems[g_extern.shader_dir.ptr];

      strlcpy(g_settings.video.bsnes_shader_path, shader, sizeof(g_settings.video.bsnes_shader_path));
      g_settings.video.shader_type = SSNES_SHADER_BSNES;

      msg_queue_clear(g_extern.msg_queue);
      char msg[512];
      snprintf(msg, sizeof(msg), "XML shader #%u: \"%s\"", (unsigned)g_extern.shader_dir.ptr, shader);
      msg_queue_push(g_extern.msg_queue, msg, 1, 120);
      SSNES_LOG("Applying shader \"%s\"\n", shader);

      if (!video_xml_shader_func(shader))
         SSNES_WARN("Failed to apply shader.\n");
   }

   old_pressed_next = pressed_next;
   old_pressed_prev = pressed_prev;
}

static void check_cheats(void)
{
   if (!g_extern.cheat)
      return;

   static bool old_pressed_prev = false;
   static bool old_pressed_next = false;
   static bool old_pressed_toggle = false;

   bool pressed_next = input_key_pressed_func(SSNES_CHEAT_INDEX_PLUS);
   bool pressed_prev = input_key_pressed_func(SSNES_CHEAT_INDEX_MINUS);
   bool pressed_toggle = input_key_pressed_func(SSNES_CHEAT_TOGGLE);

   if (pressed_next && !old_pressed_next)
      cheat_manager_index_next(g_extern.cheat);
   else if (pressed_prev && !old_pressed_prev)
      cheat_manager_index_prev(g_extern.cheat);
   else if (pressed_toggle && !old_pressed_toggle)
      cheat_manager_toggle(g_extern.cheat);

   old_pressed_prev = pressed_prev;
   old_pressed_next = pressed_next;
   old_pressed_toggle = pressed_toggle;
}
#endif

#ifdef HAVE_SCREENSHOTS
static void check_screenshot(void)
{
   static bool old_pressed = false;
   bool pressed = input_key_pressed_func(SSNES_SCREENSHOT);
   if (pressed && !old_pressed)
      take_screenshot();

   old_pressed = pressed;
}
#endif

#ifdef HAVE_DYLIB
static void check_dsp_config(void)
{
   if (!g_extern.audio_data.dsp_plugin || !g_extern.audio_data.dsp_plugin->config)
      return;

   static bool old_pressed = false;
   bool pressed = input_key_pressed_func(SSNES_DSP_CONFIG);
   if (pressed && !old_pressed)
      g_extern.audio_data.dsp_plugin->config(g_extern.audio_data.dsp_handle);

   old_pressed = pressed;
}
#endif

#ifndef SSNES_CONSOLE
static void check_mute(void)
{
   if (!g_extern.audio_active)
      return;

   static bool old_pressed = false;
   bool pressed = input_key_pressed_func(SSNES_MUTE);
   if (pressed && !old_pressed)
   {
      g_extern.audio_data.mute = !g_extern.audio_data.mute;

      const char *msg = g_extern.audio_data.mute ? "Audio muted." : "Audio unmuted.";
      msg_queue_clear(g_extern.msg_queue);
      msg_queue_push(g_extern.msg_queue, msg, 1, 180);

      SSNES_LOG("%s\n", msg);
   }

   old_pressed = pressed;
}
#endif

#ifdef HAVE_NETPLAY
static void check_netplay_flip(void)
{
   static bool old_pressed = false;
   bool pressed = input_key_pressed_func(SSNES_NETPLAY_FLIP);
   if (pressed && !old_pressed)
      netplay_flip_players(g_extern.netplay);

   old_pressed = pressed;
}
#endif

static void do_state_checks(void)
{
#ifdef HAVE_SCREENSHOTS
   check_screenshot();
#endif
#ifndef SSNES_CONSOLE
   check_mute();
#endif

#ifdef HAVE_NETPLAY
   if (!g_extern.netplay)
   {
#endif
#ifndef SSNES_CONSOLE
      check_pause();
#endif
      check_oneshot();

#ifdef SSNES_CONSOLE
      if (g_extern.is_paused)
#else
      if (check_fullscreen() && g_extern.is_paused)
#endif
      {
         ssnes_render_cached_frame();
      }

#ifndef SSNES_CONSOLE
      if (g_extern.is_paused && !g_extern.is_oneshot)
         return;
#endif

      set_fast_forward_button(
            input_key_pressed_func(SSNES_FAST_FORWARD_KEY),
            input_key_pressed_func(SSNES_FAST_FORWARD_HOLD_KEY));

      check_stateslots();
#ifdef HAVE_BSV_MOVIE
      check_savestates(g_extern.bsv.movie);
#else
      check_savestates(false);
#endif

      check_rewind();
      check_slowmotion();

#ifdef HAVE_BSV_MOVIE
      check_movie();
#endif
     
#ifdef HAVE_XML
      check_shader_dir();
      check_cheats();
#endif

#ifdef HAVE_DYLIB
      check_dsp_config();
#endif
      check_reset();
#ifdef HAVE_NETPLAY
   }
   else
   {
      check_netplay_flip();
#ifndef SSNES_CONSOLE
      check_fullscreen();
#endif
   }
#endif

#ifdef HAVE_DYLIB
   // DSP plugin doesn't use variable input rate.
   if (!g_extern.audio_data.dsp_plugin)
#endif
      check_input_rate();
}

static void fill_title_buf(void)
{
   snprintf(g_extern.title_buf, sizeof(g_extern.title_buf), "SSNES : %s", psnes_library_id());
}

static void init_state(void)
{
   g_extern.video_active = true;
   g_extern.audio_active = true;
   g_extern.game_type = SSNES_CART_NORMAL;
}

void ssnes_main_clear_state(void)
{
   memset(&g_settings, 0, sizeof(g_settings));

   free(g_extern.system.environment);
   free(g_extern.system.environment_split);
   memset(&g_extern, 0, sizeof(g_extern));

#ifdef SSNES_CONSOLE
   memset(&g_console, 0, sizeof(g_console));
#endif

   init_state();
}

int ssnes_main_init(int argc, char *argv[])
{
   init_state();
   parse_input(argc, argv);

   int sjlj_ret;
   if ((sjlj_ret = setjmp(g_extern.error_sjlj_context)) > 0)
   {
      SSNES_ERR("Fatal error received in: \"%s\"\n", g_extern.error_string); 
      return sjlj_ret;
   }
   g_extern.error_in_init = true;

   if (g_extern.verbose)
   {
      fprintf(stderr, "=== Build =======================================");
      print_compiler(stderr);
      fprintf(stderr, "=================================================\n");
   }

   config_load();
   init_libsnes_sym();
   fill_title_buf();
   init_drivers_pre();

   psnes_init();
   if (*g_extern.basename)
      psnes_set_cartridge_basename(g_extern.basename);

   SSNES_LOG("Version of libsnes API: %u.%u\n",
         psnes_library_revision_major(), psnes_library_revision_minor());

   g_extern.use_sram = true;
#ifdef HAVE_XML
   bool allow_cheats = true;
#endif

   fill_pathnames();
   set_savestate_auto_index();

   if (!init_rom_file(g_extern.game_type))
      goto error;

   init_msg_queue();

   if (!g_extern.sram_load_disable)
      load_save_files();
   else
      SSNES_LOG("Skipping SRAM load.\n");

   load_auto_state();

#ifdef HAVE_BSV_MOVIE
   init_movie();
#endif

#ifdef HAVE_NETPLAY
   init_netplay();
#endif
   init_drivers();

#ifdef HAVE_NETPLAY
   if (!g_extern.netplay)
#endif
      init_rewind();
      
   init_libsnes_cbs();
   init_controllers();
   
#ifdef HAVE_FFMPEG
   init_recording();
#endif

#ifdef HAVE_NETPLAY
   g_extern.use_sram = !g_extern.sram_save_disable && !g_extern.netplay_is_client;
#else
   g_extern.use_sram = !g_extern.sram_save_disable;
#endif

   if (!g_extern.use_sram)
      SSNES_LOG("SRAM will not be saved.\n");

#ifdef HAVE_THREADS
   if (g_extern.use_sram)
      init_autosave();
#endif
      
#ifdef HAVE_XML
#ifdef HAVE_NETPLAY
   allow_cheats &= !g_extern.netplay;
#endif
#ifdef HAVE_BSV_MOVIE
   allow_cheats &= !g_extern.bsv.movie;
#endif
   if (allow_cheats)
      init_cheats();
#endif

   g_extern.error_in_init = false;
   return 0;

error:
   psnes_unload_cartridge();
   psnes_term();
   uninit_drivers();
   uninit_libsnes_sym();

   return 1;
}

bool ssnes_main_iterate(void)
{
#ifdef HAVE_DYLIB
   // DSP plugin GUI events.
   if (g_extern.audio_data.dsp_handle && g_extern.audio_data.dsp_plugin->events)
      g_extern.audio_data.dsp_plugin->events(g_extern.audio_data.dsp_handle);
#endif

   // Time to drop?
   if (input_key_pressed_func(SSNES_QUIT_KEY) ||
         !video_alive_func())
      return false;

   // Checks for stuff like fullscreen, save states, etc.
   do_state_checks();

   // Run libsnes for one frame.
#ifndef SSNES_CONSOLE // On consoles pausing is handled better elsewhere.
   if (!g_extern.is_paused || g_extern.is_oneshot)
#endif
   {
#ifdef HAVE_THREADS
      lock_autosave();
#endif

#ifdef HAVE_NETPLAY
      if (g_extern.netplay)
         netplay_pre_frame(g_extern.netplay);
#endif

#ifdef HAVE_BSV_MOVIE
      if (g_extern.bsv.movie)
         bsv_movie_set_frame_start(g_extern.bsv.movie);
#endif

      psnes_run();

#ifdef HAVE_BSV_MOVIE
      if (g_extern.bsv.movie)
         bsv_movie_set_frame_end(g_extern.bsv.movie);
#endif

#ifdef HAVE_NETPLAY
      if (g_extern.netplay)
         netplay_post_frame(g_extern.netplay);
#endif

#ifdef HAVE_THREADS
      unlock_autosave();
#endif
   }
#ifndef SSNES_CONSOLE
   else
   {
      input_poll();
      ssnes_sleep(10);
   }
#endif

   return true;
}

void ssnes_main_deinit(void)
{
#ifdef HAVE_NETPLAY
   deinit_netplay();
#endif

#ifdef HAVE_THREADS
   if (g_extern.use_sram)
      deinit_autosave();
#endif

#ifdef HAVE_FFMPEG
   deinit_recording();
#endif

   if (g_extern.use_sram)
      save_files();

#ifdef HAVE_NETPLAY
   if (!g_extern.netplay)
#endif
      deinit_rewind();

#ifdef HAVE_XML
   deinit_cheats();
#endif

#ifdef HAVE_BSV_MOVIE
   deinit_movie();
#endif
   deinit_msg_queue();

   psnes_unload_cartridge();
   psnes_term();
   uninit_drivers();
   uninit_libsnes_sym();
}

#ifndef SSNES_CONSOLE
// Consoles use the higher level API.
int main(int argc, char *argv[])
{
   int init_ret;
   if ((init_ret = ssnes_main_init(argc, argv))) return init_ret;
   while (ssnes_main_iterate());
   ssnes_main_deinit();
   ssnes_main_clear_state();
   return 0;
}
#endif

