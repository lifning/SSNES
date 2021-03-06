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

#ifndef THREAD_H__
#define THREAD_H__

#include "boolean.h"

// Implements the bare minimum needed for SSNES. :)

typedef struct sthread sthread_t;

// Threading
sthread_t *sthread_create(void (*thread_func)(void*), void *userdata);
void sthread_join(sthread_t *thread);

// Mutexes
typedef struct slock slock_t;

slock_t *slock_new(void);
void slock_free(slock_t *lock);

void slock_lock(slock_t *lock);
void slock_unlock(slock_t *lock);

// Condition variables.
typedef struct scond scond_t;

scond_t *scond_new(void);
void scond_free(scond_t *cond);

void scond_wait(scond_t *cond, slock_t *lock);
#ifndef SSNES_CONSOLE
bool scond_wait_timeout(scond_t *cond, slock_t *lock, unsigned timeout_ms);
#endif
void scond_signal(scond_t *cond);


#endif

