#include "timelapse.h"
#include <iostream>
#include <string>
#include <cctype>

// check that inputted timelapse length only contains digits
bool validTimelapseLength(std::string length) {
  for (unsigned char c : length) {
    if (!std::isdigit(c)) {
      return false;
    }
  }
  return true;
}


int main(int argc, char* argv[]) {

  int err = 0;
  int timelapseLength = 0;

  // validate input arguments
  if (argc > 2) {
    std::cout << "Usage: camera or camera <timelapse length in minutes>" << std::endl;
    err = 1;
    return err;
  } else if (argc == 2) {

    std::string arg = argv[1];

    if (!validTimelapseLength(arg)) {
      std::cerr << "Timelapse length must only contain digits" << std::endl;
      err = 1;
      return err;
    }

    timelapseLength = std::stoi(arg);
    if (timelapseLength < 0) {
      std::cerr << "Timelapse length must be positive" << std::endl;
      err = 1;
      return err;
    }
  }

  err = timelapseHandler(timelapseLength);

  return err;
}
