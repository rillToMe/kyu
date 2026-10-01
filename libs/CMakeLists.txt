# ============================================================================
# libs/CMakeLists.txt
# ============================================================================

add_subdirectory(gui)
add_subdirectory(core)
add_subdirectory(media)
add_subdirectory(text)

if(KYUZEN_BUILD_LIBC_SDK)
    add_subdirectory(c)
    add_subdirectory(cpp)

    # -----------------------------------------------------------------------
    # Forward the SDK paths to the TOP-LEVEL scope.
    #
    # libs/c and libs/cpp export them with PARENT_SCOPE, which only reaches
    # libs/. apps/ is a sibling of libs/ (both added from the top level), so
    # without this relay the variables would be empty there and the SDK's
    # crt.o / libc.a would silently vanish from every app link line.
    # -----------------------------------------------------------------------
    set(KYUZEN_SDK_C_INC   "${KYUZEN_SDK_C_INC}"   PARENT_SCOPE)
    set(KYUZEN_SDK_C_LIB   "${KYUZEN_SDK_C_LIB}"   PARENT_SCOPE)
    set(KYUZEN_SDK_C_CRT   "${KYUZEN_SDK_C_CRT}"   PARENT_SCOPE)
    set(KYUZEN_SDK_C_LD    "${KYUZEN_SDK_C_LD}"    PARENT_SCOPE)
    set(KYUZEN_SDK_C_STAGE "${KYUZEN_SDK_C_STAGE}" PARENT_SCOPE)

    set(KYUZEN_SDK_CPP_INC     "${KYUZEN_SDK_CPP_INC}"     PARENT_SCOPE)
    set(KYUZEN_SDK_CPP_CXXRT   "${KYUZEN_SDK_CPP_CXXRT}"   PARENT_SCOPE)
    set(KYUZEN_SDK_CPP_LD      "${KYUZEN_SDK_CPP_LD}"      PARENT_SCOPE)
    set(KYUZEN_SDK_CPP_WRAPPER "${KYUZEN_SDK_CPP_WRAPPER}" PARENT_SCOPE)
    set(KYUZEN_SDK_CPP_STAGE   "${KYUZEN_SDK_CPP_STAGE}"   PARENT_SCOPE)
    set(KYUZEN_LIBCXXRT_ARCHIVE "${KYUZEN_LIBCXXRT_ARCHIVE}" PARENT_SCOPE)

    set(KYUZEN_LIBC_ARCHIVE   "${KYUZEN_LIBC_ARCHIVE}"   PARENT_SCOPE)
    set(KYUZEN_LIBC_CMAKE_DIR "${KYUZEN_LIBC_CMAKE_DIR}" PARENT_SCOPE)
    set(KYUZEN_LIBC_INCLUDE   "${KYUZEN_LIBC_INCLUDE}"   PARENT_SCOPE)
endif()
