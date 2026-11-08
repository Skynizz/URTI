cmake_minimum_required(VERSION 3.20)
project(urti CXX)
set(CMAKE_CXX_STANDARD 17)
add_compile_options(/utf-8 /W3)
add_compile_definitions(NOMINMAX _CRT_SECURE_NO_WARNINGS WIN32_LEAN_AND_MEAN)

# application de test DirectX 12 (géométrie connue)
add_executable(urti_testapp testapp/main.cpp)
target_link_libraries(urti_testapp d3d12 dxgi d3dcompiler)
add_executable(urti_sandbox sandbox/main.cpp)
target_link_libraries(urti_sandbox d3d12 dxgi d3dcompiler)

# add-on ReShade (.addon64), avec les shaders du mode direct compilés par dxc
set(URTI_DXC "F:/rtxgi/sdk/build/_deps/dxc-src/bin/x64/dxc.exe" CACHE FILEPATH "compilateur de shaders dxc")
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/live_convert_cs.h ${CMAKE_CURRENT_BINARY_DIR}/live_shadow_cs.h ${CMAKE_CURRENT_BINARY_DIR}/live_sunfit_cs.h ${CMAKE_CURRENT_BINARY_DIR}/live_sunvote_cs.h ${CMAKE_CURRENT_BINARY_DIR}/live_composite_cs.h ${CMAKE_CURRENT_BINARY_DIR}/live_reflect_cs.h ${CMAKE_CURRENT_BINARY_DIR}/live_depthfit_cs.h ${CMAKE_CURRENT_BINARY_DIR}/live_mvcal_cs.h ${CMAKE_CURRENT_BINARY_DIR}/live_reflresolve_cs.h ${CMAKE_CURRENT_BINARY_DIR}/live_shadowtemporal_cs.h ${CMAKE_CURRENT_BINARY_DIR}/live_regionstats_cs.h ${CMAKE_CURRENT_BINARY_DIR}/live_uicompare_cs.h ${CMAKE_CURRENT_BINARY_DIR}/live_uiselect_cs.h ${CMAKE_CURRENT_BINARY_DIR}/live_sunlight_cs.h ${CMAKE_CURRENT_BINARY_DIR}/live_counts_cs.h ${CMAKE_CURRENT_BINARY_DIR}/live_surfcache_cs.h ${CMAKE_CURRENT_BINARY_DIR}/live_camfit_cs.h ${CMAKE_CURRENT_BINARY_DIR}/live_mvsynth_cs.h ${CMAKE_CURRENT_BINARY_DIR}/live_planes_cs.h ${CMAKE_CURRENT_BINARY_DIR}/live_geoconvert_cs.h ${CMAKE_CURRENT_BINARY_DIR}/live_geoinst_cs.h ${CMAKE_CURRENT_BINARY_DIR}/live_refpairs_cs.h ${CMAKE_CURRENT_BINARY_DIR}/live_objplanes_cs.h ${CMAKE_CURRENT_BINARY_DIR}/live_hudless_cs.h ${CMAKE_CURRENT_BINARY_DIR}/live_hudgain_cs.h ${CMAKE_CURRENT_BINARY_DIR}/live_film_cs.h
    COMMAND ${URTI_DXC} -nologo -T cs_6_0 -E main -Vn g_live_convert -Fh ${CMAKE_CURRENT_BINARY_DIR}/live_convert_cs.h ${CMAKE_CURRENT_SOURCE_DIR}/addon/live_convert.hlsl
    COMMAND ${URTI_DXC} -nologo -T cs_6_5 -E main -Vn g_live_shadow -Fh ${CMAKE_CURRENT_BINARY_DIR}/live_shadow_cs.h ${CMAKE_CURRENT_SOURCE_DIR}/addon/live_shadow.hlsl
    COMMAND ${URTI_DXC} -nologo -T cs_6_0 -E main -Vn g_live_sunfit -Fh ${CMAKE_CURRENT_BINARY_DIR}/live_sunfit_cs.h ${CMAKE_CURRENT_SOURCE_DIR}/addon/live_sunfit.hlsl
    COMMAND ${URTI_DXC} -nologo -T cs_6_0 -E main -Vn g_live_sunvote -Fh ${CMAKE_CURRENT_BINARY_DIR}/live_sunvote_cs.h ${CMAKE_CURRENT_SOURCE_DIR}/addon/live_sunvote.hlsl
    COMMAND ${URTI_DXC} -nologo -T cs_6_0 -E main -Vn g_live_composite -Fh ${CMAKE_CURRENT_BINARY_DIR}/live_composite_cs.h ${CMAKE_CURRENT_SOURCE_DIR}/addon/live_composite.hlsl
    COMMAND ${URTI_DXC} -nologo -T cs_6_0 -E main -Vn g_live_shadowtemporal -Fh ${CMAKE_CURRENT_BINARY_DIR}/live_shadowtemporal_cs.h ${CMAKE_CURRENT_SOURCE_DIR}/addon/live_shadowtemporal.hlsl
    COMMAND ${URTI_DXC} -nologo -T cs_6_0 -E main -Vn g_live_reflresolve -Fh ${CMAKE_CURRENT_BINARY_DIR}/live_reflresolve_cs.h ${CMAKE_CURRENT_SOURCE_DIR}/addon/live_reflresolve.hlsl
    COMMAND ${URTI_DXC} -nologo -T cs_6_0 -E main -Vn g_live_uiselect -Fh ${CMAKE_CURRENT_BINARY_DIR}/live_uiselect_cs.h ${CMAKE_CURRENT_SOURCE_DIR}/addon/live_uiselect.hlsl
    COMMAND ${URTI_DXC} -nologo -T cs_6_0 -E main -Vn g_live_hudless -Fh ${CMAKE_CURRENT_BINARY_DIR}/live_hudless_cs.h ${CMAKE_CURRENT_SOURCE_DIR}/addon/live_hudless.hlsl
    COMMAND ${URTI_DXC} -nologo -T cs_6_0 -E main -Vn g_live_hudgain -Fh ${CMAKE_CURRENT_BINARY_DIR}/live_hudgain_cs.h ${CMAKE_CURRENT_SOURCE_DIR}/addon/live_hudgain.hlsl
    COMMAND ${URTI_DXC} -nologo -T cs_6_0 -E main -Vn g_live_film -Fh ${CMAKE_CURRENT_BINARY_DIR}/live_film_cs.h ${CMAKE_CURRENT_SOURCE_DIR}/addon/live_film.hlsl
    COMMAND ${URTI_DXC} -nologo -T cs_6_0 -E main -Vn g_live_sunlight -Fh ${CMAKE_CURRENT_BINARY_DIR}/live_sunlight_cs.h ${CMAKE_CURRENT_SOURCE_DIR}/addon/live_sunlight.hlsl
    COMMAND ${URTI_DXC} -nologo -T cs_6_0 -E main -Vn g_live_counts -Fh ${CMAKE_CURRENT_BINARY_DIR}/live_counts_cs.h ${CMAKE_CURRENT_SOURCE_DIR}/addon/live_counts.hlsl
    COMMAND ${URTI_DXC} -nologo -T cs_6_0 -E main -Vn g_live_uicompare -Fh ${CMAKE_CURRENT_BINARY_DIR}/live_uicompare_cs.h ${CMAKE_CURRENT_SOURCE_DIR}/addon/live_uicompare.hlsl
    COMMAND ${URTI_DXC} -nologo -T cs_6_0 -E main -Vn g_live_regionstats -Fh ${CMAKE_CURRENT_BINARY_DIR}/live_regionstats_cs.h ${CMAKE_CURRENT_SOURCE_DIR}/addon/live_regionstats.hlsl
    COMMAND ${URTI_DXC} -nologo -T cs_6_0 -E main -Vn g_live_mvcal -Fh ${CMAKE_CURRENT_BINARY_DIR}/live_mvcal_cs.h ${CMAKE_CURRENT_SOURCE_DIR}/addon/live_mvcal.hlsl
    COMMAND ${URTI_DXC} -nologo -T cs_6_0 -E main -Vn g_live_depthfit -Fh ${CMAKE_CURRENT_BINARY_DIR}/live_depthfit_cs.h ${CMAKE_CURRENT_SOURCE_DIR}/addon/live_depthfit.hlsl
    COMMAND ${URTI_DXC} -nologo -T cs_6_5 -E main -Vn g_live_reflect -Fh ${CMAKE_CURRENT_BINARY_DIR}/live_reflect_cs.h ${CMAKE_CURRENT_SOURCE_DIR}/addon/live_reflect.hlsl
    COMMAND ${URTI_DXC} -nologo -T cs_6_5 -E main -Vn g_live_surfcache -Fh ${CMAKE_CURRENT_BINARY_DIR}/live_surfcache_cs.h ${CMAKE_CURRENT_SOURCE_DIR}/addon/live_surfcache.hlsl
    COMMAND ${URTI_DXC} -nologo -T cs_6_0 -E main -Vn g_live_camfit -Fh ${CMAKE_CURRENT_BINARY_DIR}/live_camfit_cs.h ${CMAKE_CURRENT_SOURCE_DIR}/addon/live_camfit.hlsl
    COMMAND ${URTI_DXC} -nologo -T cs_6_0 -E main -Vn g_live_mvsynth -Fh ${CMAKE_CURRENT_BINARY_DIR}/live_mvsynth_cs.h ${CMAKE_CURRENT_SOURCE_DIR}/addon/live_mvsynth.hlsl
    COMMAND ${URTI_DXC} -nologo -T cs_6_0 -E main -Vn g_live_planes -Fh ${CMAKE_CURRENT_BINARY_DIR}/live_planes_cs.h ${CMAKE_CURRENT_SOURCE_DIR}/addon/live_planes.hlsl
    COMMAND ${URTI_DXC} -nologo -T cs_6_0 -E main -Vn g_live_geoconvert -Fh ${CMAKE_CURRENT_BINARY_DIR}/live_geoconvert_cs.h ${CMAKE_CURRENT_SOURCE_DIR}/addon/live_geoconvert.hlsl
    COMMAND ${URTI_DXC} -nologo -T cs_6_0 -E main -Vn g_live_geoinst -Fh ${CMAKE_CURRENT_BINARY_DIR}/live_geoinst_cs.h ${CMAKE_CURRENT_SOURCE_DIR}/addon/live_geoinst.hlsl
    COMMAND ${URTI_DXC} -nologo -T cs_6_0 -E main -Vn g_live_refpairs -Fh ${CMAKE_CURRENT_BINARY_DIR}/live_refpairs_cs.h ${CMAKE_CURRENT_SOURCE_DIR}/addon/live_refpairs.hlsl
    COMMAND ${URTI_DXC} -nologo -T cs_6_0 -E main -Vn g_live_objplanes -Fh ${CMAKE_CURRENT_BINARY_DIR}/live_objplanes_cs.h ${CMAKE_CURRENT_SOURCE_DIR}/addon/live_objplanes.hlsl
    DEPENDS addon/live_convert.hlsl addon/live_shadow.hlsl addon/live_sunfit.hlsl addon/live_sunvote.hlsl addon/live_composite.hlsl addon/live_reflect.hlsl addon/live_depthfit.hlsl addon/live_mvcal.hlsl addon/live_reflresolve.hlsl addon/live_shadowtemporal.hlsl addon/live_regionstats.hlsl addon/live_uicompare.hlsl addon/live_uiselect.hlsl addon/live_sunlight.hlsl addon/live_counts.hlsl addon/live_surfcache.hlsl addon/live_camfit.hlsl addon/live_mvsynth.hlsl addon/live_planes.hlsl addon/live_geoconvert.hlsl addon/live_geoinst.hlsl addon/live_refpairs.hlsl addon/live_objplanes.hlsl addon/live_hudless.hlsl addon/live_hudgain.hlsl addon/live_film.hlsl)
