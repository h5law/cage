CC := cc

CFLAGS := -D_GNU_SOURCE -std=c11 -O2 -Wall -Wextra -Wpedantic
CPPFLAGS := -Iinclude

TARGET := cage

SOURCES := \
	src/cage.c \
	src/container.c

OBJECTS := $(SOURCES:.c=.o)

$(TARGET): $(OBJECTS)
	$(CC) $(CFLAGS) $(OBJECTS) -o $@

%.o: %.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

clean:
	rm -f $(OBJECTS) $(TARGET)

.PHONY: clean
