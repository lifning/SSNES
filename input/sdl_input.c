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

#include "../gfx/sdlwrap.h"
#include "../boolean.h"
#include "../general.h"
#include <stdint.h>
#include <stdlib.h>
#include "../libsnes.hpp"
#include "ssnes_sdl_input.h"
#include "keysym.h"

struct key_bind
{
   unsigned sdl;
   enum ssnes_key sk;
};

static unsigned keysym_lut[SK_LAST];
static const struct key_bind lut_binds[] = {
   { SDLK_LEFT, SK_LEFT },
   { SDLK_RIGHT, SK_RIGHT },
   { SDLK_UP, SK_UP },
   { SDLK_DOWN, SK_DOWN },
   { SDLK_RETURN, SK_RETURN },
   { SDLK_TAB, SK_TAB },
   { SDLK_INSERT, SK_INSERT },
   { SDLK_DELETE, SK_DELETE },
   { SDLK_RSHIFT, SK_RSHIFT },
   { SDLK_LSHIFT, SK_LSHIFT },
   { SDLK_LCTRL, SK_LCTRL },
   { SDLK_LALT, SK_LALT },
   { SDLK_SPACE, SK_SPACE },
   { SDLK_ESCAPE, SK_ESCAPE },
   { SDLK_BACKSPACE, SK_BACKSPACE },
   { SDLK_KP_ENTER, SK_KP_ENTER },
   { SDLK_KP_PLUS, SK_KP_PLUS },
   { SDLK_KP_MINUS, SK_KP_MINUS },
   { SDLK_KP_MULTIPLY, SK_KP_MULTIPLY },
   { SDLK_KP_DIVIDE, SK_KP_DIVIDE },
   { SDLK_BACKQUOTE, SK_BACKQUOTE },
   { SDLK_PAUSE, SK_PAUSE },
   { SDLK_KP0, SK_KP0 },
   { SDLK_KP1, SK_KP1 },
   { SDLK_KP2, SK_KP2 },
   { SDLK_KP3, SK_KP3 },
   { SDLK_KP4, SK_KP4 },
   { SDLK_KP5, SK_KP5 },
   { SDLK_KP6, SK_KP6 },
   { SDLK_KP7, SK_KP7 },
   { SDLK_KP8, SK_KP8 },
   { SDLK_KP9, SK_KP9 },
   { SDLK_0, SK_0 },
   { SDLK_1, SK_1 },
   { SDLK_2, SK_2 },
   { SDLK_3, SK_3 },
   { SDLK_4, SK_4 },
   { SDLK_5, SK_5 },
   { SDLK_6, SK_6 },
   { SDLK_7, SK_7 },
   { SDLK_8, SK_8 },
   { SDLK_9, SK_9 },
   { SDLK_F1, SK_F1 },
   { SDLK_F2, SK_F2 },
   { SDLK_F3, SK_F3 },
   { SDLK_F4, SK_F4 },
   { SDLK_F5, SK_F5 },
   { SDLK_F6, SK_F6 },
   { SDLK_F7, SK_F7 },
   { SDLK_F8, SK_F8 },
   { SDLK_F9, SK_F9 },
   { SDLK_F10, SK_F10 },
   { SDLK_F11, SK_F11 },
   { SDLK_F12, SK_F12 },
   { SDLK_a, SK_a },
   { SDLK_b, SK_b },
   { SDLK_c, SK_c },
   { SDLK_d, SK_d },
   { SDLK_e, SK_e },
   { SDLK_f, SK_f },
   { SDLK_g, SK_g },
   { SDLK_h, SK_h },
   { SDLK_i, SK_i },
   { SDLK_j, SK_j },
   { SDLK_k, SK_k },
   { SDLK_l, SK_l },
   { SDLK_m, SK_m },
   { SDLK_n, SK_n },
   { SDLK_o, SK_o },
   { SDLK_p, SK_p },
   { SDLK_q, SK_q },
   { SDLK_r, SK_r },
   { SDLK_s, SK_s },
   { SDLK_t, SK_t },
   { SDLK_u, SK_u },
   { SDLK_v, SK_v },
   { SDLK_w, SK_w },
   { SDLK_x, SK_x },
   { SDLK_y, SK_y },
   { SDLK_z, SK_z },
};

static void init_lut(void)
{
   memset(keysym_lut, 0, sizeof(keysym_lut));
   for (unsigned i = 0; i < sizeof(lut_binds) / sizeof(lut_binds[0]); i++)
      keysym_lut[lut_binds[i].sk] = lut_binds[i].sdl;
}

