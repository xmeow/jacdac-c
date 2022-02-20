#include "jacs_internal.h"

void jacs_jd_get_register(jacs_ctx_t *ctx, unsigned role_idx, unsigned code, unsigned timeout,
                          unsigned arg) {
    jd_device_service_t *serv = ctx->roles[role_idx]->service;
    if (serv != NULL) {
        jacs_regcache_entry_t *cached = jacs_regcache_lookup(&ctx->regcache, role_idx, code, arg);
        if (cached != NULL) {
            if (!timeout || timeout > JACS_MAX_REG_VALIDITY)
                timeout = JACS_MAX_REG_VALIDITY;
            // DMESG("cached cmd=%x %d < %d", code, cached->last_refresh_time + timeout,
            //      jacs_now(ctx));
            if (cached->last_refresh_time + timeout < jacs_now(ctx)) {
                jacs_regcache_free(&ctx->regcache, cached);
            } else {
                cached = jacs_regcache_mark_used(&ctx->regcache, cached);
                memset(&ctx->packet, 0, sizeof(ctx->packet));
                ctx->packet.service_command = cached->service_command;
                ctx->packet.service_size = cached->resp_size;
                ctx->packet.service_index = serv->service_index;
                ctx->packet.device_identifier = jd_service_parent(serv)->device_identifier;
                memcpy(ctx->packet.data, jacs_regcache_data(cached), cached->resp_size);
                // DMESG("cached reg %x sz=%d cmd=%d", code, cached->resp_size, cached->service_command);
                return;
            }
        }
    }

    jacs_fiber_t *fib = ctx->curr_fiber;
    fib->role_idx = role_idx;
    fib->service_command = code;
    fib->command_arg = arg;
    fib->resend_timeout = 20;

    // DMESG("wait reg %x", code);
    jacs_fiber_sleep(fib, 0);
}

void jacs_jd_send_cmd(jacs_ctx_t *ctx, unsigned role_idx, unsigned code) {
    if (JD_IS_SET(code)) {
        jacs_regcache_entry_t *cached = jacs_regcache_lookup(
            &ctx->regcache, role_idx, (code & ~JD_CMD_SET_REGISTER) | JD_CMD_GET_REGISTER, 0);
        if (cached != NULL)
            jacs_regcache_free(&ctx->regcache, cached);
    }

    const jacs_role_desc_t *role = jacs_img_get_role(&ctx->img, role_idx);
    jacs_fiber_t *fib = ctx->curr_fiber;

    if (role->service_class == JD_SERVICE_CLASS_JACSCRIPT_CONDITION) {
        jacs_fiber_sleep(fib, 0);
        DMESG("wake condition");
        jacs_jd_wake_role(ctx, role_idx);
        return;
    }

    fib->role_idx = role_idx;
    fib->service_command = code;
    fib->resend_timeout = 20;

    unsigned sz = ctx->packet.service_size;
    fib->payload = jd_alloc(sz);
    fib->payload_size = sz;
    memcpy(fib->payload, ctx->packet.data, sz);
    jacs_fiber_sleep(fib, 0);
}

static void jacs_jd_set_packet(jacs_ctx_t *ctx, unsigned role_idx, unsigned service_command,
                               const void *payload, unsigned sz) {
    jd_packet_t *pkt = &ctx->packet;
    pkt->_size = (sz + 4 + 3) & ~3;
    pkt->flags = JD_FRAME_FLAG_COMMAND;
    jd_device_t *dev = jd_service_parent(ctx->roles[role_idx]->service);
    pkt->device_identifier = dev->device_identifier;
    pkt->service_size = sz;
    pkt->service_index = ctx->roles[role_idx]->service->service_index;
    pkt->service_command = service_command;
    if (payload)
        memcpy(pkt->data, payload, sz);
}

