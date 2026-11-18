# The propagate program's library (apps/propagate), called directly.
satsim_add_test(satsim_propagate_tests
  SOURCES arguments_test.cpp csv_test.cpp run_test.cpp run_main_test.cpp physical_sanity_test.cpp
  DEPS satsim::propagate_app satsim::orbit satsim::frames satsim::time satsim::core)

# The built executable, run as a child process. Under the valgrind step the child is traced
# too (scripts/ci.sh passes --trace-children=yes), so the program itself runs under Valgrind.
satsim_add_test(satsim_propagate_e2e_tests
  SOURCES e2e_test.cpp
  DEPS satsim::propagate_app)
target_compile_definitions(satsim_propagate_e2e_tests
  PRIVATE SATSIM_PROPAGATE_EXE="$<TARGET_FILE:propagate>")
add_dependencies(satsim_propagate_e2e_tests propagate)
