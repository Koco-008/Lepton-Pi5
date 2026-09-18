CC ?= gcc
CPPFLAGS += -I../lepton_vospi_lib -I../lepton_sdk -I../lepton_control
CPPFLAGS += -DI2C_DEVICE_FILE=\"/dev/i2c-1\"
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic

APPLICATION := lepton_streamer
TEST_APPLICATION := test_frame_pipeline
COMMON_OBJECTS := frame_pipeline.o lepton_vospi_funcs.o
APPLICATION_OBJECTS := lepton_streamer.o $(COMMON_OBJECTS) lep_i2cdev.o
SDK_LIB := ../lepton_sdk/liblepton_sdk.a
SDK_CFLAGS ?= -g -O2

all: $(APPLICATION)

$(SDK_LIB):
	$(MAKE) -C ../lepton_sdk CFLAGS='$(SDK_CFLAGS)'

$(APPLICATION): $(APPLICATION_OBJECTS) $(SDK_LIB)
	$(CC) $(CFLAGS) -o $@ $(APPLICATION_OBJECTS) $(SDK_LIB) $(LDFLAGS) $(LDLIBS) -ldl

$(TEST_APPLICATION): test_frame_pipeline.o $(COMMON_OBJECTS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) $(LDLIBS)

lepton_vospi_funcs.o: ../lepton_vospi_lib/lepton_vospi_funcs.c \
	../lepton_vospi_lib/lepton_vospi_funcs.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

lep_i2cdev.o: ../lepton_control/lep_i2cdev.c ../lepton_control/lep_i2cdev.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

check: $(TEST_APPLICATION)
	./$(TEST_APPLICATION)

clean:
	rm -f *.o $(APPLICATION) $(TEST_APPLICATION)

.PHONY: all check clean
