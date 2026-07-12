# Guard with NOT TARGET gtest so a superbuild that bundles several xmotion
# components (each carrying googletest) doesn't add_subdirectory it twice and
# clash on the gtest target.
if (BUILD_TESTS AND NOT TARGET gtest)
  # Test-only dependency: it must NEVER reach the install tree or deb
  # payload. Without these, googletest's own install rules ship gtest/gmock
  # headers, static libs, and pkgconfig files into any prefix xmBase is
  # installed to — and a downstream that compiles against those leaked
  # headers while linking its own gtest gets silent ABI mismatches
  # (discovered via xmMessaging built against an installed xmBase).
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  add_subdirectory(googletest EXCLUDE_FROM_ALL)
endif ()