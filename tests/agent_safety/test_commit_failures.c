/* Host fault injection against the unmodified production app_history_store.c. */
#define _DARWIN_C_SOURCE
#include "app_history_store.h"
#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum fault { NONE, PARTIAL_ENOSPC, SYNC_EIO, READBACK_EIO };
static enum fault injected;
static unsigned faults;

int evidence_fprintf(FILE *stream, const char *format, ...) {
  if (injected == PARTIAL_ENOSPC) {
    injected = NONE;
    ++faults;
    assert(fwrite("{torn", 1, 5, stream) == 5);
    errno = ENOSPC;
    return -1;
  }
  va_list args;
  va_start(args, format);
  int result = vfprintf(stream, format, args);
  va_end(args);
  return result;
}

int evidence_fsync(int fd) {
  if (injected == SYNC_EIO) {
    injected = NONE;
    ++faults;
    errno = EIO;
    return -1;
  }
  return fsync(fd);
}

FILE *evidence_fopen(const char *path, const char *mode) {
  if (injected == READBACK_EIO && !strcmp(mode, "rb")) {
    injected = NONE;
    ++faults;
    errno = EIO;
    return NULL;
  }
  return fopen(path, mode);
}

static void expect_summary(const char *dir, app_conversation_t *restored,
                           const char *expected) {
  assert(app_history_restore(dir, restored) == 1);
  assert(!strcmp(restored->summary, expected));
}

int main(int argc, char **argv) {
  assert(argc == 2);
  const char *dir = argv[1];
  app_conversation_t *history = calloc(1, sizeof(*history));
  app_conversation_t *restored = calloc(1, sizeof(*restored));
  assert(history && restored);
  assert(app_history_restore(dir, restored) == 0);
  strcpy(history->summary, "synthetic baseline");
  app_commit_result_t committed = app_history_commit(dir, history);
  assert(committed.status == APP_COMMIT_COMMITTED && committed.revision == 1);
  expect_summary(dir, restored, "synthetic baseline");
  puts("PASS real storage: successful snapshot restores");

  strcpy(history->summary, "synthetic torn write");
  injected = PARTIAL_ENOSPC;
  assert(app_history_commit(dir, history).status == APP_COMMIT_UNCERTAIN);
  assert(app_history_commit(dir, history).status == APP_COMMIT_UNCERTAIN);
  expect_summary(dir, restored, "synthetic baseline");
  puts("PASS real storage: partial ENOSPC preserves previous complete slot");

  strcpy(history->summary, "synthetic sync uncertainty");
  injected = SYNC_EIO;
  assert(app_history_commit(dir, history).status == APP_COMMIT_UNCERTAIN);
  assert(app_history_commit(dir, history).status == APP_COMMIT_UNCERTAIN);
  cJSON *observed = app_history_task_receipt(dir, NULL); assert(observed);
  assert(app_history_commit(dir, history).status == APP_COMMIT_UNCERTAIN);
  cJSON *different = cJSON_Duplicate(observed, true); assert(different);
  assert(cJSON_AddItemToArray(different, cJSON_CreateString("different read")));
  assert(!app_history_task_receipt(dir, different));
  assert(app_history_commit(dir, history).status == APP_COMMIT_UNCERTAIN);
  cJSON_Delete(different);
  cJSON *verified = app_history_task_receipt(dir, observed); assert(verified);
  cJSON_Delete(verified); cJSON_Delete(observed);
  app_commit_result_t reconciled = app_history_commit(dir, history);
  assert(reconciled.status == APP_COMMIT_COMMITTED && reconciled.revision == 3);
  puts("PASS read-only and mismatched reconciliation retain fence; matching read restores generation then permits next revision");
  expect_summary(dir, restored, "synthetic sync uncertainty");
  puts("PASS fsync EIO is explicitly uncertain; readback cannot prove power-loss durability");

  strcpy(history->summary, "synthetic readback uncertainty");
  injected = READBACK_EIO;
  assert(app_history_commit(dir, history).status == APP_COMMIT_UNCERTAIN);
  assert(app_history_commit(dir, history).status == APP_COMMIT_UNCERTAIN);
  expect_summary(dir, restored, "synthetic readback uncertainty");
  puts("PASS readback EIO is explicitly uncertain and restored revision reflects the written snapshot");
  assert(faults == 3 && injected == NONE);
  char invalid[1024]; memset(invalid, 'a', sizeof(invalid)); invalid[sizeof(invalid)-1] = 0;
  assert(app_history_commit(invalid, history).status == APP_COMMIT_PERMANENT_FAILURE);
  puts("PASS uncertain commits fence subsequent writes until explicit restore; invalid path is permanent");
  free(history);
  free(restored);
  return 0;
}