void jacs_jd_wake_role(jacs_ctx_t *ctx, unsigned role_idx) {
    for (jacs_fiber_t *fiber = ctx->fibers; fiber; fiber = fiber->next) {
        if (fiber->role_idx == role_idx) {
            jacs_fiber_run(fiber);
        }
    }
}

static int jacs_jd_reg_arg_length(jacs_ctx_t *ctx, unsigned command_arg) {
    assert(command_arg != 0);
    jd_packet_t *pkt = &ctx->packet;
    int slen = jacs_img_get_string_len(&ctx->img, command_arg);
    if (pkt->service_size >= slen + 1 && pkt->data[slen] == 0 &&
        memcmp(jacs_img_get_string_ptr(&ctx->img, command_arg), pkt->data, slen) == 0) {
        return slen + 1;
    } else {
        return 0;
    }
}

static jacs_regcache_entry_t *jacs_jd_update_regcache(jacs_ctx_t *ctx, unsigned role_idx,
                                                      unsigned command_arg) {
    jd_packet_t *pkt = &ctx->packet;

    int resp_size = pkt->service_size;
    uint8_t *dp = pkt->data;
    if (command_arg) {
        int slen = jacs_jd_reg_arg_length(ctx, command_arg);
        if (!slen)
            return NULL;
        dp += slen;
        resp_size -= slen;
    }

    jacs_regcache_entry_t *q =
        jacs_regcache_lookup(&ctx->regcache, role_idx, pkt->service_command, command_arg);
    if (q && q->resp_size != resp_size) {
        jacs_regcache_free(&ctx->regcache, q);
        q = NULL;
    }

    if (!q) {
        q = jacs_regcache_alloc(&ctx->regcache, role_idx, pkt->service_command, resp_size);
        q->argument = command_arg;
    }

    memcpy(jacs_regcache_data(q), dp, resp_size);
    q->last_refresh_time = jacs_now(ctx);

    return q;
}

static bool jacs_jd_pkt_matches_role(jacs_ctx_t *ctx, unsigned role_idx) {
    jd_packet_t *pkt = &ctx->packet;
    jd_device_service_t *serv = ctx->roles[role_idx]->service;
    return serv &&
           ((pkt->service_index == 0 && pkt->service_command == 0) ||
            serv->service_index == pkt->service_index) &&
           jd_service_parent(serv)->device_identifier == pkt->device_identifier;
}

#define RESUME_USER_CODE 1
#define KEEP_WAITING 0

bool jacs_jd_should_run(jacs_fiber_t *fiber) {
    if (!fiber->service_command) {
        return RESUME_USER_CODE;
    }

    jacs_ctx_t *ctx = fiber->ctx;
    jd_device_service_t *serv = ctx->roles[fiber->role_idx]->service;

    if (serv == NULL) {
        // role unbound, keep waiting, no timeout
        jacs_fiber_set_wake_time(fiber, 0);
        return KEEP_WAITING;
    }

    if (fiber->payload) {
        jacs_jd_set_packet(ctx, fiber->role_idx, fiber->service_command, fiber->payload,
                           fiber->payload_size);
        jd_send_pkt(&ctx->packet);
        DMESG("send pkt cmd=%x", fiber->service_command);
        fiber->service_command = 0;
        jd_free(fiber->payload);
        fiber->payload = NULL;
        return RESUME_USER_CODE;
    }

    jd_packet_t *pkt = &ctx->packet;

    if (jd_is_report(pkt) && pkt->service_command &&
        pkt->service_command == fiber->service_command &&
        jacs_jd_pkt_matches_role(ctx, fiber->role_idx)) {
        jacs_regcache_entry_t *q =
            jacs_jd_update_regcache(ctx, fiber->role_idx, fiber->command_arg);
        if (q) {
            q = jacs_regcache_mark_used(&ctx->regcache, q);
            return RESUME_USER_CODE;
        }
    }

    if (jacs_now(ctx) >= fiber->wake_time) {
        int arglen = 0;
        const void *argp = NULL;
        if (fiber->command_arg) {
            arglen = jacs_img_get_string_len(&ctx->img, fiber->command_arg);
            argp = jacs_img_get_string_ptr(&ctx->img, fiber->command_arg);
        }

        jacs_jd_set_packet(ctx, fiber->role_idx, fiber->service_command, argp, arglen);
        jd_send_pkt(&ctx->packet);
        DMESG("(re)send pkt cmd=%x", fiber->service_command);

        if (fiber->resend_timeout < 1000)
            fiber->resend_timeout *= 2;
        jacs_fiber_sleep(fiber, fiber->resend_timeout);
    }

    return KEEP_WAITING;
}

