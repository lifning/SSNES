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

#include "file.h"
#include "general.h"
#include <stdlib.h>
#include "boolean.h"
#include "libsnes.hpp"
#include <string.h>
#include <time.h>
#include "dynamic.h"
#include "movie.h"
#include "patch.h"
#include "compat/strl.h"
#include "hash.h"

#ifdef __CELLOS_LV2__
#include <cell/cell_fs.h>
#endif

#if defined(_WIN32) && !defined(_XBOX)
#include <io.h>
#include <fcntl.h>
#include <windows.h>
#ifdef _MSC_VER
#define setmode _setmode
#endif
#elif defined(_XBOX)
#include <xtl.h>
#define setmode _setmode
#define INVALID_FILE_ATTRIBUTES -1
#else
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#endif

// Generic file loader.
ssize_t read_file(const char *path, void **buf)
{
   void *rom_buf = NULL;
   FILE *file = fopen(path, "rb");
   ssize_t rc = 0;
   size_t len = 0;
   if (!file)
      goto error;

   fseek(file, 0, SEEK_END);
   len = ftell(file);
   rewind(file);
   rom_buf = malloc(len + 1);
   if (!rom_buf)
   {
      SSNES_ERR("Couldn't allocate memory.\n");
      goto error;
   }

   if ((rc = fread(rom_buf, 1, len, file)) < (ssize_t)len)
      SSNES_WARN("Didn't read whole file.\n");

   *buf = rom_buf;
   // Allow for easy reading of strings to be safe.
   // Will only work with sane character formatting (Unix).
   ((char*)rom_buf)[len] = '\0'; 
   fclose(file);
   return rc;

error:
   if (file)
      fclose(file);
   free(rom_buf);
   *buf = NULL;
   return -1;
}

// Reads file content as one string.
bool read_file_string(const char *path, char **buf)
{
   *buf = NULL;
   FILE *file = fopen(path, "r");
   size_t len = 0;
   char *ptr = NULL;

   if (!file)
      goto error;

   fseek(file, 0, SEEK_END);
   len = ftell(file) + 2; // Takes account of being able to read in EOF and '\0' at end.
   rewind(file);

   *buf = (char*)calloc(len, sizeof(char));
   if (!*buf)
      goto error;

   ptr = *buf;

   while (ptr && !feof(file))
   {
      size_t bufsize = (size_t)(((ptrdiff_t)*buf + (ptrdiff_t)len) - (ptrdiff_t)ptr);
      fgets(ptr, bufsize, file);

      ptr += strlen(ptr);
   }

   ptr = strchr(ptr, EOF);
   if (ptr)
      *ptr = '\0';

   fclose(file);
   return true;

error:
   if (file)
      fclose(file);
   if (*buf)
      free(*buf);
   return false;
}

