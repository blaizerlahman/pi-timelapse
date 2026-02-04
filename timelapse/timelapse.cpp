#include "timelapse.h"

#include <iomanip>
#include <iostream>
#include <memory>
#include <thread>
#include <mutex>
#include <atomic>
#include <cerrno>
#include <unistd.h>
#include <fcntl.h>
#include <filesystem>
#include <stdexcept>
#include <cstdlib>
#include <cstring>
#include <sys/mman.h>
#include <unistd.h>
#include <sstream>
#include <chrono>
#include <condition_variable>
#include <csignal>

#include <libcamera/libcamera.h>
#include <libcamera/framebuffer.h>

using namespace libcamera;
using namespace std::chrono_literals;

// width/height of camera frames
int WIDTH = 1920;
int HEIGHT = 1080;

std::atomic<bool> shouldStop{false}; // used to dictate when frame requests should be stopped 

std::mutex reqCompleteMutex;
std::condition_variable reqCompleteCV;
std::atomic<bool> requestCompleted{false};

// time to be taken between frame captures
std::chrono::milliseconds CAP_INTERVAL{300};

static std::shared_ptr<Camera> camera;


// captureless lambda that runs to set the global variable based on the CAM_FRAME_PATH env var
std::filesystem::path FRAME_PATH = [] {
  const char* env = std::getenv("CAM_FRAME_PATH");
  if (!env) {
    throw std::runtime_error("CAM_FRAME_PATH not set");
  }

  return std::filesystem::path(env);
}(); // invoke now


// handle stop signals by ending frame processing after current one is finished
void interruptHandler(int signum) {
  if (signum) {
    //keep the compiler happy
  } 
  shouldStop.store(true);
}


static void requestComplete(Request *request) {

  // don't complete request if cancelled or timelapse has ended
  if (request->status() == Request::RequestCancelled || shouldStop.load()) {
    return;
  }

  const std::map<const Stream *, FrameBuffer *> &buffers = request->buffers();

  // iterate through buffers and write frame data to storage 
  for (auto bufferPair : buffers) {
    FrameBuffer *buffer = bufferPair.second;
    const FrameMetadata &metadata = buffer->metadata();

    std::cout << "seq: " << std::setw(6) << std::setfill('0') << metadata.sequence << " bytesused: ";

    unsigned int nplane = 0;
    for (const FrameMetadata::Plane &plane : metadata.planes()) {
      std::cout << plane.bytesused;
      if (++nplane < metadata.planes().size()) std::cout << "/";
    }
    std::cout << std::endl;

    // create frame file name
    std::ostringstream oss;
    oss << "frame_" << std::setw(6) << std::setfill('0') << metadata.sequence << ".yuv";
    auto filename = FRAME_PATH / oss.str();

    int fd = open(filename.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
      std::cerr << "Error opening file: " << std::strerror(errno) << std::endl;
      break;
    }

    const auto &planes = buffer->planes();
    
    size_t totalLength = 0;
    for (unsigned int i = 0; i < planes.size(); ++i) {
      totalLength += planes[i].length;
    }

    // map entire buffer from first plane's fd
    void *baseMem = mmap(nullptr,
                         totalLength,
                         PROT_READ,
                         MAP_SHARED,
                         planes[0].fd.get(),
                         0);

    if (baseMem == MAP_FAILED) {
      std::cerr << "mmap failed: " << std::strerror(errno) << std::endl;
      close(fd);
      break;
    }

    // write only the actual used bytes for each plane
    uint8_t *ptr = static_cast<uint8_t *>(baseMem);
    for (unsigned int i = 0; i < planes.size(); ++i) {
      const unsigned int bytesUsed = metadata.planes()[i].bytesused;
      
      if (bytesUsed == 0)
        continue;

      // write to file
      ssize_t ret = write(fd, ptr, bytesUsed);
      if (ret < 0) {
        std::cerr << "write error: " << std::strerror(errno) << std::endl;
      } else if (ret != (ssize_t)bytesUsed) {
        std::cerr << "partial write" << std::endl;
      }

      // advance to next plane
      ptr += planes[i].length; 
    }

    munmap(baseMem, totalLength);
    close(fd);
  }

  // only requeue if shouldn't stop
  //if (!shouldStop.load()) {
  //  request->reuse(Request::ReuseBuffers);
  //  camera->queueRequest(request);
  //}

  // notify main thread that frame has been processed
  {
    std::lock_guard<std::mutex> lock(reqCompleteMutex);
    requestCompleted.store(true);
  }
  reqCompleteCV.notify_one();
}