static void jacs_jd_update_all_regcache(jacs_ctx_t *ctx, unsigned role_idx) {
    jacs_regcache_entry_t *q = NULL;
    jd_packet_t *pkt = &ctx->packet;

    if (jd_is_command(pkt))
        return;

    if (jd_is_event(pkt) && (pkt->service_command & JD_CMD_EVENT_CODE_MASK) == JD_EV_CHANGE) {
        jacs_regcache_age(&ctx->regcache, role_idx, jacs_now(ctx) - 10000);
        return;
    }

    for (;;) {
        q = jacs_regcache_next(&ctx->regcache, role_idx, pkt->service_command, q);
        if (!q)
            break;
        if (jacs_jd_update_regcache(ctx, q->role_idx, q->argument))
            break; // we only allow for one update
    }
}

static const char *jacs_jd_role_name(jacs_ctx_t *ctx, unsigned idx) {
    const jacs_role_desc_t *role = jacs_img_get_role(&ctx->img, idx);
    return jacs_img_get_string_ptr(&ctx->img, role->name_idx);
}

void jacs_jd_process_pkt(jacs_ctx_t *ctx, jd_device_service_t *serv, jd_packet_t *pkt) {
    if (ctx->error_code)
        return;

    memcpy(&ctx->packet, pkt, pkt->service_size + 16);
    pkt = &ctx->packet;

    unsigned numroles = jacs_img_num_roles(&ctx->img);
    for (unsigned idx = 0; idx < numroles; ++idx) {
        if (jacs_jd_pkt_matches_role(ctx, idx)) {
            // DMESG("wake pkt %x / %d", pkt->service_command, pkt->service_size);
            jacs_fiber_sync_now(ctx);
            jacs_jd_update_all_regcache(ctx, idx);
            jacs_jd_wake_role(ctx, idx);
        }
    }

    jacs_fiber_poke(ctx);
}

void jacs_jd_role_changed(jacs_ctx_t *ctx, jd_role_t *role) {
    unsigned numroles = jacs_img_num_roles(&ctx->img);
    for (unsigned idx = 0; idx < numroles; ++idx) {
        if (ctx->roles[idx] == role) {
            jacs_regcache_free_role(&ctx->regcache, idx);
            jacs_jd_reset_packet(ctx);
            jacs_jd_wake_role(ctx, idx);
            break;
        }
    }
    jacs_fiber_poke(ctx);
}

void jacs_jd_reset_packet(jacs_ctx_t *ctx) {
    memset(&ctx->packet, 0xff, 32);
}

void jacs_jd_init_roles(jacs_ctx_t *ctx) {
    unsigned numroles = jacs_img_num_roles(&ctx->img);
    for (unsigned idx = 0; idx < numroles; ++idx) {
        const jacs_role_desc_t *role = jacs_img_get_role(&ctx->img, idx);
        ctx->roles[idx] = jd_role_alloc(jacs_jd_role_name(ctx, idx), role->service_class);
    }
}

void jacs_jd_free_roles(jacs_ctx_t *ctx) {
    unsigned numroles = jacs_img_num_roles(&ctx->img);
    for (unsigned idx = 0; idx < numroles; ++idx) {
        jd_role_free(ctx->roles[idx]);
        ctx->roles[idx] = NULL;
    }
}