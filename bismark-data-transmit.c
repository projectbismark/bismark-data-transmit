#include <dirent.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <utime.h>

#include <curl/curl.h>

#ifndef BISMARK_ID_FILENAME
#define BISMARK_ID_FILENAME  "/etc/bismark/ID"
#endif
#define BISMARK_ID_LEN  14
#ifndef UPLOADS_ROOT
#define UPLOADS_ROOT  "/tmp/bismark-uploads"
#endif
#ifndef RETRY_INTERVAL_MINUTES
#define RETRY_INTERVAL_MINUTES  30
#endif
#define RETRY_INTERVAL_SECONDS  (RETRY_INTERVAL_MINUTES * 60)
#ifndef UPLOADS_URL
#define UPLOADS_URL  "https://projectbismark.net:8001/upload/"
#endif
#ifndef BUILD_ID
#define BUILD_ID  "git"
#endif
#define MAX_URL_LENGTH  2000
#define BUF_LEN  (sizeof(struct inotify_event) * 10)

/* Will be filled in with this node's Bismark ID. */
static char bismark_id[BISMARK_ID_LEN];

/* A dynamically allocated list of directories to monitor for files to upload.
 * These are directory names relative to UPLOADS_ROOT. */
static const char** upload_subdirectories = NULL;
static int num_upload_subdirectories = 0;

/* This gets populated with the absolute paths of the upload directories we
 * monitor, whose relative paths are specified in upload_subdirectories. */
static const char** upload_directories;

/* This gets populated with the inotify watch descriptors corresponding
 * to the directories in upload_directories. */
static int* watch_descriptors;

/* cURL's error buffer. Any time cURL has an error, it writes it here. */
static const char curl_error_message[CURL_ERROR_SIZE];

/* This thread runs the function "retry_uploads" */
static pthread_t retry_thread;

/* Concatenate two paths. They will be separated with a '/'. result must be at
 * least PATH_MAX bytes long. Return 0 if successful and -1 otherwise. */
static int join_paths(const char* first, const char* second, char* result) {
  if (snprintf(result, PATH_MAX, "%s/%s", first, second) < 0) {
    perror("snprintf");
    return -1;
  } else {
    return 0;
  }
}

/* Build the upload_subdirectories array
 * by scanning UPLOADS_ROOT for subdirectories. */
static int initialize_upload_subdirectories() {
  DIR* handle = opendir(UPLOADS_ROOT);
  if (handle == NULL) {
    perror("opendir " UPLOADS_ROOT);
    return -1;
  }
  struct dirent* entry;
  while ((entry = readdir(handle))) {
    if (entry->d_name[0] == '.') {  /* Skip hidden, ".", and ".." */
      continue;
    }
    char absolute_filename[PATH_MAX];
    if (join_paths(UPLOADS_ROOT, entry->d_name, absolute_filename)) {
      return -1;
    }
    struct stat dir_info;
    if (stat(absolute_filename, &dir_info)) {
      perror("stat");
      return -1;
    }
    if (S_ISDIR(dir_info.st_mode)) {
      ++num_upload_subdirectories;
      upload_subdirectories = realloc(
          upload_subdirectories,
          num_upload_subdirectories * sizeof(upload_subdirectories[0]));
      if (upload_subdirectories == NULL) {
        perror("realloc");
        return -1;
      }
      upload_subdirectories[num_upload_subdirectories - 1]
          = strdup(entry->d_name);
    }
  }
  (void)closedir(handle);
  return 0;
}

/* Build the upload_directories array by converting upload_subdirectories to
 * absolute path names. The generated paths don't have a trailing slash. */
