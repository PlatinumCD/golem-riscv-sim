# -*- Makefile -*-

AM_CPPFLAGS += \
	$(MPI_CPPFLAGS) \
	-I$(srcdir)/include

compdir = $(pkglibdir)
comp_LTLIBRARIES = libmittens.la

libmittens_la_SOURCES = \
	packetEvent.h \
	qemuProcess.h \
	qemuProcess.cc \
	sharedMemoryBridge.h \
	sharedMemoryBridge.cc \
	include/mittens/bridge.h \
	tile.h \
	tile.cc

libmittens_la_LDFLAGS = -module -avoid-version

EXTRA_DIST = \
	README.md \
	tests/run-test.sh \
	tests/component_lifecycle.py \
	tests/multi_tile_boot.py \
	tests/qemu_boot.py \
	tests/two_tile_bridge.py

install-exec-hook:
	$(SST_REGISTER_TOOL) SST_ELEMENT_SOURCE mittens=$(abs_srcdir)
	$(SST_REGISTER_TOOL) SST_ELEMENT_TESTS mittens=$(abs_srcdir)/tests
