/*
 *
 * honggfuzz - the main file
 * -----------------------------------------
 *
 * Authors: Robert Swiecki <swiecki@google.com>
 *          Felix Gröbert <groebert@google.com>
 *
 * Copyright 2010-2018 by Google Inc. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License"); you may
 * not use this file except in compliance with the License. You may obtain
 * a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
 * implied. See the License for the specific language governing
 * permissions and limitations under the License.
 *
 */

#include <getopt.h>
#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "cmdline.h"
#include "display.h"
#include "fuzz.h"
#include "input.h"
#include "libhfcommon/common.h"
#include "libhfcommon/files.h"
#include "libhfcommon/log.h"
#include "libhfcommon/util.h"
#include "socketfuzzer.h"

static int sigReceived = 0;

/*
 * CygWin/MinGW incorrectly copies stack during fork(), so we need to keep some
 * structures in the data section
 */
honggfuzz_t hfuzz;

static void exitWithMsg(const char* msg, int exit_code) {
    HF_ATTR_UNUSED ssize_t sz = write(STDERR_FILENO, msg, strlen(msg));
    exit(exit_code);
    abort();
}

static bool showDisplay = true;
void sigHandler(int sig) {
    /* We should not terminate upon SIGALRM delivery */
    if (sig == SIGALRM) {
        if (fuzz_shouldTerminate()) {
            exitWithMsg("Terminating forcefully\n", EXIT_FAILURE);
        }
        showDisplay = true;
        return;
    }

    if (ATOMIC_GET(sigReceived) != 0) {
        exitWithMsg("Repeated termination signal caugth\n", EXIT_FAILURE);
    }

    ATOMIC_SET(sigReceived, sig);
}

static void setupRLimits(void) {
    struct rlimit rlim;
    if (getrlimit(RLIMIT_NOFILE, &rlim) == -1) {
        PLOG_W("getrlimit(RLIMIT_NOFILE)");
        return;
    }
    if (rlim.rlim_cur >= 1024) {
        return;
    }
    if (rlim.rlim_max < 1024) {
        LOG_E("RLIMIT_NOFILE max limit < 1024 (%zu). Expect troubles!", (size_t)rlim.rlim_max);
        return;
    }
    rlim.rlim_cur = MIN(1024, rlim.rlim_max);  // we don't need more
    if (setrlimit(RLIMIT_NOFILE, &rlim) == -1) {
        PLOG_E("Couldn't setrlimit(RLIMIT_NOFILE, cur=%zu/max=%zu)", (size_t)rlim.rlim_cur,
            (size_t)rlim.rlim_max);
    }
}

static void setupMainThreadTimer(void) {
    const struct itimerval it = {
        .it_value = {.tv_sec = 1, .tv_usec = 0},
        .it_interval = {.tv_sec = 0, .tv_usec = 1000 * 200},
    };
    if (setitimer(ITIMER_REAL, &it, NULL) == -1) {
        PLOG_F("setitimer(ITIMER_REAL)");
    }
}

static void setupSignalsPreThreads(void) {
    /* Block signals which should be handled or blocked in the main thread */
    sigset_t ss;
    sigemptyset(&ss);
    sigaddset(&ss, SIGTERM);
    sigaddset(&ss, SIGINT);
    sigaddset(&ss, SIGQUIT);
    sigaddset(&ss, SIGALRM);
    sigaddset(&ss, SIGPIPE);
    sigaddset(&ss, SIGIO);
    sigaddset(&ss, SIGCHLD);
    if (sigprocmask(SIG_BLOCK, &ss, NULL) != 0) {
        PLOG_F("pthread_sigmask(SIG_BLOCK)");
    }
}

static void setupSignalsMainThread(void) {
    struct sigaction sa = {
        .sa_handler = sigHandler,
        .sa_flags = 0,
    };
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGTERM, &sa, NULL) == -1) {
        PLOG_F("sigaction(SIGTERM) failed");
    }
    if (sigaction(SIGINT, &sa, NULL) == -1) {
        PLOG_F("sigaction(SIGINT) failed");
    }
    if (sigaction(SIGQUIT, &sa, NULL) == -1) {
        PLOG_F("sigaction(SIGQUIT) failed");
    }
    if (sigaction(SIGALRM, &sa, NULL) == -1) {
        PLOG_F("sigaction(SIGQUIT) failed");
    }
    /* Unblock signals which should be handled by the main thread */
    sigset_t ss;
    sigemptyset(&ss);
    sigaddset(&ss, SIGTERM);
    sigaddset(&ss, SIGINT);
    sigaddset(&ss, SIGQUIT);
    sigaddset(&ss, SIGALRM);
    if (sigprocmask(SIG_UNBLOCK, &ss, NULL) != 0) {
        PLOG_F("pthread_sigmask(SIG_UNBLOCK)");
    }
}