static void *sdl_input_init(void)
{
   init_lut();
   sdl_input_t *sdl = (sdl_input_t*)calloc(1, sizeof(*sdl));
   if (!sdl)
      return NULL;

#ifdef HAVE_DINPUT
   sdl->di = sdl_dinput_init();
   if (!sdl->di)
   {
      free(sdl);
      SSNES_ERR("Failed to init SDL/DInput.\n");
      return NULL;
   }
#else
   if (SDL_Init(SDL_INIT_JOYSTICK) < 0)
      return NULL;

   SDL_JoystickEventState(SDL_IGNORE);
   sdl->num_joysticks = SDL_NumJoysticks();

   for (unsigned i = 0; i < MAX_PLAYERS; i++)
   {
      if (g_settings.input.joypad_map[i] < 0)
         continue;

      unsigned port = g_settings.input.joypad_map[i];

      if (sdl->num_joysticks <= port)
         continue;

      sdl->joysticks[i] = SDL_JoystickOpen(port);
      if (!sdl->joysticks[i])
      {
         SSNES_ERR("Couldn't open SDL joystick #%u on SNES port %u\n", port, i + 1);
         free(sdl);
         SDL_QuitSubSystem(SDL_INIT_JOYSTICK);
         return NULL;
      }

      SSNES_LOG("Opened Joystick: %s (#%u) on port %u\n", 
            SDL_JoystickName(port), port, i + 1);

      sdl->num_axes[i] = SDL_JoystickNumAxes(sdl->joysticks[i]);
      sdl->num_buttons[i] = SDL_JoystickNumButtons(sdl->joysticks[i]);
      sdl->num_hats[i] = SDL_JoystickNumHats(sdl->joysticks[i]);
      SSNES_LOG("Joypad has: %u axes, %u buttons, %u hats.\n",
            sdl->num_axes[i], sdl->num_buttons[i], sdl->num_hats[i]);
   }
#endif

   sdl->use_keyboard = true;
   return sdl;
}

static bool sdl_key_pressed(int key)
{
   return sdlwrap_key_pressed(keysym_lut[key]);
}

#ifndef HAVE_DINPUT
static bool sdl_joykey_pressed(sdl_input_t *sdl, int port_num, uint16_t joykey)
{
   if (joykey == NO_BTN)
      return false;

   // Check hat.
   if (GET_HAT_DIR(joykey))
   {
      uint16_t hat = GET_HAT(joykey);
      if (hat >= sdl->num_hats[port_num])
         return false;

      Uint8 dir = SDL_JoystickGetHat(sdl->joysticks[port_num], hat);
      switch (GET_HAT_DIR(joykey))
      {
         case HAT_UP_MASK:
            return dir & SDL_HAT_UP;
         case HAT_DOWN_MASK:
            return dir & SDL_HAT_DOWN;
         case HAT_LEFT_MASK:
            return dir & SDL_HAT_LEFT;
         case HAT_RIGHT_MASK:
            return dir & SDL_HAT_RIGHT;
         default:
            return false;
      }
   }
   else // Check the button
   {
      if (joykey < sdl->num_buttons[port_num] && SDL_JoystickGetButton(sdl->joysticks[port_num], joykey))
         return true;

      return false;
   }
}

static bool sdl_axis_pressed(sdl_input_t *sdl, int port_num, uint32_t joyaxis)
{
   if (joyaxis == AXIS_NONE)
      return false;

   if (AXIS_NEG_GET(joyaxis) < sdl->num_axes[port_num])
   {
      Sint16 val = SDL_JoystickGetAxis(sdl->joysticks[port_num], AXIS_NEG_GET(joyaxis));
      float scaled = (float)val / 0x8000;
      if (scaled < -g_settings.input.axis_threshold)
         return true;
   }
   if (AXIS_POS_GET(joyaxis) < sdl->num_axes[port_num])
   {
      Sint16 val = SDL_JoystickGetAxis(sdl->joysticks[port_num], AXIS_POS_GET(joyaxis));
      float scaled = (float)val / 0x8000;
      if (scaled > g_settings.input.axis_threshold)
         return true;
   }

   return false;
}
#endif

static bool sdl_is_pressed(sdl_input_t *sdl, unsigned port_num, const struct snes_keybind *key)
{
   if (sdl->use_keyboard && sdl_key_pressed(key->key))
      return true;

#ifdef HAVE_DINPUT
   return sdl_dinput_pressed(sdl->di, port_num, key);
#else
   if (sdl->joysticks[port_num] == NULL)
      return false;
   if (sdl_joykey_pressed(sdl, port_num, key->joykey))
      return true;
   if (sdl_axis_pressed(sdl, port_num, key->joyaxis))
      return true;
#endif

   return false;
}

static bool sdl_bind_button_pressed(void *data, int key)
{
   const struct snes_keybind *binds = g_settings.input.binds[0];
   if (key >= 0 && key < SSNES_BIND_LIST_END)
   {
      const struct snes_keybind *bind = &binds[key];
      return sdl_is_pressed((sdl_input_t*)data, 0, bind);
   }
   else
      return false;
}

static int16_t sdl_joypad_device_state(sdl_input_t *sdl, const struct snes_keybind **binds_, 
      unsigned port_num, unsigned id)
{
   const struct snes_keybind *binds = binds_[port_num];
   if (id < SSNES_BIND_LIST_END)
   {
      const struct snes_keybind *bind = &binds[id];
      return bind->valid ? (sdl_is_pressed(sdl, port_num, bind) ? 1 : 0) : 0;
   }
   else
      return 0;
}

