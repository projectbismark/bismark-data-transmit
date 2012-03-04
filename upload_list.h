#ifndef _BISMARK_DATA_TRANSMIT_UPLOADS_LIST_H_
#define _BISMARK_DATA_TRANSMIT_UPLOADS_LIST_H_

#include <limits.h>
#include <stddef.h>
#include <time.h>

typedef struct {
  char filename[PATH_MAX + 1];
  time_t last_modified;
  size_t size;
  int index;
} upload_entry_t;

typedef struct {
  int capacity;
  int length;
  upload_entry_t* entries;
} upload_list_t;

int upload_list_init(upload_list_t* list);
void upload_list_destroy(upload_list_t* list);

int upload_list_append(upload_list_t* list,
                       const char* filename,
                       const time_t last_mofidied,
                       const size_t size,
                       const int index);
void upload_list_sort(upload_list_t* list);

#endif
