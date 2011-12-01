#include <stdio.h>
#include <string.h>
#include <sys/inotify.h>
#include <unistd.h>

#include <curl/curl.h>

#define ARRAYSIZE(a)  (sizeof(a)/sizeof(a[0]))

#ifndef UPLOADS_URL
#define UPLOADS_URL  "http://127.0.0.1:8000/upload/"
#endif
#define MAX_URL_LENGTH  2000
#define BUF_LEN  (sizeof(struct inotify_event) * 10)

/* The list of directories to monitor for files to upload. They must exist
 * before the program starts. IMPORTANT: These must end with a slash! */
static const char* upload_directories[] = {
  "/tmp/bismark-uploads/passive/",
  "/tmp/bismark-uploads/passive-frequent/",
};

/* This gets filled in with the watch descriptors corresponding
 * to the directories in upload_directories. */
static int watch_descriptors[ARRAYSIZE(upload_directories)];

static const char curl_error_message[CURL_ERROR_SIZE];

int curl_send(CURL* curl, char* filename) {
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
  if (encoded_filename == NULL) {
    fprintf(stderr, "Failed to encode URL: %s\n", curl_error_message);
    return -1;
  }
  char url[MAX_URL_LENGTH];
  snprintf(url, sizeof(url), "%s?filename=%s", UPLOADS_URL, filename);
  curl_free(encoded_filename);

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

int main(int argc, char** argv) {
  /* Initialize cURL */
  CURL* curl = curl_easy_init();
  if (!curl) {
    fprintf(stderr, "Error initializing cURL\n");
    return 1;
  }
  int rc = curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, curl_error_message);
  if (rc) {
    fprintf(stderr, "Error initializing cURL: %s\n", curl_easy_strerror(rc));
    return rc;
  }
  if (curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L)) {
    fprintf(stderr, "Error intializing cURL: %s\n", curl_error_message);
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
            if (curl_send(curl, filename) == 0) {
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
