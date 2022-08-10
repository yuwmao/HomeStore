cmake_minimum_required(VERSION 3.11)

if (DEFINED TEST_TARGET)
  macro(can_build_io_tests ret)
      set(${ret} false)
      if (("${TEST_TARGET}" STREQUAL "full") OR ("${TEST_TARGET}" STREQUAL "spdk_mode") OR 
          ("${TEST_TARGET}" STREQUAL "epoll_mode") OR ("${TEST_TARGET}" STREQUAL "coverage"))
          set(${ret} true)
      endif()
  endmacro()

  macro(can_build_nonio_tests ret)
      set(${ret} false)
      can_build_io_tests(build_io_tests)
      if (${build_io_tests} OR ("${TEST_TARGET}" STREQUAL "min"))
          set(${ret} true)
      endif()
  endmacro()

  macro(is_non_coverage_build ret)
      set(${ret} false)
      can_build_nonio_tests(build_nonio_tests)
      if (${build_nonio_tests} AND (NOT "${TEST_TARGET}" STREQUAL "coverage"))
          set(${ret} true)
      endif()
  endmacro()

  macro(can_build_spdk_io_tests ret)
      set(${ret} false)
      if (("${TEST_TARGET}" STREQUAL "full") OR ("${TEST_TARGET}" STREQUAL "spdk_mode"))
          set(${ret} true)
      endif()
  endmacro()

  macro(can_build_epoll_io_tests ret)
      set(${ret} false)
      if (("${TEST_TARGET}" STREQUAL "full") OR ("${TEST_TARGET}" STREQUAL "epoll_mode") OR 
          ("${TEST_TARGET}" STREQUAL "coverage"))
          set(${ret} true)
      endif()
  endmacro()
endif ()
