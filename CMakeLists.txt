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

if(MSVC AND CMAKE_GENERATOR MATCHES "Ninja" AND
   CMAKE_CL_SHOWINCLUDES_PREFIX STREQUAL "娉ㄦ剰: 鍖呭惈鏂囦欢:  ")
    set(CMAKE_CL_SHOWINCLUDES_PREFIX "注意: 包含文件:  ")
endif()

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
find_package(PostgreSQL REQUIRED)
find_package(hiredis CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)
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

add_library(postgres_storage STATIC src/business/postgres_client.cpp)
target_include_directories(postgres_storage PUBLIC "${PROJECT_INCLUDE_DIR}")
target_link_libraries(postgres_storage PUBLIC PostgreSQL::PostgreSQL)

add_library(people_flow_storage STATIC src/business/people_flow_repository.cpp)
target_include_directories(people_flow_storage PUBLIC "${PROJECT_INCLUDE_DIR}")
target_link_libraries(people_flow_storage PUBLIC postgres_storage Threads::Threads)

add_library(camera_task_storage STATIC src/business/camera_task_repository.cpp)
target_include_directories(camera_task_storage PUBLIC "${PROJECT_INCLUDE_DIR}")
target_link_libraries(camera_task_storage PUBLIC postgres_storage nlohmann_json::nlohmann_json)

add_library(app_support STATIC
    src/server/app_config.cpp
    src/server/app_logger.cpp
    src/server/camera_profile.cpp
    src/server/camera_profile_registry.cpp
    src/server/uri_masker.cpp
)
target_include_directories(app_support PUBLIC "${PROJECT_INCLUDE_DIR}")
target_link_libraries(app_support PUBLIC yaml-cpp::yaml-cpp spdlog::spdlog)

add_library(camera_capture_runtime STATIC
    src/business/rtsp_capture_reader.cpp
    src/business/ffmpeg_process_capture_reader.cpp
    src/server/ffmpeg_open_coordinator.cpp
    src/server/shared_camera_frame_hub.cpp
    src/server/rtsp_camera_frame_source.cpp
)
target_include_directories(camera_capture_runtime PUBLIC "${PROJECT_INCLUDE_DIR}" ${OpenCV_INCLUDE_DIRS})
target_link_libraries(camera_capture_runtime PUBLIC app_support spdlog::spdlog ${OpenCV_LIBS} Threads::Threads)

add_library(camera_task_scheduling STATIC src/server/camera_task_manager.cpp)
target_include_directories(camera_task_scheduling PUBLIC "${PROJECT_INCLUDE_DIR}")
target_link_libraries(camera_task_scheduling PUBLIC spdlog::spdlog Threads::Threads)

add_library(camera_inference_pool STATIC src/server/camera_inference_pool.cpp)
target_include_directories(camera_inference_pool PUBLIC "${PROJECT_INCLUDE_DIR}" ${OpenCV_INCLUDE_DIRS})
target_link_libraries(camera_inference_pool PUBLIC
    app_support spdlog::spdlog ${OpenCV_LIBS} Threads::Threads)

add_library(camera_algorithm_runtime STATIC src/server/camera_algorithm_processor.cpp)
target_include_directories(camera_algorithm_runtime PUBLIC "${PROJECT_INCLUDE_DIR}" ${OpenCV_INCLUDE_DIRS})
target_link_libraries(camera_algorithm_runtime PUBLIC
    camera_inference_pool camera_task_storage people_flow_vision security_analytics_core
    nlohmann_json::nlohmann_json OpenSSL::Crypto ${OpenCV_LIBS} Threads::Threads)

add_library(callback_delivery_runtime STATIC src/server/callback_delivery_worker.cpp)
target_include_directories(callback_delivery_runtime PUBLIC "${PROJECT_INCLUDE_DIR}")
target_link_libraries(callback_delivery_runtime PUBLIC
    app_support camera_task_storage nlohmann_json::nlohmann_json
    OpenSSL::Crypto spdlog::spdlog Threads::Threads)
if(WIN32)
    target_link_libraries(callback_delivery_runtime PUBLIC winhttp)
endif()

add_library(camera_task_redis STATIC src/server/camera_task_queue.cpp)
target_include_directories(camera_task_redis PUBLIC "${PROJECT_INCLUDE_DIR}")
target_link_libraries(camera_task_redis PUBLIC camera_task_scheduling hiredis::hiredis nlohmann_json::nlohmann_json)
if(WIN32)
    target_link_libraries(camera_task_redis PUBLIC ws2_32)
