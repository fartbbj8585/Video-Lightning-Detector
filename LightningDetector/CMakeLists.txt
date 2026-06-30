cmake_minimum_required(VERSION 3.20)
project(LightningDetector VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

#  Find packages (installed via vcpkg) 
find_package(OpenCV REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

#  Source files 
set(SOURCES
    src/LightningDetector.cpp
    src/main_gui.cpp
)

#  Executable (WIN32 = no console window) 
add_executable(LightningDetector WIN32 ${SOURCES})

# Statically link the MSVC C/C++ runtime into the .exe so end users do NOT
# need the Visual C++ Redistributable installed - the installer can then
# be a single self-contained package with zero external downloads.
set_property(TARGET LightningDetector PROPERTY
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

#  Include paths 
target_include_directories(LightningDetector PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${OpenCV_INCLUDE_DIRS}
)

#  Link libraries 
target_link_libraries(LightningDetector PRIVATE
    ${OpenCV_LIBS}
    nlohmann_json::nlohmann_json
    # Windows system libraries
    comctl32
    comdlg32
    ole32
    shell32
    gdi32
    user32
    msimg32
)

#  Compiler flags 
if(MSVC)
    target_compile_options(LightningDetector PRIVATE
        /W3          # Warning level 3
        /O2          # Full optimisation
        /MP          # Parallel compilation
        /EHsc        # C++ exceptions
        /utf-8       # Source files are UTF-8
    )
    # Suppress some MSVC warnings that fire in OpenCV/nlohmann headers
    target_compile_definitions(LightningDetector PRIVATE
        _CRT_SECURE_NO_WARNINGS
        NOMINMAX
    )
endif()

# Note: DLL copying is handled entirely by build.ps1 after the build
# completes (avoids PowerShell quoting issues inside MSBuild custom commands).

#  Install target 
install(TARGETS LightningDetector
    RUNTIME DESTINATION bin
)
