cmake_minimum_required(VERSION 3.24)

# CMake needs the CUDA compiler before project(... LANGUAGES CUDA).
set(YOLO11_CUDA_ROOT "" CACHE PATH "CUDA toolkit root")
if(NOT YOLO11_CUDA_ROOT AND DEFINED ENV{CUDA_PATH})
    file(TO_CMAKE_PATH "$ENV{CUDA_PATH}" YOLO11_CUDA_ROOT)
endif()
if(YOLO11_CUDA_ROOT AND NOT DEFINED CMAKE_CUDA_COMPILER)
    set(CMAKE_CUDA_COMPILER "${YOLO11_CUDA_ROOT}/bin/nvcc.exe" CACHE FILEPATH "CUDA compiler")
endif()

project(four_stage_qt_demo VERSION 1.0.0 LANGUAGES CXX CUDA)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CUDA_STANDARD 17)
set(CMAKE_CUDA_STANDARD_REQUIRED ON)
if(NOT CMAKE_CUDA_ARCHITECTURES)
    set(CMAKE_CUDA_ARCHITECTURES 89)
endif()

if(MSVC)
    add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:/utf-8>")
    set(CMAKE_CUDA_FLAGS "${CMAKE_CUDA_FLAGS} -Xcompiler=/utf-8")
endif()
add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)

set(TENSORRT_ROOT "" CACHE PATH "TensorRT 10 root")
if(NOT TENSORRT_ROOT AND DEFINED ENV{TENSORRT_ROOT})
    file(TO_CMAKE_PATH "$ENV{TENSORRT_ROOT}" TENSORRT_ROOT)
endif()
if(NOT TENSORRT_ROOT)
    message(FATAL_ERROR "Pass -DTENSORRT_ROOT=<TensorRT-10-root>")
endif()
if(NOT YOLO11_CUDA_ROOT)
    message(FATAL_ERROR "Pass -DYOLO11_CUDA_ROOT=<CUDA-root> or set CUDA_PATH")
endif()

find_library(NVINFER_LIB NAMES nvinfer_10 PATHS "${TENSORRT_ROOT}/lib" NO_DEFAULT_PATH REQUIRED)
find_library(NVINFER_PLUGIN_LIB NAMES nvinfer_plugin_10 PATHS "${TENSORRT_ROOT}/lib" NO_DEFAULT_PATH REQUIRED)
find_library(CUDART_LIB NAMES cudart PATHS "${YOLO11_CUDA_ROOT}/lib/x64" NO_DEFAULT_PATH REQUIRED)

