/****************************************************************************
 * arch/z16/src/common/z16_sigdeliver.c
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

#include <stdint.h>
#include <sched.h>
#include <assert.h>
#include <debug.h>

#include <nuttx/irq.h>
#include <nuttx/arch.h>
#include <nuttx/board.h>
#include <arch/board/board.h>

#include "sched/sched.h"
#include "signal/signal.h"
#include "z16_internal.h"

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: z16_sigdeliver
 *
 * Description:
 *   This is the a signal handling trampoline.  When a signal action was
 *   posted.  The task context was mucked with and forced to branch to this
 *   location with interrupts disabled.
 *
 ****************************************************************************/

void z16_sigdeliver(void)
{
  FAR struct tcb_s *rtcb = this_task();
  chipreg_t regs[XCPTCONTEXT_REGS];
  FAR uint32_t *regs32 = (FAR uint32_t *)regs;

  board_autoled_on(LED_SIGNAL);

  sinfo("rtcb=%p sigpendactionq.head=%p\n",
        rtcb, rtcb->sigpendactionq.head);
  DEBUGASSERT((rtcb->flags & TCB_FLAG_SIGDELIVER) != 0);

  /* Save the return state on the stack. */

  z16_copystate(regs, rtcb->xcp.regs);

#ifndef CONFIG_SUPPRESS_INTERRUPTS
  /* Then make sure that interrupts are enabled.  Signal handlers must always
   * run with interrupts enabled.
   */

  up_irq_enable();
#endif

  /* Deliver the signals */

  nxsig_deliver(rtcb);

  /* Output any debug messages BEFORE restoring errno (because they may
   * alter errno), then disable interrupts again and restore the original
   * errno that is needed by the user logic (it is probably EINTR).
   */

  sinfo("Resuming\n");
  up_irq_save();

  /* Modify the saved return state with the actual saved values in the
   * TCB.  This depends on the fact that nested signal handling is
   * not supported.  Therefore, these values will persist throughout the
   * signal handling action.
   *
   * Keeping this data in the TCB resolves a security problem in protected
   * and kernel mode:  The regs[] array is visible on the user stack and
   * could be modified by a hostile program.
   */

  regs32[REG_PC / 2] = rtcb->xcp.saved_pc;
  regs[REG_FLAGS]    = rtcb->xcp.saved_i;
  rtcb->flags &= ~TCB_FLAG_SIGDELIVER;

  /* Then restore the correct state for this thread of execution. */

  board_autoled_off(LED_SIGNAL);
  SIGNAL_RETURN(regs);
}