static void patch_rom(uint8_t **buf, ssize_t *size)
{
   uint8_t *ret_buf = *buf;
   ssize_t ret_size = *size;

   const char *patch_desc = NULL;
   const char *patch_path = NULL;
   patch_error_t err = PATCH_UNKNOWN;
   patch_func_t func = NULL;

   ssize_t patch_size = 0;
   void *patch_data = NULL;
   bool success = false;

   if (g_extern.ups_pref + g_extern.bps_pref + g_extern.ips_pref > 1)
   {
      SSNES_WARN("Several patches are explicitly defined, ignoring all ...\n");
      return;
   }

   bool allow_bps = !g_extern.ups_pref && !g_extern.ips_pref;
   bool allow_ups = !g_extern.bps_pref && !g_extern.ips_pref;
   bool allow_ips = !g_extern.ups_pref && !g_extern.bps_pref;

   if (allow_ups && *g_extern.ups_name && (patch_size = read_file(g_extern.ups_name, &patch_data)) >= 0)
   {
      patch_desc = "UPS";
      patch_path = g_extern.ups_name;
      func = ups_apply_patch;
   }
   else if (allow_bps && *g_extern.bps_name && (patch_size = read_file(g_extern.bps_name, &patch_data)) >= 0)
   {
      patch_desc = "BPS";
      patch_path = g_extern.bps_name;
      func = bps_apply_patch;
   }
   else if (allow_ips && *g_extern.ips_name && (patch_size = read_file(g_extern.ips_name, &patch_data)) >= 0)
   {
      patch_desc = "IPS";
      patch_path = g_extern.ips_name;
      func = ips_apply_patch;
   }
   else
   {
      SSNES_LOG("Did not find a valid ROM patch.\n");
      return;
   }

   SSNES_LOG("Found %s file in \"%s\", attempting to patch ...\n", patch_desc, patch_path);

   size_t target_size = ret_size * 4; // Just to be sure ...
   uint8_t *patched_rom = (uint8_t*)malloc(target_size);
   if (!patched_rom)
   {
      SSNES_ERR("Failed to allocate memory for patched ROM ...\n");
      goto error;
   }

   err = func((const uint8_t*)patch_data, patch_size, ret_buf, ret_size, patched_rom, &target_size);
   if (err == PATCH_SUCCESS)
   {
      SSNES_LOG("ROM patched successfully (%s).\n", patch_desc);
      success = true;
   }
   else
      SSNES_ERR("Failed to patch %s: Error #%u\n", patch_desc, (unsigned)err);

   if (success)
   {
      free(ret_buf);
      *buf = patched_rom;
      *size = target_size;
   }

   if (patch_data)
      free(patch_data);

   return;

error:
   *buf = ret_buf;
   *size = ret_size;
   if (patch_data)
      free(patch_data);
}

// Load SNES rom only. Applies a hack for headered ROMs.
static ssize_t read_rom_file(FILE *file, void **buf)
{
   ssize_t ret = 0;
   uint8_t *ret_buf = NULL;

   if (file == NULL) // stdin
   {
#if defined(_WIN32) && !defined(_XBOX)
      setmode(0, O_BINARY);
#endif

      SSNES_LOG("Reading ROM from stdin ...\n");
      size_t buf_size = 0xFFFFF; // Some initial guesstimate.
      size_t buf_ptr = 0;
      uint8_t *rom_buf = (uint8_t*)malloc(buf_size);
      if (rom_buf == NULL)
      {
         SSNES_ERR("Couldn't allocate memory.\n");
         return -1;
      }

      for (;;)
      {
         size_t ret = fread(rom_buf + buf_ptr, 1, buf_size - buf_ptr, stdin);
         buf_ptr += ret;

         // We've reached the end
         if (buf_ptr < buf_size)
            break;

         rom_buf = (uint8_t*)realloc(rom_buf, buf_size * 2);
         if (rom_buf == NULL)
         {
            SSNES_ERR("Couldn't allocate memory.\n");
            return -1;
         }

         buf_size *= 2;
      }

      ret_buf = rom_buf;
      ret = buf_ptr;
   }
   else
   {
      fseek(file, 0, SEEK_END);
      ret = ftell(file);
      rewind(file);

      void *rom_buf = malloc(ret);
      if (rom_buf == NULL)
      {
         SSNES_ERR("Couldn't allocate memory.\n");
         return -1;
      }

      if (fread(rom_buf, 1, ret, file) < (size_t)ret)
      {
         SSNES_ERR("Didn't read whole file.\n");
         free(rom_buf);
         return -1;
      }

      ret_buf = (uint8_t*)rom_buf;
   }

   if (!g_extern.block_patch)
   {
      // Attempt to apply a patch.
      patch_rom(&ret_buf, &ret);
   }
   
   // Remove copier header if present (512 first bytes).
   if ((ret & 0x7fff) == 512)
   {
      memmove(ret_buf, ret_buf + 512, ret - 512);
      ret -= 512;
   }

   g_extern.cart_crc = crc32_calculate(ret_buf, ret);
#ifdef HAVE_XML
   sha256_hash(g_extern.sha256, ret_buf, ret);
   SSNES_LOG("SHA256 sum: %s\n", g_extern.sha256);
#endif
   *buf = ret_buf;
   return ret;
}


