include(ExternalProject)

if (WIN32)

  if (ocos_target_platform STREQUAL "AMD64")
    set(vcpkg_target_platform "x64")
  else()
    set(vcpkg_target_platform ${ocos_target_platform})
  endif()

  function(get_vcpkg)
    ExternalProject_Add(vcpkg
      GIT_REPOSITORY https://github.com/microsoft/vcpkg.git
      GIT_TAG 2022.11.14
      PREFIX vcpkg
      SOURCE_DIR ${CMAKE_CURRENT_BINARY_DIR}/_deps/vcpkg-src
      BINARY_DIR ${CMAKE_CURRENT_BINARY_DIR}/_deps/vcpkg-build
      CONFIGURE_COMMAND ""
      INSTALL_COMMAND ""
      UPDATE_COMMAND ""
      BUILD_COMMAND "<SOURCE_DIR>/bootstrap-vcpkg.bat")

    ExternalProject_Get_Property(vcpkg SOURCE_DIR)
    set(VCPKG_SRC ${SOURCE_DIR} PARENT_SCOPE)
    set(VCPKG_DEPENDENCIES "vcpkg" PARENT_SCOPE)
  endfunction()

  function(vcpkg_install PACKAGE_NAME)
    add_custom_command(
      OUTPUT ${VCPKG_SRC}/packages/${PACKAGE_NAME}_${vcpkg_target_platform}-windows-static/BUILD_INFO
      #COMMAND ${VCPKG_SRC}/vcpkg install ${PACKAGE_NAME}:${vcpkg_target_platform}-windows-static --vcpkg-root=${VCPKG_SRC}
      COMMAND ${VCPKG_SRC}/vcpkg install ${PACKAGE_NAME}:${vcpkg_target_platform}-windows-static
      WORKING_DIRECTORY ${VCPKG_SRC}
      DEPENDS vcpkg)

    add_custom_target(get${PACKAGE_NAME}
      ALL
      DEPENDS ${VCPKG_SRC}/packages/${PACKAGE_NAME}_${vcpkg_target_platform}-windows-static/BUILD_INFO)

    list(APPEND VCPKG_DEPENDENCIES "get${PACKAGE_NAME}")
    set(VCPKG_DEPENDENCIES ${VCPKG_DEPENDENCIES} PARENT_SCOPE)
  endfunction()

  #unset(ENV{VCPKG_ROOT})
  #get_vcpkg()
  #set(ENV{VCPKG_ROOT} ${VCPKG_SRC})

  set(VCPKG_SRC $ENV{VCPKG_ROOT})
  set(VCPKG_DEPENDENCIES "")

  vcpkg_install(cmake)
  vcpkg_install(openssl)
  vcpkg_install(openssl-windows)
  vcpkg_install(rapidjson)
  vcpkg_install(re2)
  vcpkg_install(boost-interprocess)
  vcpkg_install(boost-stacktrace)
  vcpkg_install(pthread)
  vcpkg_install(b64)

  add_dependencies(getb64 getpthread)
  add_dependencies(getpthread getboost-stacktrace)
  add_dependencies(getboost-stacktrace getboost-interprocess)
  add_dependencies(getboost-interprocess getre2)
  add_dependencies(getre2 getrapidjson)
  add_dependencies(getrapidjson getopenssl-windows)
  add_dependencies(getopenssl-windows getopenssl)
  add_dependencies(getopenssl getcmake)

  ExternalProject_Add(triton
                      GIT_REPOSITORY https://github.com/triton-inference-server/client.git
                      GIT_TAG r23.05
                      PREFIX triton
                      SOURCE_DIR ${CMAKE_CURRENT_BINARY_DIR}/_deps/triton-src
                      BINARY_DIR ${CMAKE_CURRENT_BINARY_DIR}/_deps/triton-build
                      CMAKE_ARGS -DVCPKG_TARGET_TRIPLET=${vcpkg_target_platform}-windows-static -DCMAKE_TOOLCHAIN_FILE=${VCPKG_SRC}/scripts/buildsystems/vcpkg.cmake -DCMAKE_INSTALL_PREFIX=binary -DTRITON_ENABLE_CC_HTTP=ON -DTRITON_ENABLE_ZLIB=OFF
                      INSTALL_COMMAND ""
                      UPDATE_COMMAND "")

  add_dependencies(triton ${VCPKG_DEPENDENCIES})

else()

  ExternalProject_Add(triton
                      GIT_REPOSITORY https://github.com/triton-inference-server/client.git
                      GIT_TAG r23.05
                      PREFIX triton
                      SOURCE_DIR ${CMAKE_CURRENT_BINARY_DIR}/_deps/triton-src
                      BINARY_DIR ${CMAKE_CURRENT_BINARY_DIR}/_deps/triton-build
                      CMAKE_ARGS -DCMAKE_INSTALL_PREFIX=binary -DTRITON_ENABLE_CC_HTTP=ON -DTRITON_ENABLE_ZLIB=OFF
                      INSTALL_COMMAND ""
                      UPDATE_COMMAND "")

endif() #if (WIN32)

ExternalProject_Get_Property(triton SOURCE_DIR)
set(TRITON_SRC ${SOURCE_DIR})

ExternalProject_Get_Property(triton BINARY_DIR)
set(TRITON_BIN ${BINARY_DIR}/binary)
set(TRITON_THIRD_PARTY ${BINARY_DIR}/third-party)
