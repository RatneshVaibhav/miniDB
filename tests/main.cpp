#include "test_harness.h"

int main(int argc, char **argv) {
  std::printf("MiniDB test suite\n=================\n");
  return minidb::test::RunAll(argc, argv);
}
