/* Prints a backtrace to stderr when Sunshine crashes, since the DroidDeck runtime has no debugger.
 * Frames read "binary(+offset)"; resolve them against the unstripped sunshine.debug from the build artifact. */
#define _GNU_SOURCE
#include <execinfo.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static void on_crash(int sig, siginfo_t *info, void *context) {
    (void)context;
    char head[128];
    int n = snprintf(head, sizeof head, "\n*** sunshine crashed: signal %d (%s), address %p, thread %d\n", sig,
                     strsignal(sig), info->si_addr, (int)gettid());
    write(STDERR_FILENO, head, n);
    void *frames[64];
    backtrace_symbols_fd(frames, backtrace(frames, 64), STDERR_FILENO);
    raise(sig);
}

__attribute__((constructor)) static void install(void) {
    static char stack[64 * 1024];
    stack_t ss = { .ss_sp = stack, .ss_size = sizeof stack };
    sigaltstack(&ss, NULL);
    struct sigaction sa = { .sa_sigaction = on_crash, .sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESETHAND };
    sigemptyset(&sa.sa_mask);
    int signals[] = { SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT };
    for (size_t i = 0; i < sizeof signals / sizeof *signals; i++) sigaction(signals[i], &sa, NULL);
    /* backtrace() loads libgcc_s on first use, which is not safe inside the handler. */
    void *warm[1];
    backtrace(warm, 1);
}
