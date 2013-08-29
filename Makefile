CC = gcc
CFLAGS += -c -Wall -O3 -fno-strict-aliasing
ifdef BISMARK_ID_FILENAME
CFLAGS += -DBISMARK_ID_FILENAME="\"$(BISMARK_ID_FILENAME)\""
endif
ifdef BUILD_ID
CFLAGS += -DBUILD_ID="\"$(BUILD_ID)\""
endif
ifdef DEFAULT_UPLOADS_URL
CFLAGS += -DDEFAULT_UPLOADS_URL="\"$(DEFAULT_UPLOADS_URL)\""
endif
ifdef UPLOADS_ROOT
CFLAGS += -DUPLOADS_ROOT="\"$(UPLOADS_ROOT)\""
endif
ifdef RETRY_INTERVAL_MINUTES
CFLAGS += -DRETRY_INTERVAL_MINUTES="$(RETRY_INTERVAL_MINUTES)"
endif
ifdef MAX_UPLOADS_BYTES
CFLAGS += -DMAX_UPLOADS_BYTES="$(MAX_UPLOADS_BYTES)"
endif
ifdef SKIP_SSL_VERIFICATION
CFLAGS += -DSKIP_SSL_VERIFICATION="yes"
endif
ifdef DEBUG_MESSAGES
CFLAGS += -DDEBUG_MESSAGES="yes"
endif
LDFLAGS += -lcurl -lz -lssl -lcrypto
SRCS = \
	bismark-data-transmit.c \
	upload_list.c
OBJS = $(SRCS:.c=.o)
EXE = bismark-data-transmit

.c.o:
	$(CC) $(CFLAGS) $< -o $@

$(EXE): $(OBJS)
	$(CC) $(LDFLAGS) $(OBJS) -o $@

clean:
	rm -rf $(OBJS) $(EXE)
