/*
 * Copyright 2006, The Android Open Source Project
 * Copyright (c) 2013, The Linux Foundation. All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "DEBUG"

#include <stdio.h>
#include <errno.h>
#include <signal.h>
#include <pthread.h>
#include <stdarg.h>
#include <fcntl.h>
#include <sys/types.h>
#include <dirent.h>
#include <time.h>

#include <sys/ptrace.h>
#include <sys/wait.h>
#include <elf.h>
#include <sys/stat.h>
#include <sys/poll.h>

#include <log/logger.h>

#include <cutils/sockets.h>
#include <cutils/properties.h>
#include <cutils/debugger.h>

#include <linux/input.h>

#include <private/android_filesystem_config.h>

#include "backtrace.h"
#include "getevent.h"
#include "tombstone.h"
#include "utility.h"

struct debugger_request_t {
  debugger_action_t action;
  pid_t pid, tid;
  uid_t uid, gid;
  uintptr_t abort_msg_address;
  int32_t original_si_code;
};

static void wait_for_user_action(const debugger_request_t &request) {
  // Find out the name of the process that crashed.
  char path[64];
  snprintf(path, sizeof(path), "/proc/%d/exe", request.pid);

  char exe[PATH_MAX];
  int count;
  if ((count = readlink(path, exe, sizeof(exe) - 1)) == -1) {
    ALOGE("readlink('%s') failed: %s", path, strerror(errno));
    strlcpy(exe, "unknown", sizeof(exe));
  } else {
    exe[count] = '\0';
  }

  // Explain how to attach the debugger.
  ALOGI("********************************************************\n"
        "* Process %d has been suspended while crashing.\n"
        "* To attach gdbserver for a gdb connection on port 5039\n"
        "* and start gdbclient:\n"
        "*\n"
        "*     gdbclient %s :5039 %d\n"
        "* or\n"
        "*     dddclient %s :5039 %d\n"
        "*\n"
        "* Wait for gdb to start, then press the VOLUME DOWN key\n"
        "* to let the process continue crashing.\n"
        "********************************************************",
        request.pid, exe, request.tid, exe, request.tid);

  // Wait for VOLUME DOWN.
  if (init_getevent() == 0) {
    while (true) {
      input_event e;
      if (get_event(&e, -1) == 0) {
        if (e.type == EV_KEY && e.code == KEY_VOLUMEDOWN && e.value == 0) {
          break;
        }
      }
    }
    uninit_getevent();
  }

  ALOGI("debuggerd resuming process %d", request.pid);
}

static int get_process_info(pid_t tid, pid_t* out_pid, uid_t* out_uid, uid_t* out_gid) {
  char path[64];
  snprintf(path, sizeof(path), "/proc/%d/status", tid);

  FILE* fp = fopen(path, "r");
  if (!fp) {
    return -1;
  }

  int fields = 0;
  char line[1024];
  while (fgets(line, sizeof(line), fp)) {
    size_t len = strlen(line);
    if (len > 6 && !memcmp(line, "Tgid:\t", 6)) {
      *out_pid = atoi(line + 6);
      fields |= 1;
    } else if (len > 5 && !memcmp(line, "Uid:\t", 5)) {
      *out_uid = atoi(line + 5);
      fields |= 2;
    } else if (len > 5 && !memcmp(line, "Gid:\t", 5)) {
      *out_gid = atoi(line + 5);
      fields |= 4;
    }
  }
  fclose(fp);
  return fields == 7 ? 0 : -1;
}

static bool copy_file(const char* src, char* dest)
{
   #define BUF_SIZE 64
   ssize_t bytes;
   int  source_fh, dest_fh;
   int  total_size = 0;
   char buffer[BUF_SIZE];

   if ((source_fh = open(src, O_RDONLY, O_NOFOLLOW)) == -1) {
        ALOGE("Unable to open source file %s\n", src);
   } else {
        if((dest_fh = open(dest, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0640)) == -1) {
            ALOGE("Unable to write destination file %s\n", dest);
        } else {
            while ((bytes = read(source_fh, buffer, BUF_SIZE)) > 0) {
                if (write(dest_fh, buffer, bytes) < 0) {
                    ALOGE("Write failed for destination file %s. Copied %d bytes\n",
                            dest, total_size);
                    break;
                }
                total_size += bytes;
            }
            ALOGI("Copied %s to %s - size: %d\n", src, dest, total_size);
            fsync(dest_fh);
            close(dest_fh);
        }
        close(source_fh);
        if (total_size > 0) {
            return true;
        }
    }
    return false;
}

static void collect_etb_map(int cr_pid)
{
    struct stat s;
    char   src_buf[64];
    char   dest_buf[64];

    snprintf(dest_buf, sizeof dest_buf, "/data/core/etb.%d", cr_pid);
    if (!copy_file("/dev/coresight-tmc-etf", dest_buf)) {
        ALOGE("Unable to copy ETB buffer file /dev/coresight-tmc-etf\n");
    } else {
        memset(src_buf, 0, sizeof(src_buf));
        snprintf(src_buf, sizeof(src_buf), "/proc/%d/maps", cr_pid);
        if(stat(src_buf, &s)) {
            ALOGE("map file /proc/%d/maps does not exist for pid %d\n",
                cr_pid, cr_pid);
        } else {
            snprintf(dest_buf, sizeof dest_buf, "/data/core/maps.%d", cr_pid);
            if (!copy_file(src_buf, dest_buf)) {
                ALOGE("Unable to copy map file /proc/%d/maps", cr_pid);
            }
        }
    }
}

static void enable_etb_trace(struct ucred cr) {
    char value[PROPERTY_VALUE_MAX];
    property_get("persist.debug.trace", value, "");
    if ((strcmp(value,"1") == 0)) {
        /* Allow ETB collection only once; Note: in future this behavior can be changed
        * To allow this, use a property to indicate whether the ETB has been collected */
        property_get("debug.etb.collected", value, "");
        if(strcmp(value,"1")) {
            ALOGI("Collecting ETB dumps (from pid=%d uid=%d)\n",
                     cr.pid, cr.uid);
            property_set("debug.etb.collected", "1");
            collect_etb_map(cr.pid);
        }
        else {
            ALOGI("ETB already collected once, skipping (from pid=%d uid=%d)\n",
                     cr.pid, cr.uid);
        }
    }
}

