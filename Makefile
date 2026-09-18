CC := cc

CFLAGS := -D_GNU_SOURCE -std=c11 -O2 -Wall -Wextra -Wpedantic
CPPFLAGS := -Iinclude

TARGET := cage

SOURCES := \
	src/cage.c \
	src/container.c \
	src/config.c

OBJECTS := $(SOURCES:.c=.o)

TEST_RUNNER := tests/test_runner
TEST_PROBE := tests/container_probe

$(TARGET): $(OBJECTS)
	$(CC) $(CFLAGS) $(OBJECTS) -o $@

%.o: %.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

$(TEST_PROBE): tests/container_probe.c
	$(CC) $(CFLAGS) $< -static -o $@

$(TEST_RUNNER): tests/test_runner.c
	$(CC) $(CFLAGS) $< -o $@

test: $(TARGET) $(TEST_RUNNER) $(TEST_PROBE)
	$(TEST_RUNNER) ./$(TARGET) ./$(TEST_PROBE)

clean:
	rm -f $(OBJECTS) $(TARGET) $(TEST_RUNNER) $(TEST_PROBE)

.PHONY: clean test
