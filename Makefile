CC := cc

CFLAGS := -D_GNU_SOURCE -std=c11 -O2 -Wall -Wextra -Wpedantic
CPPFLAGS := -Iinclude

TARGET := cage

SOURCES := \
	src/cage.c \
	src/container.c \
	src/config.c

OBJECTS := $(SOURCES:.c=.o)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

TEST_RUNNER := tests/test_runner
TEST_PROBE := tests/container_probe
TEST_CONFIG := tests/test_config

TEST_CONFIG_PROPERTY := tests/property/test_config_property
TEST_CONFIG_INVALID_PROPERTY := tests/property/test_config_invalid_property
TEST_CONFIG_FORMAT_PROPERTY := tests/property/test_config_format_property
TEST_CONFIG_HASH_PROPERTY := tests/property/test_config_hash_property
TEST_CONFIG_WHITESPACE_PROPERTY := tests/property/test_config_whitespace_property
TEST_ESCAPE_PROPERTY := tests/property/test_escape_property

TEST_ESCAPE_RUNNER := tests/escape/escape_runner
TEST_ESCAPE_PROBE := tests/escape/escape_probe

# ---------------------------------------------------------------------------
# Vendored dependencies
# ---------------------------------------------------------------------------

THEFT_DIR := vendor/theft
THEFT_LIB := $(THEFT_DIR)/build/libtheft.a
THEFT_INC := $(THEFT_DIR)/inc

# ---------------------------------------------------------------------------
# Main binary
# ---------------------------------------------------------------------------

$(TARGET): $(OBJECTS)
	$(CC) $(CFLAGS) $(OBJECTS) -o $@

%.o: %.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

# ---------------------------------------------------------------------------
# Basic tests
# ---------------------------------------------------------------------------

$(TEST_PROBE): tests/container_probe.c
	$(CC) $(CFLAGS) $< -static -o $@

$(TEST_RUNNER): tests/test_runner.c tests/utils.c tests/utils.h
	$(CC) $(CFLAGS) $(CPPFLAGS) \
		tests/test_runner.c \
		tests/utils.c \
		-o $@

$(TEST_CONFIG): tests/test_config.c \
                src/config.c \
                tests/utils.c \
                tests/utils.h
	$(CC) $(CFLAGS) $(CPPFLAGS) \
		tests/test_config.c \
		src/config.c \
		tests/utils.c \
		-o $@

test-config: $(TEST_CONFIG)
	./$(TEST_CONFIG)

# ---------------------------------------------------------------------------
# Theft
# ---------------------------------------------------------------------------

$(THEFT_LIB):
	$(MAKE) -C $(THEFT_DIR)

$(TEST_CONFIG_PROPERTY): tests/property/test_config_property.c \
                         src/config.c \
                         $(THEFT_LIB)
	$(CC) $(CFLAGS) $(CPPFLAGS) -I$(THEFT_INC) \
		tests/property/test_config_property.c \
		src/config.c \
		$(THEFT_LIB) \
		-o $@

$(TEST_CONFIG_INVALID_PROPERTY): tests/property/test_config_invalid_property.c \
                                src/config.c \
                                $(THEFT_LIB)
	$(CC) $(CFLAGS) $(CPPFLAGS) -I$(THEFT_INC) \
		tests/property/test_config_invalid_property.c \
		src/config.c \
		$(THEFT_LIB) \
		-o $@

$(TEST_CONFIG_FORMAT_PROPERTY): tests/property/test_config_format_property.c \
                               src/config.c \
                               $(THEFT_LIB)
	$(CC) $(CFLAGS) $(CPPFLAGS) -I$(THEFT_INC) \
		tests/property/test_config_format_property.c \
		src/config.c \
		$(THEFT_LIB) \
		-o $@

$(TEST_CONFIG_HASH_PROPERTY): tests/property/test_config_hash_property.c \
                             src/config.c \
                             $(THEFT_LIB)
	$(CC) $(CFLAGS) $(CPPFLAGS) -I$(THEFT_INC) \
		tests/property/test_config_hash_property.c \
		src/config.c \
		$(THEFT_LIB) \
		-o $@

$(TEST_CONFIG_WHITESPACE_PROPERTY): tests/property/test_config_whitespace_property.c \
                                     src/config.c \
                                     $(THEFT_LIB)
	$(CC) $(CFLAGS) $(CPPFLAGS) -I$(THEFT_INC) \
		tests/property/test_config_whitespace_property.c \
		src/config.c \
		$(THEFT_LIB) \
		-o $@

$(TEST_ESCAPE_PROPERTY): tests/property/test_escape_property.c \
                        tests/escape/escape_utils.c \
                        tests/escape/escape_utils.h \
                        $(THEFT_LIB)
	$(CC) $(CFLAGS) $(CPPFLAGS) -I$(THEFT_INC) \
		tests/property/test_escape_property.c \
		tests/escape/escape_utils.c \
		$(THEFT_LIB) \
		-o $@

test-config-property: $(TEST_CONFIG_PROPERTY)
	./$(TEST_CONFIG_PROPERTY)

test-config-invalid-property: $(TEST_CONFIG_INVALID_PROPERTY)
	./$(TEST_CONFIG_INVALID_PROPERTY)

test-config-format-property: $(TEST_CONFIG_FORMAT_PROPERTY)
	./$(TEST_CONFIG_FORMAT_PROPERTY)

test-config-hash-property: $(TEST_CONFIG_HASH_PROPERTY)
	./$(TEST_CONFIG_HASH_PROPERTY)

test-config-whitespace-property: $(TEST_CONFIG_WHITESPACE_PROPERTY)
	./$(TEST_CONFIG_WHITESPACE_PROPERTY)

test-escape-property: $(TEST_ESCAPE_PROPERTY) $(TARGET) $(TEST_ESCAPE_PROBE)
	./$(TEST_ESCAPE_PROPERTY) ./$(TARGET) ./$(TEST_ESCAPE_PROBE)

test-property: \
	test-config-property \
	test-config-invalid-property \
	test-config-format-property \
	test-config-hash-property \
	test-config-whitespace-property \
	test-escape-property

# ---------------------------------------------------------------------------
# Escape tests
# ---------------------------------------------------------------------------

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

# ---------------------------------------------------------------------------
# Test suite
# ---------------------------------------------------------------------------

test: $(TARGET) $(TEST_RUNNER) $(TEST_PROBE) $(TEST_CONFIG)
	./$(TEST_CONFIG)
	./$(TEST_RUNNER) ./$(TARGET) ./$(TEST_PROBE)

test-all: \
	test \
	test-escape \
	test-property

# ---------------------------------------------------------------------------
# Clean
# ---------------------------------------------------------------------------

clean:
	rm -f $(OBJECTS) \
		$(TARGET) \
		$(TEST_RUNNER) \
		$(TEST_PROBE) \
		$(TEST_CONFIG) \
		$(TEST_CONFIG_PROPERTY) \
		$(TEST_CONFIG_INVALID_PROPERTY) \
		$(TEST_CONFIG_FORMAT_PROPERTY) \
		$(TEST_CONFIG_HASH_PROPERTY) \
		$(TEST_CONFIG_WHITESPACE_PROPERTY) \
		$(TEST_ESCAPE_PROPERTY) \
		$(TEST_ESCAPE_RUNNER) \
		$(TEST_ESCAPE_PROBE)

clean-vendor:
	$(MAKE) -C $(THEFT_DIR) clean

.PHONY: \
	clean \
	clean-vendor \
	test \
	test-all \
	test-config \
	test-property \
	test-escape
