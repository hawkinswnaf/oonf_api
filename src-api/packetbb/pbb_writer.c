/*
 * PacketBB handler library (see RFC 5444)
 * Copyright (c) 2010 Henning Rogge <hrogge@googlemail.com>
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
 * Visit http://www.olsr.org/git for more information.
 *
 * If you find this software useful feel free to make a donation
 * to the project. For more information see the website or contact
 * the copyright holders.
 */

#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "common/avl.h"
#include "common/avl_comp.h"
#include "common/common_types.h"
#include "common/list.h"
#include "packetbb/pbb_context.h"
#include "packetbb/pbb_writer.h"
#include "packetbb/pbb_api_config.h"

static int _msgaddr_avl_comp(const void *k1, const void *k2, void *ptr);
static void *_copy_addrtlv_value(struct pbb_writer *writer, void *value, size_t length);
static void _free_tlvtype_tlvs(struct pbb_writer *writer, struct pbb_writer_tlvtype *tlvtype);
static void _lazy_free_message(struct pbb_writer *writer, struct pbb_writer_message *msg);
static struct pbb_writer_message *_get_message(struct pbb_writer *writer, uint8_t msgid);
static struct pbb_writer_address *_malloc_address_entry(void);
static struct pbb_writer_addrtlv *_malloc_addrtlv_entry(void);

/**
 * Creates a new packetbb writer context
 * @param writer pointer to writer context
 */
void
pbb_writer_init(struct pbb_writer *writer) {
  assert (writer->msg_buffer != NULL && writer->msg_size > 0);
  assert (writer->addrtlv_buffer != NULL && writer->addrtlv_size > 0);

  /* set default memory handler functions */
  if (!writer->malloc_address_entry)
    writer->malloc_address_entry = _malloc_address_entry;
  if (!writer->malloc_addrtlv_entry)
    writer->malloc_addrtlv_entry = _malloc_addrtlv_entry;
  if (!writer->free_address_entry)
    writer->free_address_entry = free;
  if (!writer->free_addrtlv_entry)
    writer->free_addrtlv_entry = free;

  list_init_head(&writer->_interfaces);

  /* initialize packet buffer */
  writer->_msg.buffer = writer->msg_buffer;
  _pbb_tlv_writer_init(&writer->_msg, 0, writer->msg_size);

  list_init_head(&writer->_pkthandlers);
  avl_init(&writer->_msgcreators, avl_comp_uint8, false, NULL);

#if WRITER_STATE_MACHINE == true
  writer->_state = PBB_WRITER_NONE;
#endif
}

/**
 * Cleanup a writer context and free all reserved memory
 * This function must not be called outside the message_addresses callback.
 *
 * @param writer pointer to writer context
 */
void
pbb_writer_cleanup(struct pbb_writer *writer) {
  struct pbb_writer_message *msg, *safe_msg;
  struct pbb_writer_pkthandler *pkt, *safe_pkt;
  struct pbb_writer_content_provider *provider, *safe_prv;
  struct pbb_writer_tlvtype *tlvtype, *safe_tt;
  struct pbb_writer_interface *interf, *safe_interf;

  assert(writer);
#if WRITER_STATE_MACHINE == true
  assert(writer->_state == PBB_WRITER_NONE);
#endif

  /* remove all packet handlers */
  list_for_each_element_safe(&writer->_pkthandlers, pkt, _pkthandle_node, safe_pkt) {
    pbb_writer_unregister_pkthandler(writer, pkt);
  }

  /* remove all _interfaces */
  list_for_each_element_safe(&writer->_interfaces, interf, _if_node, safe_interf) {
    pbb_writer_unregister_interface(writer, interf);
  }

  /* remove all message creators */
  avl_for_each_element_safe(&writer->_msgcreators, msg, _msgcreator_node, safe_msg) {
    /* prevent message from being freed in the middle of the processing */
    msg->_registered = true;

    /* remove all message content providers */
    avl_for_each_element_safe(&msg->_provider_tree, provider, _provider_node, safe_prv) {
      pbb_writer_unregister_content_provider(writer, provider);
    }

    /* remove all _registered address tlvs */
    list_for_each_element_safe(&msg->_tlvtype_head, tlvtype, _tlvtype_node, safe_tt) {
      /* reset usage counter */
      tlvtype->_usage_counter = 1;
      pbb_writer_unregister_addrtlvtype(writer, tlvtype);
    }

    /* remove message and addresses */
    pbb_writer_unregister_message(writer, msg);
  }
}

