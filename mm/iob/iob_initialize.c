/****************************************************************************
 * mm/iob/iob_initialize.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.  The
 * ASF licenses this file to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance with the
 * License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdbool.h>

#include <nuttx/nuttx.h>
#include <nuttx/mm/iob.h>

#include "iob.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Fix the I/O Buffer size with specified alignment size */

#ifdef CONFIG_IOB_ALLOC
#  define IOB_ALIGN_SIZE  ALIGN_UP(sizeof(struct iob_s) + CONFIG_IOB_BUFSIZE, \
                                   CONFIG_IOB_ALIGNMENT)
#else
#  define IOB_ALIGN_SIZE  ALIGN_UP(sizeof(struct iob_s), CONFIG_IOB_ALIGNMENT)
#endif

#define IOB_BUFFER_SIZE   (IOB_ALIGN_SIZE * CONFIG_IOB_NBUFFERS + \
                           CONFIG_IOB_ALIGNMENT - 1)

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* Following raw buffer will be divided into iob_s instances, the initial
 * procedure will ensure that the member io_data of each iob_s is aligned
 * to the CONFIG_IOB_ALIGNMENT memory boundary.
 */

#ifdef IOB_SECTION
static uint8_t g_iob_buffer[IOB_BUFFER_SIZE] locate_data(IOB_SECTION);
#else
static uint8_t g_iob_buffer[IOB_BUFFER_SIZE];
#endif

#if CONFIG_IOB_NCHAINS > 0
/* This is a pool of pre-allocated iob_qentry_s buffers */

static struct iob_qentry_s g_iob_qpool[CONFIG_IOB_NCHAINS];
#endif

/****************************************************************************
 * Public Data
 ****************************************************************************/

/* A list of all free, unallocated I/O buffers */

FAR struct iob_s *g_iob_freelist;

/* A list of I/O buffers that are committed for allocation */

FAR struct iob_s *g_iob_committed;

#if CONFIG_IOB_NCHAINS > 0
/* A list of all free, unallocated I/O buffer queue containers */

FAR struct iob_qentry_s *g_iob_freeqlist;

/* A list of I/O buffer queue containers that are committed for allocation */

FAR struct iob_qentry_s *g_iob_qcommitted;
#endif

sem_t g_iob_sem = SEM_INITIALIZER(0);

/* Counting that tracks the number of free IOBs/qentries */

int16_t g_iob_count = CONFIG_IOB_NBUFFERS;

#if CONFIG_IOB_THROTTLE > 0

sem_t g_throttle_sem = SEM_INITIALIZER(0);

/* Wait Counts for throttle */

int16_t g_throttle_wait = 0;
#endif

#if CONFIG_IOB_NCHAINS > 0
sem_t g_qentry_sem = SEM_INITIALIZER(0);

/* Wait Counts for qentry */

int16_t g_qentry_wait = 0;
#endif

volatile spinlock_t g_iob_lock = SP_UNLOCKED;

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: iob_initialize
 *
 * Description:
 *   Set up the I/O buffers for normal operations.
 *
 ****************************************************************************/

void iob_initialize(void)
{
  int i;
  uintptr_t buf;

  /* Get a start address which plus offsetof(struct iob_s, io_data) is
   * aligned to the CONFIG_IOB_ALIGNMENT memory boundary
   */

  buf = ALIGN_UP((uintptr_t)g_iob_buffer + offsetof(struct iob_s, io_data),
                 CONFIG_IOB_ALIGNMENT) - offsetof(struct iob_s, io_data);

  /* Get I/O buffer instance from the start address and add each I/O buffer
   * to the free list
   */

  for (i = 0; i < CONFIG_IOB_NBUFFERS; i++)
    {
      FAR struct iob_s *iob = (FAR struct iob_s *)(buf + i * IOB_ALIGN_SIZE);

      /* Add the pre-allocate I/O buffer to the head of the free list */

      iob->io_flink   = g_iob_freelist;
#ifdef CONFIG_IOB_ALLOC
      iob->io_bufsize = CONFIG_IOB_BUFSIZE;
      iob->io_data    = (FAR uint8_t *)(iob + 1);
#endif
      g_iob_freelist  = iob;
    }

#if CONFIG_IOB_NCHAINS > 0
  /* Add each I/O buffer chain queue container to the free list */

  for (i = 0; i < CONFIG_IOB_NCHAINS; i++)
    {
      FAR struct iob_qentry_s *iobq = &g_iob_qpool[i];

      /* Add the pre-allocate buffer container to the head of the free
       * list
       */

      iobq->qe_flink  = g_iob_freeqlist;
      g_iob_freeqlist = iobq;
    }
#endif
}
