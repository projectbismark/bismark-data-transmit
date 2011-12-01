CC = gcc
CFLAGS += -c -Wall -O3 -fno-strict-aliasing
ifdef BUILD_ID
CFLAGS += -DBUILD_ID="\"$(BUILD_ID)\""
endif
ifdef UPLOADS_URL
CFLAGS += -DUPLOADS_URL="\"$(UPLOADS_URL)\""
endif
LDFLAGS += -lcurl
SRCS = \
	bismark-data-transmit.c
OBJS = $(SRCS:.c=.o)
EXE = bismark-data-transmit

.c.o:
	$(CC) $(CFLAGS) $< -o $@

$(EXE): $(OBJS)
	$(CC) $(LDFLAGS) $(OBJS) -o $@

clean:
	rm -rf $(OBJS) $(EXE)
