add_library(eqdsp STATIC
    src/BiquadDesign.cpp
    src/Equalizer.cpp
    src/AdaptiveResampler.cpp
    src/PresetParser.cpp
)

target_include_directories(eqdsp PUBLIC include)
