/* SPDX-License-Identifier: MIT
 * LD_PRELOAD interposer for glibc's legacy mallinfo() (SPEC §2.3). libcef calls it from its
 * memory dump provider; its int fields overflow once the process heap passes 2 GiB, and
 * Chromium's checked cast then CHECK()s (SIGILL, exit 132, after hours or weeks; seen in
 * LeeO86/strom, CEF issue 3963). Returning zeros only empties that memory statistic.
 */
#include <malloc.h>
#include <string.h>

struct mallinfo mallinfo(void)
{
    struct mallinfo info;
    memset(&info, 0, sizeof(info));
    return info;
}