// Dump stuff to file.
static bool dump_to_file(const char *path, const void *data, size_t size)
{
   FILE *file = fopen(path, "wb");
   if (!file)
      return false;
   else
   {
      bool ret = fwrite(data, 1, size, file) == size;
      fclose(file);
      return ret;
   }
}

static const char *ramtype2str(int type)
{
   switch (type)
   {
      case SNES_MEMORY_CARTRIDGE_RAM:
      case SNES_MEMORY_GAME_BOY_RAM:
      case SNES_MEMORY_BSX_RAM:
         return ".srm";

      case SNES_MEMORY_CARTRIDGE_RTC:
      case SNES_MEMORY_GAME_BOY_RTC:
         return ".rtc";

      case SNES_MEMORY_BSX_PRAM:
         return ".pram";

      case SNES_MEMORY_SUFAMI_TURBO_A_RAM:
         return ".aram";
      case SNES_MEMORY_SUFAMI_TURBO_B_RAM:
         return ".bram";

      default:
         return "";
   }
}

// Attempt to save valuable RAM data somewhere ...
static void dump_to_file_desperate(const void *data, size_t size, int type)
{
#if defined(_WIN32) && !defined(_XBOX)
   const char *base = getenv("APPDATA");
#elif defined(__CELLOS_LV2__) || defined(_XBOX)
   const char *base = NULL;
#else
   const char *base = getenv("HOME");
#endif

   if (!base)
      goto error;

   char path[PATH_MAX];
   snprintf(path, sizeof(path), "%s/SSNES-recovery-", base);
   char timebuf[PATH_MAX];

   time_t time_;
   time(&time_);
   strftime(timebuf, sizeof(timebuf), "%Y-%m-%d-%H-%M-%S", localtime(&time_));
   strlcat(path, timebuf, sizeof(path));
   strlcat(path, ramtype2str(type), sizeof(path));

   if (dump_to_file(path, data, size))
      SSNES_WARN("Succeeded in saving RAM data to \"%s\". Phew ... :D\n", path);
   else
      goto error;

   return;

error:
   SSNES_WARN("Failed ... Tough luck ... :(\n");
}

bool save_state(const char *path)
{
   SSNES_LOG("Saving state: \"%s\".\n", path);
   size_t size = psnes_serialize_size();
   if (size == 0)
      return false;

   void *data = malloc(size);
   if (!data)
   {
      SSNES_ERR("Failed to allocate memory for save state buffer.\n");
      return false;
   }

   SSNES_LOG("State size: %d bytes.\n", (int)size);
   bool ret = psnes_serialize((uint8_t*)data, size);
   if (ret)
      ret = dump_to_file(path, data, size);

   if (!ret)
      SSNES_ERR("Failed to save state to \"%s\".\n", path);

   free(data);
   return ret;
}