static int initialize_upload_directories() {
  upload_directories = calloc(num_upload_subdirectories,
                              sizeof(upload_directories[0]));
  if (upload_directories == NULL) {
    perror("calloc");
    return -1;
  }
  int idx;
  for (idx = 0; idx < num_upload_subdirectories; ++idx) {
    char absolute_path[PATH_MAX];
    if (join_paths(UPLOADS_ROOT, upload_subdirectories[idx], absolute_path)) {
      return -1;
    }
    upload_directories[idx] = strdup(absolute_path);
    if (upload_directories[idx] == NULL) {
      perror("strdup for upload_directories");
      return -1;
    }
  }
  return 0;
}

/* Send a file to the server using cURL. */
static int curl_send(CURL* curl, const char* filename, const char* directory) {
  /* Open the file we're going to upload and determine its
   * size. (cURL needs to the know the size.) */
  FILE* handle = fopen(filename, "rb");
  if (!handle) {
    perror("fopen");
    return -1;
  }
  fseek(handle, 0L, SEEK_END);
  long file_size = ftell(handle);
  rewind(handle);

  /* Build the URL. */
  char* encoded_filename = curl_easy_escape(curl, filename, 0);
  char* encoded_nodeid = curl_easy_escape(curl, bismark_id, 0);
  char* encoded_buildid = curl_easy_escape(curl, BUILD_ID, 0);
  char* encoded_directory = curl_easy_escape(curl, directory, 0);
  if (encoded_filename == NULL
      || encoded_nodeid == NULL
      || encoded_buildid == NULL
      || encoded_directory == NULL) {
    fprintf(stderr, "Failed to encode URL: %s\n", curl_error_message);
    return -1;
  }
  char url[MAX_URL_LENGTH];
  snprintf(url,
           sizeof(url),
           "%s?filename=%s&node_id=%s&build_id=%s&directory=%s",
           UPLOADS_URL,
           encoded_filename,
           encoded_nodeid,
           encoded_buildid,
           encoded_directory);
  curl_free(encoded_filename);
  curl_free(encoded_nodeid);
  curl_free(encoded_buildid);
  curl_free(encoded_directory);

  /* Set up and execute the transfer. */
  if (curl_easy_setopt(curl, CURLOPT_URL, url)
      || curl_easy_setopt(curl, CURLOPT_READDATA, handle)
      || curl_easy_setopt(curl, CURLOPT_INFILESIZE, file_size)) {
    fprintf(stderr, "Failed to set cURL options: %s\n", curl_error_message);
    return -1;
  }
  if (curl_easy_perform(curl)) {
    fprintf(stderr, "Failed to upload: %s\n", curl_error_message);
    fclose(handle);
    return -1;
  }
  fclose(handle);
  return 0;
}

static CURL* initialize_curl() {
  CURL* curl = curl_easy_init();
  if (!curl) {
    fprintf(stderr, "Error initializing cURL\n");
    return NULL;
  }
  int rc = curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, curl_error_message);
  if (rc) {
    fprintf(stderr, "Error initializing cURL: %s\n", curl_easy_strerror(rc));
    curl_easy_cleanup(curl);
    return NULL;
  }
  /* CURLOPT_UPLOAD uses HTTP PUT by default. CURLOPT_FAILONERROR causes cURL to
   * return an error if the Web server returns an HTTP error code. */
  if (curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L)
      || curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1)) {
    fprintf(stderr, "Error intializing cURL: %s\n", curl_error_message);
    curl_easy_cleanup(curl);
    return NULL;
  }
#ifdef SKIP_SSL_VERIFICATION
  if (curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0)) {
    fprintf(stderr, "Error setting SSL options: %s\n", curl_error_message);
    curl_easy_cleanup(curl);
    return NULL;
  }
#endif
  return curl;
}

/* This function runs in a separate thread. It periodically scans the
 * directories we're monitoring for stray files and tries to upload them again.
 * A file is considered stray if its "change time" (i.e., ctime) is older than
 * RETRY_INTERVAL_SECONDS. A file's ctime is updated any time it's modified or
 * moved, so it is a lower bound on the time since the file was moved into the
 * uploads directory. */
