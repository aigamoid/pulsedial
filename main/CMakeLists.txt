file(GLOB_RECURSE MY_SRCS
    "apps/*.c"
    "apps/*.cc"
    "apps/*.cpp"
    "assets/*.c"
    "assets/*.cc"
    "assets/*.cpp"
    "hal/*.c"
    "hal/*.cc"
    "hal/*.cpp"
)

set(MY_INCS
    "."
)

idf_component_register(
    SRCS 
        "main.cpp"
        ${MY_SRCS}
    INCLUDE_DIRS 
        ${MY_INCS}
    EMBED_FILES
        "assets/sfx/boot_sfx.bin"
    EMBED_TXTFILES
        "hal/utils/config_ap/assets/badge_config_ap.html"
)
# PulseDial: the visualizer and the screen are drawn every frame, so optimize these files for speed (the rest is -Og)
set_source_files_properties(apps/app_pulsedial/SpotifyViz.cpp PROPERTIES COMPILE_OPTIONS "-O2")
set_source_files_properties(apps/app_pulsedial/GlassUi.cpp PROPERTIES COMPILE_OPTIONS "-O2")
