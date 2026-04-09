#include "timelapse.h"

#include <httplib.h>
#include <string>
#include <iostream>
#include <thread>
#include <atomic>
#include <condition_variable>
#include <csignal>
#include <fstream>

extern std::atomic<bool> shouldRecordStop;
extern std::atomic<bool> shouldCreateStop;

extern std::filesystem::path FRAME_PATH;
extern std::filesystem::path TIMELAPSE_PATH;

static httplib::Server *globalServer = nullptr;


// stops running camera process and then shuts down httplib server
void shutdownServer() {
  shouldRecordStop.store(true);
  shouldCreateStop.store(true);

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
  std::atomic<bool> isDownloadingTimelapse{false};


  svr.Get("/start-cam", [&isCamRunning, &camThread, &isCreatingTimelapse](const httplib::Request& req, httplib::Response& res) {
    
    if (isCamRunning.load()) {
      res.status = 500;
      std::cerr << "Camera has already been started." << std::endl;
      res.set_content("Error: camera has already been started.\n", "text/plain");
    } else if (isCreatingTimelapse.load()) {
      res.status = 500;
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
      } else {
        std::cout << "No length parameter specified" << std::endl;
      }

      int capInterval = 0;
      if (req.has_param("cap-interval")) {
        capInterval = std::stoi(req.get_param_value("cap-interval"));
      } else {
        std::cout << "No cap-interval parameter specified" << std::endl;
      }

      isCamRunning.store(true);
      shouldRecordStop.store(false);

      camThread = std::make_unique<std::thread>([length, capInterval, &isCamRunning]() {
          int err = recordTimelapseHandler(length, capInterval);
          isCamRunning.store(false);
          
          std::cout << "Timelapse finished with code " << err << std::endl;
      });

      res.set_content("Timelapse started\n", "text/plain");
    }
  });


  svr.Get("/stop-cam", [&isCamRunning](const httplib::Request& req, httplib::Response& res) {
    
    if (!isCamRunning.load()) {
      res.status = 500;
      std::cerr << "No camera is currently running" << std::endl;
      res.set_content("Error: no camera is currently running.\n", "text/plain");
    } else {

      std::cout << "STOPPING CAMERA..." << std::endl;
      
      shouldRecordStop.store(true);

      std::cout << "Succesfully stopped camera process" << std::endl;
      res.set_content("Timelapse has been stopped\n", "text/plain");
    }
  });


  svr.Get("/clear-frames", [&isCamRunning](const httplib::Request& req, httplib::Response& res) {
    if (isCamRunning.load()) {
      res.status = 500;
      std::cerr << "Frames attempted to clear while camera running" << std::endl;
      res.set_content("Error: cannot clear frames while camera is running.\n", "text/plain");
    } else {

      std::cout << "Clearing frames..." << std::endl;

      // remove all regular files in frame directory if specified
      if (req.has_param("all")) {
        if (req.get_param_value("all") != "true") {
          res.status = 500;
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


  svr.Get("/create-timelapse", [&isCamRunning, &isCreatingTimelapse, &createTimelapseThread](const httplib::Request& req, httplib::Response& res) {

    if (isCamRunning.load()) {
      res.status = 500;
      std::cerr << "Camera is currently running, cannot create timelapse" << std::endl;
      res.set_content("Error: cannot create timelapse, camera is currently running.\n", "text/plain");
    } else if (isCreatingTimelapse.load()) {
      res.status = 500;
      std::cerr << "Cannot create timelapse while timelapse is already being created" << std::endl;
      res.set_content("Error: cannot create timelapse, timelapse is already being created.\n", "text/plain");
    } else {

      // check if directory has frames
      if (std::filesystem::is_empty(FRAME_PATH)) {
        res.status = 500;
        std::cerr << "Cannot create timelapse, no frames in frame directory" << std::endl;
        res.set_content("Error: cannot create timelapse, there are no frames in frame directory.\n", "text/plain");
        return;
      }

      // check if timelapse directory exists
      if (!std::filesystem::exists(TIMELAPSE_PATH) || !std::filesystem::is_directory(TIMELAPSE_PATH)) {
        res.status = 404;
        std::cerr << "Cannot create timelapse, the timelapse path does not point to an existing directory" << std::endl;
        res.set_content("Error: cannot create timelapse, the timelapse path does not point to an existing directory.\n", "text/plain");
        return;
      }

      // join previous thread if it exists
      if (createTimelapseThread && createTimelapseThread->joinable()) {
        createTimelapseThread->join();
      }

      int fps = 0;
      int preset = 0;
      int crf = -1;
      std::string requestedFilename = "";

      if (req.has_param("fps")) {
        fps = std::stoi(req.get_param_value("fps"));
      }
      if (req.has_param("preset")) {
        preset = std::stoi(req.get_param_value("preset"));
      }
      if (req.has_param("crf")) {
        crf = std::stoi(req.get_param_value("crf"));
      }
      if (req.has_param("filename")) {
        requestedFilename = req.get_param_value("filename");
      }
      
      std::cout << "CREATING TIMELAPSE..." << std::endl;
      res.set_content("Creating timelapse... this may take awhile\n", "text/plain");

      isCreatingTimelapse.store(true);

      createTimelapseThread = std::make_unique<std::thread>([fps, preset, crf, requestedFilename, &res, &isCreatingTimelapse]() {
        int err = createTimelapseHandler(fps, preset, crf, requestedFilename);
        isCreatingTimelapse.store(false);

        std::cout << "Timelapse creation finished with code " << err << std::endl;
      });
    }
  });


  svr.Get("/stop-create", [&isCreatingTimelapse](const httplib::Request& req, httplib::Response& res) {
    
    // check if timelapse is already being created and return an error if it is not
    if (!isCreatingTimelapse.load()) {
      res.status = 500;
      std::cerr << "No timelapse is currently being created" << std::endl;
      res.set_content("Error: no timelapse is currently being created.\n", "text/plain");
    } else { // stop timelapse creation (creating thread will read shouldCreateStop and will stop)
      std::cout << "STOPPING TIMELAPSE CREATION..." << std::endl;
      shouldCreateStop.store(true);
      res.set_content("Timelapse creation is being stopped.\n", "text/plain");
    }
  });

  svr.Get("/download-timelapse", [&isDownloadingTimelapse](const httplib::Request& req, httplib::Response& res) {
    
    // check if timelapse is already being downloaded on client and return error if so
    if (isDownloadingTimelapse.load()) {
      res.status = 500;
      std::cerr << "Timelapse is currently being downloaded" << std::endl;
      res.set_content("Error: A timelapse is already being downloaded.\n", "text/plain");
      return;
    } 

    // check if client requested a filename and if the file exists on the timelapse server
    if (!req.has_param("filename")) {
      std::cerr << "Filename not provided in request" << std::endl;
      res.status = 500;
      res.set_content("Error: Filename not provided in the request.\n", "text/plain");
      return;
    }

    // add .mp4 onto end of filename if not provided
    std::string requestedFilename = req.get_param_value("filename");
    std::string filename = (requestedFilename.ends_with(".mp4")) ? requestedFilename : requestedFilename + ".mp4";

    std::filesystem::path filepath = TIMELAPSE_PATH / filename;

    // return error if given filename does not exist
    if (!std::filesystem::exists(filepath)) {
      std::cerr << "File does not exist" << std::endl;
      res.status = 404;
      std::string error = std::format("Error: Timelapse {} does not exist on the server.\n", filepath.string());
      res.set_content(error, "text/plain");
      return;
    }

    std::cout << "CLIENT DOWNLOADING..." << filepath.string() << "..." << std::endl;

    // create shared pointer to the timelapse
    auto videoFile = std::make_shared<std::ifstream>(filepath, std::ios::binary);
    if (!videoFile->is_open()) {
      std::cerr << "Could not open timelapse file" << std::endl;
      res.status = 500;
      std::string error = std::format("Error: Failed to open timelapse {}.\n", filepath.string());
      res.set_content(error, "text/plain");
    }

    // tell client to treat it as a file download and name the file
    res.set_header("Content-Disposition", "attachment; filename=\"" + filepath.filename().string() + "\"");

    auto filesize = std::filesystem::file_size(filepath);

    isDownloadingTimelapse.store(true);

    res.set_content_provider(
      filesize, 
      "video/mp4",
      [videoFile](size_t offset, size_t length, httplib::DataSink &sink) { // register the streaming lambda, passing the pointer to the timelapse file

        // seek to position in file that httplib requests (cast to streamoff because that's what seekg takes)
        videoFile->seekg(static_cast<std::streamoff>(offset));
        
        // create byte buffer that is size of requested chunk
        std::vector<char> buffer(length);

        // read data from the file into the buffer at chunk length
        videoFile->read(buffer.data(), static_cast<std::streamsize>(length));

        auto bytesRead = videoFile->gcount(); // returns how many bytes were read in the last read call
        if (bytesRead <= 0) return false; // signals httplib to exit read loop if no more bytes were read
       
        // writes data in buffer to client socket (via httplib DataSink object)
        sink.write(buffer.data(), bytesRead);
        
        return true;
      },
      // clear isDownloadingTimelapse once httplib is done with lambda (finished streaming)
      [&isDownloadingTimelapse](bool success) {
        isDownloadingTimelapse.store(false);
      }
    );

  });

  svr.Get("/shutdown", [](const httplib::Request& req, httplib::Response& res) {
    std::cout << "Shutting down server" << std::endl;
    res.set_content("Shutting down...\n", "text/plain");

    shutdownServer();
  });

  svr.listen("0.0.0.0", 8000);

  if (camThread) {
    std::cout << "Server stopped, waiting for camera to finish..." << std::endl;
    if (camThread->joinable()) {
      camThread->join();
    }
    std::cout << "Camera shutdown complete." << std::endl;
  }

  if (createTimelapseThread) {
    std::cout << "Server stopped, waiting for timelapse to finish..." << std::endl;
    if (createTimelapseThread->joinable()) {
      createTimelapseThread->join();
    }
  }

  globalServer = nullptr;

  return 0;
}
