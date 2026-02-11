#include "timelapse.h"

#include <httplib.h>
#include <string>
#include <iostream>
#include <thread>
#include <atomic>
#include <condition_variable>
#include <csignal>


extern std::atomic<bool> shouldStop;

extern std::filesystem::path FRAME_PATH;
extern std::filesystem::path TIMELAPSE_PATH;

static httplib::Server *globalServer = nullptr;


// stops running camera process and then shuts down httplib server
void shutdownServer() {
  shouldStop.store(true);

  if (globalServer) {
    globalServer->stop();
  }
}


// first handles interrupting a timelapse if currently running, then interrupts server
void interruptHandler(int signum) {
  if (signum) {
    //keep the compiler happy
  }

  shutdownServer();
}


int main() {

  std::signal(SIGINT, interruptHandler);

  httplib::Server svr;
  globalServer = &svr;

  std::unique_ptr<std::thread> camThread;
  std::unique_ptr<std::thread> createTimelapseThread;

  std::atomic<bool> isCamRunning{false};
  std::atomic<bool> isCreatingTimelapse{false};


  svr.Get("/start-cam", [&isCamRunning, &camThread, &isCreatingTimelapse](const httplib::Request& req, httplib::Response& res) {
    
    if (isCamRunning.load()) {
      std::cerr << "Camera has already been started." << std::endl;
      res.set_content("Error: camera has already been started.\n", "text/plain");
    } else if (isCreatingTimelapse.load()) {
      std::cerr << "Cannot start camera while timelapse is being created." << std::endl; 
      res.set_content("Error: cannot start camera while timelapse is being created.\n", "text/plain");
    } else {

      std::cout << "CAMERA STARTED by " << req.remote_addr << std::endl;

      // join previous thread if it exists
      if (camThread && camThread->joinable()) {
        camThread->join();
      }

      int length = 0;
      if (req.has_param("length")) {
        length = std::stoi(req.get_param_value("length"));
      }

      int capInterval = 0;
      if (req.has_param("cap-interval")) {
        capInterval = std::stoi(req.get_param_value("cap-interval"));
      }

      isCamRunning.store(true);
      shouldStop.store(false);

      camThread = std::make_unique<std::thread>([length, capInterval, &isCamRunning]() {
          int err = timelapseHandler(length, capInterval);
          isCamRunning.store(false);
          
          std::cout << "Timelapse finished with code " << err << std::endl;
      });

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


  svr.Get("/create-timelapse", [&isCamRunning, &isCreatingTimelapse, &createTimelapseThread](const httplib::Request& req, httplib::Response& res) {

    if (isCamRunning.load()) {
      std::cerr << "Camera is currently running, cannot create timelapse" << std::endl;
      res.set_content("Error: cannot create timelapse, camera is currently running.\n", "text/plain");
    } else if (isCreatingTimelapse.load()) {
      std::cerr << "Cannot create timelapse while timelapse is already being created" << std::endl;
      res.set_content("Error: cannot create timelapse, timelapse is already being created.\n", "text/plain");
    } else {

      // check if directory has frames
      if (std::filesystem::is_empty(FRAME_PATH)) {
        std::cerr << "Cannot create timelapse, no frames in frame directory" << std::endl;
        res.set_content("Error: cannot create timelapse, there are no frames in frame directory.\n", "text/plain");
      }

      // check if timelapse directory exists
      if (std::filesystem::exists(TIMELAPSE_PATH) && std::filesystem::is_directory(TIMELAPSE_PATH)) {
        std::cerr << "Cannot create timelapse, the timelapse path does not point to an existing directory" << std::endl;
        res.set_content("Error: cannot create timelapse, the timelapse path does not point to an existing directory.\n", "text/plain");
      }

      int fps = 0;
      int preset = 0;
      int crf = -1;

      if (req.has_param("fps")) {
        fps = std::stoi(req.get_param_value("fps"));
      }
      if (req.has_param("preset")) {
        preset = std::stoi(req.get_param_value("preset"));
      }
      if (req.has_param("crf")) {
        crf = std::stoi(req.get_param_value("crf"));
      }
      
      std::cout << "CREATING TIMELAPSE..." << std::endl;
      res.set_content("Creating timelapse... this may take awhile\n", "text/plain");

      isCreatingTimelapse.store(true);

      createTimelapseThread = std::make_unique<std::thread>([fps, preset, crf, &isCreatingTimelapse]() {
        int err = createTimelapseHandler(fps, preset, crf);
        isCreatingTimelapse.store(false);

        std::cout << "Timelapse creation finished with code " << err << std::endl;
      });

      std::cout << "Succesfully created timelapse" << std::endl;
      res.set_content("Timelapse has been created\n", "text/plain");
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
          if (file.is_regular_file() && file.path().extension() == ".jpg") {
            std::filesystem::remove(file.path());
          }
        }
        
        res.set_content("Frames have been successfully cleared\n", "text/plain");
      } 
    }
  });

  svr.Get("/shutdown", [](const httplib::Request& req, httplib::Response& res) {
    std::cout << "Shutting down server" << std::endl;
    res.set_content("Shutting down...\n", "text/plain");

    shutdownServer();
  });

  svr.listen("0.0.0.0", 8000);

  if (camThread) std::cout << "Server stopped, waiting for camera to finish..." << std::endl;

  if (camThread) {
    std::cout << "Server stopped, waiting for camera to finish..." << std::endl;
    if (camThread->joinable()) {
      camThread->join();
    }
    std::cout << "Camera shutdown complete." << std::endl;
  }

  globalServer = nullptr;

  return 0;
}