/**
 * Adds a tlv to an address.
 * This function must not be called outside the message_addresses callback.
 *
 * @param writer pointer to writer context
 * @param addr pointer to address object
 * @param _tlvtype pointer to predefined _tlvtype object
 * @param value pointer to value or NULL if no value
 * @param length length of value in bytes or 0 if no value
 * @param allow_dup true if multiple TLVs of the same type are allowed,
 *   false otherwise
 * @return PBB_OKAY if tlv has been added successfully, PBB_... otherwise
 */
enum pbb_result
pbb_writer_add_addrtlv(struct pbb_writer *writer, struct pbb_writer_address *addr,
    struct pbb_writer_tlvtype *tlvtype, void *value, size_t length, bool allow_dup) {
  struct pbb_writer_addrtlv *addrtlv;

#if WRITER_STATE_MACHINE == true
  assert(writer->_state == PBB_WRITER_ADD_ADDRESSES);
#endif

  /* check for collision if necessary */
  if (!allow_dup && avl_find(&addr->_addrtlv_tree, &tlvtype->_full_type) != NULL) {
    return PBB_DUPLICATE_TLV;
  }

  if ((addrtlv = writer->malloc_addrtlv_entry()) == NULL) {
    /* out of memory error */
    return PBB_OUT_OF_MEMORY;
  }

  /* set back pointer */
  addrtlv->address = addr;
  addrtlv->_tlvtype = tlvtype;

  /* copy value(length) */
  addrtlv->length = length;
  if (length > 0 && (addrtlv->value = _copy_addrtlv_value(writer, value, length)) == NULL) {
    writer->free_addrtlv_entry(addrtlv);
    return PBB_OUT_OF_ADDRTLV_MEM;
  }

  /* add to address tree */
  addrtlv->_addrtlv_node.key = &tlvtype->_full_type;
  avl_insert(&addr->_addrtlv_tree, &addrtlv->_addrtlv_node);

  /* add to _tlvtype tree */
  addrtlv->_tlv_node.key = &addr->index;
  avl_insert(&tlvtype->_tlv_tree, &addrtlv->_tlv_node);

  return PBB_OKAY;
}

/**
 * Add a network prefix to a pbb message.
 * This function must not be called outside the message_addresses callback.
 *
 * @param writer pointer to writer context
 * @param _msg pointer to message object
 * @param addr pointer to binary address in network byte order
 * @param prefix prefix length
 * @return pointer to address object, NULL if an error happened
 */
struct pbb_writer_address *
pbb_writer_add_address(struct pbb_writer *writer __attribute__ ((unused)),
    struct pbb_writer_message *msg, uint8_t *addr, uint8_t prefix) {
  struct pbb_writer_address *address;
#if CLEAR_ADDRESS_POSTFIX == true
  int i, p;
  uint8_t cleaned_addr[PBB_MAX_ADDRLEN];
#endif

#if WRITER_STATE_MACHINE == true
  assert(writer->_state == PBB_WRITER_ADD_ADDRESSES);
#endif

#if CLEAR_ADDRESS_POSTFIX == true
  /* only copy prefix part of address */
  for (p = prefix, i=0; i < _msg->addr_len; i++, p -= 8) {
    if (p > 7) {
      cleaned_addr[i] = addr[i];
    }
    else if (p <= 0) {
      cleaned_addr[i] = 0;
    }
    else {
      uint8_t mask = 255 << (8-p);
      cleaned_addr[i] = addr[i] & mask;
    }
  }

  address = avl_find_element(&_msg->_addr_tree, cleaned_addr, address, _addr_tree_node);
#else
  address = avl_find_element(&msg->_addr_tree, addr, address, _addr_tree_node);
#endif


  if (address == NULL) {
    if ((address = writer->malloc_address_entry()) == NULL) {
      return NULL;
    }

#if CLEAR_ADDRESS_POSTFIX == true
    memcpy(address->addr, cleaned_addr, _msg->addr_len);
#else
    memcpy(address->addr, addr, msg->addr_len);
#endif
    address->prefixlen = prefix;

    address->_addr_tree_node.key = address->addr;

    list_add_tail(&msg->_addr_head, &address->_addr_node);
    avl_insert(&msg->_addr_tree, &address->_addr_tree_node);

    avl_init(&address->_addrtlv_tree, avl_comp_uint32, true, NULL);
  }
  return address;
}

