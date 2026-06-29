# Guard with NOT TARGET gtest so a superbuild that bundles several xmotion
# components (each carrying googletest) doesn't add_subdirectory it twice and
# clash on the gtest target.
if (BUILD_TESTS AND NOT TARGET gtest)
  add_subdirectory(googletest)
endif ()