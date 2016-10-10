set(SOURCES PixelConverter.cpp
            PixelConverterRBP.cpp
            RetroPlayer.cpp
            RetroPlayerAudio.cpp
            RetroPlayerVideo.cpp)

set(HEADERS IPixelConverter.h
            PixelConverter.h
            PixelConverterRBP.h
            RetroPlayer.h
            RetroPlayerAudio.h
            RetroPlayerDefines.h
            RetroPlayerVideo.h)

core_add_library(retroplayer)
