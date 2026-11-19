add_executable(TodoListTest
  TodoListTest.cpp
  TodoApplyTest.cpp
  ${REPO_ROOT}/lib/TodoList/TodoList.cpp
)

target_include_directories(TodoListTest PRIVATE
  ${REPO_ROOT}/lib/TodoList
)

target_link_libraries(TodoListTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(TodoListTest)
