cmake_minimum_required(VERSION 3.20)
project(urti CXX)
set(CMAKE_CXX_STANDARD 17)
add_compile_options(/utf-8 /W3)
add_compile_definitions(NOMINMAX _CRT_SECURE_NO_WARNINGS WIN32_LEAN_AND_MEAN)

# application de test DirectX 12 (géométrie connue)
add_executable(urti_testapp testapp/main.cpp)
target_link_libraries(urti_testapp d3d12 dxgi d3dcompiler)

# add-on ReShade (.addon64), avec les shaders du mode direct compilés par dxc
set(URTI_DXC "F:/rtxgi/sdk/build/_deps/dxc-src/bin/x64/dxc.exe" CACHE FILEPATH "compilateur de shaders dxc")
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/live_convert_cs.h ${CMAKE_CURRENT_BINARY_DIR}/live_shadow_cs.h ${CMAKE_CURRENT_BINARY_DIR}/live_sunfit_cs.h ${CMAKE_CURRENT_BINARY_DIR}/live_sunvote_cs.h
    COMMAND ${URTI_DXC} -nologo -T cs_6_0 -E main -Vn g_live_convert -Fh ${CMAKE_CURRENT_BINARY_DIR}/live_convert_cs.h ${CMAKE_CURRENT_SOURCE_DIR}/addon/live_convert.hlsl
    COMMAND ${URTI_DXC} -nologo -T cs_6_5 -E main -Vn g_live_shadow -Fh ${CMAKE_CURRENT_BINARY_DIR}/live_shadow_cs.h ${CMAKE_CURRENT_SOURCE_DIR}/addon/live_shadow.hlsl
    COMMAND ${URTI_DXC} -nologo -T cs_6_0 -E main -Vn g_live_sunfit -Fh ${CMAKE_CURRENT_BINARY_DIR}/live_sunfit_cs.h ${CMAKE_CURRENT_SOURCE_DIR}/addon/live_sunfit.hlsl
    COMMAND ${URTI_DXC} -nologo -T cs_6_0 -E main -Vn g_live_sunvote -Fh ${CMAKE_CURRENT_BINARY_DIR}/live_sunvote_cs.h ${CMAKE_CURRENT_SOURCE_DIR}/addon/live_sunvote.hlsl
    DEPENDS addon/live_convert.hlsl addon/live_shadow.hlsl addon/live_sunfit.hlsl addon/live_sunvote.hlsl)
add_library(urti_addon SHARED addon/urti_addon.cpp addon/urti_live.inl ${CMAKE_CURRENT_BINARY_DIR}/live_convert_cs.h ${CMAKE_CURRENT_BINARY_DIR}/live_shadow_cs.h ${CMAKE_CURRENT_BINARY_DIR}/live_sunfit_cs.h ${CMAKE_CURRENT_BINARY_DIR}/live_sunvote_cs.h)
target_include_directories(urti_addon PRIVATE third_party/reshade/include third_party/imgui ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(urti_addon d3d12)
set_target_properties(urti_addon PROPERTIES OUTPUT_NAME urti SUFFIX ".addon64")

# ombres du soleil en ray tracing sur une capture (outil hors ligne, DXR 1.1)
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/rtshadow_cs.h
    COMMAND ${URTI_DXC} -nologo -T cs_6_5 -E main -Vn g_main -Fh ${CMAKE_CURRENT_BINARY_DIR}/rtshadow_cs.h ${CMAKE_CURRENT_SOURCE_DIR}/tools/rtshadow.hlsl
    DEPENDS tools/rtshadow.hlsl)
add_executable(urti_rtshadow tools/rtshadow.cpp ${CMAKE_CURRENT_BINARY_DIR}/rtshadow_cs.h)
target_include_directories(urti_rtshadow PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(urti_rtshadow d3d12)
