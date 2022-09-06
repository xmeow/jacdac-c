// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "services/jd_services.h"
#include "client_internal.h"
#include "jacdac/dist/c/rolemanager.h"

#define LOG JD_LOG
#define AUTOBIND_MS 980

struct srv_state {
    SRV_COMMON;
    uint8_t auto_bind_enabled;
    uint8_t all_roles_allocated;
    uint8_t changed;
    uint8_t locked;
    jd_role_t *roles;
    uint32_t next_autobind;
    uint32_t changed_timeout;
    jd_opipe_desc_t list_pipe;
    jd_role_t *list_ptr;
};

REG_DEFINITION(                                      //
    rolemgr_regs,                                    //
    REG_SRV_COMMON,                                  //
    REG_U8(JD_ROLE_MANAGER_REG_AUTO_BIND),           //
    REG_U8(JD_ROLE_MANAGER_REG_ALL_ROLES_ALLOCATED), //
)

static srv_t *_state;

#define LOCK()                                                                                     \
    do {                                                                                           \
        JD_ASSERT(!state->locked);                                                                 \
        state->locked = 1;                                                                         \
    } while (0)

#define UNLOCK()                                                                                   \
    do {                                                                                           \
        JD_ASSERT(state->locked);                                                                  \
        state->locked = 0;                                                                         \
    } while (0)

static void rolemgr_update_allocated(srv_t *state) {
    state->all_roles_allocated = 1;
    for (jd_role_t *r = state->roles; r; r = r->_next) {
        if (!r->service) {
            state->all_roles_allocated = 0;
            break;
        }
    }
}

static void rolemgr_set(srv_t *state, jd_role_t *role, jd_device_service_t *serv) {
    if (role->service != serv) {
        if (role->service)
            role->service->flags &= ~JD_DEVICE_SERVICE_FLAG_ROLE_ASSIGNED;
        if (serv) {
            serv->flags |= JD_DEVICE_SERVICE_FLAG_ROLE_ASSIGNED;
            char shortId[5];
            jd_device_short_id(shortId, jd_service_parent(serv)->device_identifier);
            LOG("set role %s -> %s:%d", role->name, shortId, serv->service_index);
        } else {
            LOG("clear role %s", role->name);
        }
        role->service = serv;
        state->changed = 1;
        rolemgr_role_changed(role);
    }
}

static void rolemgr_autobind(srv_t *state) {
    if (!state->auto_bind_enabled)
        return;

    // LOG("autobind");
    LOCK();
    for (jd_role_t *r = state->roles; r; r = r->_next) {
        if (r->service)
            continue;

        for (jd_device_t *d = jd_devices; d; d = d->next) {
            for (int i = 1; i < d->num_services; i++) {
                jd_device_service_t *serv = &d->services[i];
                if (serv->service_class == r->service_class &&
                    !(serv->flags & JD_DEVICE_SERVICE_FLAG_ROLE_ASSIGNED)) {
                    rolemgr_set(state, r, serv);
                    goto next_role;
                }
            }
        }

    next_role:;
    }
    UNLOCK();
}

static jd_role_t *rolemgr_lookup(srv_t *state, const char *name, int name_len) {
    for (jd_role_t *r = state->roles; r; r = r->_next) {
        if (memcmp(r->name, name, name_len) == 0)
            return r;
    }
    return NULL;
}

unsigned rolemgr_serialized_role_size(jd_role_t *r) {
    return offsetof(jd_role_manager_roles_t, role) + strlen(r->name);
}

jd_role_manager_roles_t *rolemgr_serialize_role(jd_role_t *r) {
    jd_role_manager_roles_t *tmp = jd_alloc(rolemgr_serialized_role_size(r));
    if (r->service) {
        tmp->device_id = jd_service_parent(r->service)->device_identifier;
        tmp->service_idx = r->service->service_index;
    }
    tmp->service_class = r->service_class;
    memcpy(tmp->role, r->name, strlen(r->name));
    return tmp;
}

void rolemgr_process(srv_t *state) {
    while (state->list_ptr) {
        jd_role_t *r = state->list_ptr;
        while (r) {
            if (r->hidden)
                r = r->_next;
            else
                break;
        }
        if (!r)
            break;

        unsigned sz = rolemgr_serialized_role_size(r);
        int err = jd_opipe_check_space(&state->list_pipe, sz);
        if (err == JD_PIPE_TRY_AGAIN)
            break;
        if (err != 0) {
            state->list_ptr = NULL;
            jd_opipe_close(&state->list_pipe);
            break;
        }
        jd_role_manager_roles_t *tmp = rolemgr_serialize_role(r);
        err = jd_opipe_write(&state->list_pipe, tmp, sz);
        JD_ASSERT(err == 0);
        jd_free(tmp);

        state->list_ptr = r->_next;

        if (state->list_ptr == NULL)
            jd_opipe_close(&state->list_pipe);
    }

    if (jd_should_sample(&state->next_autobind, AUTOBIND_MS * 1000)) {
        rolemgr_autobind(state);
    }

    if (jd_should_sample(&state->changed_timeout, 50 * 1000)) {
        if (state->changed) {
            state->changed = 0;
            jd_send_event(state, JD_EV_CHANGE);
        }
    }
}

