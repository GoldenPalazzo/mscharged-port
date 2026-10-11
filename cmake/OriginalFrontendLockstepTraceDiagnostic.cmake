include_guard(GLOBAL)

# Temporary network desync diagnostic (patch 0693). With MSCHARGED_RNG_TRACE
# set at run time, every simulation step of a lockstep game writes the shared
# random seed with its callers, ball and player state, synchronized input, AI
# script machine states and fuzzy script results to that file. Diffing the
# files of two machines finds the first value in which they diverge.
option(MSCHARGED_DIAGNOSTIC_LOCKSTEP_TRACE
    "Trace lockstep simulation state to MSCHARGED_RNG_TRACE for network desync hunting" OFF)

function(mscharged_select_original_frontend_lockstep_trace target)
    if(NOT MSCHARGED_DIAGNOSTIC_LOCKSTEP_TRACE)
        return()
    endif()
    # The traced session, sockets and AI owners come with the network owner.
    if(NOT MSCHARGED_DIAGNOSTIC_MAIN_NETWORK_OWNER)
        message(FATAL_ERROR "The lockstep trace requires MSCHARGED_DIAGNOSTIC_MAIN_NETWORK_OWNER")
    endif()
    # Caller stacks use glibc/libSystem backtrace() and dladdr().
    if(WIN32)
        message(FATAL_ERROR "The lockstep trace needs backtrace() and dladdr(), absent on Windows")
    endif()
    get_target_property(_trace_type "${target}" TYPE)
    if(NOT _trace_type STREQUAL "MODULE_LIBRARY")
        message(FATAL_ERROR "The lockstep trace belongs to the isolated original game module")
    endif()
    target_compile_definitions("${target}" PRIVATE
        MSCHARGED_DIAGNOSTIC_LOCKSTEP_TRACE=1)
endfunction()
