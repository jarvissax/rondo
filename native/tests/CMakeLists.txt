# N0's tests (private/native-plan.md): small programs run on the Y1 with the Java layer stopped
add_executable(s1 s1-display/s1.cpp)
y1_private(s1)
target_link_libraries(s1 PRIVATE y1_gui y1_ui y1_binder y1_utils y1_cutils EGL GLESv2)

add_executable(hwvsync s1-display/hwvsync.cpp)
add_executable(s1fb s1-display/s1fb.cpp)
y1_private(s1fb)
target_link_libraries(s1fb PRIVATE y1_ui y1_utils y1_cutils EGL GLESv2)

add_executable(s5 s5-audio/s5.cpp)
target_link_libraries(s5 PRIVATE OpenSLES)

add_executable(s2 s2-video/s2.cpp)
y1_private(s2)
target_link_libraries(s2 PRIVATE y1_stagefright y1_stagefright_foundation y1_surfaceflinger y1_gui y1_ui y1_binder y1_utils y1_cutils EGL GLESv2)
