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

#include <libcamera/libcamera.h>
#include <libcamera/framebuffer.h>

using namespace libcamera;
using namespace std::chrono_literals;

int WIDTH = 1920;
int HEIGHT = 1080;

static std::shared_ptr<Camera> camera;

// captureless lambda that runs to set the global variable based on the CAM_FRAME_PATH env var
std::filesystem::path FRAME_PATH = [] {
  const char* env = std::getenv("CAM_FRAME_PATH");
  if (!env) {
    throw std::runtime_error("CAM_FRAME_PATH not set");
  }

  return std::filesystem::path(env);
}(); // invoke now

std::atomic<bool> shouldStop{false}; // used to dictate when frame requests should no longer be completed

static void requestComplete(Request *request) {

  if (request->status() == Request::RequestCancelled) {
    return;
  }

  if (shouldStop.load()) {
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
  if (!shouldStop.load()) {
    request->reuse(Request::ReuseBuffers);
    camera->queueRequest(request);
  }
}

int main() {

  // create camera manager before anything else and start it
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

  // assign a camera using its ID
  std::string cameraId = cameras[0]->id();
  camera = cm->get(cameraId);

  // acquire camera
  camera->acquire();

  // create config for camera depending on the role you want
  std::unique_ptr<CameraConfiguration> config = camera->generateConfiguration( { StreamRole::StillCapture });

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

  // now need to allocate FrameBuffers so libcamera can write incoming frames to them (and app can read them)
  // amount of memory reserved should be based on configured image size/format
  // libcamera consumes buffers, should usually be provided by application but
  // if only on same device (like the Pi) you can use a FrameBufferAllocator to allocate manually
  
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
  for (int i = 0; i < buffers.size(); i++) {

    // create request
    std::unique_ptr<Request> request = camera->createRequest();
    if (!request) {
      std::cerr << "Can't create request" << std::endl;
      return -ENOMEM;
    }

    // get buffer
    const std::unique_ptr<FrameBuffer> &buffer = buffers[i];

    // add buffer to request so it can be filled by it
    int ret =request->addBuffer(stream, buffer.get());
    if (ret < 0) {
      std::cerr << "Can't set buffer for reqeust" << std::endl;
      return ret;
    }

    requests.push_back(std::move(request));
  }

  // can notify app that a buffer with data is available and also that a requst has been completed
  // a request being completed would mean that all the buffers the request contains ahve been completed
  // request completions are registers in the same order as the requests were queued to the camera in

  camera->requestCompleted.connect(requestComplete); // requestComplete is a slot function that handles the app accessing the image data

  // EXAMPLE OF HOW TO WRITE IMAGE DATA TO DISK IS IN FileSink CLASS WHICH IS PART OF THE cam UTILITY APPLICATION

  // now we can actually start camera and queue requests for it
  camera->start();
  for (std::unique_ptr<Request> &request : requests) {
    camera->queueRequest(request.get());
  }

  // the actual event processing occurs in an internal thread, so the application can manage its own execution in its own thread
  // basically only has to respond to events emitted through signals
  // example: let record for 5 seconds while libcamera generates completion events that
  // the app will handle in requestComplete() slot function connected to the Camera::requstCompleted signal
  std::this_thread::sleep_for(10000ms);

  shouldStop.store(true); // stop the requesting of frames

  std::this_thread::sleep_for(300ms); // allow any currently processing frames to finsih

  // now we can clean ip and stop the camera
  // need to first stop camera
  camera->stop();

  allocator->free(stream); // free the buffers in the FrameBufferAllocator
  delete allocator;

  camera->release(); // release lock on camera
  camera.reset(); // reset its pointer

  cm->stop(); // stop camera manager
  
  return 0;
}
