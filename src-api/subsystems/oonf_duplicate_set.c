
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

#include "common/avl.h"
#include "common/avl_comp.h"
#include "common/common_types.h"
#include "common/netaddr.h"
#include "rfc5444/rfc5444.h"
#include "core/oonf_subsystem.h"
#include "subsystems/oonf_class.h"
#include "subsystems/oonf_timer.h"
#include "subsystems/oonf_duplicate_set.h"

/* prototypes */
static int _init(void);
static void _cleanup(void);

static enum oonf_duplicate_result _test(struct oonf_duplicate_entry *,
    uint16_t seqno, bool set);
static int _avl_cmp_dupkey(const void *, const void*);

static void _cb_vtime(void *);

static struct oonf_timer_info _vtime_info = {
  .name = "Valdity time for duplicate set",
  .callback = _cb_vtime,
};

static struct oonf_class _dupset_class = {
  .name = "Duplicate set",
  .size = sizeof(struct oonf_duplicate_entry),
};

/* dupset result names */
const char *OONF_DUPSET_RESULT_STR[OONF_DUPSET_MAX] = {
  [OONF_DUPSET_TOO_OLD]   = "too old",
  [OONF_DUPSET_DUPLICATE] = "duplicate",
  [OONF_DUPSET_CURRENT]   = "current",
  [OONF_DUPSET_NEW]       = "new",
  [OONF_DUPSET_NEWEST]    = "newest",
  [OONF_DUPSET_FIRST]     = "first",
};

/* subsystem definition */
struct oonf_subsystem oonf_duplicate_set_subsystem = {
  .name = "duplicate_set",
  .init = _init,
  .cleanup = _cleanup,
};

/**
 * Initialize duplicate set subsystem
 * @return always returns 0
 */
static int
_init(void) {
  oonf_class_add(&_dupset_class);
  oonf_timer_add(&_vtime_info);
  return 0;
}

/**
 * Cleanup duplicate set subsystem
 */
static void
_cleanup(void) {
  oonf_timer_remove(&_vtime_info);
  oonf_class_remove(&_dupset_class);
}

/**
 * Initialize a new duplicate set
 * @param set pointer to duplicate set;
 */
void
oonf_duplicate_set_add(struct oonf_duplicate_set *set) {
  avl_init(&set->_tree, _avl_cmp_dupkey, false);
}

/**
 * Remove all allocated resources from a duplicate set
 * @param set pointer to duplicate set
 */
void
oonf_duplicate_set_remove(struct oonf_duplicate_set *set) {
  struct oonf_duplicate_entry *entry, *it;

  avl_for_each_element_safe(&set->_tree, entry, _node, it) {
    _cb_vtime(entry);
  }
}

/**
 * Test a originator/seqno pair against a duplicate set and add
 * it to the set if necessary
 * @param set duplicate set
 * @param msg_type message type with incoming sequence number
 * @param originator originator of sequence number
 * @param seqno sequence number
 * @param vtime validity time of sequence number
 * @return OONF_DUPSET_TOO_OLD if sequence number is more than 32 behind
 *   the current one, OONF_DUPSET_DUPLICATE if the number is in the set,
 *   OONF_DUPSET_NEW if the number was added to the set and OONF_DUPSET_NEWEST
 *   if the sequence number is newer than the newest in the set
 */
enum oonf_duplicate_result
oonf_duplicate_entry_add(struct oonf_duplicate_set *set, uint8_t msg_type,
    struct netaddr *originator, uint16_t seqno, uint64_t vtime) {
  struct oonf_duplicate_entry *entry;
  struct oonf_duplicate_entry_key key;
  enum oonf_duplicate_result result;

#ifdef OONF_LOG_DEBUG_INFO
  struct netaddr_str nbuf;
#endif

  /* generate combined key */
  memcpy(&key.addr, originator, sizeof(*originator));
  key.msg_type = msg_type;

  entry = avl_find_element(&set->_tree, &key, entry, _node);
  if (!entry) {
    entry = oonf_class_malloc(&_dupset_class);
    if (entry == NULL) {
      return OONF_DUPSET_TOO_OLD;
    }

    /* initialize history and current sequence number */
    entry->current = seqno;
    entry->history = 1;

    /* initialize backpointer */
    entry->set = set;

    /* initialize vtime */
    entry->_vtime.info = &_vtime_info;
    entry->_vtime.cb_context = entry;

    oonf_timer_start(&entry->_vtime, vtime);

    /* set key and link entry to set */
    memcpy(&entry->key, &key, sizeof(key));
    entry->_node.key = &entry->key;
    avl_insert(&set->_tree, &entry->_node);

    result = OONF_DUPSET_FIRST;
  }
  else {
    result = _test(entry, seqno, true);
  }
  OONF_DEBUG(LOG_DUPLICATE_SET, "Test/Add msgtype %u, originator %s, seqno %u: %s",
      msg_type, netaddr_to_string(&nbuf, originator), seqno,
      OONF_DUPSET_RESULT_STR[result]);

  if (oonf_duplicate_is_new(result)) {
    /* reset validity timer */
    oonf_timer_set(&entry->_vtime, vtime);
  }
  return result;
}