/**
 * Register an tlv type for addressblocks of a certain message.
 * This function must NOT be called from the pbb writer callbacks.
 *
 * @param writer pointer to writer context
 * @param msgtype messagetype for this tlv
 * @param tlv _tlvtype of this tlv
 * @param tlvext extended tlv type, 0 if no extended type necessary
 * @return pointer to _tlvtype object, NULL if an error happened
 */
struct pbb_writer_tlvtype *
pbb_writer_register_addrtlvtype(struct pbb_writer *writer, uint8_t msgtype, uint8_t tlv, uint8_t tlvext) {
  struct pbb_writer_tlvtype *tlvtype;
  struct pbb_writer_message *msg;

#if WRITER_STATE_MACHINE == true
  assert(writer->_state == PBB_WRITER_NONE);
#endif
  if ((msg = _get_message(writer, msgtype)) == NULL) {
    /* out of memory error ? */
    return NULL;
  }

  /* Look for existing addrtlv */
  list_for_each_element(&msg->_tlvtype_head, tlvtype, _tlvtype_node) {
    if (tlvtype->type == tlv && tlvtype->exttype == tlvext) {
      tlvtype->_usage_counter++;
      return tlvtype;
    }
  }

  /* create a new addrtlv entry */
  if ((tlvtype = calloc(1, sizeof(*tlvtype))) == NULL) {
    _lazy_free_message(writer, msg);
    return NULL;
  }

  /* initialize addrtlv fields */
  tlvtype->type = tlv;
  tlvtype->exttype = tlvext;
  tlvtype->_creator = msg;
  tlvtype->_usage_counter++;
  tlvtype->_full_type = tlv*256 + tlvext;

  avl_init(&tlvtype->_tlv_tree, avl_comp_uint32, true, false);

  /* add to message _creator list */
  list_add_tail(&msg->_tlvtype_head, &tlvtype->_tlvtype_node);
  return tlvtype;
}

/**
 * Remove registration of _tlvtype for addresses.
 * This function must not be called outside the message_addresses callback.
 *
 * @param writer pointer to writer context
 * @param _tlvtype pointer to _tlvtype object
 */
void
pbb_writer_unregister_addrtlvtype(struct pbb_writer *writer, struct pbb_writer_tlvtype *tlvtype) {
#if WRITER_STATE_MACHINE == true
  assert(writer->_state == PBB_WRITER_NONE);
#endif
  if (--tlvtype->_usage_counter) {
    return;
  }

  _free_tlvtype_tlvs(writer, tlvtype);
  list_remove(&tlvtype->_tlvtype_node);
  _lazy_free_message(writer, tlvtype->_creator);
  free(tlvtype);
}

/**
 * Register a content provider for a message type
 * This function must not be called outside the message_addresses callback.
 *
 * @param writer pointer to writer context
 * @param cpr pointer to message content provider object
 * @param msgid message type
 * @param priority defines the order the content providers will be called
 * @return -1 if an error happened, 0 otherwise
 */
int
pbb_writer_register_msgcontentprovider(struct pbb_writer *writer,
    struct pbb_writer_content_provider *cpr, uint8_t msgid, int priority) {
  struct pbb_writer_message *msg;

#if WRITER_STATE_MACHINE == true
  assert(writer->_state == PBB_WRITER_NONE);
#endif

  if ((msg = _get_message(writer, msgid)) == NULL) {
    return -1;
  }

  cpr->_creator = msg;
  cpr->priority = priority;
  cpr->_provider_node.key = &cpr->priority;

  avl_insert(&msg->_provider_tree, &cpr->_provider_node);
  return 0;
}

/**
 * Unregister a message content provider
 * This function must not be called outside the message_addresses callback.
 * @param writer pointer to writer context
 * @param cpr pointer to message context provider object
 */
void
pbb_writer_unregister_content_provider(struct pbb_writer *writer, struct pbb_writer_content_provider *cpr) {
#if WRITER_STATE_MACHINE == true
  assert(writer->_state == PBB_WRITER_NONE);
#endif

  avl_remove(&cpr->_creator->_provider_tree, &cpr->_provider_node);
  _lazy_free_message(writer, cpr->_creator);
}

