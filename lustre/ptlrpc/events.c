/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 *  Copyright (C) 2002 Cluster File Systems, Inc.
 *
 *   This file is part of Lustre, http://www.lustre.org.
 *
 *   Lustre is free software; you can redistribute it and/or
 *   modify it under the terms of version 2 of the GNU General Public
 *   License as published by the Free Software Foundation.
 *
 *   Lustre is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with Lustre; if not, write to the Free Software
 *   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 */

#define DEBUG_SUBSYSTEM S_RPC

#include <linux/module.h>
#include <linux/obd_support.h>
#include <linux/lustre_net.h>

ptl_handle_eq_t request_out_eq, reply_in_eq, reply_out_eq, bulk_source_eq,
        bulk_sink_eq;
static const ptl_handle_ni_t *socknal_nip = NULL, *toenal_nip = NULL, 
        *qswnal_nip = NULL, *gmnal_nip = NULL;

/*
 *  Free the packet when it has gone out
 */
static int request_out_callback(ptl_event_t *ev)
{
        struct ptlrpc_request *req = ev->mem_desc.user_ptr;
        ENTRY;

        /* requests always contiguous */
        LASSERT((ev->mem_desc.options & PTL_MD_IOV) == 0);

        if (ev->type != PTL_EVENT_SENT) {
                // XXX make sure we understand all events, including ACK's
                CERROR("Unknown event %d\n", ev->type);
                LBUG();
        }

        /* this balances the atomic_inc in ptl_send_rpc */
        ptlrpc_req_finished(req);
        RETURN(1);
}


/*
 *  Free the packet when it has gone out
 */
static int reply_out_callback(ptl_event_t *ev)
{
        ENTRY;

        /* replies always contiguous */
        LASSERT((ev->mem_desc.options & PTL_MD_IOV) == 0);

        if (ev->type == PTL_EVENT_SENT) {
                OBD_FREE(ev->mem_desc.start, ev->mem_desc.length);
        } else {
                // XXX make sure we understand all events, including ACK's
                CERROR("Unknown event %d\n", ev->type);
                LBUG();
        }

        RETURN(1);
}

/*
 * Wake up the thread waiting for the reply once it comes in.
 */
static int reply_in_callback(ptl_event_t *ev)
{
        struct ptlrpc_request *req = ev->mem_desc.user_ptr;
        ENTRY;

        /* replies always contiguous */
        LASSERT((ev->mem_desc.options & PTL_MD_IOV) == 0);

        if (req->rq_xid == 0x5a5a5a5a5a5a5a5a) {
                CERROR("Reply received for freed request!  Probably a missing "
                       "ptlrpc_abort()\n");
                LBUG();
        }

        if (req->rq_xid != ev->match_bits) {
                CERROR("Reply packet for wrong request\n");
                LBUG();
        }

        if (ev->type == PTL_EVENT_PUT) {
                req->rq_repmsg = ev->mem_desc.start + ev->offset;
                barrier();
                wake_up(&req->rq_wait_for_rep);
        } else {
                // XXX make sure we understand all events, including ACK's
                CERROR("Unknown event %d\n", ev->type);
                LBUG();
        }

        RETURN(1);
}

int request_in_callback(ptl_event_t *ev)
{
        struct ptlrpc_request_buffer_desc *rqbd = ev->mem_desc.user_ptr;
        struct ptlrpc_service *service = rqbd->rqbd_service;

        /* requests always contiguous */
        LASSERT((ev->mem_desc.options & PTL_MD_IOV) == 0);
        /* we only enable puts */
        LASSERT(ev->type == PTL_EVENT_PUT);
        LASSERT(atomic_read(&service->srv_nrqbds_receiving) > 0);
        LASSERT(atomic_read(&rqbd->rqbd_refcount) > 0);

        if (ev->rlength != ev->mlength)
                CERROR("Warning: Possibly truncated rpc (%d/%d)\n",
                       ev->mlength, ev->rlength);

        if (ptl_is_valid_handle(&ev->unlinked_me)) {
                /* This is the last request to be received into this
                 * request buffer.  We don't bump the refcount, since the
                 * thread servicing this event is effectively taking over
                 * portals' reference.
                 */
#warning ev->unlinked_me.nal_idx is not set properly in a callback
                LASSERT(ev->unlinked_me.handle_idx==rqbd->rqbd_me_h.handle_idx);

                /* we're off the air */
                /* we'll probably start dropping packets in portals soon */
                if (atomic_dec_and_test(&service->srv_nrqbds_receiving))
                        CERROR("All request buffers busy\n");
        } else {
                /* +1 ref for service thread */
                atomic_inc(&rqbd->rqbd_refcount);
        }

        wake_up(&service->srv_waitq);

        return 0;
}

