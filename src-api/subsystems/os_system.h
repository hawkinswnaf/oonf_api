
/*
 * The olsr.org Optimized Link-State Routing daemon version 2 (olsrd2)
 * Copyright (c) 2004-2013, the olsr.org team - see HISTORY file
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * * Redistributions of source code must retain the above copyright
 *   notice, this list of conditions and the following disclaimer.
 * * Redistributions in binary form must reproduce the above copyright
 *   notice, this list of conditions and the following disclaimer in
 *   the documentation and/or other materials provided with the
 *   distribution.
 * * Neither the name of olsr.org, olsrd nor the names of its
 *   contributors may be used to endorse or promote products derived
 *   from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 * Visit http://www.olsr.org for more information.
 *
 * If you find this software useful feel free to make a donation
 * to the project. For more information see the website or contact
 * the copyright holders.
 *
 */

#ifndef OS_SYSTEM_H_
#define OS_SYSTEM_H_

#include <stdio.h>
#include <sys/time.h>

#include "common/common_types.h"
#include "common/list.h"
#include "core/oonf_logging.h"

#define MSEC_PER_SEC 1000
#define USEC_PER_MSEC 1000

struct os_system_if_listener {
  void (*if_changed)(const char *ifname, bool up);

  struct list_entity _node;
};

/* include os-specific headers */
#if defined(__linux__)
#include "subsystems/os_linux/os_system_linux.h"
#elif defined (BSD)
#include "subsystems/os_bsd/os_system_bsd.h"
#elif defined (_WIN32)
#include "subsystems/os_win32/os_system_win32.h"
#else
#error "Unknown operation system"
#endif

struct os_system_address {
  /* used for delivering feedback about netlink commands */
  struct os_system_address_internal _internal;

  /* source, gateway and destination */
  struct netaddr address;

  /* index of interface */
  unsigned int if_index;

  /* address scope */
  enum os_addr_scope scope;

  /* set or reset address */
  bool set;

  /* callback when operation is finished */
  void (*cb_finished)(struct os_system_address *, int error);
};

#define LOG_OS_SYSTEM oonf_os_system_subsystem.logging
EXPORT extern struct oonf_subsystem oonf_os_system_subsystem;

/* prototypes for all os_system functions */
EXPORT void os_system_iflistener_add(struct os_system_if_listener *);
EXPORT void os_system_iflistener_remove(struct os_system_if_listener *);
EXPORT int os_system_set_interface_state(const char *dev, bool up);
EXPORT int os_system_ifaddr_set(struct os_system_address *addr);
EXPORT void os_system_ifaddr_interrupt(struct os_system_address *addr);

#endif /* OS_SYSTEM_H_ */