/**
 * Register a message type for the writer
 * This function must not be called outside the message_addresses callback.
 *
 * @param writer pointer to writer context
 * @param msgid message type
 * @param if_specific true if an unique message must be created for each
 *   interface
 * @param addr_len address length in bytes for this message
 * @return message object, NULL if an error happened
 */
struct pbb_writer_message *
pbb_writer_register_message(struct pbb_writer *writer, uint8_t msgid,
    bool if_specific, uint8_t addr_len) {
  struct pbb_writer_message *msg;

#if WRITER_STATE_MACHINE == true
  assert(writer->_state == PBB_WRITER_NONE);
#endif

  msg = _get_message(writer, msgid);
  if (msg == NULL) {
    /* out of memory error */
    return NULL;
  }

  if (msg->_registered) {
    /* message was already _registered */
    return NULL;
  }

  /* mark message as _registered */
  msg->_registered = true;

  /* set real address length and if_specific flag */
  msg->addr_len = addr_len;
  msg->if_specific = if_specific;
  return msg;
}

/**
 * Unregisters a message type
 * This function must not be called outside the message_addresses callback.
 *
 * @param writer pointer to writer context
 * @param _msg pointer to message object
 */
void
pbb_writer_unregister_message(struct pbb_writer *writer, struct pbb_writer_message *msg) {
#if WRITER_STATE_MACHINE == true
  assert(writer->_state == PBB_WRITER_NONE);
#endif

  /* free addresses */
  _pbb_writer_free_addresses(writer, msg);

  /* mark message as unregistered */
  msg->_registered = false;
  _lazy_free_message(writer, msg);
}

/**
 * Registers a packet handler for the writer
 * This function must not be called outside the message_addresses callback.
 *
 * @param writer pointer to writer context
 * @param _pkt pointer to packet handler object
 */
void
pbb_writer_register_pkthandler(struct pbb_writer *writer,
    struct pbb_writer_pkthandler *pkt) {

#if WRITER_STATE_MACHINE == true
  assert(writer->_state == PBB_WRITER_NONE);
#endif

  list_add_tail(&writer->_pkthandlers, &pkt->_pkthandle_node);
}

/**
 * Unregisters a packet handler
 * This function must not be called outside the message_addresses callback.
 *
 * @param writer pointer to writer context
 * @param _pkt pointer to packet handler object
 */
void
pbb_writer_unregister_pkthandler(struct pbb_writer *writer  __attribute__ ((unused)),
    struct pbb_writer_pkthandler *pkt) {
#if WRITER_STATE_MACHINE == true
  assert(writer->_state == PBB_WRITER_NONE);
#endif

  list_remove(&pkt->_pkthandle_node);
}

/**
 * Registers a new outgoing interface for the writer context
 * @param writer pointer to writer context
 * @param interf pointer to interface object
 */
void
pbb_writer_register_interface(struct pbb_writer *writer,
    struct pbb_writer_interface *interf) {
#if WRITER_STATE_MACHINE == true
  assert(writer->_state == PBB_WRITER_NONE);
#endif

  assert (interf->packet_buffer != NULL && interf->packet_size > 0);

  interf->_pkt.buffer = interf->packet_buffer;
  _pbb_tlv_writer_init(&interf->_pkt, interf->packet_size, interf->packet_size);

  interf->_is_flushed = true;

  list_add_tail(&writer->_interfaces, &interf->_if_node);
}

/**
 * Unregisters an interface from the writer context and free
 * all allocated memory associated with the interface
 * @param writer pointer to writer context
 * @param interf pointer to interface object
 */
void
pbb_writer_unregister_interface(
    struct pbb_writer *writer  __attribute__ ((unused)),
    struct pbb_writer_interface *interf) {
#if WRITER_STATE_MACHINE == true
  assert(writer->_state == PBB_WRITER_NONE);
#endif

  /* remove interface from writer */
  list_remove(&interf->_if_node);
}

/**
 * Creates a pbb writer object
 * @param writer pointer to writer context
 * @param msgid message type
 * @return pointer to message object, NULL if an error happened
 */