static int read_request(int fd, debugger_request_t* out_request) {
  ucred cr;
  socklen_t len = sizeof(cr);
  int status = getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cr, &len);
  if (status != 0) {
    ALOGE("cannot get credentials");
    return -1;
  }

  ALOGV("reading tid");
  fcntl(fd, F_SETFL, O_NONBLOCK);

  pollfd pollfds[1];
  pollfds[0].fd = fd;
  pollfds[0].events = POLLIN;
  pollfds[0].revents = 0;
  status = TEMP_FAILURE_RETRY(poll(pollfds, 1, 3000));
  if (status != 1) {
    ALOGE("timed out reading tid (from pid=%d uid=%d)\n", cr.pid, cr.uid);
    return -1;
  }

  debugger_msg_t msg;
  memset(&msg, 0, sizeof(msg));
  status = TEMP_FAILURE_RETRY(read(fd, &msg, sizeof(msg)));
  if (status < 0) {
    ALOGE("read failure? %s (pid=%d uid=%d)\n", strerror(errno), cr.pid, cr.uid);
    return -1;
  }
  if (status != sizeof(debugger_msg_t)) {
    ALOGE("invalid crash request of size %d (from pid=%d uid=%d)\n", status, cr.pid, cr.uid);
    return -1;
  }

  out_request->action = msg.action;
  out_request->tid = msg.tid;
  out_request->pid = cr.pid;
  out_request->uid = cr.uid;
  out_request->gid = cr.gid;
  out_request->abort_msg_address = msg.abort_msg_address;
  out_request->original_si_code = msg.original_si_code;

  if (msg.action == DEBUGGER_ACTION_CRASH) {
    // Ensure that the tid reported by the crashing process is valid.
    char buf[64];
    struct stat s;
    enable_etb_trace(cr);
    snprintf(buf, sizeof buf, "/proc/%d/task/%d", out_request->pid, out_request->tid);
    if (stat(buf, &s)) {
      ALOGE("tid %d does not exist in pid %d. ignoring debug request\n",
          out_request->tid, out_request->pid);
      return -1;
    }
  } else if (cr.uid == 0
            || (cr.uid == AID_SYSTEM && msg.action == DEBUGGER_ACTION_DUMP_BACKTRACE)) {
    // Only root or system can ask us to attach to any process and dump it explicitly.
    // However, system is only allowed to collect backtraces but cannot dump tombstones.
    status = get_process_info(out_request->tid, &out_request->pid,
                              &out_request->uid, &out_request->gid);
    if (status < 0) {
      ALOGE("tid %d does not exist. ignoring explicit dump request\n", out_request->tid);
      return -1;
    }
  } else {
    // No one else is allowed to dump arbitrary processes.
    return -1;
  }
  return 0;
}

