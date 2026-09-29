OBJECTS := $(patsubst %.c,%.o,$(wildcard *.c))
CPPFLAGS += -I../lepton_control
CFLAGS ?= -g -O2

all: liblepton_sdk.a

liblepton_sdk.a: ${OBJECTS}
	${CROSS_COMPILE}ar rcs $@ ${OBJECTS}

clean:
	rm -f *.o *.a

.PHONY: all clean
