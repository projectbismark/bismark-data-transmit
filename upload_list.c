#include "upload_list.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>

int upload_list_init(upload_list_t* list) {
  list->capacity = 16;
  list->length = 0;
  list->entries = malloc(list->capacity * sizeof(list->entries[0]));
  if (list->entries == NULL) {
    syslog(LOG_ERR, "uploads_list_init:malloc: %s", strerror(errno));
    return -1;
  }
  return 0;
}

void upload_list_destroy(upload_list_t* list) {
  free(list->entries);
}

int upload_list_append(upload_list_t* list,
                       const char* filename,
                       const time_t last_modified,
                       const size_t size) {
  if (list->entries == NULL) {
    return -1;
  }
  if (list->length >= list->capacity) {
    list->capacity *= 2;
    void* new_entries = realloc(list->entries,
                                list->capacity * sizeof(list->entries[0]));
    if (new_entries == NULL) {
      syslog(LOG_ERR, "uploads_list_append:realloc: %s", strerror(errno));
      return -1;
    } else {
      list->entries = new_entries;
    }
  }

  ++list->length;
  upload_entry_t* entry = &list->entries[list->length - 1];
  strncpy(entry->filename, filename, PATH_MAX);
  entry->last_modified = last_modified;
  entry->size = size;
  return 0;
}

static int compare_uploads(const void* first, const void* second) {
  const upload_entry_t* first_entry = first;
  const upload_entry_t* second_entry = second;
  if (first_entry->last_modified < second_entry->last_modified) {
    return 1;
  } else if (first_entry->last_modified > second_entry->last_modified) {
    return -1;
  } else {
    return 0;
  }
}

void upload_list_sort(upload_list_t* list) {
  qsort(list->entries, list->length, sizeof(list->entries[0]), compare_uploads);
}
