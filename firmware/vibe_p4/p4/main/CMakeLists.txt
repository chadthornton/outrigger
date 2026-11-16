set(H ${CMAKE_CURRENT_LIST_DIR}/../../host)
idf_component_register(SRCS bench_main.c ${H}/vh_scenario.c ${H}/vh_wamr.c ${H}/vh_native_host.c
                       INCLUDE_DIRS . ${H} ${VH_MODULES}
                       REQUIRES wamr pthread vibe_native_vs vibe_native_fs esp_driver_uart esp_timer esp_hw_support efuse
                       EMBED_FILES ${VH_MODULES}/example-synth.wasm ${VH_MODULES}/faust-synth.wasm
                                   ${VH_MODULES}/example-synth.aot ${VH_MODULES}/faust-synth.aot)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-format-truncation)
if (NOT DEFINED VH_NATIVE_PSRAM OR VH_NATIVE_PSRAM)
  target_compile_definitions(${COMPONENT_LIB} PRIVATE VH_NATIVE_BSS="psram")
else ()
  target_compile_definitions(${COMPONENT_LIB} PRIVATE VH_NATIVE_BSS="internal")
endif ()
# WAMR's component exports WASM_ENABLE_AOT/INTERP only; tell vh_wamr.c which interpreter this image has
if (CONFIG_WAMR_INTERP_FAST)
  target_compile_definitions(${COMPONENT_LIB} PRIVATE WASM_ENABLE_FAST_INTERP=1)
endif ()
