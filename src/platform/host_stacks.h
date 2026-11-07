/* host_stacks.h - every host thread's native call stack on demand (host_stacks.c). */
#ifndef XBOX_HOST_STACKS_H
#define XBOX_HOST_STACKS_H

/* Prints each thread of the process as "[STACKS] tid <n> <name> module+off ..."
 * to stderr (Linux and Android; nothing elsewhere). For RECOMP_STALL_DUMP. */
void xbox_host_stacks_dump(void);

#endif