/**
 * Test a originator/sequence number pair against a duplicate set
 * @param set duplicate set
 * @param msg_type message type with incoming sequence number
 * @param originator originator of sequence number
 * @param seqno sequence number
 * @return OONF_DUPSET_TOO_OLD if sequence number is more than 32 behind
 *   the current one, OONF_DUPSET_DUPLICATE if the number is in the set,
 *   OONF_DUPSET_NEW if the number was added to the set and OONF_DUPSET_NEWEST
 *   if the sequence number is newer than the newest in the set
 */
enum oonf_duplicate_result
oonf_duplicate_test(struct oonf_duplicate_set *set, uint8_t msg_type,
    struct netaddr *originator, uint16_t seqno) {
  struct oonf_duplicate_entry *entry;
  struct oonf_duplicate_entry_key key;
  enum oonf_duplicate_result result;

#ifdef OONF_LOG_DEBUG_INFO
  struct netaddr_str nbuf;
#endif

  /* generate combined key */
  memcpy(&key.addr, originator, sizeof(*originator));
  key.msg_type = msg_type;

  entry = avl_find_element(&set->_tree, &key, entry, _node);
  if (!entry) {
    result = OONF_DUPSET_FIRST;
  }
  else {
    result = _test(entry, seqno, false);
  }

  OONF_DEBUG(LOG_DUPLICATE_SET, "Test msgtype %u, originator %s, seqno %u: %s",
      msg_type, netaddr_to_string(&nbuf, originator), seqno,
      OONF_DUPSET_RESULT_STR[result]);

  return result;
}

/**
 * Test a sequence number against a duplicate set entry
 * @param entry duplicate set entry
 * @param seqno sequence number
 * @param set true to add the sequence number to the entry, false
 *   to leave the entry unchanged.
 * @return OONF_DUPSET_TOO_OLD if sequence number is more than 32 behind
 *   the current one, OONF_DUPSET_DUPLICATE if the number is in the set,
 *   OONF_DUPSET_CURRENT if the number is exactly the current seqence number,
 *   OONF_DUPSET_NEW if the number was added to the set and OONF_DUPSET_NEWEST
 *   if the sequence number is newer than the newest in the set
 */
enum oonf_duplicate_result
_test(struct oonf_duplicate_entry *entry,
    uint16_t seqno, bool set) {
  int diff;

  if (seqno == entry->current) {
    return OONF_DUPSET_CURRENT;
  }

  /* eliminate rollover */
  diff = rfc5444_seqno_difference(seqno, entry->current);
  if (diff < -31) {
    entry->too_old_count++;
    if (entry->too_old_count > OONF_DUPSET_MAXIMUM_TOO_OLD) {
      /*
       * we got a long continuous series of too old messages,
       * most likely the did reset and changed its sequence number
       */
      entry->history = 1;
      entry->too_old_count = 0;
      entry->current = seqno;

      return OONF_DUPSET_NEWEST;
    }
    return OONF_DUPSET_TOO_OLD;
  }

  /* reset counter of too old messages */
  entry->too_old_count = 0;

  if (diff <= 0) {
    uint32_t bitmask = 1 << ((uint32_t) (-diff));

    if ((entry->history & bitmask) != 0) {
      return OONF_DUPSET_DUPLICATE;
    }

    if (set) {
      entry->history |= bitmask;
    }
    return OONF_DUPSET_NEW;
  }

  if (set) {
    /* new sequence number is larger than last one */
    entry->current = seqno;

    if (diff >= 32) {
      entry->history = 1;
    }
    else {
      entry->history <<= diff;
      entry->history |= 1;
    }
  }
  return OONF_DUPSET_NEWEST;
}

/**
 * Comparator for duplicate entry keys
 * @param p1 key1
 * @param p2 key2
 * @return <0 if p1<p2, 0 if p1==p2, >0 if p1>p2
 */
static int
_avl_cmp_dupkey(const void *p1, const void *p2) {
  const struct oonf_duplicate_entry_key *k1, *k2;

  k1 = p1;
  k2 = p2;

  if (k1->msg_type != k2->msg_type) {
    return (int)(k1->msg_type) - (int)(k2->msg_type);
  }

  return avl_comp_netaddr(&k1->addr, &k2->addr);
}

/**
 * Callback fired when duplicate entry times out
 * @param ptr pointer to duplicate entry
 */
static void
_cb_vtime(void *ptr) {
  struct oonf_duplicate_entry *entry = ptr;
#ifdef OONF_LOG_DEBUG_INFO
  struct netaddr_str nbuf;
#endif

  OONF_DEBUG(LOG_DUPLICATE_SET, "Duplicate entry timed out: %s/%u",
      netaddr_to_string(&nbuf, &entry->key.addr), entry->key.msg_type);
  oonf_timer_stop(&entry->_vtime);
  avl_remove(&entry->set->_tree, &entry->_node);

  oonf_class_free(&_dupset_class, entry);
}
