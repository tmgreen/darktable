#ifndef __WIN_H__
#define __WIN_H__

#ifdef __MSVCRT_VERSION__
#undef __MSVCRT_VERSION__
#endif
#define __MSVCRT_VERSION__ 0x0700

#undef __STRICT_ANSI__
#define XMD_H
#include <windows.h>
#define sleep(n) Sleep(1000 * n)
#define HAVE_BOOLEAN

#endif

// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
