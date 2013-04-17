set(SOURCES ConsoleUPowerSyscall.cpp
            UPowerSyscall.cpp
            LogindUPowerSyscall.cpp)

set(HEADERS ConsoleUPowerSyscall.h
            FallbackPowerSyscall.h
            LogindUPowerSyscall.h
            UPowerSyscall.h)

if(MMAL_FOUND)
  list(APPEND SOURCES RaspberryPIPowerSyscall.cpp)
  list(APPEND HEADERS RaspberryPIPowerSyscall.h)
endif()

core_add_library(powermanagement_linux)