static void rolemgr_set_role(srv_t *state, jd_packet_t *pkt) {
    jd_role_manager_set_role_t *cmd = (jd_role_manager_set_role_t *)pkt->data;
    int name_len = pkt->service_size - sizeof(*cmd);
    jd_role_t *r = rolemgr_lookup(state, cmd->role, name_len);
    if (!r)
        return;
    if (cmd->device_id == 0) {
        rolemgr_set(state, r, NULL);
    } else {
        jd_device_service_t *serv =
            jd_device_get_service(jd_device_lookup(cmd->device_id), cmd->service_idx);
        if (serv)
            rolemgr_set(state, r, serv);
    }
}

void rolemgr_handle_packet(srv_t *state, jd_packet_t *pkt) {
    JD_ASSERT(!state->locked);

    switch (pkt->service_command) {
    case JD_ROLE_MANAGER_CMD_CLEAR_ALL_ROLES:
        for (jd_role_t *r = state->roles; r; r = r->_next)
            rolemgr_set(state, r, NULL);
        break;

    case JD_ROLE_MANAGER_CMD_SET_ROLE:
        rolemgr_set_role(state, pkt);
        break;

    case JD_ROLE_MANAGER_CMD_LIST_ROLES:
        if (jd_opipe_open_cmd(&state->list_pipe, pkt) == 0) {
            if (NULL == (state->list_ptr = state->roles)) {
                // if nothing to list, close immediately
                jd_opipe_close(&state->list_pipe);
            }
        }
        break;

    default:
        rolemgr_update_allocated(state);
        service_handle_register_final(state, pkt, rolemgr_regs);
        break;
    }
}

SRV_DEF(rolemgr, JD_SERVICE_CLASS_ROLE_MANAGER);

void jd_role_manager_init(void) {
    SRV_ALLOC(rolemgr);
    _state = state;
    state->auto_bind_enabled = 1;
    // wait with first autobind
    state->next_autobind = now + AUTOBIND_MS * 1000;
}

void rolemgr_device_destroyed(jd_device_t *dev) {
    srv_t *state = _state;

    LOCK();
    for (jd_role_t *r = state->roles; r; r = r->_next) {
        if (r->service && jd_service_parent(r->service) == dev) {
            rolemgr_set(state, r, NULL);
        }
    }
    UNLOCK();
}

static void stop_list(srv_t *state) {
    JD_ASSERT(!state->locked);
    if (state->list_ptr) {
        state->list_ptr = NULL;
        jd_opipe_close(&state->list_pipe);
    }
}

jd_role_t *jd_role_alloc(const char *name, uint32_t service_class) {
    srv_t *state = _state;
    if (!state)
        jd_panic();

    if (rolemgr_lookup(state, name, strlen(name)))
        jd_panic();

    stop_list(state);

    jd_role_t *r = jd_alloc(sizeof(jd_role_t));

    r->name = name;
    r->service_class = service_class;

    if (!state->roles || strcmp(name, state->roles->name) < 0) {
        r->_next = state->roles;
        state->roles = r;
    } else {
        for (jd_role_t *q = state->roles; q; q = q->_next) {
            if (!q->_next || strcmp(name, q->_next->name) < 0) {
                r->_next = q->_next;
                q->_next = r;
                break;
            }
        }
    }

    state->changed = 1;

    return r;
}

void jd_role_free(jd_role_t *role) {
    if (!role)
        return;
    srv_t *state = _state;

    stop_list(state);
    LOCK();
    rolemgr_set(state, role, NULL);
    UNLOCK();

    if (state->roles == role) {
        state->roles = role->_next;
    } else {
        jd_role_t *q;
        for (q = state->roles; q; q = q->_next) {
            if (q->_next == role) {
                q->_next = role->_next;
                break;
            }
        }
        if (!q)
            jd_panic();
    }
    role->name = NULL;
    jd_free(role);
}

void jd_role_free_all() {
    srv_t *state = _state;

    stop_list(state);
    LOCK();
    for (jd_role_t *r = state->roles; r; r = r->_next)
        rolemgr_set(state, r, NULL);
    UNLOCK();

    while (state->roles) {
        jd_role_t *r = state->roles;
        if (r->service)
            r->service->flags &= ~JD_DEVICE_SERVICE_FLAG_ROLE_ASSIGNED;
        state->roles = r->_next;
        r->name = NULL;
        jd_free(r);
    }

    state->changed = 1;
}

jd_role_t *jd_role_by_service(jd_device_service_t *serv) {
    srv_t *state = _state;
    for (jd_role_t *r = state->roles; r; r = r->_next)
        if (r->service == serv)
            return r;
    return NULL;
}
