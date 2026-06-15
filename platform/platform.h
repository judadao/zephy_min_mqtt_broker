#ifndef PLATFORM_H
#define PLATFORM_H

#ifdef __ZEPHYR__
#  include "zephyr/platform_zephyr.h"
#else
#  include "posix/platform_posix.h"
#endif

#endif /* PLATFORM_H */