static bool should_attach_gdb(debugger_request_t* request) {
  if (request->action == DEBUGGER_ACTION_CRASH) {
    char value[PROPERTY_VALUE_MAX];
    property_get("debug.db.uid", value, "-1");
    int debug_uid = atoi(value);
    if (debug_uid >= 0 && request->uid <= (uid_t)debug_uid) {
      return true;
    } else {
      /* External docs say to use 10,000 but more is likely needed; be helpful. */
      if (request->uid > (uid_t)debug_uid) {
        ALOGI("request->uid:%d > property debug.db.uid:%d; NOT waiting for gdb.",
               request->uid,                 debug_uid);
      } else {
        ALOGI("property debug.db.uid not set; NOT waiting for gdb.");
        ALOGI("HINT: adb shell setprop debug.db.uid 100000");
        ALOGI("HINT: adb forward tcp:5039 tcp:5039");
      }
    }
  }
  return false;
}

static void handle_request(int fd) {
  ALOGV("handle_request(%d)\n", fd);

  debugger_request_t request;
  memset(&request, 0, sizeof(request));
  int status = read_request(fd, &request);
  if (!status) {
    ALOGV("BOOM: pid=%d uid=%d gid=%d tid=%d\n",
         request.pid, request.uid, request.gid, request.tid);

    // At this point, the thread that made the request is blocked in
    // a read() call.  If the thread has crashed, then this gives us
    // time to PTRACE_ATTACH to it before it has a chance to really fault.
    //
    // The PTRACE_ATTACH sends a SIGSTOP to the target process, but it
    // won't necessarily have stopped by the time ptrace() returns.  (We
    // currently assume it does.)  We write to the file descriptor to
    // ensure that it can run as soon as we call PTRACE_CONT below.
    // See details in bionic/libc/linker/debugger.c, in function
    // debugger_signal_handler().
    if (ptrace(PTRACE_ATTACH, request.tid, 0, 0)) {
      ALOGE("ptrace attach failed: %s\n", strerror(errno));
    } else {
      bool detach_failed = false;
      bool tid_unresponsive = false;
      bool attach_gdb = should_attach_gdb(&request);
      if (TEMP_FAILURE_RETRY(write(fd, "\0", 1)) != 1) {
        ALOGE("failed responding to client: %s\n", strerror(errno));
      } else {
        char* tombstone_path = NULL;

        if (request.action == DEBUGGER_ACTION_CRASH) {
          close(fd);
          fd = -1;
        }

        int total_sleep_time_usec = 0;
        for (;;) {
          int signal = wait_for_sigstop(request.tid, &total_sleep_time_usec, &detach_failed);
          if (signal == -1) {
            tid_unresponsive = true;
            break;
          }

          switch (signal) {
            case SIGSTOP:
              if (request.action == DEBUGGER_ACTION_DUMP_TOMBSTONE) {
                ALOGV("stopped -- dumping to tombstone\n");
                tombstone_path = engrave_tombstone(request.pid, request.tid,
                                                   signal, request.original_si_code,
                                                   request.abort_msg_address, true,
                                                   &detach_failed, &total_sleep_time_usec);
              } else if (request.action == DEBUGGER_ACTION_DUMP_BACKTRACE) {
                ALOGV("stopped -- dumping to fd\n");
                dump_backtrace(fd, -1, request.pid, request.tid, &detach_failed,
                               &total_sleep_time_usec);
              } else {
                ALOGV("stopped -- continuing\n");
                status = ptrace(PTRACE_CONT, request.tid, 0, 0);
                if (status) {
                  ALOGE("ptrace continue failed: %s\n", strerror(errno));
                }
                continue; // loop again
              }
              break;

            case SIGABRT:
            case SIGBUS:
            case SIGFPE:
            case SIGILL:
            case SIGPIPE:
            case SIGSEGV:
#ifdef SIGSTKFLT
            case SIGSTKFLT:
#endif
            case SIGTRAP:
              ALOGV("stopped -- fatal signal\n");
              // Send a SIGSTOP to the process to make all of
              // the non-signaled threads stop moving.  Without
              // this we get a lot of "ptrace detach failed:
              // No such process".
              kill(request.pid, SIGSTOP);
              // don't dump sibling threads when attaching to GDB because it
              // makes the process less reliable, apparently...
              tombstone_path = engrave_tombstone(request.pid, request.tid,
                                                 signal, request.original_si_code,
                                                 request.abort_msg_address, !attach_gdb,
                                                 &detach_failed, &total_sleep_time_usec);
              break;

            default:
              ALOGE("process stopped due to unexpected signal %d\n", signal);
              break;
          }
          break;
        }

        if (request.action == DEBUGGER_ACTION_DUMP_TOMBSTONE) {
          if (tombstone_path) {
            write(fd, tombstone_path, strlen(tombstone_path));
          }
          close(fd);
          fd = -1;
        }
        free(tombstone_path);
      }

      if (!tid_unresponsive) {
        ALOGV("detaching");
        if (attach_gdb) {
          // stop the process so we can debug
          kill(request.pid, SIGSTOP);
        }
        if (ptrace(PTRACE_DETACH, request.tid, 0, 0)) {
          ALOGE("ptrace detach from %d failed: %s", request.tid, strerror(errno));
          detach_failed = true;
        } else if (attach_gdb) {
          // if debug.db.uid is set, its value indicates if we should wait
          // for user action for the crashing process.
          // in this case, we log a message and turn the debug LED on
          // waiting for a gdb connection (for instance)
          wait_for_user_action(request);
        }
      }

      // resume stopped process (so it can crash in peace).
      kill(request.pid, SIGCONT);

      // If we didn't successfully detach, we're still the parent, and the
      // actual parent won't receive a death notification via wait(2).  At this point
      // there's not much we can do about that.
      if (detach_failed) {
        ALOGE("debuggerd committing suicide to free the zombie!\n");
        kill(getpid(), SIGKILL);
      }
    }

  }
  if (fd >= 0) {
    close(fd);
  }
}