bool load_state(const char *path)
{
   SSNES_LOG("Loading state: \"%s\".\n", path);
   void *buf = NULL;
   ssize_t size = read_file(path, &buf);

   if (size < 0)
   {
      SSNES_ERR("Failed to load state from \"%s\".\n", path);
      return false;
   }

   bool ret = true;
   SSNES_LOG("State size: %d bytes.\n", (int)size);

   uint8_t *block_buf[2] = {NULL, NULL};
   int block_type[2] = {-1, -1};
   unsigned block_size[2] = {0};

   if (g_settings.block_sram_overwrite)
   {
      SSNES_LOG("Blocking SRAM overwrite.\n");
      switch (g_extern.game_type)
      {
         case SSNES_CART_NORMAL:
            block_type[0] = SNES_MEMORY_CARTRIDGE_RAM;
            block_type[1] = SNES_MEMORY_CARTRIDGE_RTC;
            break;

         case SSNES_CART_BSX:
         case SSNES_CART_BSX_SLOTTED:
            block_type[0] = SNES_MEMORY_BSX_RAM;
            block_type[1] = SNES_MEMORY_BSX_PRAM;
            break;

         case SSNES_CART_SUFAMI:
            block_type[0] = SNES_MEMORY_SUFAMI_TURBO_A_RAM;
            block_type[1] = SNES_MEMORY_SUFAMI_TURBO_B_RAM;
            break;

         case SSNES_CART_SGB:
            block_type[0] = SNES_MEMORY_GAME_BOY_RAM;
            block_type[1] = SNES_MEMORY_GAME_BOY_RTC;
            break;
      }
   }

   for (unsigned i = 0; i < 2; i++)
      if (block_type[i] != -1)
         block_size[i] = psnes_get_memory_size(block_type[i]);

   for (unsigned i = 0; i < 2; i++)
      if (block_size[i])
         block_buf[i] = (uint8_t*)malloc(block_size[i]);

   // Backup current SRAM which is overwritten by unserialize.
   for (unsigned i = 0; i < 2; i++)
   {
      if (block_buf[i])
      {
         const uint8_t *ptr = psnes_get_memory_data(block_type[i]);
         if (ptr)
            memcpy(block_buf[i], ptr, block_size[i]);
      }
   }

   ret = psnes_unserialize((uint8_t*)buf, size);

   // Flush back :D
   for (unsigned i = 0; i < 2 && ret; i++)
   {
      if (block_buf[i])
      {
         uint8_t *ptr = psnes_get_memory_data(block_type[i]);
         if (ptr)
            memcpy(ptr, block_buf[i], block_size[i]);
      }
   }

   for (unsigned i = 0; i < 2; i++)
      if (block_buf[i])
         free(block_buf[i]);

   free(buf);
   return ret;
}

void load_ram_file(const char *path, int type)
{
   size_t size = psnes_get_memory_size(type);
   uint8_t *data = psnes_get_memory_data(type);

   if (size == 0 || !data)
      return;

   void *buf = NULL;
   ssize_t rc = read_file(path, &buf);
   if (rc > 0 && rc <= (ssize_t)size)
      memcpy(data, buf, rc);

   free(buf);
}

void save_ram_file(const char *path, int type)
{
   size_t size = psnes_get_memory_size(type);
   uint8_t *data = psnes_get_memory_data(type);

   if (data && size > 0)
   {
      if (!dump_to_file(path, data, size))
      {
         SSNES_ERR("Failed to save SNES RAM.\n");
         SSNES_WARN("Attempting to recover ...\n");
         dump_to_file_desperate(data, size, type);
      }
   }
}

static char *load_xml_map(const char *path)
{
   char *xml_buf = NULL;
   if (*path)
   {
      if (read_file_string(path, &xml_buf))
         SSNES_LOG("Found XML memory map in \"%s\"\n", path);
   }

   return xml_buf;
}

static bool load_sgb_rom(void)
{
   void *rom_buf = NULL;
   ssize_t rom_len = 0;

   FILE *extra_rom = NULL;
   void *extra_rom_buf = NULL;
   ssize_t extra_rom_len = 0;
   char *xml_buf = 0;
   bool ret = true;

   if ((rom_len = read_rom_file(g_extern.rom_file, &rom_buf)) == -1)
   {
      SSNES_ERR("Could not read ROM file.\n");
      ret = false;
      goto end;
   }

   if ((extra_rom_len = read_file(g_extern.gb_rom_path, &extra_rom_buf)) == -1)
   {
      SSNES_ERR("Cannot read GameBoy rom.\n");
      ret = false;
      goto end;
   }

   xml_buf = load_xml_map(g_extern.xml_name);

   if (!psnes_load_cartridge_super_game_boy(
            xml_buf, (const uint8_t*)rom_buf, rom_len,
            NULL, (const uint8_t*)extra_rom_buf, extra_rom_len))
   {
      SSNES_ERR("Cannot load SGB/GameBoy rom.\n");
      ret = false;
      goto end;
   }

   if (xml_buf)
      free(xml_buf);

end:
   if (g_extern.rom_file)
      fclose(g_extern.rom_file);
   if (extra_rom)
      fclose(extra_rom);
   free(rom_buf);
   free(extra_rom_buf);
   return ret;
}

