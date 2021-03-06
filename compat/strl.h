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

#ifndef __SSNES_STRL_H
#define __SSNES_STRL_H

#include <string.h>
#include <stddef.h>

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifndef HAVE_STRL

#ifdef __cplusplus
extern "C" {
#endif
// Avoid possible naming collisions during link since we prefer to use the actual name.
#define strlcpy(dst, src, size) strlcpy_ssnes__(dst, src, size)
#define strlcat(dst, src, size) strlcat_ssnes__(dst, src, size)

size_t strlcpy(char *dest, const char *source, size_t size);
size_t strlcat(char *dest, const char *source, size_t size);
#ifdef __cplusplus
}
#endif
#endif
#endif