int timelapseHandler(int timelapseLength) {

  std::signal(SIGINT, interruptHandler);

  std::unique_ptr<CameraManager> cm = std::make_unique<CameraManager>();
  cm->start();

  for (auto const &camera : cm->cameras()) {
    std::cout << camera->id() << std::endl;
  }

  auto cameras = cm->cameras();
  if (cameras.empty()) {
    std::cout << "No cameras were identified on the system." << std::endl;
    cm->stop();
    return EXIT_FAILURE;
  }

  std::string cameraId = cameras[0]->id();
  camera = cm->get(cameraId);

  camera->acquire();

  std::unique_ptr<CameraConfiguration> config = camera->generateConfiguration( { StreamRole::Viewfinder });

  // use the stream config to configure the actual camera stream (size/format)
  StreamConfiguration &streamConfig = config->at(0);
  std::cout << "Default viewfinder configuration is: " << streamConfig.toString() << std::endl;

  // camera dependent
  streamConfig.size.width = WIDTH;
  streamConfig.size.height = HEIGHT;

  // will give plane 0/1/2 = Y/U/V
  streamConfig.pixelFormat = formats::YUV420;

  // now must validate the updated configuration to ensure it is okay (may change parameters to closest suitable ones)
  config->validate();
  std::cout << "Validated viewfinder config is: " << streamConfig.toString() << std::endl;

  // now that it has been validated we can give it to the camera
  camera->configure(config.get());

  FrameBufferAllocator *allocator = new FrameBufferAllocator(camera);

  // go into config and allocate FrameBuffers and see how many buffers were allocated
  for (StreamConfiguration &cfg : *config) {
    int ret = allocator->allocate(cfg.stream());
    if (ret < 0) {
      std::cerr << "Can't alloc buffers" << std::endl;
      return -ENOMEM;
    }

    size_t allocated = allocator->buffers(cfg.stream()).size();
    std::cout << "Allocated " << allocated << " buffers for stream" << std::endl;
  }

  // libcamera uses streaming model per-frame
  // app must queue a request for each frame it wants
  // Request = one Stream associated with a FrameBuffer representing where frames iwll be stored
  Stream *stream = streamConfig.stream();
  const std::vector<std::unique_ptr<FrameBuffer>> &buffers = allocator->buffers(stream);
  std::vector<std::unique_ptr<Request>> requests;

  // fill requests by created Request instances for the camera and assocaite a buffer with each of them
  for (unsigned int i = 0; i < buffers.size(); i++) {

    // create request
    std::unique_ptr<Request> request = camera->createRequest();
    if (!request) {
      std::cerr << "Can't create request" << std::endl;
      return -ENOMEM;
    }

    const std::unique_ptr<FrameBuffer> &buffer = buffers[i];

    // add buffer to request so it can be filled by it
    int ret =request->addBuffer(stream, buffer.get());
    if (ret < 0) {
      std::cerr << "Can't set buffer for reqeust" << std::endl;
      return ret;
    }

    requests.push_back(std::move(request));
  }

  camera->requestCompleted.connect(requestComplete); 

  camera->start();

  // set length of timelapse in minutes to be inputted time if given or a full day if not given
  int minutes = (timelapseLength > 0) ? timelapseLength : 1440;

  const int totalFrames = (minutes * 60 * 1000) / (CAP_INTERVAL.count());
  for (int i = 0; i < totalFrames && !shouldStop.load(); i++) {

    auto startTime = std::chrono::steady_clock::now();

    if (i > 0) {

      // wait until request is completed
      std::unique_lock<std::mutex> lock(reqCompleteMutex); // grab mutex
      reqCompleteCV.wait(lock, []{ return requestCompleted.load(); });

      if (shouldStop.load()) break; // break loop without requeuing if stop requested

      requestCompleted.store(false);
      requests[0]->reuse(Request::ReuseBuffers);
    }

    camera->queueRequest(requests[0].get()); // requeue request

    auto timeSince = std::chrono::steady_clock::now() - startTime;
    auto timeLeft = CAP_INTERVAL - timeSince;

    // sleep for any remaining time until next frame capture
    if (timeLeft > 0ms) std::this_thread::sleep_for(timeLeft);
  }

  if (shouldStop.load()) {
    std::cout << "\nInterrupt received, finishing current frame..." << std::endl;
  }

  std::this_thread::sleep_for(300ms); // allow time for any remaining frame to process

  shouldStop.store(true); 

  camera->stop();
  allocator->free(stream); // free the buffers in the FrameBufferAllocator
  delete allocator;

  camera->release();
  camera.reset(); 
  cm->stop(); 
  
  return 0;
}


