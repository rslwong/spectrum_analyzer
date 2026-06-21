# Wrapper so `make` builds without manually sourcing ESP-IDF's export.sh.
# Override IDF_PATH or PORT on the command line if yours differ:
#   make IDF_PATH=~/esp/esp-idf PORT=/dev/cu.usbmodem1101 flash
SHELL := /bin/bash
IDF_PATH ?= $(HOME)/esp/esp-idf
PORT ?=
PORTARG = $(if $(PORT),-p $(PORT),)

IDF = source $(IDF_PATH)/export.sh >/dev/null 2>&1 && idf.py

.PHONY: all build flash monitor clean menuconfig

all: build

# Set the target from sdkconfig.defaults on first build only.
sdkconfig:
	$(IDF) set-target esp32p4

build: | sdkconfig
	$(IDF) build

flash: build
	$(IDF) $(PORTARG) flash

monitor:
	$(IDF) $(PORTARG) monitor

# Build, flash and open the serial monitor in one go.
run: build
	$(IDF) $(PORTARG) flash monitor

menuconfig:
	$(IDF) menuconfig

clean:
	$(IDF) fullclean
