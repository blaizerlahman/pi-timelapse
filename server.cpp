#include "timelapse.h"

#include <httplib.h>
#include <string>
#include <iostream>
#include <format>
#include <thread>

int main() {

  httplib::Server svr;

  svr.Get("/hi", [](const httplib::Request& req, httplib::Response& res) {
    std::string userIP = req.remote_addr;

    //res.set_content(std::format("Hello, {0}\n", userIP), "text/plain");
    std::cout << "Hello there " << userIP << std::endl;
  });

  svr.Get("/start-cam", [](const httplib::Request& req, httplib::Response& res) {
    
    std::cout << "CAMERA STARTED!" << std::endl;

    int length = 0;

    if (req.has_param("length")) {
      std::string lengthStr = req.get_param_value("length");
      length = std::stoi(lengthStr);
    }

    std::thread([length]() {
        int err = timelapseHandler(length);
    }).detach();

    res.set_content("Timelapse started in background\n", "text/plain");
  });

  svr.listen("0.0.0.0", 8000);
}
