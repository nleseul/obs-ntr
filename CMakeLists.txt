cmake_minimum_required (VERSION 2.8.12)

foreach(flag_var
        CMAKE_C_FLAGS CMAKE_C_FLAGS_DEBUG CMAKE_C_FLAGS_RELEASE
        CMAKE_C_FLAGS_MINSIZEREL CMAKE_C_FLAGS_RELWITHDEBINFO)
   if(${flag_var} MATCHES "/MD")
      string(REGEX REPLACE "/MD" "/MT" ${flag_var} "${${flag_var}}")
   endif(${flag_var} MATCHES "/MD")
endforeach(flag_var)

project (obs-ntr)

if (OBSSourcePath OR DEFINED ENV{OBSSourcePath})
    # Set already
else()
    set(OBSSourcePath "" CACHE PATH "Path to OBS source code (e.g., C:/Dev/obs-studio/libobs/)")
    message("OBSSourcePath is missing. Please set this variable to the location of the OBS source (e.g., C:/Dev/obs-studio/libobs/).")
endif()

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
	set(_lib_suffix 64)
else()
	set(_lib_suffix 32)
endif()

find_path(OBS_LIB_DIR
    NAMES obs.dll obs.lib
    HINTS
        ${OBSSourcePath}/../build${_lib_suffix}/libobs/Release/
        ${OBSSourcePath}/../build/libobs/Release/
        ${OBSSourcePath}/../build${_lib_suffix}/libobs/Debug/
        ${OBSSourcePath}/../build/libobs/Debug/
    PATHS
		/usr/lib /usr/local/lib /opt/local/lib /sw/lib)
        
find_path(OBS_W32_PTHREADS_LIB_DIR
    NAMES w32-pthreads.dll w32-pthreads.lib
    HINTS
        ${OBSSourcePath}/../build${_lib_suffix}/deps/w32-pthreads//Release/
        ${OBSSourcePath}/../build/deps/w32-pthreads//Release/
        ${OBSSourcePath}/../build${_lib_suffix}/deps/w32-pthreads//Debug/
        ${OBSSourcePath}/../build/deps/w32-pthreads//Debug/)
        
find_path(TURBOJPEG_INCLUDE_DIR
    NAMES turbojpeg.h
    HINTS ${TurboJPEGPath}/include
    PATHS
        /usr/include /usr/local/include /opt/local/include /sw/include)
        
find_path(TURBOJPEG_LIB_DIR
    NAMES turbojpeg.lib
    HINTS
        ${TurboJPEGPath}/lib
    PATHS
        /usr/lib /usr/local/lib /opt/local/lib /sw/lib)
        
find_path(TURBOJPEG_BIN_DIR
    NAMES turbojpeg.dll
    HINTS
        ${TurboJPEGPath}/bin
    PATHS
        /usr/bin /usr/local/bin /opt/local/bin /sw/bin)
        
if (NOT TURBOJPEG_INCLUDE_DIR OR NOT TURBOJPEG_LIB_DIR OR NOT TURBOJPEG_BIN_DIR)
    message("TurboJPEG headers or libraries could not be found! Please ensure that TurboJPEG is installed somewhere, and set the TurboJPEGPath variable if necessary.")
endif()

# Source
file (GLOB SOURCES ${CMAKE_SOURCE_DIR}/src/*.c)

include_directories (include ${CMAKE_BINARY_DIR}/config)
add_library (${PROJECT_NAME} SHARED
	${SOURCES}
	${HEADER_FILES}
)

# libobs
include_directories(${OBSSourcePath})
add_library (libobs SHARED IMPORTED)
set_property (TARGET libobs PROPERTY IMPORTED_LOCATION ${OBS_LIB_DIR}/obs.dll)
set_property (TARGET libobs PROPERTY IMPORTED_IMPLIB ${OBS_LIB_DIR}/obs.lib)
add_library (obs_w32_pthreads SHARED IMPORTED)
set_property (TARGET obs_w32_pthreads PROPERTY IMPORTED_LOCATION ${OBS_W32_PTHREADS_LIB_DIR}/w32-pthreads.dll)
set_property (TARGET obs_w32_pthreads PROPERTY IMPORTED_IMPLIB ${OBS_W32_PTHREADS_LIB_DIR}/w32-pthreads.lib)
target_link_libraries (${PROJECT_NAME} libobs obs_w32_pthreads)

# winsock
target_link_libraries (${PROJECT_NAME} wsock32 ws2_32)

# turbojpeg
include_directories(${TURBOJPEG_INCLUDE_DIR})
add_library (turbojpeg SHARED IMPORTED)
set_property(TARGET turbojpeg PROPERTY IMPORTED_LOCATION ${TURBOJPEG_BIN_DIR}/turbojpeg.dll)
set_property(TARGET turbojpeg PROPERTY IMPORTED_IMPLIB ${TURBOJPEG_LIB_DIR}/turbojpeg.lib)
target_link_libraries(${PROJECT_NAME} turbojpeg)

install(TARGETS ${PROJECT_NAME} RUNTIME DESTINATION obs-plugins/${_lib_suffix}bit)
install(FILES ${CMAKE_BINARY_DIR}/Debug/${PROJECT_NAME}.pdb DESTINATION obs-plugins/${_lib_suffix}bit CONFIGURATIONS Debug)
install(DIRECTORY data/ DESTINATION data/obs-plugins/${PROJECT_NAME}/)
