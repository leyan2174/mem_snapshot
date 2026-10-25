#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

__attribute__((noinline)) void intentional_leak(void)
{
    volatile unsigned char *p = malloc(1234);
    if (!p) exit(1);
    p[0] = 42;
    /* Deliberately lose this pointer: demonstration only. */
}
int main(void)
{
    intentional_leak();
    printf("Demo PID=%ld. Send SIGUSR2 for a snapshot; SIGTERM to exit.\n", (long)getpid());
    fflush(stdout);
    for (;;) pause();
}
