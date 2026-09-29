# Copyright (C) 2025-2026 Tactical Computing Laboratories, LLC
# All Rights Reserved
# contact@tactcomplabs.com
# See LICENSE in the top level directory for licensing details

# mordred/src/CMakeLists.txt
# this is triggered via the top-level CMakeLists.txt in mordred

set(MordredSrcs
        MordredEvents.h
        MordredNicBase.h
        MordredNicBase.cc
        MordredNIC.h
        MordredNIC.cc
        MordredRouter.h
        MordredRouter.cc
        TopologyAPI.h
        MeshTopology.h
        MeshTopology.cc
        GatewayTopology.h
        GatewayTopology.cc
        RtrPortControlAPI.h
        RtrPortControlBase.h
        RtrPortControlBase.cc
        RtrPortControl.h
        RtrPortControl.cc
        XbarArbAPI.h
        XbarArbRR.h
        XbarArbRR.cc
        VcAllocAPI.h
        VcAllocRR.h
        VcAllocRR.cc
        FlatButterflyTopo.h
        FlatButterflyTopo.cc
        Torus2dTopo.h
        Torus2dTopo.cc
        Torus3dTopo.h
        Torus3dTopo.cc
        MordredTestEP.h
        MordredTestEP.cc
)

if(MORDRED_ENABLE_PHYS_CHANNEL)
    set(PRYDWEN_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../prydwen/src"
        CACHE PATH "Path to prydwen/src (for PhysChannelAPI.h)")
    if(NOT EXISTS "${PRYDWEN_SRC_DIR}/PhysChannelAPI.h")
        message(FATAL_ERROR
            "MORDRED_ENABLE_PHYS_CHANNEL is ON but PhysChannelAPI.h was not found at "
            "${PRYDWEN_SRC_DIR}. Build and install prydwen first, or set "
            "-DMORDRED_ENABLE_PHYS_CHANNEL=OFF to skip the PhysChannel components.")
    endif()
    list(APPEND MordredSrcs
        RtrPortControlPC.h
        RtrPortControlPC.cc
        MordredNicPC.h
        MordredNicPC.cc
    )
    message(STATUS "PhysChannel components enabled (prydwen: ${PRYDWEN_SRC_DIR})")
else()
    message(STATUS "PhysChannel components disabled (MORDRED_ENABLE_PHYS_CHANNEL=OFF)")
endif()

add_library(mordred SHARED ${MordredSrcs})

target_include_directories(mordred PUBLIC ${SST_INSTALL_DIR}/include)
if(MORDRED_ENABLE_PHYS_CHANNEL)
    target_include_directories(mordred PUBLIC ${PRYDWEN_SRC_DIR})
    target_compile_definitions(mordred PUBLIC MORDRED_ENABLE_PHYS_CHANNEL)
endif()

install(TARGETS mordred DESTINATION ${CMAKE_CURRENT_SOURCE_DIR})
install(CODE "execute_process(COMMAND sst-register mordred mordred_LIBDIR=${CMAKE_CURRENT_SOURCE_DIR})")

# EOF
