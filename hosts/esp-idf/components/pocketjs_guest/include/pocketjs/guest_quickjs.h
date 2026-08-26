#pragma once

#include "pocketjs/guest.h"
#include "quickjs.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Version-pinned escape hatch used by native surface components. */
typedef esp_err_t (*pocketjs_guest_quickjs_install_fn)(JSContext *context,
                                                       void *user_data);

esp_err_t
pocketjs_guest_quickjs_install(pocketjs_guest_t *guest,
                               pocketjs_guest_quickjs_install_fn install,
                               void *user_data);

/** Valid only for the duration of a synchronous owner-task operation. */
JSContext *pocketjs_guest_quickjs_context(pocketjs_guest_t *guest);

#ifdef __cplusplus
}
#endif
