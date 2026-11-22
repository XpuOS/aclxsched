add_hal_lib(ascend)
add_shim_lib(ascend)

find_package(Python3 3.9 REQUIRED COMPONENTS Interpreter)
if(ASCEND_CANN_ROOT)
    set(ASCEND_ACLNN_HEADERS "${ASCEND_CANN_ROOT}/include/aclnnop")
else()
    find_path(ASCEND_ACLNN_HEADERS aclnn_apply_rotary_pos_emb.h
        HINTS "$ENV{ASCEND_HOME_PATH}/include/aclnnop"
              /usr/local/Ascend/ascend-toolkit/latest/include/aclnnop)
endif()
set(ASCEND_EXTRA_HEADERS_ARGS)
set(ASCEND_ACLNN_SDK_HEADERS)
if(ASCEND_ACLNN_HEADERS AND EXISTS "${ASCEND_ACLNN_HEADERS}/aclnn_apply_rotary_pos_emb.h")
    list(APPEND ASCEND_EXTRA_HEADERS_ARGS --extra-header-dir "${ASCEND_ACLNN_HEADERS}")
    file(GLOB ASCEND_ACLNN_SDK_HEADERS CONFIGURE_DEPENDS "${ASCEND_ACLNN_HEADERS}/*.h")
endif()
set(ASCEND_ACLNN_GENERATED_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
set(ASCEND_ACLNN_WORKSPACE_SOURCE ${ASCEND_ACLNN_GENERATED_DIR}/aclnn_workspace_intercept.cpp)
set(ASCEND_ACLNN_WORKSPACE_DLSYM ${ASCEND_ACLNN_GENERATED_DIR}/aclnn_workspace_intercept_dlsym.inc)
set(ASCEND_ACLNN_HEADER_LIST ${ASCEND_ACLNN_GENERATED_DIR}/sdk_headers.txt)
file(GENERATE OUTPUT "${ASCEND_ACLNN_HEADER_LIST}" CONTENT "${ASCEND_ACLNN_SDK_HEADERS}\n")
add_custom_command(
    OUTPUT ${ASCEND_ACLNN_WORKSPACE_SOURCE} ${ASCEND_ACLNN_WORKSPACE_DLSYM}
    COMMAND ${Python3_EXECUTABLE}
            ${PROJECT_SOURCE_DIR}/tools/autogen/gen_aclnn_workspace_intercept.py
            --header ${CMAKE_CURRENT_LIST_DIR}/hal/include/xsched/ascend/hal/acl.h
            ${ASCEND_EXTRA_HEADERS_ARGS}
            --existing-intercepts ${CMAKE_CURRENT_LIST_DIR}/shim/src/intercept.cpp
            --source-output ${ASCEND_ACLNN_WORKSPACE_SOURCE}
            --dlsym-output ${ASCEND_ACLNN_WORKSPACE_DLSYM}
    DEPENDS
        ${PROJECT_SOURCE_DIR}/tools/autogen/gen_aclnn_workspace_intercept.py
        ${CMAKE_CURRENT_LIST_DIR}/hal/include/xsched/ascend/hal/acl.h
        ${CMAKE_CURRENT_LIST_DIR}/shim/src/intercept.cpp
        ${ASCEND_ACLNN_HEADER_LIST}
        ${ASCEND_ACLNN_SDK_HEADERS}
    VERBATIM)
add_custom_target(ascend_aclnn_workspace_codegen
    DEPENDS ${ASCEND_ACLNN_WORKSPACE_SOURCE} ${ASCEND_ACLNN_WORKSPACE_DLSYM})
add_dependencies(shimascend ascend_aclnn_workspace_codegen)
target_sources(shimascend PRIVATE ${ASCEND_ACLNN_WORKSPACE_SOURCE})
target_include_directories(shimascend PRIVATE ${ASCEND_ACLNN_GENERATED_DIR})

if(ASCEND_ENABLE_ATB)
    set(ASCEND_ATB_ROOT "$ENV{ATB_HOME_PATH}" CACHE PATH "ATB installation (directory containing include/atb)")
    find_path(ASCEND_ATB_INCLUDE_DIR atb/operation.h
        HINTS "${ASCEND_ATB_ROOT}/include" "$ENV{ATB_HOME_PATH}/include"
        PATHS /usr/local/Ascend/nnal/atb/latest/atb/cxx_abi_1/include)
    find_path(ASCEND_CANN_INCLUDE_DIR acl/acl.h
        HINTS "${ASCEND_CANN_ROOT}/include" "$ENV{ASCEND_HOME_PATH}/include"
        PATHS /usr/local/Ascend/ascend-toolkit/latest/include)
    if(ASCEND_ATB_INCLUDE_DIR AND ASCEND_CANN_INCLUDE_DIR)
        target_include_directories(shimascend SYSTEM PRIVATE
            ${ASCEND_ATB_INCLUDE_DIR} ${ASCEND_CANN_INCLUDE_DIR})
        target_compile_definitions(shimascend PRIVATE XSCHED_HAS_ATB=1)
        message(STATUS "Ascend ATB adapter enabled: ${ASCEND_ATB_INCLUDE_DIR}")
    else()
        message(FATAL_ERROR "ASCEND_ENABLE_ATB=ON requires ATB/CANN headers; load set_env.sh, set ASCEND_ATB_ROOT/ASCEND_CANN_ROOT, or disable ASCEND_ENABLE_ATB")
    endif()
else()
    message(STATUS "Ascend ATB adapter disabled (ASCEND_ENABLE_ATB=OFF)")
endif()

if(XSCHED_ASCEND_XVMM)
    target_link_libraries(halascend PUBLIC XVMM::xvmm)
    target_compile_definitions(halascend PUBLIC XSCHED_HAS_XVMM=1)
    message(STATUS "Ascend xvmm adapter enabled (3rdparty/xvmm)")
else()
    message(STATUS "Ascend xvmm adapter disabled (XSCHED_ASCEND_XVMM=OFF)")
endif()
