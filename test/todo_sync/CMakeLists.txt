add_executable(TodoSyncTest
  TodoSyncTest.cpp
  ${REPO_ROOT}/lib/TodoSync/TodoSync.cpp
)

target_include_directories(TodoSyncTest PRIVATE
  ${REPO_ROOT}/lib/TodoSync
  ${REPO_ROOT}/lib/Memory
)

target_link_libraries(TodoSyncTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(TodoSyncTest)