static void* retry_uploads(void* arg) {
  while (1) {
    time_t current_time = time(NULL);
    CURL* curl = initialize_curl();
    if (curl == NULL) {
      exit(1);
    }
    int idx;
    for (idx = 0; idx < num_upload_subdirectories; ++idx) {
      DIR* handle = opendir(upload_directories[idx]);
      if (handle == NULL) {
        perror("opendir from retry thread");
        continue;
      }
      struct dirent* entry;
      while ((entry = readdir(handle))) {
        char absolute_path[PATH_MAX];
        if (join_paths(upload_directories[idx], entry->d_name, absolute_path)) {
          continue;
        }
        struct stat file_info;
        if (stat(absolute_path, &file_info)) {
          perror("stat from retry thread");
          continue;
        }
        if ((S_ISREG(file_info.st_mode) || S_ISLNK(file_info.st_mode))
            && current_time - file_info.st_ctime > RETRY_INTERVAL_SECONDS) {
          printf("Retrying file %s\n", absolute_path);
          if (curl_send(curl, absolute_path, upload_subdirectories[idx]) == 0) {
            if (unlink(absolute_path)) {
              perror("unlink from retry thread");
              fprintf(stderr, "Uploaded file not garbage collected\n");
            }
          }
        }
      }
      if (closedir(handle)) {
        perror("closedir from retry thread");
      }
    }
    curl_easy_cleanup(curl);
    sleep(RETRY_INTERVAL_SECONDS);
  }
}

int read_bismark_id() {
  FILE* handle = fopen(BISMARK_ID_FILENAME, "r");
  if (handle == NULL) {
    perror("fopen");
    return -1;
  }
  if (fread(bismark_id, 1, BISMARK_ID_LEN, handle) != BISMARK_ID_LEN) {
    perror("fread");
    return -1;
  }
  return 0;
}

int main(int argc, char** argv) {
  if (read_bismark_id()) {
    return 1;
  }

  if (initialize_upload_subdirectories() || initialize_upload_directories()) {
    return 1;
  }

  if (pthread_create(&retry_thread, NULL, retry_uploads, NULL)) {
    perror("pthread_create");
    return 1;
  }

  CURL* curl = initialize_curl();
  if (!curl) {
    return 1;
  }

  /* Initialize inotify */
  int inotify_handle = inotify_init();
  if (inotify_handle < 0) {
    perror("inotify_init");
    return 1;
  }
  int idx;
  watch_descriptors = calloc(num_upload_subdirectories,
                             sizeof(watch_descriptors[0]));
  if (watch_descriptors == NULL) {
    perror("calloc");
    return 1;
  }
  for (idx = 0; idx < num_upload_subdirectories; ++idx) {
    watch_descriptors[idx] = inotify_add_watch(inotify_handle,
                                               upload_directories[idx],
                                               IN_MOVED_TO);
    if (watch_descriptors[idx] < 0) {
      perror("inotify_add_watch");
      return 1;
    }
  }

  while (1) {
    char events_buffer[BUF_LEN];
    int length = read(inotify_handle, events_buffer, BUF_LEN);
    if (length < 0) {
      perror("read");
      curl_easy_cleanup(curl);
      return 1;
    }
    int offset = 0;
    while (offset < length) {
      struct inotify_event* event \
        = (struct inotify_event*)(events_buffer + offset);
      if (event->len && (event->mask & IN_MOVED_TO)) {
        int idx;
        for (idx = 0; idx < num_upload_subdirectories; ++idx) {
          if (event->wd == watch_descriptors[idx]) {
            char absolute_path[PATH_MAX];
            if (join_paths(upload_directories[idx], event->name, absolute_path)) {
              break;
            }
            printf("File move detected: %s\n", absolute_path);
            if (!curl_send(curl, absolute_path, upload_subdirectories[idx])) {
              if (unlink(absolute_path)) {
                perror("unlink failed; uploaded file not garbage collected");
              }
            }
            break;
          }
        }
      }
      offset += sizeof(*event) + event->len;
    }
  }
  return 0;
}
