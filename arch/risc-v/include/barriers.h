/****************************************************************************
 * arch/risc-v/include/barriers.h
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

#ifndef __ARCH_RISCV_INCLUDE_BARRIERS_H
#define __ARCH_RISCV_INCLUDE_BARRIERS_H

/* Common memory barriers (p=predecessor, s=successor) */

#define __FENCE(p, s) __asm__ __volatile__ ("fence "#p", "#s  ::: "memory")

/* UP_DMB() is used to flush local data caches (memory) */

#define UP_DMB()       __FENCE(rw, rw)

/* UP_DSB() is a full memory barrier */

#define UP_DSB()       __FENCE(iorw, iorw)

/* UP_ISB() is used to synchronize the instruction and data streams */

#define UP_ISB()       __asm__ __volatile__ ("fence.i" ::: "memory")

#endif /* __ARCH_RISCV_INCLUDE_BARRIERS_H */