endif()

add_library(camera_task_runtime STATIC
    src/business/frame_artifact_writer.cpp
    src/business/camera_frame_retention.cpp
    src/business/camera_pipeline.cpp
    src/server/camera_task_runtime.cpp
)
target_include_directories(camera_task_runtime PUBLIC "${PROJECT_INCLUDE_DIR}" ${OpenCV_INCLUDE_DIRS})
target_link_libraries(camera_task_runtime PUBLIC
    camera_capture_runtime camera_task_storage camera_task_redis
    spdlog::spdlog ${OpenCV_LIBS} Threads::Threads)

add_library(camera_task_http STATIC src/server/camera_task_http_controller.cpp)
target_include_directories(camera_task_http PUBLIC "${PROJECT_INCLUDE_DIR}")
target_link_libraries(camera_task_http PUBLIC
    app_support camera_task_storage camera_task_redis Crow::Crow
    nlohmann_json::nlohmann_json OpenSSL::Crypto)

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
    app_support camera_task_http people_flow_redis people_flow_storage
    Crow::Crow nlohmann_json::nlohmann_json spdlog::spdlog)

add_executable(four_stage_worker
    src/server/main_people_flow_worker.cpp
    src/server/people_flow_inference_worker.cpp
    src/server/people_flow_session_runner.cpp
    src/server/vision_worker_host.cpp
    src/server/model_runner.cpp
)
target_include_directories(four_stage_worker PRIVATE
    "${PROJECT_INCLUDE_DIR}" "${PROJECT_PLUGIN_DIR}" ${OpenCV_INCLUDE_DIRS})
target_link_libraries(four_stage_worker PRIVATE
    app_support camera_capture_runtime camera_task_runtime camera_task_scheduling
    camera_inference_pool camera_algorithm_runtime callback_delivery_runtime
    people_flow_redis people_flow_storage security_analytics_core pose_runtime myplugins
    nlohmann_json::nlohmann_json spdlog::spdlog ${OpenCV_LIBS})
add_dependencies(four_stage_worker myplugins)
add_custom_command(TARGET four_stage_worker POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:myplugins> $<TARGET_FILE_DIR:four_stage_worker>)

enable_testing()
add_executable(app_config_runtime_test tests/app_config_runtime_test.cpp)
target_include_directories(app_config_runtime_test PRIVATE "${PROJECT_INCLUDE_DIR}")
target_link_libraries(app_config_runtime_test PRIVATE app_support camera_capture_runtime)
add_test(NAME app_config_runtime_test COMMAND app_config_runtime_test
    "${CMAKE_SOURCE_DIR}/config/server.yaml" "${CMAKE_SOURCE_DIR}/config/worker.yaml")
