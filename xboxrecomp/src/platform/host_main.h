/* host_main.h -- the main thread for the window system (see host_main.c). */
#ifndef XBOXRECOMP_HOST_MAIN_H
#define XBOXRECOMP_HOST_MAIN_H

/* Run fn(argc, argv) -- the title -- and return its result. On macOS on a
 * thread of its own while this (main) thread serves the calls below and
 * pumps events; elsewhere directly. */
int  xbox_host_main_run(int (*fn)(int, char **), int argc, char **argv);

/* Run fn(arg) on the main thread and wait for it (directly when already
 * there, or when nothing serves the main thread). */
void xbox_host_main_call(void (*fn)(void *), void *arg);

/* 1 when this thread may touch windows and events directly. */
int  xbox_host_on_main(void);

/* Something the main thread runs after each event pump (controllers). */
void xbox_host_main_tick(void (*fn)(void));

#endif