static bool load_bsx_rom(bool slotted)
{
   void *rom_buf = NULL;
   ssize_t rom_len = 0;

   FILE *extra_rom = NULL;
   void *extra_rom_buf = NULL;
   ssize_t extra_rom_len = 0;
   char *xml_buf = 0;
   bool ret = true;

   if ((rom_len = read_rom_file(g_extern.rom_file, &rom_buf)) == -1)
   {
      SSNES_ERR("Could not read ROM file.\n");
      ret = false;
      goto end;
   }

   if ((extra_rom_len = read_file(g_extern.bsx_rom_path, &extra_rom_buf)) == -1)
   {
      SSNES_ERR("Cannot read BSX game rom.\n");
      ret = false;
      goto end;
   }

   xml_buf = load_xml_map(g_extern.xml_name);

   if (slotted)
   {   
      if (!psnes_load_cartridge_bsx_slotted(
               xml_buf, (const uint8_t*)rom_buf, rom_len,
               NULL, (const uint8_t*)extra_rom_buf, extra_rom_len))
      {
         SSNES_ERR("Cannot load BSX slotted rom.\n");
         ret = false;
         goto end;
      }

   }
   else
   {
      if (!psnes_load_cartridge_bsx(
               NULL, (const uint8_t*)rom_buf, rom_len,
               NULL, (const uint8_t*)extra_rom_buf, extra_rom_len))
      {
         SSNES_ERR("Cannot load BSX rom.\n");
         ret = false;
         goto end;
      }
   }

   if (xml_buf)
      free(xml_buf);

end:
   if (g_extern.rom_file)
      fclose(g_extern.rom_file);
   if (extra_rom)
      fclose(extra_rom);
   free(rom_buf);
   free(extra_rom_buf);
   return ret;
}

static bool load_sufami_rom(void)
{
   void *rom_buf = NULL;
   ssize_t rom_len = 0;

   FILE *extra_rom[2] = {NULL};
   void *extra_rom_buf[2] = {NULL};
   ssize_t extra_rom_len[2] = {0};
   char *xml_buf = 0;
   const char *roms[2] = {0};
   bool ret = true;

   if ((rom_len = read_rom_file(g_extern.rom_file, &rom_buf)) == -1)
   {
      SSNES_ERR("Could not read ROM file.\n");
      ret = false;
      goto end;
   }
   
   roms[0] = g_extern.sufami_rom_path[0];
   roms[1] = g_extern.sufami_rom_path[1];

   for (unsigned i = 0; i < 2; i++)
   {
      if (*(roms[i]))
      {
         if ((extra_rom_len[i] = read_file(roms[i], &extra_rom_buf[i])) == -1)
         {
            SSNES_ERR("Cannot read Sufami game rom.\n");
            ret = false;
            goto end;
         }
      }
   }

   xml_buf = load_xml_map(g_extern.xml_name);

   if (!psnes_load_cartridge_sufami_turbo(
            xml_buf, (const uint8_t*)rom_buf, rom_len,
            NULL, (const uint8_t*)extra_rom_buf[0], extra_rom_len[0],
            NULL, (const uint8_t*)extra_rom_buf[1], extra_rom_len[1]))
   {
      SSNES_ERR("Cannot load Sufami Turbo rom.\n");
      ret = false;
      goto end;
   }

   if (xml_buf)
      free(xml_buf);

end:
   if (g_extern.rom_file)
      fclose(g_extern.rom_file);
   for (unsigned i = 0; i < 2; i++)
   {
      if (extra_rom[i])
         fclose(extra_rom[i]);
      free(extra_rom_buf[i]);
   }
   free(rom_buf);
   return ret;
}

