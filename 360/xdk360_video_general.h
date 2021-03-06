/*  SSNES - A frontend for libretro.
 *  Copyright (C) 2010-2012 - Hans-Kristian Arntzen
 *  Copyright (C) 2011-2012 - Daniel De Matteis
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

#ifndef _XDK360_VIDEO_GENERAL_H
#define _XDK360_VIDEO_GENERAL_H

#define LAST_ASPECT_RATIO ASPECT_RATIO_CUSTOM

#define IS_TIMER_NOT_EXPIRED() (g_frame_count < g_console.timer_expiration_frame_count)
#define IS_TIMER_EXPIRED() 	(!(IS_TIMER_NOT_EXPIRED()))
#define SET_TIMER_EXPIRATION(value) g_console.timer_expiration_frame_count = g_frame_count + value;

extern unsigned g_frame_count;

#endif