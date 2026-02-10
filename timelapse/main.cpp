#include "timelapse.h"
#include <iostream>
#include <string>
#include <cctype>

// check that inputted timelapse length/interval only contains digits
bool validInput(std::string length) {
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
  int capInterval = 0;

  // validate input arguments
  if (argc > 3) {
    std::cout << "Usage: camera or camera <timelapse length in minutes> <capture interval in milliseconds>" << std::endl;
    err = 1;
    return err;
  } else if (argc == 3) {

    std::string lengthArg = argv[1];

    if (!validInput(lengthArg)) {
      std::cerr << "Timelapse length must only contain digits" << std::endl;
      err = 1;
      return err;
    }

    timelapseLength = std::stoi(lengthArg);
    if (timelapseLength < 0) {
      std::cerr << "Timelapse length must be positive" << std::endl;
      err = 1;
      return err;
    }

    std::string intervalArg = argv[2];

    if (!validInput(intervalArg)) {
      std::cerr << "Frame capture interval must only contain digits" << std::endl;
      err = 1;
      return err;
    }

    capInterval = std::stoi(intervalArg);
    if (capInterval < 0) {
      std::cerr << "Framce capture interval must be postivie" << std::endl;
      err = 1;
      return err;
    }
  }

  err = timelapseHandler(timelapseLength, capInterval);

  return err;
}