static bool load_normal_rom(void)
{
   void *rom_buf = NULL;
   ssize_t rom_len = 0;

   if (!g_extern.system.need_fullpath)
   {
      if ((rom_len = read_rom_file(g_extern.rom_file, &rom_buf)) == -1)
      {
         SSNES_ERR("Could not read ROM file.\n");
         return false;
      }

      if (g_extern.rom_file)
         fclose(g_extern.rom_file);

      SSNES_LOG("ROM size: %d bytes\n", (int)rom_len);
   }
   else
   {
      if (!g_extern.rom_file)
      {
         SSNES_ERR("Implementation requires a full path to be set, cannot load ROM from stdin. Aborting ...\n");
         return false;
      }

      fclose(g_extern.rom_file);
      SSNES_LOG("ROM loading skipped. Implementation will load it on its own.\n");
   }
   
   char *xml_buf = load_xml_map(g_extern.xml_name);

   if (!psnes_load_cartridge_normal(xml_buf, (const uint8_t*)rom_buf, rom_len))
   {
      SSNES_ERR("ROM file is not valid.\n");
      free(rom_buf);
      free(xml_buf);
      return false;
   }

   free(xml_buf);
   free(rom_buf);
   return true;
}

bool init_rom_file(enum ssnes_game_type type)
{
   switch (type)
   {
      case SSNES_CART_SGB:
         if (!load_sgb_rom())
            return false;
         break;

      case SSNES_CART_NORMAL:
         if (!load_normal_rom())
            return false;
         break;

      case SSNES_CART_BSX:
         if (!load_bsx_rom(false))
            return false;
         break;

      case SSNES_CART_BSX_SLOTTED:
         if (!load_bsx_rom(true))
            return false;
         break;

      case SSNES_CART_SUFAMI:
         if (!load_sufami_rom())
            return false;
         break;
         
      default:
         SSNES_ERR("Invalid ROM type.\n");
         return false;
   }

   return true;
}

