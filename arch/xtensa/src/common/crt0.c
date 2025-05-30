/****************************************************************************
 * arch/xtensa/src/common/crt0.c
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

#include <sys/types.h>
#include <stdlib.h>

#include <nuttx/addrenv.h>
#include <nuttx/arch.h>

#include <arch/syscall.h>

/****************************************************************************
 * Public Data
 ****************************************************************************/

/* Linker defined symbols to .ctors and .dtors */

extern initializer_t _sctors[];
extern initializer_t _ectors[];
extern initializer_t _sdtors[];
extern initializer_t _edtors[];

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

int main(int argc, char *argv[]);

/****************************************************************************
 * Private Functions
 ****************************************************************************/

#ifdef CONFIG_HAVE_CXXINITIALIZE

/****************************************************************************
 * Name: exec_ctors
 *
 * Description:
 *   Call static constructors
 *
 ****************************************************************************/

static void exec_ctors(void)
{
  for (initializer_t *ctor = _sctors; ctor != _ectors; ctor++)
    {
      (*ctor)();
    }
}

/****************************************************************************
 * Name: exec_dtors
 *
 * Description:
 *   Call static destructors
 *
 ****************************************************************************/

static void exec_dtors(void)
{
  for (initializer_t *dtor = _sdtors; dtor != _edtors; dtor++)
    {
      (*dtor)();
    }
}

#endif

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: __start
 *
 * Description:
 *   This function is the low level entry point into the main thread of
 *   execution of a task.  It receives initial control when the task is
 *   started and calls main entry point of the newly started task.
 *
 * Input Parameters:
 *   argc - The number of parameters being passed.
 *   argv - The parameters being passed. These lie in kernel-space memory
 *     and will have to be reallocated  in user-space memory.
 *
 * Returned Value:
 *   This function should not return.  It should call the user-mode start-up
 *   main() function.  If that function returns, this function will call
 *   exit.
 *
 ****************************************************************************/

void __start(int argc, char *argv[])
{
  int ret;

  /* Initialize the reserved area at the beginning of the .bss/.data region
   * that is visible to the RTOS.
   */

#ifdef CONFIG_BUILD_KERNEL
  ARCH_DATA_RESERVE->ar_sigtramp = (addrenv_sigtramp_t)sig_trampoline;
#endif

#ifdef CONFIG_HAVE_CXXINITIALIZE
  /* Call C++ constructors */

  exec_ctors();

  /* Setup so that C++ destructors called on task exit */

#  if CONFIG_LIBC_MAX_EXITFUNS > 0
  atexit(exec_dtors);
#  endif
#endif

  /* Call the main() entry point passing argc and argv. */

  ret = main(argc, argv);

#if defined(CONFIG_HAVE_CXXINITIALIZE) && CONFIG_LIBC_MAX_EXITFUNS <= 0
  exec_dtors();
#endif

  /* Call exit() if/when the main() returns */

  exit(ret);
}
