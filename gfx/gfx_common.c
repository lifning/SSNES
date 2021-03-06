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

#include "gfx_common.h"
#include "../general.h"

#ifndef _MSC_VER
#include <sys/time.h>
#else
#include <winsock2.h>
#include <mmsystem.h>
static int gettimeofday(struct timeval *val, void *dummy)
{
   (void)dummy;
   DWORD msec = timeGetTime();
   uint64_t usec = msec * 1000;
   val->tv_sec = usec / 1000000;
   val->tv_usec = usec % 1000000;
   return 0;
}
#endif

#ifdef __CELLOS_LV2__
#include <sys/sys_time.h>
static int gettimeofday(struct timeval *val, void *dummy)
{
   (void)dummy;
   uint64_t usec = sys_time_get_system_time();
   val->tv_sec = usec / 1000000;
   val->tv_usec = usec % 1000000;
   return 0;
}
#endif

static float tv_to_fps(const struct timeval *tv, const struct timeval *new_tv, int frames)
{
   float time = new_tv->tv_sec - tv->tv_sec + (new_tv->tv_usec - tv->tv_usec)/1000000.0;
   return frames/time;
}

static unsigned gl_frames = 0;

void gfx_window_title_reset(void)
{
   gl_frames = 0;
}

bool gfx_window_title(char *buf, size_t size)
{
   static struct timeval tv;
   struct timeval new_tv;
   bool ret = false;

   if (gl_frames == 0)
   {
      gettimeofday(&tv, NULL);
      snprintf(buf, size, "%s", g_extern.title_buf);
      ret = true;
   }
   else if ((gl_frames % 180) == 0)
   {
      gettimeofday(&new_tv, NULL);
      struct timeval tmp_tv = tv;
      tv = new_tv;

      float fps = tv_to_fps(&tmp_tv, &new_tv, 180);

      snprintf(buf, size, "%s || FPS: %6.1f || Frames: %d", g_extern.title_buf, fps, gl_frames);
      ret = true;
   }

   gl_frames++;
   return ret;
}

#ifdef _WIN32
#include <windows.h>
#include "../dynamic.h"
// We only load this library once, so we let it be unloaded at application shutdown,
// since unloading it early seems to cause issues on some systems.

static dylib_t dwmlib = NULL;

static void gfx_dwm_shutdown(void)
{
   if (dwmlib)
      dylib_close(dwmlib);
}

void gfx_set_dwm(void)
{
   static bool inited = false;
   if (inited)
      return;
   inited = true;

   dwmlib = dylib_load("dwmapi.dll");
   if (!dwmlib)
   {
      SSNES_LOG("Did not find dwmapi.dll.\n");
      return;
   }
   atexit(gfx_dwm_shutdown);

   HRESULT (WINAPI *mmcss)(BOOL) = (HRESULT (WINAPI*)(BOOL))dylib_proc(dwmlib, "DwmEnableMMCSS");
   if (mmcss)
   {
      SSNES_LOG("Setting multimedia scheduling for DWM.\n");
      mmcss(TRUE);
   }

   if (!g_settings.video.disable_composition)
      return;

   HRESULT (WINAPI *composition_enable)(UINT) = (HRESULT (WINAPI*)(UINT))dylib_proc(dwmlib, "DwmEnableComposition");
   if (!composition_enable)
   {
      SSNES_ERR("Did not find DwmEnableComposition ...\n");
      return;
   }

   HRESULT ret = composition_enable(0);
   if (FAILED(ret))
      SSNES_ERR("Failed to set composition state ...\n");
}
#endif

