/****************************************************************************
 * arch/risc-v/src/esp32c3-legacy/esp32c3_idle.c
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

#include <assert.h>
#include <stdint.h>
#include <debug.h>
#include <sys/param.h>
#include <nuttx/config.h>
#include <nuttx/arch.h>
#include <nuttx/spinlock.h>
#include <nuttx/power/pm.h>

#include "esp32c3.h"
#include "esp32c3_pm.h"

#ifdef CONFIG_ESP32C3_RT_TIMER
#include "esp32c3_rt_timer.h"
#endif

#ifdef CONFIG_SCHED_TICKLESS
#include "esp32c3_tickless.h"
#endif

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Values for the RTC Alarm to wake up from the PM_STANDBY mode
 * (which corresponds to ESP32-C3 stop mode).  If this alarm expires,
 * the logic in this file will wakeup from PM_STANDBY mode and
 * transition to PM_SLEEP mode (ESP32-C3 standby mode).
 */

#ifdef CONFIG_PM
#ifndef CONFIG_PM_ALARM_SEC
#  define CONFIG_PM_ALARM_SEC 15
#endif

#ifndef CONFIG_PM_ALARM_NSEC
#  define CONFIG_PM_ALARM_NSEC 0
#endif

#ifndef CONFIG_PM_SLEEP_WAKEUP_SEC
#  define CONFIG_PM_SLEEP_WAKEUP_SEC 20
#endif

#ifndef CONFIG_PM_SLEEP_WAKEUP_NSEC
#  define CONFIG_PM_SLEEP_WAKEUP_NSEC 0
#endif

#define EXPECTED_IDLE_TIME_US (800)
#define EARLY_WAKEUP_US       (200)

#endif

/****************************************************************************
 * Private Data
 ****************************************************************************/

#ifdef CONFIG_PM
static spinlock_t g_esp32c3_idle_lock = SP_UNLOCKED;
#endif

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: up_idlepm
 *
 * Description:
 *   Perform IDLE state power management.
 *
 ****************************************************************************/

#ifdef CONFIG_PM
static void up_idlepm(void)
{
  irqstate_t flags;

#ifdef CONFIG_ESP32C3_AUTO_SLEEP
  flags = spin_lock_irqsave(&g_esp32c3_idle_lock);
  if (esp32c3_pm_lockstatus() == 0 &&
     (esp32c3_should_skip_light_sleep() == false))
    {
      uint64_t os_start_us;
      uint64_t os_end_us;
      uint64_t os_step_us;
      uint64_t hw_start_us;
      uint64_t hw_end_us;
      uint64_t hw_step_us;
      uint64_t rtc_diff_us;
      struct   timespec ts;
      uint64_t os_idle_us = up_get_idletime();
      uint64_t hw_idle_us = rt_timer_get_alarm();
      uint64_t sleep_us   = MIN(os_idle_us, hw_idle_us);
      if (sleep_us > EXPECTED_IDLE_TIME_US)
        {
          esp32c3_sleep_enable_rtc_timer_wakeup(sleep_us - EARLY_WAKEUP_US);
          up_timer_gettime(&ts);
          os_start_us = (ts.tv_sec * USEC_PER_SEC +
                         ts.tv_nsec /  NSEC_PER_USEC);
          hw_start_us = rt_timer_time_us();

          esp32c3_light_sleep_start(&rtc_diff_us);

          hw_end_us = rt_timer_time_us();
          up_timer_gettime(&ts);
          os_end_us = (ts.tv_sec * USEC_PER_SEC +
                         ts.tv_nsec /  NSEC_PER_USEC);
          hw_step_us = rtc_diff_us - (hw_end_us - hw_start_us);
          os_step_us = rtc_diff_us - (os_end_us - os_start_us);
          DEBUGASSERT(hw_step_us > 0);
          DEBUGASSERT(os_step_us > 0);

          /* Adjust current RT timer by a certain value. */

          rt_timer_calibration(hw_step_us);

          /* Adjust system time by a certain value. */

          up_step_idletime((uint32_t)os_step_us);
        }
    }

  spin_unlock_irqrestore(&g_esp32c3_idle_lock, flags);
#else /* CONFIG_ESP32C3_AUTO_SLEEP */
  static enum pm_state_e oldstate = PM_NORMAL;
  enum pm_state_e newstate;
  int ret;

  /* Decide, which power saving level can be obtained */

  newstate = pm_checkstate(PM_IDLE_DOMAIN);

  /* Check for state changes */

  if (newstate != oldstate)
    {
      flags = spin_lock_irqsave(&g_esp32c3_idle_lock);

      /* Perform board-specific, state-dependent logic here */

      _info("newstate= %d oldstate=%d\n", newstate, oldstate);

      /* Then force the global state change */

      ret = pm_changestate(PM_IDLE_DOMAIN, newstate);
      if (ret < 0)
        {
          /* The new state change failed, revert to the preceding state */

          pm_changestate(PM_IDLE_DOMAIN, oldstate);
        }
      else
        {
          /* Save the new state */

          oldstate = newstate;
        }

      spin_unlock_irqrestore(&g_esp32c3_idle_lock, flags);

      /* MCU-specific power management logic */

      switch (newstate)
        {
        case PM_NORMAL:
          break;

        case PM_IDLE:
          break;

        case PM_STANDBY:
          {
            /* Enter Force-sleep mode */

            esp32c3_pmstandby(CONFIG_PM_ALARM_SEC * 1000000 +
                                  CONFIG_PM_ALARM_NSEC / 1000);
          }
          break;

        case PM_SLEEP:
          {
            /* Enter Deep-sleep mode */

            esp32c3_pmsleep(CONFIG_PM_SLEEP_WAKEUP_SEC * 1000000 +
                                CONFIG_PM_SLEEP_WAKEUP_NSEC / 1000);
          }

        default:
          break;
        }
    }
  else
    {
#ifdef CONFIG_WATCHDOG
      /* Announce the power management state change to feed watchdog */

      pm_changestate(PM_IDLE_DOMAIN, PM_NORMAL);
#endif
    }
#endif
}
#else
#  define up_idlepm()
#endif

/****************************************************************************
 * Name: up_idle
 *
 * Description:
 *   up_idle() is the logic that will be executed when their is no other
 *   ready-to-run task.  This is processor idle time and will continue until
 *   some interrupt occurs to cause a context switch from the idle task.
 *
 *   Processing in this state may be processor-specific. e.g., this is where
 *   power management operations might be performed.
 *
 ****************************************************************************/

void up_idle(void)
{
#if defined(CONFIG_SUPPRESS_INTERRUPTS) || defined(CONFIG_SUPPRESS_TIMER_INTS)
  /* If the system is idle and there are no timer interrupts, then process
   * "fake" timer interrupts. Hopefully, something will wake up.
   */

  nxsched_process_timer();
#else
  /* This would be an appropriate place to put some MCU-specific logic to
   * sleep in a reduced power mode until an interrupt occurs to save power
   */

  asm("WFI");

  /* Perform IDLE mode power management */

  up_idlepm();

#endif
}