add_library(urti_addon SHARED addon/urti_addon.cpp addon/urti_live.inl ${CMAKE_CURRENT_BINARY_DIR}/live_convert_cs.h ${CMAKE_CURRENT_BINARY_DIR}/live_shadow_cs.h ${CMAKE_CURRENT_BINARY_DIR}/live_sunfit_cs.h ${CMAKE_CURRENT_BINARY_DIR}/live_sunvote_cs.h ${CMAKE_CURRENT_BINARY_DIR}/live_composite_cs.h ${CMAKE_CURRENT_BINARY_DIR}/live_reflect_cs.h ${CMAKE_CURRENT_BINARY_DIR}/live_depthfit_cs.h ${CMAKE_CURRENT_BINARY_DIR}/live_mvcal_cs.h ${CMAKE_CURRENT_BINARY_DIR}/live_reflresolve_cs.h ${CMAKE_CURRENT_BINARY_DIR}/live_shadowtemporal_cs.h ${CMAKE_CURRENT_BINARY_DIR}/live_regionstats_cs.h ${CMAKE_CURRENT_BINARY_DIR}/live_uicompare_cs.h ${CMAKE_CURRENT_BINARY_DIR}/live_uiselect_cs.h ${CMAKE_CURRENT_BINARY_DIR}/live_sunlight_cs.h ${CMAKE_CURRENT_BINARY_DIR}/live_counts_cs.h ${CMAKE_CURRENT_BINARY_DIR}/live_surfcache_cs.h ${CMAKE_CURRENT_BINARY_DIR}/live_camfit_cs.h ${CMAKE_CURRENT_BINARY_DIR}/live_mvsynth_cs.h ${CMAKE_CURRENT_BINARY_DIR}/live_planes_cs.h ${CMAKE_CURRENT_BINARY_DIR}/live_geoconvert_cs.h ${CMAKE_CURRENT_BINARY_DIR}/live_geoinst_cs.h ${CMAKE_CURRENT_BINARY_DIR}/live_refpairs_cs.h ${CMAKE_CURRENT_BINARY_DIR}/live_objplanes_cs.h ${CMAKE_CURRENT_BINARY_DIR}/live_hudless_cs.h ${CMAKE_CURRENT_BINARY_DIR}/live_hudgain_cs.h ${CMAKE_CURRENT_BINARY_DIR}/live_film_cs.h)
target_include_directories(urti_addon PRIVATE third_party/reshade/include third_party/imgui ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(urti_addon d3d12)
set_target_properties(urti_addon PROPERTIES OUTPUT_NAME urti SUFFIX ".addon64")
# symboles de débogage aussi en Release (piles d'appels lisibles en cas de plantage)
target_compile_options(urti_addon PRIVATE /Zi)
target_link_options(urti_addon PRIVATE /DEBUG /OPT:REF /OPT:ICF)

# débruiteurs de NVIDIA (NRD 4.17.3 avec NRI : SIGMA pour les ombres), s'ils ont été compilés dans
# third_party/NRD (licence NVIDIA RTX SDK, hors du dépôt ; voir JOURNAL v39)
set(URTI_NRD_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/NRD)
if(EXISTS ${URTI_NRD_DIR}/_Bin/Release/NRD.lib)
    add_custom_command(
        OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/live_nrdprep_cs.h ${CMAKE_CURRENT_BINARY_DIR}/live_nrdpost_cs.h
        COMMAND ${URTI_DXC} -nologo -T cs_6_0 -E main -Vn g_live_nrdprep -I ${URTI_NRD_DIR}/Shaders -Fh ${CMAKE_CURRENT_BINARY_DIR}/live_nrdprep_cs.h ${CMAKE_CURRENT_SOURCE_DIR}/addon/live_nrdprep.hlsl
        COMMAND ${URTI_DXC} -nologo -T cs_6_0 -E main -Vn g_live_nrdpost -Fh ${CMAKE_CURRENT_BINARY_DIR}/live_nrdpost_cs.h ${CMAKE_CURRENT_SOURCE_DIR}/addon/live_nrdpost.hlsl
        DEPENDS addon/live_nrdprep.hlsl addon/live_nrdpost.hlsl)
    target_sources(urti_addon PRIVATE addon/urti_nrd.inl ${CMAKE_CURRENT_BINARY_DIR}/live_nrdprep_cs.h ${CMAKE_CURRENT_BINARY_DIR}/live_nrdpost_cs.h)
    target_compile_definitions(urti_addon PRIVATE URTI_NRD=1)
    target_include_directories(urti_addon PRIVATE ${URTI_NRD_DIR}/Include ${URTI_NRD_DIR}/Integration ${URTI_NRD_DIR}/_Build/_deps/nri-src/Include)
    file(GLOB URTI_NRI_LIBS ${URTI_NRD_DIR}/_Build/_deps/nri-build/Release/*.lib)
    target_link_libraries(urti_addon ${URTI_NRD_DIR}/_Bin/Release/NRD.lib ${URTI_NRD_DIR}/_Bin/Release/NRI.lib ${URTI_NRI_LIBS}
        ${URTI_NRD_DIR}/_Build/_deps/shadermake-build/Release/ShaderMakeBlob.lib dxgi dxguid dbghelp)
    message(STATUS "URTI : débruiteurs NRD de NVIDIA inclus")
endif()

# ombres du soleil en ray tracing sur une capture (outil hors ligne, DXR 1.1)
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/rtshadow_cs.h
    COMMAND ${URTI_DXC} -nologo -T cs_6_5 -E main -Vn g_main -Fh ${CMAKE_CURRENT_BINARY_DIR}/rtshadow_cs.h ${CMAKE_CURRENT_SOURCE_DIR}/tools/rtshadow.hlsl
    DEPENDS tools/rtshadow.hlsl)
add_executable(urti_rtshadow tools/rtshadow.cpp ${CMAKE_CURRENT_BINARY_DIR}/rtshadow_cs.h)
target_include_directories(urti_rtshadow PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(urti_rtshadow d3d12)