// Yep, this is C alright ;)
char **dir_list_new(const char *dir, const char *ext)
{
   size_t cur_ptr = 0;
   size_t cur_size = 32;
   char **dir_list = NULL;

#ifdef _WIN32
   WIN32_FIND_DATA ffd;
   HANDLE hFind = INVALID_HANDLE_VALUE;

   char path_buf[PATH_MAX];

   if (strlcpy(path_buf, dir, sizeof(path_buf)) >= sizeof(path_buf))
      goto error;
   if (strlcat(path_buf, "/*", sizeof(path_buf)) >= sizeof(path_buf))
      goto error;

   if (ext)
   {
      if (strlcat(path_buf, ext, sizeof(path_buf)) >= sizeof(path_buf))
         goto error;
   }

   hFind = FindFirstFile(path_buf, &ffd);
   if (hFind == INVALID_HANDLE_VALUE)
      goto error;
#else
   DIR *directory = NULL;
   const struct dirent *entry = NULL;

   directory = opendir(dir);
   if (!directory)
      goto error;
#endif

   dir_list = (char**)calloc(cur_size, sizeof(char*));
   if (!dir_list)
      goto error;

#ifdef _WIN32 // Hard to read? Blame non-POSIX heathens!
   do
#else
   while ((entry = readdir(directory)))
#endif
   {
      // Not a perfect search of course, but hopefully good enough in practice.
#ifdef _WIN32
      if (ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
         continue;
      if (ext && !strstr(ffd.cFileName, ext))
         continue;
#else
      if (ext && !strstr(entry->d_name, ext))
         continue;
#endif

      dir_list[cur_ptr] = (char*)malloc(PATH_MAX);
      if (!dir_list[cur_ptr])
         goto error;

      strlcpy(dir_list[cur_ptr], dir, PATH_MAX);
      strlcat(dir_list[cur_ptr], "/", PATH_MAX);
#ifdef _WIN32
      strlcat(dir_list[cur_ptr], ffd.cFileName, PATH_MAX);
#else
      strlcat(dir_list[cur_ptr], entry->d_name, PATH_MAX);
#endif

      cur_ptr++;
      if (cur_ptr + 1 == cur_size) // Need to reserve for NULL.
      {
         cur_size *= 2;
         dir_list = (char**)realloc(dir_list, cur_size * sizeof(char*));
         if (!dir_list)
            goto error;

         // Make sure it's all NULL'd out since we cannot rely on realloc to do this.
         memset(dir_list + cur_ptr, 0, (cur_size - cur_ptr) * sizeof(char*));
      }
   }
#if defined(_WIN32)
   while (FindNextFile(hFind, &ffd) != 0);
#endif

#ifdef _WIN32
   FindClose(hFind);
#else
   closedir(directory);
#endif
   return dir_list;

error:
   SSNES_ERR("Failed to open directory: \"%s\"\n", dir);
#ifdef _WIN32
   if (hFind != INVALID_HANDLE_VALUE)
      FindClose(hFind);
#else
   if (directory)
      closedir(directory);
#endif
   dir_list_free(dir_list);
   return NULL;
}

void dir_list_free(char **dir_list)
{
   if (!dir_list)
      return;

   char **orig = dir_list;
   while (*dir_list)
      free(*dir_list++);
   free(orig);
}

bool path_is_directory(const char *path)
{
#ifdef _WIN32
   DWORD ret = GetFileAttributes(path);
   return (ret & FILE_ATTRIBUTE_DIRECTORY) && (ret != INVALID_FILE_ATTRIBUTES);
#elif defined(__CELLOS_LV2__)
   CellFsStat buf;
   if (cellFsStat(path, &buf) < 0)
      return false;

   return buf.st_mode & CELL_FS_S_IFDIR;
#elif defined(XENON)
   // Dummy
   (void)path;
   return false;
#else
   struct stat buf;
   if (stat(path, &buf) < 0)
      return false;

   return S_ISDIR(buf.st_mode);
#endif
}

bool path_file_exists(const char *path)
{
   FILE *dummy = fopen(path, "rb");
   if (dummy)
   {
      fclose(dummy);
      return true;
   }
   return false;
}

void fill_pathname(char *out_path, const char *in_path, const char *replace, size_t size)
{
   char tmp_path[PATH_MAX];

   ssnes_assert(strlcpy(tmp_path, in_path, sizeof(tmp_path)) < sizeof(tmp_path));
   char *tok = strrchr(tmp_path, '.');
   if (tok)
      *tok = '\0';

   ssnes_assert(strlcpy(out_path, tmp_path, size) < size);
   ssnes_assert(strlcat(out_path, replace, size) < size);
}

void fill_pathname_noext(char *out_path, const char *in_path, const char *replace, size_t size)
{
   ssnes_assert(strlcpy(out_path, in_path, size) < size);
   ssnes_assert(strlcat(out_path, replace, size) < size);
}

void fill_pathname_dir(char *in_dir, const char *in_basename, const char *replace, size_t size)
{
   ssnes_assert(strlcat(in_dir, "/", size) < size);

   const char *base = strrchr(in_basename, '/');
   if (!base)
      base = strrchr(in_basename, '\\');

   if (base)
      base++;
   else
      base = in_basename;

   ssnes_assert(strlcat(in_dir, base, size) < size);
   ssnes_assert(strlcat(in_dir, replace, size) < size);
}

void fill_pathname_base(char *out_dir, const char *in_path, size_t size)
{
   const char *ptr = strrchr(in_path, '/');
   if (!ptr)
      ptr = strrchr(in_path, '\\');

   if (ptr)
      ptr++;
   else
      ptr = in_path;

   ssnes_assert(strlcpy(out_dir, ptr, size) < size);
}