static int bulk_source_callback(ptl_event_t *ev)
{
        struct ptlrpc_bulk_desc *desc = ev->mem_desc.user_ptr;
        struct ptlrpc_bulk_page *bulk;
        struct list_head        *tmp;
        struct list_head        *next;
        ENTRY;

        CDEBUG(D_NET, "got %s event %d\n",
               (ev->type == PTL_EVENT_SENT) ? "SENT" :
               (ev->type == PTL_EVENT_ACK)  ? "ACK"  : "UNEXPECTED", ev->type);

        LASSERT(ev->type == PTL_EVENT_SENT || ev->type == PTL_EVENT_ACK);

        LASSERT(atomic_read(&desc->bd_source_callback_count) > 0 &&
                atomic_read(&desc->bd_source_callback_count) <= 2);

        /* 1 fragment for each page always */
        LASSERT(ev->mem_desc.niov == desc->bd_page_count);

        if (atomic_dec_and_test(&desc->bd_source_callback_count)) {
                void (*event_handler)(struct ptlrpc_bulk_desc *);

                list_for_each_safe(tmp, next, &desc->bd_page_list) {
                        bulk = list_entry(tmp, struct ptlrpc_bulk_page,
                                          bp_link);

                        if (bulk->bp_cb != NULL)
                                bulk->bp_cb(bulk);
                }

                /* We need to make a note of whether there's an event handler
                 * before we call wake_up, because if there is no event handler,
                 * 'desc' might be freed before we're scheduled again. */
                event_handler = desc->bd_ptl_ev_hdlr;

                desc->bd_flags |= PTL_BULK_FL_SENT;
                wake_up(&desc->bd_waitq);
                if (event_handler) {
                        LASSERT(desc->bd_ptl_ev_hdlr == event_handler);
                        event_handler(desc);
                }
        }

        RETURN(0);
}

static int bulk_sink_callback(ptl_event_t *ev)
{
        struct ptlrpc_bulk_desc *desc = ev->mem_desc.user_ptr;
        struct ptlrpc_bulk_page *bulk;
        struct list_head        *tmp;
        struct list_head        *next;
        ptl_size_t               total = 0;
        void                   (*event_handler)(struct ptlrpc_bulk_desc *);
        ENTRY;

        LASSERT(ev->type == PTL_EVENT_PUT);

        /* put with zero offset */
        LASSERT(ev->offset == 0);
        /* used iovs */
        LASSERT((ev->mem_desc.options & PTL_MD_IOV) != 0);
        /* 1 fragment for each page always */
        LASSERT(ev->mem_desc.niov == desc->bd_page_count);

        list_for_each_safe (tmp, next, &desc->bd_page_list) {
                bulk = list_entry(tmp, struct ptlrpc_bulk_page, bp_link);

                total += bulk->bp_buflen;

                if (bulk->bp_cb != NULL)
                        bulk->bp_cb(bulk);
        }

        LASSERT(ev->mem_desc.length == total);

        /* We need to make a note of whether there's an event handler
         * before we call wake_up, because if there is no event
         * handler, 'desc' might be freed before we're scheduled again. */
        event_handler = desc->bd_ptl_ev_hdlr;

        desc->bd_flags |= PTL_BULK_FL_RCVD;
        wake_up(&desc->bd_waitq);
        if (event_handler) {
                LASSERT(desc->bd_ptl_ev_hdlr == event_handler);
                event_handler(desc);
        }

        RETURN(1);
}

int ptlrpc_init_portals(void)
{
        int rc;
        ptl_handle_ni_t ni;

        /* Use the qswnal if it's there */
        if ((qswnal_nip = inter_module_get("kqswnal_ni")) != NULL)
                ni = *qswnal_nip;
        else if ((gmnal_nip = inter_module_get("kgmnal_ni")) != NULL)
                ni = *gmnal_nip;
        else if ((socknal_nip = inter_module_get("ksocknal_ni")) != NULL)
                ni = *socknal_nip;
        else if ((toenal_nip = inter_module_get("ktoenal_ni")) != NULL)
                ni = *toenal_nip;
        else {
                CERROR("get_ni failed: is a NAL module loaded?\n");
                return -EIO;
        }

        rc = PtlEQAlloc(ni, 1024, request_out_callback, &request_out_eq);
        if (rc != PTL_OK)
                CERROR("PtlEQAlloc failed: %d\n", rc);

        rc = PtlEQAlloc(ni, 1024, reply_out_callback, &reply_out_eq);
        if (rc != PTL_OK)
                CERROR("PtlEQAlloc failed: %d\n", rc);

        rc = PtlEQAlloc(ni, 1024, reply_in_callback, &reply_in_eq);
        if (rc != PTL_OK)
                CERROR("PtlEQAlloc failed: %d\n", rc);

        rc = PtlEQAlloc(ni, 1024, bulk_source_callback, &bulk_source_eq);
        if (rc != PTL_OK)
                CERROR("PtlEQAlloc failed: %d\n", rc);

        rc = PtlEQAlloc(ni, 1024, bulk_sink_callback, &bulk_sink_eq);
        if (rc != PTL_OK)
                CERROR("PtlEQAlloc failed: %d\n", rc);

        return rc;
}

void ptlrpc_exit_portals(void)
{
        PtlEQFree(request_out_eq);
        PtlEQFree(reply_out_eq);
        PtlEQFree(reply_in_eq);
        PtlEQFree(bulk_source_eq);
        PtlEQFree(bulk_sink_eq);

        if (qswnal_nip != NULL)
                inter_module_put("kqswnal_ni");
        if (socknal_nip != NULL)
                inter_module_put("ksocknal_ni");
        if (gmnal_nip != NULL)
                inter_module_put("kgmnal_ni");
        if (toenal_nip != NULL)
                inter_module_put("ktoenal_ni");
}