find_package(OpenCV REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(yaml-cpp CONFIG REQUIRED)
find_package(Crow CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(unofficial-sqlite3 CONFIG REQUIRED)
find_package(hiredis CONFIG REQUIRED)
find_package(Threads REQUIRED)

set(PROJECT_INCLUDE_DIR "${PROJECT_SOURCE_DIR}/include")
set(PROJECT_PLUGIN_DIR "${PROJECT_SOURCE_DIR}/plugin")

add_library(myplugins SHARED plugin/yololayer.cu)
target_compile_definitions(myplugins PRIVATE API_EXPORTS)
target_include_directories(myplugins PRIVATE
    "${PROJECT_INCLUDE_DIR}" "${PROJECT_PLUGIN_DIR}"
    "${YOLO11_CUDA_ROOT}/include" "${TENSORRT_ROOT}/include" ${OpenCV_INCLUDE_DIRS})
target_link_libraries(myplugins PRIVATE "${NVINFER_LIB}" "${NVINFER_PLUGIN_LIB}" "${CUDART_LIB}")

add_library(pose_runtime STATIC
    api/yolo11_pose_api.cpp
    src/preprocess.cu
    src/postprocess.cpp
)
target_include_directories(pose_runtime PUBLIC
    "${PROJECT_INCLUDE_DIR}" "${PROJECT_PLUGIN_DIR}"
    "${YOLO11_CUDA_ROOT}/include" "${TENSORRT_ROOT}/include" ${OpenCV_INCLUDE_DIRS})
target_link_libraries(pose_runtime PUBLIC
    myplugins "${NVINFER_LIB}" "${NVINFER_PLUGIN_LIB}" "${CUDART_LIB}" ${OpenCV_LIBS})

add_library(people_flow_core STATIC
    src/business/person_tracker.cpp
    src/business/line_crossing_counter.cpp
)
target_include_directories(people_flow_core PUBLIC "${PROJECT_INCLUDE_DIR}")

add_library(people_flow_vision STATIC
    src/business/person_detector_adapter.cpp
    src/business/people_flow_renderer.cpp
)
target_include_directories(people_flow_vision PUBLIC "${PROJECT_INCLUDE_DIR}" ${OpenCV_INCLUDE_DIRS})
target_link_libraries(people_flow_vision PUBLIC people_flow_core ${OpenCV_LIBS})

add_library(security_analytics_core STATIC
    src/business/electronic_fence_engine.cpp
    src/business/track_analytics_engine.cpp
    src/business/pose_action_engine.cpp
    src/business/temporal_action_engine.cpp
    src/business/security_live_pipeline.cpp
    src/business/security_overlay_renderer.cpp
)
target_include_directories(security_analytics_core PUBLIC "${PROJECT_INCLUDE_DIR}")
target_link_libraries(security_analytics_core PUBLIC people_flow_vision)

add_library(people_flow_storage STATIC src/business/people_flow_repository.cpp)
target_include_directories(people_flow_storage PUBLIC "${PROJECT_INCLUDE_DIR}")
target_link_libraries(people_flow_storage PUBLIC unofficial::sqlite3::sqlite3 Threads::Threads)

add_library(app_support STATIC
    src/server/app_config.cpp
    src/server/app_logger.cpp
    src/server/camera_profile.cpp
    src/server/uri_masker.cpp
)
target_include_directories(app_support PUBLIC "${PROJECT_INCLUDE_DIR}")
target_link_libraries(app_support PUBLIC yaml-cpp::yaml-cpp spdlog::spdlog)

add_library(people_flow_redis STATIC src/server/redis_task_queue.cpp)
target_include_directories(people_flow_redis PUBLIC "${PROJECT_INCLUDE_DIR}")
target_link_libraries(people_flow_redis PUBLIC hiredis::hiredis)
if(WIN32)
    target_link_libraries(people_flow_redis PUBLIC ws2_32)
endif()

add_executable(four_stage_server
    src/server/main_server.cpp
    src/server/people_flow_http_server.cpp
)
target_include_directories(four_stage_server PRIVATE "${PROJECT_INCLUDE_DIR}")
target_link_libraries(four_stage_server PRIVATE
    app_support people_flow_redis people_flow_storage
    Crow::Crow nlohmann_json::nlohmann_json spdlog::spdlog)

add_executable(four_stage_worker
    src/server/main_people_flow_worker.cpp
    src/server/people_flow_inference_worker.cpp
    src/server/model_runner.cpp
    src/business/rtsp_capture_reader.cpp
)
target_include_directories(four_stage_worker PRIVATE
    "${PROJECT_INCLUDE_DIR}" "${PROJECT_PLUGIN_DIR}" ${OpenCV_INCLUDE_DIRS})
target_link_libraries(four_stage_worker PRIVATE
    app_support people_flow_redis people_flow_storage security_analytics_core pose_runtime myplugins
    nlohmann_json::nlohmann_json spdlog::spdlog ${OpenCV_LIBS})
add_dependencies(four_stage_worker myplugins)
add_custom_command(TARGET four_stage_worker POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:myplugins> $<TARGET_FILE_DIR:four_stage_worker>)

enable_testing()
add_executable(people_flow_core_test tests/phase20_people_flow_test.cpp)
target_include_directories(people_flow_core_test PRIVATE "${PROJECT_INCLUDE_DIR}")
target_link_libraries(people_flow_core_test PRIVATE people_flow_vision)
add_test(NAME people_flow_core_test COMMAND people_flow_core_test)

add_executable(security_analytics_test tests/phase24_security_analytics_test.cpp)
target_include_directories(security_analytics_test PRIVATE "${PROJECT_INCLUDE_DIR}")
target_link_libraries(security_analytics_test PRIVATE security_analytics_core)
add_test(NAME security_analytics_test COMMAND security_analytics_test)

add_executable(repository_test tests/phase22_repository_test.cpp)
target_include_directories(repository_test PRIVATE "${PROJECT_INCLUDE_DIR}")
target_link_libraries(repository_test PRIVATE people_flow_storage)
add_test(NAME repository_test COMMAND repository_test)

# Hardware smoke target; intentionally not part of CTest so CI without an
# NVIDIA GPU can still run all deterministic tests.
add_executable(pose_engine_smoke tests/pose_engine_smoke.cpp)
target_include_directories(pose_engine_smoke PRIVATE "${PROJECT_INCLUDE_DIR}")
target_link_libraries(pose_engine_smoke PRIVATE pose_runtime myplugins)
add_dependencies(pose_engine_smoke myplugins)
add_custom_command(TARGET pose_engine_smoke POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:myplugins> $<TARGET_FILE_DIR:pose_engine_smoke>)

install(TARGETS four_stage_server four_stage_worker RUNTIME DESTINATION bin)
install(FILES engines/yolo11n-pose.engine DESTINATION engines)
install(FILES config/server.yaml config/worker.yaml config/cameras.yaml DESTINATION config)
