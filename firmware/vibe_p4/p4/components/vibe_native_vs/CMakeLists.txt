# One Vibe compiled natively with its own copy of the SDK; its globals renamed vs_* (gen/prefix_vs.h) so both
# native Vibes link into one image. Same flags as the SDK's tools/build.sh uses for the .wasm: -O2 -ffast-math.
set(H ${CMAKE_CURRENT_LIST_DIR}/../../../host)
set(G ${CMAKE_CURRENT_LIST_DIR}/../../../gen)
set(FS ${CMAKE_CURRENT_LIST_DIR}/../../../faust-synth)
if (NOT DEFINED VH_NATIVE_PSRAM OR VH_NATIVE_PSRAM)
  set(VH_LDF linker.lf)
else ()
  set(VH_LDF "")
endif ()
idf_component_register(SRCS ${VIBE_SDK}/examples/example-synth/synth.c ${VIBE_SDK}/sdk/src/vibe_sdk.c ${VIBE_SDK}/sdk/src/vibe_font.c ${H}/vh_native.c
                       INCLUDE_DIRS "."
                       PRIV_INCLUDE_DIRS ${VIBE_SDK}/sdk/include ${VIBE_SDK}/sdk/src ${H}
                       LDFRAGMENTS ${VH_LDF})
target_compile_options(${COMPONENT_LIB} PRIVATE -O2 -ffast-math -fno-builtin-memset -w -include ${G}/prefix_vs.h)
target_compile_definitions(${COMPONENT_LIB} PRIVATE VH_NATIVE_ENTRY=vh_native_vibe_synth VH_NATIVE_NAME="native-synth")