static struct pbb_writer_message *
_get_message(struct pbb_writer *writer, uint8_t msgid) {
  struct pbb_writer_message *msg;

  msg = avl_find_element(&writer->_msgcreators, &msgid, msg, _msgcreator_node);
  if (msg != NULL) {
    return msg;
  }

  if ((msg = calloc(1, sizeof(*msg))) == NULL) {
    return NULL;
  }

  /* initialize key */
  msg->type = msgid;
  msg->_msgcreator_node.key = &msg->type;
  if (avl_insert(&writer->_msgcreators, &msg->_msgcreator_node)) {
    free(msg);
    return NULL;
  }

  /* pre-initialize addr_len */
  msg->addr_len = PBB_MAX_ADDRLEN;

  /* initialize list/tree heads */
  avl_init(&msg->_provider_tree, avl_comp_uint32, true, NULL);

  list_init_head(&msg->_tlvtype_head);

  avl_init(&msg->_addr_tree, _msgaddr_avl_comp, false, msg);
  list_init_head(&msg->_addr_head);
  return msg;
}

/**
 * AVL tree comparator for comparing addresses.
 * Custom pointer points to corresponding pbb_writer_message.
 */
static int
_msgaddr_avl_comp(const void *k1, const void *k2, void *ptr) {
  const struct pbb_writer_message *msg = ptr;
  return memcmp(k1, k2, msg->addr_len);
}

/**
 * Copies the value of an address tlv into the internal static buffer
 * @param writer pointer to writer context
 * @param value pointer to tlv value
 * @param length number of bytes of tlv value
 */
static void *
_copy_addrtlv_value(struct pbb_writer *writer, void *value, size_t length) {
  void *ptr;
  if (writer->_addrtlv_used + length > writer->addrtlv_size) {
    /* not enough memory for addrtlv values */
    return NULL;
  }

  ptr = &writer->addrtlv_buffer[writer->_addrtlv_used];
  memcpy(ptr, value, length);
  writer->_addrtlv_used += length;

  return ptr;
}

/**
 * Free memory of all temporary allocated tlvs of a certain type
 * @param writer pointer to writer context
 * @param _tlvtype pointer to _tlvtype object
 */
static void
_free_tlvtype_tlvs(struct pbb_writer *writer, struct pbb_writer_tlvtype *tlvtype) {
  struct pbb_writer_addrtlv *addrtlv, *ptr;

  avl_remove_all_elements(&tlvtype->_tlv_tree, addrtlv, _tlv_node, ptr) {
    /* remove from address too */
    avl_remove(&addrtlv->address->_addrtlv_tree, &addrtlv->_addrtlv_node);
    writer->free_addrtlv_entry(addrtlv);
  }
}

void
_pbb_writer_free_addresses(struct pbb_writer *writer, struct pbb_writer_message *msg) {
  struct pbb_writer_address *addr, *safe_addr;
  struct pbb_writer_addrtlv *addrtlv, *safe_addrtlv;

  avl_remove_all_elements(&msg->_addr_tree, addr, _addr_tree_node, safe_addr) {
    /* remove from list too */
    list_remove(&addr->_addr_node);

    avl_remove_all_elements(&addr->_addrtlv_tree, addrtlv, _addrtlv_node, safe_addrtlv) {
      /* remove from _tlvtype too */
      avl_remove(&addrtlv->_tlvtype->_tlv_tree, &addrtlv->_tlv_node);
      writer->free_addrtlv_entry(addrtlv);
    }
    writer->free_address_entry(addr);
  }

  /* allow overwriting of addrtlv-value buffer */
  writer->_addrtlv_used = 0;
}
/**
 * Free message object if not in use anymore
 * @param writer pointer to writer context
 * @param _msg pointer to message object
 */
static void
_lazy_free_message(struct pbb_writer *writer, struct pbb_writer_message *msg) {
  if (!msg->_registered && list_is_empty(&msg->_addr_head)
      && list_is_empty(&msg->_tlvtype_head) && avl_is_empty(&msg->_provider_tree)) {
    avl_remove(&writer->_msgcreators, &msg->_msgcreator_node);
    free(msg);
  }
}

/**
 * Default allocater for address objects
 * @return pointer to cleaned address object, NULL if an error happened
 */
static struct pbb_writer_address*
_malloc_address_entry(void) {
  return calloc(1, sizeof(struct pbb_writer_address));
}

/**
 * Default allocator for address tlv object.
 * @return pointer to cleaned address tlv object, NULL if an error happened
 */
static struct pbb_writer_addrtlv*
_malloc_addrtlv_entry(void) {
  return calloc(1, sizeof(struct pbb_writer_addrtlv));
}
