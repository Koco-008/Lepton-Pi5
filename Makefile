CROSS_COMPILE ?=
CC=${CROSS_COMPILE}gcc
TOPDIR:=$(shell pwd)

export CROSS_COMPILE CC

all: lepton_sdk lepton_control lepton_data_collector lepton_streamer

lepton_sdk:
	${MAKE} -C lepton_sdk

lepton_control: lepton_sdk
	${MAKE} -C lepton_control

lepton_data_collector:
	${MAKE} -C lepton_data_collector

lepton_streamer:
	${MAKE} -C lepton_streamer

check:
	${MAKE} -C lepton_streamer check

clean:
	${MAKE} -C lepton_sdk clean
	${MAKE} -C lepton_control clean
	${MAKE} -C lepton_data_collector clean
	${MAKE} -C lepton_streamer clean

.PHONY: all check clean lepton_sdk lepton_control lepton_data_collector lepton_streamer

