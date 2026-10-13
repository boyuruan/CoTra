if(COTRA_WITH_INDEX)
  link_libraries(coromem rdma_anns rdma_index)
else()
  link_libraries(coromem rdma_anns)
endif()

add_subdirectory(rdma)

if(COTRA_WITH_INDEX)
  add_subdirectory(anns)
  add_subdirectory(index)

  add_executable(scala_anns scala_anns.cc)
  add_executable(scala_index scala_index.cc)
  target_link_libraries(scala_index diskann)
endif()
