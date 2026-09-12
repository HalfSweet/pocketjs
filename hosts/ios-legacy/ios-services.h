#ifndef POCKET_IOS_SERVICES_H
#define POCKET_IOS_SERVICES_H
#include "quickjs.h"
int pocket_ios_services_mount(JSContext *ctx, JSValueConst global);
void pocket_ios_services_window(void *window);
void pocket_ios_services_active(int active);
void pocket_ios_services_shutdown(void);
int pocket_ios_fs_mount(JSContext *ctx, JSValueConst global, const char *root);
#endif
