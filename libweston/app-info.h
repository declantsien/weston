#ifndef LIBWESTON_APP_INFO_H
#define LIBWESTON_APP_INFO_H

#include "libweston-internal.h"

void
weston_app_info_find_flatpak(pid_t pid, int pidfd, struct weston_client_app_info *app_info);

#endif
