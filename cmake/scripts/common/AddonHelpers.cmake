# Workaround for the fact that cpack's filenames are not customizable.
# Each add-on is added as a separate component to facilitate zip/tgz packaging.
# The filenames are always of the form basename-component, which is
# incompatible with the addonid-version scheme we want. This hack renames
# the files from the file names generated by the 'package' target.
# Sadly we cannot extend the 'package' target, as it is a builtin target, see
# http://public.kitware.com/Bug/view.php?id=8438
# Thus, we have to add an 'addon-package' target.
get_property(_isMultiConfig GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(_isMultiConfig)
  add_custom_target(addon-package DEPENDS PACKAGE)
else()
  add_custom_target(addon-package
                    COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --target package)
endif()

macro(add_cpack_workaround target version ext)
  if(NOT PACKAGE_DIR)
    set(PACKAGE_DIR "${CMAKE_INSTALL_PREFIX}/zips")
  endif()

  add_custom_command(TARGET addon-package POST_BUILD
                     COMMAND ${CMAKE_COMMAND} -E make_directory ${PACKAGE_DIR}
                     COMMAND ${CMAKE_COMMAND} -E copy ${CPACK_PACKAGE_DIRECTORY}/addon-${target}-${version}-${PLATFORM_TAG}.${ext} ${PACKAGE_DIR}/${target}+${PLATFORM_TAG}/${target}-${version}.${ext})
endmacro()

# Grab the version from a given add-on's addon.xml
macro (addon_version dir prefix)
  if(EXISTS ${PROJECT_SOURCE_DIR}/${dir}/addon.xml.in)
    file(READ ${PROJECT_SOURCE_DIR}/${dir}/addon.xml.in ADDONXML)
  else()
    file(READ ${dir}/addon.xml ADDONXML)
  endif()

  string(REGEX MATCH "<addon[^>]*version.?=.?.[0-9\\.]+" VERSION_STRING ${ADDONXML})
  string(REGEX REPLACE ".*version=.([0-9\\.]+).*" "\\1" ${prefix}_VERSION ${VERSION_STRING})
  message(STATUS ${prefix}_VERSION=${${prefix}_VERSION})
endmacro()

# Build, link and optionally package an add-on
macro (build_addon target prefix libs)
  addon_version(${target} ${prefix})

  # Below comes the generation of a list with used sources where the includes to
  # kodi's headers becomes checked.
  # This goes the following steps to identify them:
  # 1. Check headers are at own depended on addon
  #    - If so, it is checked whether the whole folder is already inserted, if
  #      not, it is added.
  # 2. If headers are not defined independently and there is more as one source
  #    file.
  #    - If yes, it is checked whether the headers with the sources together
  #    - In case no headers are inserted and more than one source file exists,
  #      the whole addon folder is searched for headers.
  # 3. As a last step, the actual source files are checked.
  if(${prefix}_SOURCES)
    # Read used headers from addon, needed to identitfy used kodi addon interface headers
    if(${prefix}_HEADERS)
      # Add the used header files defined with CMakeLists.txt from addon itself
      string(FIND "${${prefix}_HEADERS}" "${PROJECT_SOURCE_DIR}" position)
      if(position GREATER -1)
        # include path name already complete
        list(APPEND USED_SOURCES ${${prefix}_HEADERS})
      else()
        # add the complete include path to begin
        foreach(hdr_file ${${prefix}_HEADERS})
          list(APPEND USED_SOURCES ${PROJECT_SOURCE_DIR}/${hdr_file})
        endforeach()
      endif()
    else()
      list(LENGTH ${prefix}_SOURCES _length)
      if(${_length} GREATER 1)
        string(REGEX MATCHALL "[.](h)" _length ${${prefix}_SOURCES}})
        if(NOT _length)
          file(GLOB_RECURSE USED_SOURCES ${PROJECT_SOURCE_DIR}/*.h*)
          if(USED_SOURCES)
            message(AUTHOR_WARNING "Header files not defined in your CMakeLists.txt. Please consider defining ${prefix}_HEADERS as list of all headers used by this addon. Falling back to recursive scan for *.h.")
          endif()
        endif()
      endif()
    endif()

    # Add the used source files defined with CMakeLists.txt from addon itself
    string(FIND "${${prefix}_SOURCES}" "${PROJECT_SOURCE_DIR}" position)
    if(position GREATER -1)
      # include path name already complete
      list(APPEND USED_SOURCES ${${prefix}_SOURCES})
    else()
      # add the complete include path to begin
      foreach(src_file ${${prefix}_SOURCES})
        list(APPEND USED_SOURCES ${PROJECT_SOURCE_DIR}/${src_file})
      endforeach()
    endif()

    message(STATUS "Addon dependency check ...")
    # Set defines used in addon.xml.in and read from versions.h to set add-on
    # version parts automatically
    file(STRINGS ${KODI_INCLUDE_DIR}/versions.h BIN_ADDON_PARTS)
    foreach(loop_var ${BIN_ADDON_PARTS})
      # Only pass strings with "#define ADDON_" from versions.h
      if(loop_var MATCHES "#define ADDON_")
        string(REGEX REPLACE "\\\n" " " loop_var ${loop_var}) # remove header line breaks
        string(REGEX REPLACE "#define " "" loop_var ${loop_var}) # remove the #define name from string
        string(REGEX MATCHALL "[//a-zA-Z0-9._-]+" loop_var "${loop_var}") # separate the define values to a list

        # Get the definition name
        list(GET loop_var 0 include_name)
        # Check definition are depends who is a bigger list
        if("${include_name}" MATCHES "_DEPENDS")
          # Use start definition name as base for other value type
          list(GET loop_var 0 list_name)
          string(REPLACE "_DEPENDS" "_MIN" depends_minver ${list_name})
          string(REPLACE "_DEPENDS" "" depends_ver ${list_name})
          string(REPLACE "_DEPENDS" "_XML_ID" xml_entry_name ${list_name})
          string(REPLACE "_DEPENDS" "_USED" used_type_name ${list_name})

          # remove the first value, not needed and wrong on "for" loop
          list(REMOVE_AT loop_var 0)

          foreach(depend_header ${loop_var})
            string(STRIP ${depend_header} depend_header)
            foreach(src_file ${USED_SOURCES})
              file(STRINGS ${src_file} BIN_ADDON_SRC_PARTS)
              foreach(loop_var ${BIN_ADDON_SRC_PARTS})
                string(REGEX MATCH "^[ \t]*#[ \t]*(include|import)[ \t]*[<\"](kodi\/)?(.+)[\">]" include_name "${loop_var}")
                if(include_name AND CMAKE_MATCH_3 MATCHES ^${depend_header})
                  get_directory_property(CURRENT_DEFS COMPILE_DEFINITIONS)
                  if(NOT used_type_name IN_LIST CURRENT_DEFS)
                    set(ADDON_DEPENDS "${ADDON_DEPENDS}\n<import addon=\"${${xml_entry_name}}\" minversion=\"${${depends_minver}}\" version=\"${${depends_ver}}\"/>")
                    # Inform with them the addon header about used type, if not present before
                    add_definitions(-D${used_type_name})
                    message(STATUS " - Added usage definition: ${used_type_name}")
                    set(FOUND_HEADER_USAGE 1)
                  endif()
                endif()
              endforeach()
              if(FOUND_HEADER_USAGE EQUAL 1) # break this loop if found but not unset, needed in parts where includes muddled up on addon
                break()
              endif()
            endforeach()
            # type is found and round becomes broken for next round with other type
            if(FOUND_HEADER_USAGE EQUAL 1)
              unset(FOUND_HEADER_USAGE)
              break()
            endif()
          endforeach()
        else()
          # read the definition values and make it by the on version.h defined names public
          list(GET loop_var 1 include_variable)
          string(REGEX REPLACE ".*\"(.*)\"" "\\1" ${include_name} ${include_variable})
          set(${include_name} ${${include_name}})
        endif()
      endif()
    endforeach()

    add_library(${target} ${${prefix}_SOURCES} ${${prefix}_HEADERS})
    target_link_libraries(${target} ${${libs}})
    set_target_properties(${target} PROPERTIES VERSION ${${prefix}_VERSION}
                                               SOVERSION ${APP_VERSION_MAJOR}.${APP_VERSION_MINOR}
                                               PREFIX ""
                                               POSITION_INDEPENDENT_CODE 1)
    if(OS STREQUAL "android")
      set_target_properties(${target} PROPERTIES PREFIX "lib")
    endif()
  elseif(${prefix}_CUSTOM_BINARY)
    add_custom_target(${target} ALL)
  endif()

  # get the library's location
  if(${prefix}_CUSTOM_BINARY)
    list(GET ${prefix}_CUSTOM_BINARY 0 LIBRARY_LOCATION)
    list(GET ${prefix}_CUSTOM_BINARY 1 LIBRARY_FILENAME)
    if(CORE_SYSTEM_NAME STREQUAL android)
      set(LIBRARY_FILENAME "lib${LIBRARY_FILENAME}")
    endif()
  else()
    set(LIBRARY_LOCATION $<TARGET_FILE:${target}>)
    # get the library's filename
    if(CORE_SYSTEM_NAME STREQUAL android)
      # for android we need the filename without any version numbers
      set(LIBRARY_FILENAME $<TARGET_LINKER_FILE_NAME:${target}>)
    else()
      set(LIBRARY_FILENAME $<TARGET_FILE_NAME:${target}>)
    endif()
  endif()

  # if there's an addon.xml.in we need to generate the addon.xml
  if(EXISTS ${PROJECT_SOURCE_DIR}/${target}/addon.xml.in)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/${target}/addon.xml.in)

    file(READ ${PROJECT_SOURCE_DIR}/${target}/addon.xml.in addon_file)

    # If sources are present must be the depends set
    if(${prefix}_SOURCES)
      string(FIND "${addon_file}" "\@ADDON_DEPENDS\@" matchres)
      if("${matchres}" EQUAL -1)
        message(FATAL_ERROR "\"\@ADDON_DEPENDS\@\" not found in addon.xml.in.")
      endif()
    endif()

    # TODO: remove this hack after v18
    string(REPLACE "<platform>\@PLATFORM\@</platform>" "<platform>\@PLATFORM_TAG\@</platform>" addon_file "${addon_file}")

    string(CONFIGURE "${addon_file}" addon_file_conf @ONLY)
    file(GENERATE OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/${target}/addon.xml CONTENT "${addon_file_conf}")
    if(${APP_NAME_UC}_BUILD_DIR)
      file(GENERATE OUTPUT ${${APP_NAME_UC}_BUILD_DIR}/addons/${target}/addon.xml CONTENT "${addon_file_conf}")
    endif()
  endif()

  # if there's an settings.xml.in we need to generate the settings.xml
  if(EXISTS ${PROJECT_SOURCE_DIR}/${target}/resources/settings.xml.in)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/${target}/resources/settings.xml.in)

    file(READ ${PROJECT_SOURCE_DIR}/${target}/resources/settings.xml.in settings_file)
    string(CONFIGURE "${settings_file}" settings_file_conf @ONLY)
    file(GENERATE OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/${target}/resources/settings.xml CONTENT "${settings_file_conf}")
    if(${APP_NAME_UC}_BUILD_DIR)
      file(GENERATE OUTPUT ${${APP_NAME_UC}_BUILD_DIR}/addons/${target}/resources/settings.xml CONTENT "${settings_file_conf}")
    endif()
  endif()

  # set zip as default if addon-package is called without PACKAGE_XXX
  set(CPACK_GENERATOR "ZIP")
  set(ext "zip")
  if(PACKAGE_ZIP OR PACKAGE_TGZ)
    if(PACKAGE_TGZ)
      set(CPACK_GENERATOR "TGZ")
      set(ext "tar.gz")
    endif()
    set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY OFF)
    set(CPACK_PACKAGE_FILE_NAME addon)
    if(CMAKE_BUILD_TYPE STREQUAL "Release")
      set(CPACK_STRIP_FILES TRUE)
    endif()
    set(CPACK_ARCHIVE_COMPONENT_INSTALL ON)
    set(CPACK_COMPONENTS_IGNORE_GROUPS 1)
    list(APPEND CPACK_COMPONENTS_ALL ${target}-${${prefix}_VERSION}-${PLATFORM_TAG})
    # Pack files together to create an archive
    install(DIRECTORY ${target} ${CMAKE_CURRENT_BINARY_DIR}/${target} DESTINATION ./
                                COMPONENT ${target}-${${prefix}_VERSION}-${PLATFORM_TAG}
                                REGEX ".+\\.xml\\.in(clude)?$" EXCLUDE)
    if(WIN32)
      if(NOT CPACK_PACKAGE_DIRECTORY)
        # determine the temporary path
        file(TO_CMAKE_PATH "$ENV{TEMP}" WIN32_TEMP_PATH)
        string(LENGTH "${WIN32_TEMP_PATH}" WIN32_TEMP_PATH_LENGTH)
        string(LENGTH "${PROJECT_BINARY_DIR}" PROJECT_BINARY_DIR_LENGTH)

        # check if the temporary path is shorter than the default packaging directory path
        if(WIN32_TEMP_PATH_LENGTH GREATER 0 AND WIN32_TEMP_PATH_LENGTH LESS PROJECT_BINARY_DIR_LENGTH)
          # set the directory used by CPack for packaging to the temp directory
          set(CPACK_PACKAGE_DIRECTORY ${WIN32_TEMP_PATH})
        endif()
      endif()

      if(${prefix}_SOURCES)
        # install the generated DLL file
        install(PROGRAMS ${LIBRARY_LOCATION} DESTINATION ${target}
                COMPONENT ${target}-${${prefix}_VERSION}-${PLATFORM_TAG})

        # for debug builds also install the PDB file
        install(FILES $<TARGET_PDB_FILE:${target}> DESTINATION ${target}
                CONFIGURATIONS Debug RelWithDebInfo
                COMPONENT ${target}-${${prefix}_VERSION}-${PLATFORM_TAG})
      endif()
      if(${prefix}_CUSTOM_BINARY)
        install(FILES ${LIBRARY_LOCATION} DESTINATION ${target} RENAME ${LIBRARY_FILENAME}
                COMPONENT ${target}-${${prefix}_VERSION}-${PLATFORM_TAG})
      endif()
      if(${prefix}_CUSTOM_DATA)
        install(DIRECTORY ${${prefix}_CUSTOM_DATA} DESTINATION ${target}/resources
                COMPONENT ${target}-${${prefix}_VERSION}-${PLATFORM_TAG})
      endif()
      if(${prefix}_ADDITIONAL_BINARY)
        install(FILES ${${prefix}_ADDITIONAL_BINARY} DESTINATION ${target}
                COMPONENT ${target}-${${prefix}_VERSION}-${PLATFORM_TAG})
      endif()
      if(${prefix}_ADDITIONAL_BINARY_EXE)
        install(PROGRAMS ${${prefix}_ADDITIONAL_BINARY_EXE} DESTINATION ${target}
                COMPONENT ${target}-${${prefix}_VERSION}-${PLATFORM_TAG})
      endif()
      if(${prefix}_ADDITIONAL_BINARY_PARTS)
        install(FILES ${${prefix}_ADDITIONAL_BINARY_PARTS} DESTINATION ${target}
                COMPONENT ${target}-${${prefix}_VERSION}-${PLATFORM_TAG})
      endif()
      if(${prefix}_ADDITIONAL_BINARY_DIRS)
        install(DIRECTORY ${${prefix}_ADDITIONAL_BINARY_DIRS} DESTINATION ${target} USE_SOURCE_PERMISSIONS
                COMPONENT ${target}-${${prefix}_VERSION}-${PLATFORM_TAG})
      endif()
    else() # NOT WIN32
      if(NOT CPACK_PACKAGE_DIRECTORY)
        set(CPACK_PACKAGE_DIRECTORY ${CMAKE_BINARY_DIR})
      endif()
      if(${prefix}_SOURCES)
        install(TARGETS ${target} DESTINATION ${target}
                COMPONENT ${target}-${${prefix}_VERSION}-${PLATFORM_TAG})
      endif()
      if(${prefix}_CUSTOM_BINARY)
        install(FILES ${LIBRARY_LOCATION} DESTINATION ${target} RENAME ${LIBRARY_FILENAME}
                COMPONENT ${target}-${${prefix}_VERSION}-${PLATFORM_TAG})
      endif()
      if(${prefix}_CUSTOM_DATA)
        install(DIRECTORY ${${prefix}_CUSTOM_DATA} DESTINATION ${target}/resources
                COMPONENT ${target}-${${prefix}_VERSION}-${PLATFORM_TAG})
      endif()
      if(${prefix}_ADDITIONAL_BINARY)
        install(FILES ${${prefix}_ADDITIONAL_BINARY} DESTINATION ${target}
                COMPONENT ${target}-${${prefix}_VERSION}-${PLATFORM_TAG})
      endif()
      if(${prefix}_ADDITIONAL_BINARY_EXE)
        install(PROGRAMS ${${prefix}_ADDITIONAL_BINARY_EXE} DESTINATION ${target}
                COMPONENT ${target}-${${prefix}_VERSION}-${PLATFORM_TAG})
      endif()
      if(${prefix}_ADDITIONAL_BINARY_PARTS)
        install(FILES ${${prefix}_ADDITIONAL_BINARY_PARTS} DESTINATION ${target}
                COMPONENT ${target}-${${prefix}_VERSION}-${PLATFORM_TAG})
      endif()
      if(${prefix}_ADDITIONAL_BINARY_DIRS)
        install(DIRECTORY ${${prefix}_ADDITIONAL_BINARY_DIRS} DESTINATION ${target} USE_SOURCE_PERMISSIONS
                COMPONENT ${target}-${${prefix}_VERSION}-${PLATFORM_TAG})
      endif()
    endif()
    add_cpack_workaround(${target} ${${prefix}_VERSION} ${ext})
  else()
    if(CORE_SYSTEM_NAME STREQUAL linux OR CORE_SYSTEM_NAME STREQUAL freebsd)
      if(NOT OVERRIDE_PATHS)
        if(CMAKE_INSTALL_PREFIX AND NOT CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT AND NOT CMAKE_INSTALL_PREFIX STREQUAL "${${APP_NAME_UC}_PREFIX}")
          message(WARNING "CMAKE_INSTALL_PREFIX ${CMAKE_INSTALL_PREFIX} differs from ${APP_NAME} prefix, changing to ${${APP_NAME_UC}_PREFIX}. Please pass -DOVERRIDE_PATHS=1 to skip this check")
        endif()
        if(CMAKE_INSTALL_LIBDIR AND NOT CMAKE_INSTALL_LIBDIR STREQUAL "${${APP_NAME_UC}_LIB_DIR}")
          message(WARNING "CMAKE_INSTALL_LIBDIR ${CMAKE_INSTALL_LIBDIR} differs from ${APP_NAME} libdir, changing to ${${APP_NAME_UC}_LIB_DIR}. Please pass -DOVERRIDE_PATHS=1 to skip this check")
        endif()
        if(CMAKE_INSTALL_DATADIR AND NOT CMAKE_INSTALL_DATADIR STREQUAL "${${APP_NAME_UC}_DATA_DIR}")
          message(WARNING "CMAKE_INSTALL_DATADIR ${CMAKE_INSTALL_DATADIR} differs from ${APP_NAME} datadir, changing to ${${APP_NAME_UC}_DATA_DIR}. Please pass -DOVERRIDE_PATHS=1 to skip this check")
        endif()
        set(CMAKE_INSTALL_PREFIX "${${APP_NAME_UC}_PREFIX}" CACHE PATH "${APP_NAME} install prefix" FORCE)
        set(CMAKE_INSTALL_LIBDIR "${${APP_NAME_UC}_LIB_DIR}" CACHE PATH "${APP_NAME} install libdir" FORCE)
        set(CMAKE_INSTALL_DATADIR "${${APP_NAME_UC}_DATA_DIR}" CACHE PATH "${APP_NAME} install datadir" FORCE)
      else()
        if(NOT CMAKE_INSTALL_LIBDIR)
          set(CMAKE_INSTALL_LIBDIR "${CMAKE_INSTALL_PREFIX}/lib/${APP_NAME_LC}")
        endif()
        if(NOT CMAKE_INSTALL_DATADIR)
          set(CMAKE_INSTALL_DATADIR "${CMAKE_INSTALL_PREFIX}/share/${APP_NAME_LC}")
        endif()
      endif()
    else()
      set(CMAKE_INSTALL_LIBDIR "lib/${APP_NAME_LC}")
      set(CMAKE_INSTALL_DATADIR "share/${APP_NAME_LC}")
    endif()
    if(${prefix}_SOURCES)
      install(TARGETS ${target} DESTINATION ${CMAKE_INSTALL_LIBDIR}/addons/${target})
    endif()
    if (${prefix}_CUSTOM_BINARY)
      install(FILES ${LIBRARY_LOCATION} DESTINATION ${CMAKE_INSTALL_LIBDIR}/addons/${target} RENAME ${LIBRARY_FILENAME})
    endif()
    install(DIRECTORY ${target} ${CMAKE_CURRENT_BINARY_DIR}/${target} DESTINATION ${CMAKE_INSTALL_DATADIR}/addons
                                REGEX ".+\\.xml\\.in(clude)?$" EXCLUDE)
    if(${prefix}_CUSTOM_DATA)
      install(DIRECTORY ${${prefix}_CUSTOM_DATA} DESTINATION ${CMAKE_INSTALL_DATADIR}/addons/${target}/resources)
    endif()
    if(${prefix}_ADDITIONAL_BINARY)
      install(FILES ${${prefix}_ADDITIONAL_BINARY} DESTINATION ${CMAKE_INSTALL_LIBDIR}/addons/${target})
    endif()
    if(${prefix}_ADDITIONAL_BINARY_EXE)
      install(PROGRAMS ${${prefix}_ADDITIONAL_BINARY_EXE} DESTINATION ${CMAKE_INSTALL_LIBDIR}/addons/${target})
    endif()
    if(${prefix}_ADDITIONAL_BINARY_PARTS)
      install(FILES ${${prefix}_ADDITIONAL_BINARY_PARTS} DESTINATION ${CMAKE_INSTALL_LIBDIR}/addons/${target})
    endif()
    if(${prefix}_ADDITIONAL_BINARY_DIRS)
      install(DIRECTORY ${${prefix}_ADDITIONAL_BINARY_DIRS} DESTINATION ${CMAKE_INSTALL_LIBDIR}/addons/${target} USE_SOURCE_PERMISSIONS)
    endif()
  endif()
  if(${APP_NAME_UC}_BUILD_DIR)
    file(GLOB_RECURSE files ${CMAKE_CURRENT_SOURCE_DIR}/${target}/*)
    if(${prefix}_CUSTOM_DATA)
      get_filename_component(dname ${${prefix}_CUSTOM_DATA} NAME)
      add_custom_command(TARGET ${target} POST_BUILD
                         COMMAND ${CMAKE_COMMAND} -E copy_directory
                                 ${${prefix}_CUSTOM_DATA}
                                 ${${APP_NAME_UC}_BUILD_DIR}/addons/${target}/resources/${dname})
    endif()
    foreach(file ${files})
      string(REPLACE "${CMAKE_CURRENT_SOURCE_DIR}/${target}/" "" name "${file}")
      # A good way to deal with () in filenames
      if(NOT ${file} MATCHES xml.in)
        configure_file(${file} ${${APP_NAME_UC}_BUILD_DIR}/addons/${target}/${name} COPYONLY)
      endif()
    endforeach()
    add_custom_command(TARGET ${target} POST_BUILD
                       COMMAND ${CMAKE_COMMAND} -E copy
                                   ${LIBRARY_LOCATION}
                                   ${${APP_NAME_UC}_BUILD_DIR}/addons/${target}/${LIBRARY_FILENAME})
    if(${prefix}_ADDITIONAL_BINARY)
        add_custom_command(TARGET ${target} POST_BUILD
                           COMMAND ${CMAKE_COMMAND} -E copy
                                   ${${prefix}_ADDITIONAL_BINARY}
                                   ${${APP_NAME_UC}_BUILD_DIR}/addons/${target})
    endif()
  endif()
endmacro()

# finds a path to a given file (recursive)
function (kodi_find_path var_name filename search_path strip_file)
  file(GLOB_RECURSE PATH_TO_FILE ${search_path} ${filename})
  if(strip_file)
    string(REPLACE ${filename} "" PATH_TO_FILE ${PATH_TO_FILE})
  endif()
  set (${var_name} ${PATH_TO_FILE} PARENT_SCOPE)
endfunction()

# Cmake build options
include(AddOptions)
include(TestCXXAcceptsFlag)
option(PACKAGE_ZIP "Package Zip file?" OFF)
option(PACKAGE_TGZ "Package TGZ file?" OFF)
option(BUILD_SHARED_LIBS "Build shared libs?" ON)

# LTO support?
CHECK_CXX_ACCEPTS_FLAG("-flto" HAVE_LTO)
if(HAVE_LTO)
  option(USE_LTO "use link time optimization" OFF)
  if(USE_LTO)
    add_options(ALL_LANGUAGES ALL_BUILDS "-flto")
  endif()
endif()

# set this to try linking dependencies as static as possible
if(ADDONS_PREFER_STATIC_LIBS)
  set(CMAKE_FIND_LIBRARY_SUFFIXES .lib .a ${CMAKE_FIND_LIBRARY_SUFFIXES})
endif()

if(${APP_NAME_UC}_BUILD_DIR)
  list(APPEND CMAKE_PREFIX_PATH ${${APP_NAME_UC}_BUILD_DIR}/build)
endif()