int main(int argc, char** argv) {
    /*
     * Work around CygWin/MinGW
     */
    char** myargs = (char**)util_Malloc(sizeof(char*) * (argc + 1));
    defer {
        free(myargs);
    };

    int i;
    for (i = 0U; i < argc; i++) {
        myargs[i] = argv[i];
    }
    myargs[i] = NULL;

    if (cmdlineParse(argc, myargs, &hfuzz) == false) {
        LOG_F("Parsing of the cmd-line arguments failed");
    }

    if (hfuzz.display.useScreen) {
        display_init();
    }

    if (hfuzz.socketFuzzer.enabled) {
        LOG_I(
            "No input file corpus loaded, the external socket_fuzzer is responsible for "
            "creating the fuzz data");
        setupSocketFuzzer(&hfuzz);
    } else if (!input_init(&hfuzz)) {
        LOG_F("Couldn't load input corpus");
        exit(EXIT_FAILURE);
    }

    if (hfuzz.mutate.dictionaryFile && (input_parseDictionary(&hfuzz) == false)) {
        LOG_F("Couldn't parse dictionary file ('%s')", hfuzz.mutate.dictionaryFile);
    }

    if (hfuzz.feedback.blacklistFile && (input_parseBlacklist(&hfuzz) == false)) {
        LOG_F("Couldn't parse stackhash blacklist file ('%s')", hfuzz.feedback.blacklistFile);
    }
#define hfuzzl hfuzz.linux
    if (hfuzzl.symsBlFile &&
        ((hfuzzl.symsBlCnt = files_parseSymbolFilter(hfuzzl.symsBlFile, &hfuzzl.symsBl)) == 0)) {
        LOG_F("Couldn't parse symbols blacklist file ('%s')", hfuzzl.symsBlFile);
    }

    if (hfuzzl.symsWlFile &&
        ((hfuzzl.symsWlCnt = files_parseSymbolFilter(hfuzzl.symsWlFile, &hfuzzl.symsWl)) == 0)) {
        LOG_F("Couldn't parse symbols whitelist file ('%s')", hfuzzl.symsWlFile);
    }

    if (hfuzz.feedback.dynFileMethod != _HF_DYNFILE_NONE) {
        if (!(hfuzz.feedback.feedbackMap = files_mapSharedMem(
                  sizeof(feedback_t), &hfuzz.feedback.bbFd, "hfuzz-feedback", hfuzz.io.workDir))) {
            LOG_F("files_mapSharedMem(sz=%zu, dir='%s') failed", sizeof(feedback_t),
                hfuzz.io.workDir);
        }
    }

    /*
     * So far, so good
     */
    pthread_t threads[hfuzz.threads.threadsMax];

    setupRLimits();
    setupSignalsPreThreads();
    fuzz_threadsStart(&hfuzz, threads);
    setupSignalsMainThread();

    setupMainThreadTimer();

    for (;;) {
        if (hfuzz.display.useScreen && showDisplay) {
            display_display(&hfuzz);
            showDisplay = false;
        }
        if (ATOMIC_GET(sigReceived) > 0) {
            LOG_I("Signal %d (%s) received, terminating", ATOMIC_GET(sigReceived),
                strsignal(ATOMIC_GET(sigReceived)));
            break;
        }
        if (ATOMIC_GET(hfuzz.threads.threadsFinished) >= hfuzz.threads.threadsMax) {
            break;
        }
        if (hfuzz.timing.runEndTime > 0 && (time(NULL) > hfuzz.timing.runEndTime)) {
            LOG_I("Maximum run time reached, terminating");
            break;
        }
        pause();
    }

    fuzz_setTerminating();
    fuzz_threadsStop(&hfuzz, threads);

    /* Clean-up global buffers */
    if (hfuzz.feedback.blacklist) {
        free(hfuzz.feedback.blacklist);
    }
    if (hfuzz.linux.symsBl) {
        free(hfuzz.linux.symsBl);
    }
    if (hfuzz.linux.symsWl) {
        free(hfuzz.linux.symsWl);
    }
    if (hfuzz.socketFuzzer.enabled) {
        cleanupSocketFuzzer();
    }

    return EXIT_SUCCESS;
}
