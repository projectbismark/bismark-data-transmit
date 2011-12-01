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

#ifndef UPLOADS_ROOT
#define UPLOADS_ROOT "/tmp/bismark-uploads"
#endif
#ifndef RETRY_INTERVAL_MINUTES
#define RETRY_INTERVAL_MINUTES 30
#endif
#define RETRY_INTERVAL_SECONDS (RETRY_INTERVAL_MINUTES * 60)
#ifndef UPLOADS_URL
#define UPLOADS_URL  "http://127.0.0.1:8000/upload/"
#endif
#ifndef BUILD_ID
#define BUILD_ID  "git"
#endif
#define MAX_URL_LENGTH  2000
#define BUF_LEN  (sizeof(struct inotify_event) * 10)

/* List of directories to monitor for files to upload, as subdirectories of
 * UPLOADS_ROOT. They must exist before the program starts and must NOT end with
 * a slash. */
static const char** upload_subdirectories = NULL;
static int num_upload_subdirectories = 0;

/* This gets filled in with the full path names of the upload
 * directories we monitor, which are specified in upload_subdirectories. */
static const char** upload_directories;

/* This gets filled in with the watch descriptors corresponding
 * to the directories in upload_directories. */
static int* watch_descriptors;

static const char curl_error_message[CURL_ERROR_SIZE];

static pthread_t retry_thread;

static int initialize_upload_directories() {
  DIR* handle = opendir(UPLOADS_ROOT);
  if (handle == NULL) {
    perror("opendir " UPLOADS_ROOT);
    return -1;
  }
  struct dirent* entry;
  while ((entry = readdir(handle))) {
    if (entry->d_name[0] == '.') {
      continue;
    }
    char filename[PATH_MAX];
    snprintf(filename,
             sizeof(filename),
             "%s/%s",
             UPLOADS_ROOT,
             entry->d_name);
    struct stat dir_info;
    if (stat(filename, &dir_info)) {
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

  upload_directories = calloc(num_upload_subdirectories,
                              sizeof(upload_directories[0]));
  if (upload_directories == NULL) {
    perror("calloc");
    return -1;
  }
  int idx;
  for (idx = 0; idx < num_upload_subdirectories; ++idx) {
    char filename[PATH_MAX];
    snprintf(filename,
             sizeof(filename),
             "%s/%s/",
             UPLOADS_ROOT,
             upload_subdirectories[idx]);
    upload_directories[idx] = strdup(filename);
    if (upload_directories[idx] == NULL) {
      perror("strdup for upload_directories");
      return -1;
    }
  }
  return 0;
}

static int curl_send(CURL* curl, const char* filename, const char* module) {
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
  char* encoded_buildid = curl_easy_escape(curl, BUILD_ID, 0);
  char* encoded_module = curl_easy_escape(curl, module, 0);
  if (!encoded_filename || !encoded_buildid || !encoded_module) {
    fprintf(stderr, "Failed to encode URL: %s\n", curl_error_message);
    return -1;
  }
  char url[MAX_URL_LENGTH];
  snprintf(url,
           sizeof(url),
           "%s?filename=%s&buildid=%s&module=%s",
           UPLOADS_URL,
           encoded_filename,
           encoded_buildid,
           encoded_module);
  curl_free(encoded_filename);
  curl_free(encoded_buildid);
  curl_free(encoded_module);

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
  if (curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L)
      || curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1)) {
    fprintf(stderr, "Error intializing cURL: %s\n", curl_error_message);
    curl_easy_cleanup(curl);
    return NULL;
  }
  return curl;
}

static void* retry_uploads(void* arg) {
  CURL* curl = initialize_curl();
  if (curl == NULL) {
    exit(1);
  }

  while (1) {
    time_t current_time = time(NULL);
    int idx;
    for (idx = 0; idx < num_upload_subdirectories; ++idx) {
      DIR* handle = opendir(upload_directories[idx]);
      if (handle != NULL) {
        struct dirent* entry;
        while ((entry = readdir(handle))) {
          char filename[PATH_MAX];
          snprintf(filename,
                   sizeof(filename),
                   "%s%s",
                   upload_directories[idx],
                   entry->d_name);
          struct stat file_info;
          if (stat(filename, &file_info)) {
            perror("stat from retry thread");
            continue;
          }
          if ((S_ISREG(file_info.st_mode) || S_ISLNK(file_info.st_mode))
              && current_time - file_info.st_ctime > RETRY_INTERVAL_SECONDS) {
            printf("Retrying file %s\n", filename);
            if (curl_send(curl, filename, upload_subdirectories[idx]) == 0) {
              if (unlink(filename)) {
                perror("unlink from retry thread");
                fprintf(stderr, "Uploaded file not garbage collected\n");
              }
            } else {
              if (utime(filename, NULL) < 0) {
                perror("utime from retry thread");
              }
            }
          }
        }
        (void)closedir(handle);
      } else {
        perror("opendir from retry thread");
      }
    }
    sleep (RETRY_INTERVAL_SECONDS);
  }
}

int main(int argc, char** argv) {
  if (initialize_upload_directories()) {
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
      break;
    }

    int offset = 0;
    while (offset < length) {
      struct inotify_event* event \
        = (struct inotify_event*)(events_buffer + offset);
      if (event->len && (event->mask & IN_MOVED_TO)) {
        int idx;
        for (idx = 0; idx < num_upload_subdirectories; ++idx) {
          if (event->wd == watch_descriptors[idx]) {
            char filename[PATH_MAX];
            strncpy(filename, upload_directories[idx], sizeof(filename));
            strncat(filename, event->name, sizeof(filename));
            printf("File move detected: %s\n", filename);
            if (curl_send(curl, filename, upload_subdirectories[idx]) == 0) {
              if (unlink(filename)) {
                perror("unlink");
                fprintf(stderr, "Uploaded file not garbage collected\n");
              }
            }
            break;
          }
        }
      }
      offset += sizeof(*event) + event->len;
    }
  }

  curl_easy_cleanup(curl);

  return 0;
}
