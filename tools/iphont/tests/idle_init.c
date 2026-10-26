/* QEMU-only process 1 fixture. Sleep while the real XNU agent runs its read-only self-test. */
void start(void)
{
    struct { long seconds, nanoseconds; } delay = {1, 0};
    for (;;) {
        register long number __asm__("x16") = 240; /* nanosleep, bsd/kern/syscalls.master */
        register long arg __asm__("x0") = (long)&delay;
        register long remaining __asm__("x1") = 0;
        __asm__ volatile ("svc #0x80" : "+r" (arg) : "r" (number), "r" (remaining) : "memory", "cc");
    }
}
