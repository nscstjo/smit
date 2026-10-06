ifeq ($(origin CC),default)
CC := $(CROSS_COMPILE)gcc
endif
CPPFLAGS ?=
CFLAGS ?= -O2 -g -Wall -Wextra -Werror
LDFLAGS ?=
LDLIBS += -pthread
OUT ?= ../build/ubuntu-x86_64
SERVER_SOURCES := $(sort $(wildcard server/*.c))
SERVER_HEADERS := $(wildcard server/*.h) ../include/smit_identity.h
OBJECTS := $(patsubst server/%.c,$(OUT)/obj/%.o,$(SERVER_SOURCES))
.PHONY: all check clean
all: $(OUT)/smit
$(OUT)/smit: $(OBJECTS)
	$(CC) $(LDFLAGS) $^ $(LDLIBS) -o "$@"
$(OUT)/obj/%.o: server/%.c $(SERVER_HEADERS)
	@mkdir -p "$(@D)"
	$(CC) $(CPPFLAGS) $(CFLAGS) -pthread -MMD -MP -c "$<" -o "$@"
-include $(OBJECTS:.o=.d)
check:
	bash testing/run_offline.sh
clean:
	rm -f $(OBJECTS) $(OBJECTS:.o=.d) "$(OUT)/smit"
