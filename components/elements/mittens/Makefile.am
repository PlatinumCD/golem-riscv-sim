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
	sharedSyncMemoryBridge.h \
	sharedSyncMemoryBridge.cc \
	sharedAnalogMemoryBridge.h \
	sharedAnalogMemoryBridge.cc \
	include/mittens/NICTileBridge.h \
	include/mittens/SyncTileBridge.h \
	include/mittens/AnalogTileBridge.h \
	analog/analogBackend.h \
	analog/nativeAnalogBackend.h \
	analog/nativeAnalogBackend.cc \
	analog/crossSimAnalogBackend.h \
	analog/crossSimAnalogBackend.cc \
	analog/analogDevice.h \
	analog/analogDevice.cc \
	tile.h \
	tile.cc

libmittens_la_LDFLAGS = -module -avoid-version

EXTRA_DIST = \
	README.md \
	tests/run-test.sh \
	tests/component_lifecycle.py \
	tests/analog_configuration.py \
	tests/multi_tile_boot.py \
	tests/qemu_boot.py \
	tests/two_tile_bridge.py \
	tests/analog_device_test.cpp \
	tests/analog_bridge_test.cpp \
	tests/sync_bridge_test.cpp \
	tests/crosssim_backend_test.cpp

install-exec-hook:
	$(SST_REGISTER_TOOL) SST_ELEMENT_SOURCE mittens=$(abs_srcdir)
	$(SST_REGISTER_TOOL) SST_ELEMENT_TESTS mittens=$(abs_srcdir)/tests
