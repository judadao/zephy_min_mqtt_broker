#include <string.h>
#include "platform/platform.h"
#include "session.h"

LOG_MODULE_REGISTER(mqtt_session, LOG_LEVEL_DBG);

static session_t    sessions[SESSION_MAX];
PLAT_MUTEX_DEFINE(session_lock);

void session_init(void)
{
    memset(sessions, 0, sizeof(sessions));
}

session_t *session_find(const char *client_id)
{
    plat_mutex_lock(&session_lock);
    for (int i = 0; i < SESSION_MAX; i++) {
        if (sessions[i].in_use &&
            strcmp(sessions[i].client_id, client_id) == 0) {
            plat_mutex_unlock(&session_lock);
            return &sessions[i];
        }
    }
    plat_mutex_unlock(&session_lock);
    return NULL;
}

session_t *session_create(const char *client_id)
{
    plat_mutex_lock(&session_lock);
    for (int i = 0; i < SESSION_MAX; i++) {
        if (!sessions[i].in_use) {
            strncpy(sessions[i].client_id, client_id,
                    sizeof(sessions[i].client_id) - 1);
            sessions[i].in_use = 1;
            plat_mutex_unlock(&session_lock);
            LOG_DBG("session created for %s", client_id);
            return &sessions[i];
        }
    }
    plat_mutex_unlock(&session_lock);
    return NULL;
}

void session_delete(const char *client_id)
{
    plat_mutex_lock(&session_lock);
    for (int i = 0; i < SESSION_MAX; i++) {
        if (sessions[i].in_use &&
            strcmp(sessions[i].client_id, client_id) == 0) {
            sessions[i].in_use = 0;
            LOG_DBG("session deleted for %s", client_id);
            break;
        }
    }
    plat_mutex_unlock(&session_lock);
}
