// PC에서 시험 돌리기: python -m ziglang c++ -std=c++11 -I rc_car test/host_main.cpp -o tests.exe
#include "tests.h"

int main() {
  return runAllTests() ? 1 : 0;
}
