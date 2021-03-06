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

#ifndef __SSNES_HASH_H
#define __SSNES_HASH_H

#include <stdint.h>
#include <stddef.h>

// Hashes sha256 and outputs a human readable string for comparing with the cheat XML values.
void sha256_hash(char *out, const uint8_t *in, size_t size);

#ifdef HAVE_ZLIB
#include "console/szlib/zlib.h"
static inline uint32_t crc32_calculate(const uint8_t *data, size_t length)
{
   return crc32(0, data, length);
}

static inline uint32_t crc32_adjust(uint32_t crc, uint8_t data)
{
   return crc32(crc, &data, 1);
}
#else
uint32_t crc32_calculate(const uint8_t *data, size_t length);
uint32_t crc32_adjust(uint32_t crc, uint8_t data);
#endif

#endif

