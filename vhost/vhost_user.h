#pragma once
#include "vhost_user_spec.h"

int vhost_ioctl(int sock, VhostUserRequest req, void *arg);