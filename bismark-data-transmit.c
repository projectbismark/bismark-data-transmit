#include <stdio.h>
#include <string.h>
#include <sys/inotify.h>
#include <unistd.h>

#include <curl/curl.h>

#define ARRAYSIZE(a)  (sizeof(a)/sizeof(a[0]))

#define UPLOADS_ROOT "/tmp/bismark-uploads"
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
static const char* upload_subdirectories[] = {
  "passive",
  "passive-frequent",
};

/* This gets filled in with the full path names of the upload
 * directories we monitor, which are specified in upload_subdirectories. */
static const char* upload_directories[ARRAYSIZE(upload_subdirectories)];

/* This gets filled in with the watch descriptors corresponding
 * to the directories in upload_directories. */
static int watch_descriptors[ARRAYSIZE(upload_subdirectories)];

static const char curl_error_message[CURL_ERROR_SIZE];

static int initialize_upload_directories() {
  int idx;
  for (idx = 0; idx < ARRAYSIZE(upload_subdirectories); ++idx) {
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

int main(int argc, char** argv) {
  initialize_upload_directories();

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
  for (idx = 0; idx < ARRAYSIZE(upload_directories); ++idx) {
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
        for (idx = 0; idx < ARRAYSIZE(upload_directories); ++idx) {
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
