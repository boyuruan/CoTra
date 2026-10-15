link_libraries(coromem rdma_anns rdma_index)

add_executable(pq_encode pq_encode.cc)
target_link_libraries(pq_encode diskann)
