CC ?= gcc
CPPFLAGS += -I../lepton_vospi_lib
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic

APPLICATION := lepton_streamer
TEST_APPLICATION := test_frame_pipeline
COMMON_OBJECTS := frame_pipeline.o lepton_vospi_funcs.o

all: $(APPLICATION)

$(APPLICATION): lepton_streamer.o $(COMMON_OBJECTS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) $(LDLIBS)

$(TEST_APPLICATION): test_frame_pipeline.o $(COMMON_OBJECTS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) $(LDLIBS)

lepton_vospi_funcs.o: ../lepton_vospi_lib/lepton_vospi_funcs.c \
	../lepton_vospi_lib/lepton_vospi_funcs.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

check: $(TEST_APPLICATION)
	./$(TEST_APPLICATION)

clean:
	rm -f *.o $(APPLICATION) $(TEST_APPLICATION)

.PHONY: all check clean