set_tests_properties(app_config_runtime_test PROPERTIES WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

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
set_tests_properties(repository_test PROPERTIES SKIP_RETURN_CODE 77)

add_executable(shared_camera_frame_hub_test tests/shared_camera_frame_hub_test.cpp)
target_include_directories(shared_camera_frame_hub_test PRIVATE "${PROJECT_INCLUDE_DIR}" ${OpenCV_INCLUDE_DIRS})
target_link_libraries(shared_camera_frame_hub_test PRIVATE camera_capture_runtime ${OpenCV_LIBS})
add_test(NAME shared_camera_frame_hub_test COMMAND shared_camera_frame_hub_test)

add_executable(people_flow_hub_regression_test tests/people_flow_hub_regression_test.cpp)
target_include_directories(people_flow_hub_regression_test PRIVATE "${PROJECT_INCLUDE_DIR}" ${OpenCV_INCLUDE_DIRS})
target_link_libraries(people_flow_hub_regression_test PRIVATE camera_capture_runtime ${OpenCV_LIBS})
add_test(NAME people_flow_hub_regression_test COMMAND people_flow_hub_regression_test)

add_executable(camera_task_manager_test tests/camera_task_manager_test.cpp)
target_include_directories(camera_task_manager_test PRIVATE "${PROJECT_INCLUDE_DIR}")
target_link_libraries(camera_task_manager_test PRIVATE camera_task_scheduling)
add_test(NAME camera_task_manager_test COMMAND camera_task_manager_test)

add_executable(camera_inference_pool_test tests/camera_inference_pool_test.cpp)
target_include_directories(camera_inference_pool_test PRIVATE "${PROJECT_INCLUDE_DIR}" ${OpenCV_INCLUDE_DIRS})
target_link_libraries(camera_inference_pool_test PRIVATE camera_inference_pool ${OpenCV_LIBS})
add_test(NAME camera_inference_pool_test COMMAND camera_inference_pool_test)

add_executable(camera_algorithm_processor_test tests/camera_algorithm_processor_test.cpp)
target_include_directories(camera_algorithm_processor_test PRIVATE
    "${PROJECT_INCLUDE_DIR}" "${PROJECT_PLUGIN_DIR}" ${OpenCV_INCLUDE_DIRS})
target_link_libraries(camera_algorithm_processor_test PRIVATE
    camera_algorithm_runtime camera_task_storage ${OpenCV_LIBS})
add_test(NAME camera_algorithm_processor_test COMMAND camera_algorithm_processor_test)
set_tests_properties(camera_algorithm_processor_test PROPERTIES SKIP_RETURN_CODE 77)

add_executable(callback_delivery_worker_test tests/callback_delivery_worker_test.cpp)
target_include_directories(callback_delivery_worker_test PRIVATE "${PROJECT_INCLUDE_DIR}")
target_link_libraries(callback_delivery_worker_test PRIVATE
    callback_delivery_runtime camera_task_storage OpenSSL::Crypto)
add_test(NAME callback_delivery_worker_test COMMAND callback_delivery_worker_test)
set_tests_properties(callback_delivery_worker_test PROPERTIES SKIP_RETURN_CODE 77)

add_executable(callback_http_transport_test tests/callback_http_transport_test.cpp)
target_include_directories(callback_http_transport_test PRIVATE "${PROJECT_INCLUDE_DIR}")
target_link_libraries(callback_http_transport_test PRIVATE callback_delivery_runtime)
if(WIN32)
    target_link_libraries(callback_http_transport_test PRIVATE ws2_32)
endif()
add_test(NAME callback_http_transport_test COMMAND callback_http_transport_test)

add_executable(algorithm_runtime_heartbeat_test
    tests/algorithm_runtime_heartbeat_test.cpp)
target_include_directories(algorithm_runtime_heartbeat_test PRIVATE
    "${PROJECT_INCLUDE_DIR}")
target_link_libraries(algorithm_runtime_heartbeat_test PRIVATE
    people_flow_redis app_support)
add_test(NAME algorithm_runtime_heartbeat_test
    COMMAND algorithm_runtime_heartbeat_test)
set_tests_properties(algorithm_runtime_heartbeat_test PROPERTIES
    SKIP_RETURN_CODE 77)

add_executable(camera_task_lease_fence_test
    tests/camera_task_lease_fence_test.cpp)
target_include_directories(camera_task_lease_fence_test PRIVATE
    "${PROJECT_INCLUDE_DIR}")
target_link_libraries(camera_task_lease_fence_test PRIVATE
    camera_task_redis)
add_test(NAME camera_task_lease_fence_test
    COMMAND camera_task_lease_fence_test)
set_tests_properties(camera_task_lease_fence_test PROPERTIES
    SKIP_RETURN_CODE 77)

add_executable(algorithm_service_integration_test
    tests/algorithm_service_integration_test.cpp)
target_include_directories(algorithm_service_integration_test PRIVATE
    "${PROJECT_INCLUDE_DIR}" "${PROJECT_PLUGIN_DIR}" ${OpenCV_INCLUDE_DIRS})
target_link_libraries(algorithm_service_integration_test PRIVATE
    camera_task_http camera_algorithm_runtime callback_delivery_runtime
    ${OpenCV_LIBS})
if(WIN32)
    target_link_libraries(algorithm_service_integration_test PRIVATE ws2_32)
endif()
add_test(NAME algorithm_service_integration_test
    COMMAND algorithm_service_integration_test)
set_tests_properties(algorithm_service_integration_test PROPERTIES
    SKIP_RETURN_CODE 77)

add_executable(camera_task_repository_test tests/camera_task_repository_test.cpp)
target_include_directories(camera_task_repository_test PRIVATE "${PROJECT_INCLUDE_DIR}")
target_link_libraries(camera_task_repository_test PRIVATE camera_task_storage)
add_test(NAME camera_task_repository_test COMMAND camera_task_repository_test)
set_tests_properties(camera_task_repository_test PROPERTIES SKIP_RETURN_CODE 77)

add_executable(camera_frame_extraction_test tests/camera_frame_extraction_test.cpp)
target_include_directories(camera_frame_extraction_test PRIVATE "${PROJECT_INCLUDE_DIR}" ${OpenCV_INCLUDE_DIRS})
target_link_libraries(camera_frame_extraction_test PRIVATE camera_task_runtime ${OpenCV_LIBS})
add_test(NAME camera_frame_extraction_test COMMAND camera_frame_extraction_test)
set_tests_properties(camera_frame_extraction_test PROPERTIES SKIP_RETURN_CODE 77)

add_executable(camera_storage_policy_test tests/camera_storage_policy_test.cpp)
target_include_directories(camera_storage_policy_test PRIVATE "${PROJECT_INCLUDE_DIR}" ${OpenCV_INCLUDE_DIRS})
target_link_libraries(camera_storage_policy_test PRIVATE camera_task_runtime ${OpenCV_LIBS})
add_test(NAME camera_storage_policy_test COMMAND camera_storage_policy_test)
set_tests_properties(camera_storage_policy_test PROPERTIES SKIP_RETURN_CODE 77)

add_executable(camera_frame_resilience_test tests/camera_frame_resilience_test.cpp)
target_include_directories(camera_frame_resilience_test PRIVATE "${PROJECT_INCLUDE_DIR}" ${OpenCV_INCLUDE_DIRS})
target_link_libraries(camera_frame_resilience_test PRIVATE camera_capture_runtime ${OpenCV_LIBS})
add_test(NAME camera_frame_resilience_test COMMAND camera_frame_resilience_test)

add_executable(camera_task_http_contract_test tests/camera_task_http_contract_test.cpp)
target_include_directories(camera_task_http_contract_test PRIVATE "${PROJECT_INCLUDE_DIR}")
target_link_libraries(camera_task_http_contract_test PRIVATE
    camera_task_http nlohmann_json::nlohmann_json)
add_test(NAME camera_task_http_contract_test COMMAND camera_task_http_contract_test)
set_tests_properties(camera_task_http_contract_test PROPERTIES SKIP_RETURN_CODE 77)

# Hardware smoke target; intentionally not part of CTest so CI without an
# NVIDIA GPU can still run all deterministic tests.
add_executable(pose_engine_smoke tests/pose_engine_smoke.cpp)
target_include_directories(pose_engine_smoke PRIVATE "${PROJECT_INCLUDE_DIR}")
target_link_libraries(pose_engine_smoke PRIVATE pose_runtime myplugins)
add_dependencies(pose_engine_smoke myplugins)
add_custom_command(TARGET pose_engine_smoke POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:myplugins> $<TARGET_FILE_DIR:pose_engine_smoke>)

# Real-RTSP smoke target; intentionally not part of CTest because it requires
# a reachable camera and its URI must be supplied through the configured env.
add_executable(rtsp_capture_smoke tests/rtsp_capture_smoke.cpp)
target_include_directories(rtsp_capture_smoke PRIVATE "${PROJECT_INCLUDE_DIR}" ${OpenCV_INCLUDE_DIRS})
target_link_libraries(rtsp_capture_smoke PRIVATE camera_capture_runtime ${OpenCV_LIBS})

# Hardware interop smoke: keeps the production TensorRT pose engine alive while
# the shared FrameHub opens and decodes RTSP.
add_executable(pose_rtsp_interop_smoke tests/pose_rtsp_interop_smoke.cpp)
target_include_directories(pose_rtsp_interop_smoke PRIVATE
    "${PROJECT_INCLUDE_DIR}" "${PROJECT_PLUGIN_DIR}" ${OpenCV_INCLUDE_DIRS})
target_link_libraries(pose_rtsp_interop_smoke PRIVATE
    app_support camera_capture_runtime pose_runtime myplugins ${OpenCV_LIBS})
add_dependencies(pose_rtsp_interop_smoke myplugins)
add_custom_command(TARGET pose_rtsp_interop_smoke POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:myplugins> $<TARGET_FILE_DIR:pose_rtsp_interop_smoke>)

install(TARGETS four_stage_server four_stage_worker RUNTIME DESTINATION bin)
install(FILES engines/yolo11n-pose.engine DESTINATION engines)
install(FILES config/server.yaml config/worker.yaml config/cameras.yaml DESTINATION config)
install(DIRECTORY web/camera-admin DESTINATION web)
