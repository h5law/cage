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
TEST_CONFIG := tests/test_config
TEST_ESCAPE_RUNNER := tests/escape/escape_runner
TEST_ESCAPE_PROBE := tests/escape/escape_probe

$(TEST_ESCAPE_PROBE): tests/escape/escape_probe.c
	$(CC) $(CFLAGS) $< -static -o $@

$(TEST_ESCAPE_RUNNER): tests/escape/escape_runner.c \
                       tests/escape/escape_utils.c \
                       tests/escape/escape_utils.h
	$(CC) $(CFLAGS) $(CPPFLAGS) \
		tests/escape/escape_runner.c \
		tests/escape/escape_utils.c \
		-o $@

test-escape: $(TARGET) $(TEST_ESCAPE_RUNNER) $(TEST_ESCAPE_PROBE)
	./$(TEST_ESCAPE_RUNNER) ./$(TARGET) ./$(TEST_ESCAPE_PROBE)

$(TARGET): $(OBJECTS)
	$(CC) $(CFLAGS) $(OBJECTS) -o $@

%.o: %.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

$(TEST_PROBE): tests/container_probe.c
	$(CC) $(CFLAGS) $< -static -o $@

$(TEST_RUNNER): tests/test_runner.c tests/utils.c tests/utils.h
	$(CC) $(CFLAGS) $(CPPFLAGS) tests/test_runner.c tests/utils.c -o $@

$(TEST_CONFIG): tests/test_config.c src/config.c tests/utils.c tests/utils.h
	$(CC) $(CFLAGS) $(CPPFLAGS) tests/test_config.c src/config.c tests/utils.c -o $@

test-config: $(TEST_CONFIG)
	./$(TEST_CONFIG)

test: $(TARGET) $(TEST_RUNNER) $(TEST_PROBE) $(TEST_CONFIG)
	./$(TEST_CONFIG)
	./$(TEST_RUNNER) ./$(TARGET) ./$(TEST_PROBE)

clean:
	rm -f $(OBJECTS) \
		$(TARGET) \
		$(TEST_RUNNER) \
		$(TEST_PROBE) \
		$(TEST_CONFIG) \
		$(TEST_ESCAPE_RUNNER) \
		$(TEST_ESCAPE_PROBE)

.PHONY: clean test test-config
