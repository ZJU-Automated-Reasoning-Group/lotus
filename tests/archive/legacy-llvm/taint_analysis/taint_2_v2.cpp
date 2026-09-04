#include <iostream>
void someFunction(int i) { std::cout << i << '\n'; }

int main(int argc, char **argv) {
  someFunction(argc);
  return 0;
};
