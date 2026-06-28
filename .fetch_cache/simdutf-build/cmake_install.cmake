# Install script for directory: /home/oleub/dev/source/repos/StructuredChaos/.fetch_cache/simdutf-src

# Set the install prefix
if(NOT DEFINED CMAKE_INSTALL_PREFIX)
  set(CMAKE_INSTALL_PREFIX "/usr/local")
endif()
string(REGEX REPLACE "/$" "" CMAKE_INSTALL_PREFIX "${CMAKE_INSTALL_PREFIX}")

# Set the install configuration name.
if(NOT DEFINED CMAKE_INSTALL_CONFIG_NAME)
  if(BUILD_TYPE)
    string(REGEX REPLACE "^[^A-Za-z0-9_]+" ""
           CMAKE_INSTALL_CONFIG_NAME "${BUILD_TYPE}")
  else()
    set(CMAKE_INSTALL_CONFIG_NAME "Release")
  endif()
  message(STATUS "Install configuration: \"${CMAKE_INSTALL_CONFIG_NAME}\"")
endif()

# Set the component getting installed.
if(NOT CMAKE_INSTALL_COMPONENT)
  if(COMPONENT)
    message(STATUS "Install component: \"${COMPONENT}\"")
    set(CMAKE_INSTALL_COMPONENT "${COMPONENT}")
  else()
    set(CMAKE_INSTALL_COMPONENT)
  endif()
endif()

# Install shared libraries without execute permission?
if(NOT DEFINED CMAKE_INSTALL_SO_NO_EXE)
  set(CMAKE_INSTALL_SO_NO_EXE "0")
endif()

# Is this installation the result of a crosscompile?
if(NOT DEFINED CMAKE_CROSSCOMPILING)
  set(CMAKE_CROSSCOMPILING "FALSE")
endif()

# Set path to fallback-tool for dependency-resolution.
if(NOT DEFINED CMAKE_OBJDUMP)
  set(CMAKE_OBJDUMP "/usr/bin/llvm-objdump")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/home/oleub/dev/source/repos/StructuredChaos/.fetch_cache/simdutf-build/src/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/home/oleub/dev/source/repos/StructuredChaos/.fetch_cache/simdutf-build/singleheader/cmake_install.cmake")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "simdutf_Development" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include" TYPE FILE FILES "/home/oleub/dev/source/repos/StructuredChaos/.fetch_cache/simdutf-src/include/simdutf.h")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "simdutf_Development" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include" TYPE FILE FILES "/home/oleub/dev/source/repos/StructuredChaos/.fetch_cache/simdutf-src/include/simdutf_c.h")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "simdutf_Development" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include" TYPE DIRECTORY FILES "/home/oleub/dev/source/repos/StructuredChaos/.fetch_cache/simdutf-src/include/simdutf")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "simdutf_Development" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib64" TYPE STATIC_LIBRARY FILES "/home/oleub/dev/source/repos/StructuredChaos/.fetch_cache/simdutf-build/src/libsimdutf.a")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "simdutf_Development" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib64/cmake/simdutf" TYPE FILE FILES
    "/home/oleub/dev/source/repos/StructuredChaos/.fetch_cache/simdutf-build/simdutf-config.cmake"
    "/home/oleub/dev/source/repos/StructuredChaos/.fetch_cache/simdutf-build/simdutf-config-version.cmake"
    )
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "example_Development" OR NOT CMAKE_INSTALL_COMPONENT)
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib64/cmake/simdutf/simdutfTargets.cmake")
    file(DIFFERENT _cmake_export_file_changed FILES
         "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib64/cmake/simdutf/simdutfTargets.cmake"
         "/home/oleub/dev/source/repos/StructuredChaos/.fetch_cache/simdutf-build/CMakeFiles/Export/543ad56b5ee23ebe72d595a5316e5031/simdutfTargets.cmake")
    if(_cmake_export_file_changed)
      file(GLOB _cmake_old_config_files "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib64/cmake/simdutf/simdutfTargets-*.cmake")
      if(_cmake_old_config_files)
        string(REPLACE ";" ", " _cmake_old_config_files_text "${_cmake_old_config_files}")
        message(STATUS "Old export file \"$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib64/cmake/simdutf/simdutfTargets.cmake\" will be replaced.  Removing files [${_cmake_old_config_files_text}].")
        unset(_cmake_old_config_files_text)
        file(REMOVE ${_cmake_old_config_files})
      endif()
      unset(_cmake_old_config_files)
    endif()
    unset(_cmake_export_file_changed)
  endif()
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib64/cmake/simdutf" TYPE FILE FILES "/home/oleub/dev/source/repos/StructuredChaos/.fetch_cache/simdutf-build/CMakeFiles/Export/543ad56b5ee23ebe72d595a5316e5031/simdutfTargets.cmake")
  if(CMAKE_INSTALL_CONFIG_NAME MATCHES "^([Rr][Ee][Ll][Ee][Aa][Ss][Ee])$")
    file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib64/cmake/simdutf" TYPE FILE FILES "/home/oleub/dev/source/repos/StructuredChaos/.fetch_cache/simdutf-build/CMakeFiles/Export/543ad56b5ee23ebe72d595a5316e5031/simdutfTargets-release.cmake")
  endif()
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib64/pkgconfig" TYPE FILE FILES "/home/oleub/dev/source/repos/StructuredChaos/.fetch_cache/simdutf-build/simdutf.pc")
endif()

string(REPLACE ";" "\n" CMAKE_INSTALL_MANIFEST_CONTENT
       "${CMAKE_INSTALL_MANIFEST_FILES}")
if(CMAKE_INSTALL_LOCAL_ONLY)
  file(WRITE "/home/oleub/dev/source/repos/StructuredChaos/.fetch_cache/simdutf-build/install_local_manifest.txt"
     "${CMAKE_INSTALL_MANIFEST_CONTENT}")
endif()
