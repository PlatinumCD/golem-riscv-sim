# -*- Makefile -*-

AM_CPPFLAGS += \
	$(MPI_CPPFLAGS) \
	-I$(srcdir)/include -I$(srcdir)/../bridge/include

compdir = $(pkglibdir)
comp_LTLIBRARIES = libmittens.la

libmittens_la_SOURCES = \
	profiling/taskTrace.h \
	profiling/taskTrace.cc \
	profiling/tileMeasurements.h \
	profiling/tileMeasurements.cc \
	profiling/tileProgress.h \
	profiling/tileProgress.cc \
	network/tx/txSnapshot.h \
	network/rx/rxSnapshot.h \
	execution/cpuActions.h \
	execution/deviceSupport.h \
	execution/diagnosticMessage.h \
	execution/uniqueFileDescriptor.h \
	memory/memoryAccessController.h \
	memory/memoryAccessController.cc \
	analog/analogController.h \
	analog/analogController.cc \
	memory/globalDMAClient.h \
	memory/globalDMAClient.cc \
	memory/globalDMAProtocol.h \
	synchronization/barrierProtocol.h \
	synchronization/initializationBarrierClient.h \
	synchronization/initializationBarrierClient.cc \
	profiling/measurementWriter.h \
	profiling/measurementWriter.cc \
	profiling/summarySnapshot.h \
	configuration/tileConfiguration.h \
	configuration/tileConfiguration.cc \
	configuration/tileParameters.h \
	configuration/networkConfiguration.h \
	configuration/networkConfiguration.cc \
	configuration/globalRAMConfiguration.h \
	configuration/globalRAMConfiguration.cc \
	configuration/resolvedConfiguration.h \
	network/packetEvent.h \
	network/wormholeEvents.h \
	network/wormholeRouter.h \
	network/wormholeRouter.cc \
	network/wormholeNetworkInterface.h \
	network/wormholeNetworkInterface.cc \
	probes/networkTimingProbe.h \
	probes/networkTimingProbe.cc \
	profiling/performanceProfile.h \
	profiling/performanceProfile.cc \
	execution/qemuProcess.h \
	execution/clockDomain.h \
	execution/cpuExecutionLedger.h \
	execution/cpuExecutionController.h \
	execution/cpuExecutionController.cc \
	execution/qemuCaptureCoordinator.h \
	execution/qemuCaptureCoordinator.cc \
	execution/qemuProcess.cc \
	execution/qemuReadySetExecutor.h \
	execution/qemuReadySetExecutor.cc \
	memory/globalRAMBacking.h \
	memory/globalRAMBacking.cc \
	memory/globalDMAEvent.h \
	memory/globalRAMReadiness.h \
	memory/globalRAMController.h \
	memory/globalRAMController.cc \
	probes/globalRAMReadinessProbe.h \
	probes/globalRAMReadinessProbe.cc \
	synchronization/epochBarrierEvent.h \
	synchronization/epochBarrierController.h \
	synchronization/epochBarrierController.cc \
	probes/epochBarrierProbe.h \
	probes/epochBarrierProbe.cc \
	synchronization/memoryInitializationBarrierEvent.h \
	synchronization/memoryInitializationBarrierController.h \
	synchronization/memoryInitializationBarrierController.cc \
	probes/memoryInitializationBarrierProbe.h \
	probes/memoryInitializationBarrierProbe.cc \
	bridge/sharedMemoryBridge.h \
	bridge/sharedMemoryBridge.cc \
	bridge/sharedSyncMemoryBridge.h \
	bridge/sharedSyncMemoryBridge.cc \
	memory/memoryAccessCoalescer.h \
	memory/addressRegion.h \
	network/receiveDMAEngine.h \
	network/receiveDMAEngine.cc \
	network/rx/rxController.h \
	network/rx/rxController.cc \
	network/tx/txController.h \
	network/tx/txController.cc \
	memory/scratchpad/scratchpadTimingModel.h \
	memory/scratchpad/scratchpadTimingModel.cc \
	bridge/sharedAnalogMemoryBridge.h \
	bridge/sharedAnalogMemoryBridge.cc \
	include/mittens/NICTileBridge.h \
	include/mittens/SyncTileBridge.h \
	include/mittens/MemoryMap.h \
	include/mittens/AnalogTileBridge.h \
	analog/analogBackend.h \
	analog/nativeAnalogBackend.h \
	analog/nativeAnalogBackend.cc \
	analog/timingAnalogBackend.h \
	analog/timingAnalogBackend.cc \
	analog/crossSimAnalogBackend.h \
	analog/crossSimAnalogBackend.cc \
	analog/analogDevice.h \
	analog/analogDevice.cc \
	tile/tile.h \
	tile/tile.cc

libmittens_la_LDFLAGS = -module -avoid-version

EXTRA_DIST = \
	tests/task_trace_test.cpp \
	profiling/tests/measurement_writer_test.cpp \
	profiling/tests/expected_summary_csv.h \
	profiling/tests/README.measurement-writer.md \
	profiling/MEASUREMENT_CONTRACT.md \
	tests/configuration.py \
	tests/run-configuration-test.py \
	tests/tile-defaults.json \
	tests/device_owners_test.cpp \
	tests/device_owners.py \
	tests/cpu_execution_ledger_test.cpp \
	tests/cpu_execution_controller_test.cpp \
	tests/cpu_memory_deadline.cpp \
	tests/cpu_memory_deadline.py \
	tests/cpu_memory_deadline_build.sh \
	tests/cpu_memory_deadline_simulation.py \
	tests/cpu_memory_deadline.md \
	README.md \
	tests/run-test.sh \
	tests/component_lifecycle.py \
	tests/analog_configuration.py \
	tests/multi_tile_boot.py \
	tests/qemu_boot.py \
	tests/two_tile_bridge.py \
	tests/analog_device_test.cpp \
	tests/analog_bridge_test.cpp \
	tests/nic_bridge_test.cpp \
	tests/receive_dma_engine_test.cpp \
	tests/scratchpad_timing_model_test.cpp \
	tests/performance_profile_test.cpp \
	tests/performance_profile.py \
	tests/wormhole_network.py \
	tests/sync_bridge_test.cpp \
	tests/qemu_ready_set_executor_test.cpp \
	tests/qemu_ready_set_boot.py \
	tests/qemu_ready_set_boot_proof.json \
	tests/run-qemu-ready-set-sst-test.sh \
	tests/epoch_barrier.py \
	tests/run-epoch-barrier-test.sh \
	tests/memory_initialization_barrier.py \
	tests/run-memory-initialization-barrier-test.sh \
	tests/memory_access_coalescer_test.cpp \
	tests/global_ram_readiness_test.cpp \
	tests/global_ram_readiness.py \
	tests/run-global-ram-readiness-test.sh \
	tests/crosssim_backend_test.cpp

install-exec-hook:
	$(SST_REGISTER_TOOL) SST_ELEMENT_SOURCE mittens=$(abs_srcdir)
	$(SST_REGISTER_TOOL) SST_ELEMENT_TESTS mittens=$(abs_srcdir)/tests
