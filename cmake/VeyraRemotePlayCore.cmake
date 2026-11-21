include_guard(GLOBAL)
find_package(Threads REQUIRED)
add_library(veyra_remoteplay_core STATIC
    "${VEYRA_ROOT}/src/remoteplay/Validation.cpp"
    "${VEYRA_ROOT}/src/remoteplay/AnnexB.cpp"
    "${VEYRA_ROOT}/src/remoteplay/Timeline.cpp"
    "${VEYRA_ROOT}/src/remoteplay/VideoIngress.cpp"
    "${VEYRA_ROOT}/src/remoteplay/SessionInbox.cpp")
target_include_directories(veyra_remoteplay_core PUBLIC "${VEYRA_ROOT}/include")
target_compile_features(veyra_remoteplay_core PUBLIC cxx_std_20)
set_target_properties(veyra_remoteplay_core PROPERTIES CXX_EXTENSIONS OFF)
target_link_libraries(veyra_remoteplay_core PUBLIC Threads::Threads)
