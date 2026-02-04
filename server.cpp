#include "timelapse.h"

#include <httplib.h>
#include <string>
#include <iostream>
#include <thread>
#include <atomic>

extern std::atomic<bool> shouldStop;
extern std::filesystem::path FRAME_PATH;

int main() {

  httplib::Server svr;

  std::atomic<bool> isCamRunning{false};

  svr.Get("/start-cam", [&isCamRunning](const httplib::Request& req, httplib::Response& res) {
    
    if (isCamRunning.load()) {
      std::cerr << "Camera has already been started." << std::endl;
      res.set_content("Camera has already been started.", "text/plain");
    } else {

      std::cout << "CAMERA STARTED by " << req.remote_addr << std::endl;

      int length = 0;
      if (req.has_param("length")) {
        std::string lengthStr = req.get_param_value("length");
        length = std::stoi(lengthStr);
      }

      isCamRunning.store(true);
      shouldStop.store(false);

      std::thread t1([length, &isCamRunning]() {
          int err = timelapseHandler(length);
          isCamRunning.store(false);
          
          std::cout << "Timelapse finished with code " << err << std::endl;
      });

      t1.detach();

      res.set_content("Timelapse started\n", "text/plain");
    }
  });

  svr.Get("/stop-cam", [&isCamRunning](const httplib::Request& req, httplib::Response& res) {
    
    if (!isCamRunning.load()) {
      std::cerr << "No camera is currently running" << std::endl;
      res.set_content("Error: no camera is currently running.\n", "text/plain");
    } else {

      std::cout << "STOPPING CAMERA..." << std::endl;
      
      shouldStop.store(true);

      std::cout << "Succesfully stopped camera process" << std::endl;
      res.set_content("Timelapse has been stopped\n", "text/plain");
    }
  });

  svr.Get("/clear-frames", [&isCamRunning](const httplib::Request& req, httplib::Response& res) {
    if (isCamRunning.load()) {
      std::cerr << "Frames attempted to clear while camera running" << std::endl;
      res.set_content("Error: cannot clear frames while camera is running.\n", "text/plain");
    } else {

      std::cout << "Clearing frames..." << std::endl;


      // remove all regular files in frame directory if specified
      if (req.has_param("all")) {
        if (req.get_param_value("all") != "true") {
          std::cerr << "Invalid param value for 'all'" << std::endl;
          res.set_content("Error: invalid param value for 'all'.\n", "text/plain");
        } else {

          for (const auto& file : std::filesystem::directory_iterator(FRAME_PATH)) {
            if (file.is_regular_file()) {
              std::filesystem::remove(file.path());
            }
          }

          std::cout << "Removed all files in frame path" << std::endl;
          
          res.set_content("All files have been successfully cleared\n", "text/plain");
        }
      } else { // just remove frames if other files not specified
        
        for (const auto& file : std::filesystem::directory_iterator(FRAME_PATH)) {
          if (file.is_regular_file() && file.path().extension() == ".yuv") {
            std::filesystem::remove(file.path());
          }
        }
        
        res.set_content("Frames have been successfully cleared\n", "text/plain");
      } 
    }

  });

  svr.listen("0.0.0.0", 8000);
}
