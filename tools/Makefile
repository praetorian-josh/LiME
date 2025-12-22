# LiME Userspace Tools Makefile

CC ?= gcc
ANDROID_CC ?= aarch64-linux-android-gcc

CFLAGS = -Wall -O2
STATIC_FLAGS = -static

all: lime_ioctl

lime_ioctl: lime_ioctl.c
	$(CC) $(CFLAGS) -o lime_ioctl lime_ioctl.c

android: lime_ioctl.c
	$(ANDROID_CC) $(CFLAGS) $(STATIC_FLAGS) -o lime_ioctl lime_ioctl.c
	@echo "Built static binary for Android (aarch64)"

clean:
	rm -f lime_ioctl

.PHONY: all android clean
