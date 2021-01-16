add_executable (tinker9 src-main/main_tinker9.cpp)
add_dependencies (tinker9 cmtinker9acc)
set_target_properties (tinker9 PROPERTIES
   CXX_STANDARD
      11
)
target_compile_definitions (tinker9 PRIVATE "${TINKER9_DEFS}")
target_include_directories (tinker9 SYSTEM PRIVATE "${TINKER9_SYS_INC_PATH}")
target_include_directories (tinker9 PRIVATE "${TINKER9_INTERNAL_INC_PATH}")
set (TINKER9_EXT_LIBS pthread LIBTINKER LIBFFTW LIBFFTW_THREADS)
if (PREC STREQUAL "m" OR PREC STREQUAL "s")
   list (APPEND TINKER9_EXT_LIBS LIBFFTWF LIBFFTWF_THREADS)
endif ()
target_link_libraries (tinker9
   "-Wl,--start-group"
   $<TARGET_FILE:tinker9_EP_acc>
   tinker9_cpp
   tinker9_f
   "-Wl,--end-group"
   "${TINKER9_EXT_LIBS}"
)


########################################################################


add_executable (all.tests)
add_dependencies (all.tests cmtinker9acc)
target_link_libraries (all.tests
   "-Wl,--start-group"
   all_tests_o
   $<TARGET_FILE:tinker9_EP_acc>
   tinker9_cpp
   tinker9_f
   "-Wl,--end-group"
   "${TINKER9_EXT_LIBS}"
)
