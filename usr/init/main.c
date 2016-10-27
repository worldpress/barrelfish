/**
 * \file
 * \brief init process for child spawning
 */

/*
 * Copyright (c) 2007, 2008, 2009, 2010, 2016, ETH Zurich.
 * All rights reserved.
 *
 * This file is distributed under the terms in the attached LICENSE file.
 * If you do not find this file, copies can be found by writing to:
 * ETH Zurich D-INFK, Haldeneggsteig 4, CH-8092 Zurich. Attn: Systems Group.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <aos/aos.h>
#include <aos/aos_rpc.h>
#include <aos/waitset.h>
#include <aos/morecore.h>
#include <aos/paging.h>

#include <mm/mm.h>
#include "mem_alloc.h"
#include <spawn/spawn.h>

coreid_t my_core_id;
struct bootinfo *bi;

struct lmp_chan* clients = NULL;
size_t num_conns;

errval_t recv_handler(void* arg);
errval_t parent_send_handshake(void* arg);
errval_t parent_send_memory(void* arg);

errval_t recv_handler(void** arg)
{
    struct lmp_chan** lc = (struct lmp_chan**) arg;
    struct lmp_recv_msg msg = LMP_RECV_MSG_INIT;
    // printf("msg buflen %d\n", msg.buf.buflen);
    struct capref cap;
    errval_t err = lmp_chan_recv(*lc, &msg, &cap);
    if (err_is_fail(err) && lmp_err_is_transient(err)) {
        // reregister
        lmp_chan_register_recv(*lc, get_default_waitset(),
                MKCLOSURE((void* )recv_handler, arg));
    }

    debug_printf("main.c: got message of size %u\n", msg.buf.msglen);
    if (msg.buf.msglen > 0) {
        void* response;
        debug_printf("init.c: msg buflen %zu\n", msg.buf.msglen);
        debug_printf("init.c: msg->words[0] = %d\n", msg.words[0]);
        void** response_args;
        switch (msg.words[0]) {
            case AOS_RPC_HANDSHAKE:
                // Create channel for newly connecting client.
                if (clients == NULL) {
                    clients = malloc(sizeof(struct lmp_chan));
                } else {
                    realloc(clients, (num_conns + 1) * sizeof(struct lmp_chan));
                }
                // Initialize dedicated channel.
                clients[num_conns] = **lc;
                // Set remote cap.
                clients[num_conns].remote = cap;
                // Set response handler to handshake.
                response = (void*) parent_send_handshake;

                // Fill in response args.
                response_args = (void**) malloc(2 * sizeof(void*));
                // First arg is the channel to send the response down.
                response_args[0] = (struct lmp_chan*) malloc(sizeof(struct lmp_chan))
                *resopnse_args[0] = clients[num_conns];
                // Second arg is the 32-bit client tag (ID).
                response_args[1] = (uint32_t*) malloc(sizeof(uint32_t));
                *response_args[1] = num_conns;
                
                ++num_conns;
                break;
            case AOS_RPC_MEMORY:
                // Send response handler to memory.
                response = (void*) parent_send_memory;

                uint32_t conn = msg.words[1];
                // Response args.
                // 1. Channel to send down.
                // 2. Client ID.
                // 3. Cap to fill with memory.
                // 4. Requested size.
                response_args = (void**) malloc(4 * sizeof(void*));
                response_args[0] = (struct lmp_chan*) malloc(sizeof(struct lmp_chan));
                *response_args[0] = clients[conn];

                response_args[1] = (uint32_t*) malloc(sizeof(uint32_t));
                *response_args[1] = conn;

                response_args[2] = (struct capref*) malloc(sizeof(struct capref));
                *response_args[2] = cap;

                response_args[3] = (size_t*) malloc(sizeof(size_t));
                *response_args[3] = msg.words[2];

                break;
            default:
                return 1;  // TODO: More meaning plz
        }

        CHECK("lmp_chan_register_send parent",
                lmp_chan_register_send(lc, get_default_waitset(),
                        MKCLOSURE(response, response_args)));  
    }

    // Reregister.
    CHECK("Create Slot", lmp_chan_alloc_recv_slot(&rpc->lc));
    lmp_chan_register_recv(&rpc->lc, get_default_waitset(),
            MKCLOSURE((void *)recv_handler, arg));

    return err;
}

errval_t parent_send_handshake(void** args)
{
    // 1. Get channel to send down.
    struct lmp_chan* lc = (struct lmp_chan*) args[0];
    // 2. Get Client ID.
    uint32_t* client_id = (uint32_t*) args[1];

    // 3. Send response.
    CHECK("lmp_chan_send handshake",
            lmp_chan_send2(lc, LMP_FLAG_SYNC, NULL_CAP, AOS_RPC_OK, *client_id));

    // 4. Free args.
    free(client_id);
    free(lc);
    free(args);

    return SYS_ERR_OK;
}

errval_t parent_send_memory(void* arg)
{
    // 1. Get channel to send down.
    struct lmp_chan* lc = (struct lmp_chan*) args[0];
    // 2. Get Client ID.
    uint32_t* client_id = (uint32_t*) args[1];
    // 3. Get cap to fill.
    struct capref* retcap = (struct capref*) args[2];
    // 4. Get requested size.
    size_t* size = (size_t*) args[3];

    // 5. Allocate frame.
    size_t retsize;
    errval_t err = frame_alloc(retcap, *size, &retsize);
    // 6. Generate response code.
    size_t code = err_is_fail(err) ? AOS_RPC_FAILED : AOS_RPC_OK;

    // 7. Send response.
    CHECK("lmp_chan_send memory",
            lmp_chan_send3(lc, LMP_FLAG_SYNC, *retcap, code, (uintptr_t) err,
                    *retsize));

    return SYS_ERR_OK;
}

int main(int argc, char *argv[])
{
    errval_t err;

    /* Set the core id in the disp_priv struct */
    err = invoke_kernel_get_core_id(cap_kernel, &my_core_id);
    assert(err_is_ok(err));
    disp_set_core_id(my_core_id);

    debug_printf("init: on core %" PRIuCOREID " invoked as:", my_core_id);
    for (int i = 0; i < argc; i++) {
       printf(" %s", argv[i]);
    }
    printf("\n");

    /* First argument contains the bootinfo location, if it's not set */
    bi = (struct bootinfo*)strtol(argv[1], NULL, 10);
    if (!bi) {
        assert(my_core_id > 0);
    }

    /* Initialize paging. */
    // err = paging_init();
    // if (err_is_fail(err)) {
    //     DEBUG_ERR(err, "paging_init");
    // }

    /* Initialize the default slot allocator. */
    // err = slot_alloc_init();
    // if (err_is_fail(err)) {
    //     DEBUG_ERR(err, "slot_alloc_init");
    // }

    err = initialize_ram_alloc();
    if(err_is_fail(err)){
        DEBUG_ERR(err, "initialize_ram_alloc");
    }

    CHECK("Retype selfep from dispatcher", cap_retype(cap_selfep, cap_dispatcher, 0, ObjType_EndPoint, 0, 1));

    struct lmp_chan** lc = (struct lmp_chan**) malloc(sizeof(struct lmp_chan*));
    lc[0] = (struct lmp_chan) malloc(sizeof(struct lmp_chan));
    CHECK("Create channel for parent", lmp_chan_accept(lc[0], DEFAULT_LMP_BUF_WORDS, NULL_CAP));

    CHECK("Create Slot", lmp_chan_alloc_recv_slot(lc[0]));
    CHECK("COpy to initep", cap_copy(cap_initep, lc.local_cap));

    CHECK("lmp_chan_register_recv child",
            lmp_chan_register_recv(lc[0], get_default_waitset(),
                    MKCLOSURE((void*) recv_handler, lc)));

    // // ALLOCATE A LOT OF MEMORY TROLOLOLOLO.
    // struct capref frame;
    // size_t retsize;
    // err = frame_alloc(&frame, 900 * 1024 * 1024, &retsize);
    // if (err_is_fail(err)) {
    //     DEBUG_ERR(err, "PANIC FRAME ALLOC 64 MB");
    // } else {
    //     debug_printf("main.c: was given frame size %u\n", retsize);
    // }
    // void* buf;
    // err = paging_map_frame_attr(get_current_paging_state(),
    //     &buf, retsize, frame,
    //     VREGION_FLAGS_READ_WRITE, NULL, NULL);
    // if (err_is_fail(err)) {
    //     DEBUG_ERR(err, "PANIC MAPPING 64 MB FRAME");
    // }

    // debug_printf("main.c: testing memory @ %p\n", buf);
    // char* cbuf = (char*)buf;
    // *cbuf = 'J';
    // sys_debug_flush_cache();
    // printf("%c\n", *cbuf);

    // cbuf += 225 * 1024 * 1024;
    // *cbuf = 'K';
    // sys_debug_flush_cache();
    // printf("%c\n", *cbuf);

    // cbuf += 225 * 1024 * 1024;
    // *cbuf = 'L';
    // sys_debug_flush_cache();
    // printf("%c\n", *cbuf);

    // cbuf += 225 * 1024 * 1024;
    // *cbuf = 'M';
    // sys_debug_flush_cache();
    // printf("%c\n", *cbuf);

    // char *a = malloc(sizeof(char));
    // *a = 'a';
    // realloc(a, 2*sizeof(char));
    // printf("Value of char after resize %c\n", *a);
    // *(a+1) = 'b';
    // printf("Value of char after resize %c\n", *(a+1));
    // spawn a few helloz
    spawn_load_by_name("hello", (struct spawninfo*) malloc(sizeof(struct spawninfo)));
    // spawn_load_by_name("byebye", (struct spawninfo*) malloc(sizeof(struct spawninfo)));
    //spawn_load_by_name("hello", (struct spawninfo*) malloc(sizeof(struct spawninfo)));
    //spawn_load_by_name("byebye", (struct spawninfo*) malloc(sizeof(struct spawninfo)));


    debug_printf("Message handler loop\n");
    // Hang around
    struct waitset *default_ws = get_default_waitset();
    while (true) {
        err = event_dispatch(default_ws);
        if (err_is_fail(err)) {
            DEBUG_ERR(err, "in event_dispatch");
            abort();
        }
    }

    return EXIT_SUCCESS;
}