static int16_t sdl_mouse_device_state(sdl_input_t *sdl, unsigned id)
{
   switch (id)
   {
      case SNES_DEVICE_ID_MOUSE_LEFT:
         return sdl->mouse_l;
      case SNES_DEVICE_ID_MOUSE_RIGHT:
         return sdl->mouse_r;
      case SNES_DEVICE_ID_MOUSE_X:
         return sdl->mouse_x;
      case SNES_DEVICE_ID_MOUSE_Y:
         return sdl->mouse_y;
      default:
         return 0;
   }
}

// TODO: Missing some controllers, but hey :)
static int16_t sdl_scope_device_state(sdl_input_t *sdl, unsigned id)
{
   switch (id)
   {
      case SNES_DEVICE_ID_SUPER_SCOPE_X:
         return sdl->mouse_x;
      case SNES_DEVICE_ID_SUPER_SCOPE_Y:
         return sdl->mouse_y;
      case SNES_DEVICE_ID_SUPER_SCOPE_TRIGGER:
         return sdl->mouse_l;
      case SNES_DEVICE_ID_SUPER_SCOPE_CURSOR:
         return sdl->mouse_m;
      case SNES_DEVICE_ID_SUPER_SCOPE_TURBO:
         return sdl->mouse_r;
      default:
         return 0;
   }
}

// TODO: Support two players.
static int16_t sdl_justifier_device_state(sdl_input_t *sdl, unsigned index, unsigned id)
{
   if (index == 0)
   {
      switch (id)
      {
         case SNES_DEVICE_ID_JUSTIFIER_X:
            return sdl->mouse_x;
         case SNES_DEVICE_ID_JUSTIFIER_Y:
            return sdl->mouse_y;
         case SNES_DEVICE_ID_JUSTIFIER_TRIGGER:
            return sdl->mouse_l;
         case SNES_DEVICE_ID_JUSTIFIER_START:
            return sdl->mouse_r;
         default:
            return 0;
      }
   }
   else
      return 0;
}

static int16_t sdl_input_state(void *data_, const struct snes_keybind **binds, bool port, unsigned device, unsigned index, unsigned id)
{
   sdl_input_t *data = (sdl_input_t*)data_;
   switch (device)
   {
      case SNES_DEVICE_JOYPAD:
         return sdl_joypad_device_state(data, binds, (port == SNES_PORT_1) ? 0 : 1, id);
      case SNES_DEVICE_MULTITAP:
         return sdl_joypad_device_state(data, binds, (port == SNES_PORT_2) ? 1 + index : 0, id);
      case SNES_DEVICE_MOUSE:
         return sdl_mouse_device_state(data, id);
      case SNES_DEVICE_SUPER_SCOPE:
         return sdl_scope_device_state(data, id);
      case SNES_DEVICE_JUSTIFIER:
      case SNES_DEVICE_JUSTIFIERS:
         return sdl_justifier_device_state(data, index, id);

      default:
         return 0;
   }
}

static void sdl_input_free(void *data)
{
   if (data)
   {
      // Flush out all pending events.
      SDL_Event event;
      while (SDL_PollEvent(&event));

      sdl_input_t *sdl = (sdl_input_t*)data;

#ifdef HAVE_DINPUT
      sdl_dinput_free(sdl->di);
#else
      for (int i = 0; i < MAX_PLAYERS; i++)
      {
         if (sdl->joysticks[i])
            SDL_JoystickClose(sdl->joysticks[i]);
      }
      SDL_QuitSubSystem(SDL_INIT_JOYSTICK);
#endif

      free(data);
   }
}

static void sdl_poll_mouse(sdl_input_t *sdl)
{
   int _x, _y;
   Uint8 btn = SDL_GetRelativeMouseState(&_x, &_y);
   sdl->mouse_x = _x;
   sdl->mouse_y = _y;
   sdl->mouse_l = SDL_BUTTON(SDL_BUTTON_LEFT) & btn ? 1 : 0;
   sdl->mouse_r = SDL_BUTTON(SDL_BUTTON_RIGHT) & btn ? 1 : 0;
   sdl->mouse_m = SDL_BUTTON(SDL_BUTTON_MIDDLE) & btn ? 1 : 0;
}

static void sdl_input_poll(void *data)
{
   SDL_PumpEvents();
   sdl_input_t *sdl = (sdl_input_t*)data;

#ifdef HAVE_DINPUT
   sdl_dinput_poll(sdl->di);
#else
   SDL_JoystickUpdate();
#endif

   sdl_poll_mouse(sdl);
}

const input_driver_t input_sdl = {
   sdl_input_init,
   sdl_input_poll,
   sdl_input_state,
   sdl_bind_button_pressed,
   sdl_input_free,
   "sdl"
};

