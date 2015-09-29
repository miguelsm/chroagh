/* Copyright (c) 2015 The crouton Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 *
 * Provides common WebSocket functions that can be used by both websocket.c
 * and fbserver.c.
 *
 * Mostly compliant with RFC 6455 - The WebSocket Protocol.
 *
 * Things that are supported, but not tested:
 *  - Fragmented packets from client
 *  - Ping packets
 */

#ifndef _COMMON_H_
#define _COMMON_H_

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/* 0 - Quiet
 * 1 - General messages (init, new connections)
 * 2 - 1 + Information on each transfer
 * 3 - 2 + Extra information */
static int verbose = 0;

#define log(level, str, ...) do { \
    if (verbose >= (level)) printf("%s: " str "\n", __func__, ##__VA_ARGS__); \
} while (0)

#define error(str, ...) printf("%s: " str "\n", __func__, ##__VA_ARGS__)

/* Aborts if expr is false */
#define trueorabort(expr, str, ...) do { \
    if (!(expr)) { \
        printf("%s: ASSERTION " #expr " FAILED (" str ")\n", \
               __func__, ##__VA_ARGS__);                     \
        abort(); \
    }            \
} while (0)

/* Similar to perror, but prints function name as well */
#define syserror(str, ...) printf("%s: " str " (%s)\n", \
                    __func__, ##__VA_ARGS__, strerror(errno))

/* Run external command, piping some data on its stdin, and reading back
 * the output. Returns the number of bytes read from the process (at most
 * outlen), or a negative number on error (-exit status). */
static int popen2(char* cmd, char *const argv[],
                  char* input, int inlen, char* output, int outlen) {
    pid_t pid = 0;
    int stdin_fd[2];
    int stdout_fd[2];

    if (pipe(stdin_fd) < 0 || pipe(stdout_fd) < 0) {
        syserror("Failed to create pipe.");
        return -1;
    }

    log(3, "pipes: in %d/%d; out %d/%d",
           stdin_fd[0], stdin_fd[1], stdout_fd[0], stdout_fd[1]);

    pid = fork();

    if (pid < 0) {
        syserror("Fork error.");
        return -1;
    } else if (pid == 0) {
        /* Child: connect stdin/out to the pipes, close the unneeded halves */
        close(stdin_fd[1]);
        dup2(stdin_fd[0], STDIN_FILENO);
        close(stdout_fd[0]);
        dup2(stdout_fd[1], STDOUT_FILENO);

        if (argv) {
            execvp(cmd, argv);
        } else {
            execlp(cmd, cmd, NULL);
        }

        syserror("Error running '%s'.", cmd);
        exit(127);
    }

    /* Parent */

    /* Close uneeded halves (those are used by the child) */
    close(stdin_fd[0]);
    close(stdout_fd[1]);

    /* Write input, and read output. We rely on POLLHUP getting set on stdout
     * when the process exits (this assumes the process does not do anything
     * strange like closing stdout and staying alive). */
    struct pollfd fds[2];
    fds[0].events = POLLIN;
    fds[0].fd = stdout_fd[0];
    fds[1].events = POLLOUT;
    fds[1].fd = stdin_fd[1];

    int readlen = 0; /* Also acts as return value */
    int writelen = 0;
    while (1) {
        int polln = poll(fds, 2, -1);

        if (polln < 0) {
            syserror("poll error.");
            readlen = -1;
            break;
        }

        log(3, "poll=%d", polln);

        /* We can write something to stdin */
        if (fds[1].revents & POLLOUT) {
            if (inlen > writelen) {
                int n = write(stdin_fd[1], input + writelen, inlen - writelen);
                if (n < 0) {
                    error("write error.");
                    readlen = -1;
                    break;
                }
                log(3, "write n=%d/%d", n, inlen);
                writelen += n;
            }

            if (writelen == inlen) {
                /* Done writing: Only poll stdout from now on. */
                close(stdin_fd[1]);
                stdin_fd[1] = -1;
                fds[1].fd = -1;
            }
            fds[1].revents &= ~POLLOUT;
        }

        if (fds[1].revents != 0) {
            error("Unknown poll event on stdout (%d).", fds[1].revents);
            readlen = -1;
            break;
        }

        /* We can read something from stdout */
        if (fds[0].revents & POLLIN) {
            int n = read(stdout_fd[0], output + readlen, outlen - readlen);
            if (n < 0) {
                error("read error.");
                readlen = -1;
                break;
            }
            log(3, "read n=%d", n);
            readlen += n;

            if (verbose >= 3) {
                fwrite(output, 1, readlen, stdout);
            }

            if (readlen >= outlen) {
                error("Output too long.");
                break;
            }
            fds[0].revents &= ~POLLIN;
        }

        /* stdout has hung up (process terminated) */
        if (fds[0].revents == POLLHUP) {
            log(3, "pollhup");
            break;
        } else if (fds[0].revents != 0) {
            error("Unknown poll event on stdin (%d).", fds[0].revents);
            readlen = -1;
            break;
        }
    }

    if (stdin_fd[1] >= 0)
        close(stdin_fd[1]);
    /* Closing the stdout pipe forces the child process to exit */
    close(stdout_fd[0]);

    /* Get child status (no timeout: we assume the child behaves well) */
    int status = 0;
    pid_t wait_pid = waitpid(pid, &status, 0);

    if (wait_pid != pid) {
        syserror("waitpid error.");
        return -1;
    }

    if (WIFEXITED(status)) {
        log(3, "child exited!");
        if (WEXITSTATUS(status) != 0) {
            error("child exited with status %d", WEXITSTATUS(status));
            return -WEXITSTATUS(status);
        }
    } else {
        error("child process did not exit: %d", status);
        return -1;
    }

    if (writelen != inlen) {
        error("Incomplete write.");
        return -1;
    }

    return readlen;
}


#endif