static int do_server() {
  // debuggerd crashes can't be reported to debuggerd.
  // Reset all of the crash handlers.
  signal(SIGABRT, SIG_DFL);
  signal(SIGBUS, SIG_DFL);
  signal(SIGFPE, SIG_DFL);
  signal(SIGILL, SIG_DFL);
  signal(SIGSEGV, SIG_DFL);
#ifdef SIGSTKFLT
  signal(SIGSTKFLT, SIG_DFL);
#endif
  signal(SIGTRAP, SIG_DFL);

  // Ignore failed writes to closed sockets
  signal(SIGPIPE, SIG_IGN);

  int logsocket = socket_local_client("logd", ANDROID_SOCKET_NAMESPACE_ABSTRACT, SOCK_DGRAM);
  if (logsocket < 0) {
    logsocket = -1;
  } else {
    fcntl(logsocket, F_SETFD, FD_CLOEXEC);
  }

  struct sigaction act;
  act.sa_handler = SIG_DFL;
  sigemptyset(&act.sa_mask);
  sigaddset(&act.sa_mask,SIGCHLD);
  act.sa_flags = SA_NOCLDWAIT;
  sigaction(SIGCHLD, &act, 0);

  int s = socket_local_server(DEBUGGER_SOCKET_NAME, ANDROID_SOCKET_NAMESPACE_ABSTRACT, SOCK_STREAM);
  if (s < 0)
    return 1;
  fcntl(s, F_SETFD, FD_CLOEXEC);

  ALOGI("debuggerd: " __DATE__ " " __TIME__ "\n");

  for (;;) {
    sockaddr addr;
    socklen_t alen = sizeof(addr);

    ALOGV("waiting for connection\n");
    int fd = accept(s, &addr, &alen);
    if (fd < 0) {
      ALOGV("accept failed: %s\n", strerror(errno));
      continue;
    }

    fcntl(fd, F_SETFD, FD_CLOEXEC);

    handle_request(fd);
  }
  return 0;
}

static int do_explicit_dump(pid_t tid, bool dump_backtrace) {
  fprintf(stdout, "Sending request to dump task %d.\n", tid);

  if (dump_backtrace) {
    fflush(stdout);
    if (dump_backtrace_to_file(tid, fileno(stdout)) < 0) {
      fputs("Error dumping backtrace.\n", stderr);
      return 1;
    }
  } else {
    char tombstone_path[PATH_MAX];
    if (dump_tombstone(tid, tombstone_path, sizeof(tombstone_path)) < 0) {
      fputs("Error dumping tombstone.\n", stderr);
      return 1;
    }
    fprintf(stderr, "Tombstone written to: %s\n", tombstone_path);
  }
  return 0;
}

static void usage() {
  fputs("Usage: -b [<tid>]\n"
        "  -b dump backtrace to console, otherwise dump full tombstone file\n"
        "\n"
        "If tid specified, sends a request to debuggerd to dump that task.\n"
        "Otherwise, starts the debuggerd server.\n", stderr);
}

int main(int argc, char** argv) {
  if (argc == 1) {
    return do_server();
  }

  bool dump_backtrace = false;
  bool have_tid = false;
  pid_t tid = 0;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "-b")) {
      dump_backtrace = true;
    } else if (!have_tid) {
      tid = atoi(argv[i]);
      have_tid = true;
    } else {
      usage();
      return 1;
    }
  }
  if (!have_tid) {
    usage();
    return 1;
  }
  return do_explicit_dump(tid, dump_backtrace);
}
