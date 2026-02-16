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
#include <sys/types.h>
#include <sys/wait.h>

#include <libcamera/libcamera.h>
#include <libcamera/framebuffer.h>
#include <jpeglib.h> 

using namespace libcamera;
using namespace std::chrono_literals;

// width/height of camera frames
int WIDTH = 1920;
int HEIGHT = 1080;

std::atomic<bool> shouldRecordStop{false};
std::atomic<bool> shouldCreateStop{false};

std::mutex reqCompleteMutex;
std::condition_variable reqCompleteCV;
std::atomic<bool> requestCompleted{false};

// default frame capture is 2/sec and length is 1 day
int CAP_INTERVAL = 500; // in ms
int TIMELAPSE_LENGTH = 1440; // in min

static std::shared_ptr<Camera> camera;


// get path to where frames will be stored
std::filesystem::path FRAME_PATH = [] {
  const char* framePath = std::getenv("CAM_FRAME_PATH");
  if (!framePath) {
    throw std::runtime_error("CAM_FRAME_PATH not set");
  }
  return std::filesystem::path(framePath);
}();


// get path to where final timelapse will be stored
std::filesystem::path TIMELAPSE_PATH = [] {
  const char* timelapsePath = std::getenv("CAM_TIMELAPSE_PATH");
  if (!timelapsePath) {
    throw std::runtime_error("CAM_TIMELAPSE_PATH");
  }
  return std::filesystem::path(timelapsePath);
}();


static void requestComplete(Request *request) {

  if (shouldRecordStop.load()) {
    {
      std::lock_guard<std::mutex> lock(reqCompleteMutex);
      requestCompleted.store(true);
    }
    reqCompleteCV.notify_one();
    return;
  }

  if (request->status() == Request::RequestCancelled) {
    return;
  }

  const std::map<const Stream *, FrameBuffer *> &buffers = request->buffers();

  for (auto bufferPair : buffers) {
    FrameBuffer *buffer = bufferPair.second;
    const FrameMetadata &metadata = buffer->metadata();

    if (metadata.sequence % 1000 == 0) {
      std::cout << "seq: " << std::setw(6) << std::setfill('0') << metadata.sequence << std::endl;
    }

    // create file name
    std::ostringstream oss;
    oss << "frame_" << std::setw(6) << std::setfill('0') << metadata.sequence << ".jpg";
    auto filename = FRAME_PATH / oss.str();

    const auto &planes = buffer->planes();

    // calculate total buffer size for all planes
    size_t totalLength = 0;
    for (unsigned int i = 0; i < planes.size(); ++i) {
      totalLength += planes[i].length;
    }

    // map the entire YUV420 buffer
    void *baseMem = mmap(nullptr,
                         totalLength,
                         PROT_READ,
                         MAP_SHARED,
                         planes[0].fd.get(),
                         0);

    if (baseMem == MAP_FAILED) {
      std::cerr << "mmap failed: " << std::strerror(errno) << std::endl;
      break;
    }

    uint8_t *yPlane = static_cast<uint8_t *>(baseMem);
    uint8_t *uPlane = yPlane + planes[0].length;
    uint8_t *vPlane = uPlane + planes[1].length;

    // initialize JPEG compression
    struct jpeg_compress_struct cinfo;
    struct jpeg_error_mgr jerr;
    
    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_compress(&cinfo);

    FILE *outfile = fopen(filename.c_str(), "wb");
    if (!outfile) {
      std::cerr << "Error opening JPEG file: " << std::strerror(errno) << std::endl;
      munmap(baseMem, totalLength);
      break;
    }

    jpeg_stdio_dest(&cinfo, outfile);

    // sst compression parameters
    cinfo.image_width = WIDTH;
    cinfo.image_height = HEIGHT;
    cinfo.input_components = 3;
    cinfo.in_color_space = JCS_YCbCr;

    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, 90, TRUE);  // 90% quality
    
    cinfo.optimize_coding = TRUE;

    jpeg_start_compress(&cinfo, TRUE);

    // convert YUV420 planar to YUV444 for JPEG
    std::vector<uint8_t> row_buffer(WIDTH * 3);
    
    for (int y = 0; y < HEIGHT; y++) {
      for (int x = 0; x < WIDTH; x++) {
        int yIdx = y * WIDTH + x;
        int uvIdx = (y / 2) * (WIDTH / 2) + (x / 2);
        
        row_buffer[x * 3 + 0] = yPlane[yIdx]; 
        row_buffer[x * 3 + 1] = uPlane[uvIdx];
        row_buffer[x * 3 + 2] = vPlane[uvIdx];
      }
      
      uint8_t *row_pointer = row_buffer.data();
      jpeg_write_scanlines(&cinfo, &row_pointer, 1);
    }

    // clean up compression
    jpeg_finish_compress(&cinfo);
    jpeg_destroy_compress(&cinfo);
    fclose(outfile);

    munmap(baseMem, totalLength);
  }

  {
    std::lock_guard<std::mutex> lock(reqCompleteMutex);
    requestCompleted.store(true);
  }
  reqCompleteCV.notify_one();
}


int recordTimelapseHandler(int timelapseLength = 0, int capInterval = 0) {

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

  {
    std::unique_ptr<CameraConfiguration> config = camera->generateConfiguration( { StreamRole::VideoRecording });

    StreamConfiguration &streamConfig = config->at(0);
    std::cout << "Default VideoRecording configuration is: " << streamConfig.toString() << std::endl;

    streamConfig.size.width = WIDTH;
    streamConfig.size.height = HEIGHT;
    streamConfig.pixelFormat = formats::YUV420;

    config->validate();
    std::cout << "Validated VideoRecording config is: " << streamConfig.toString() << std::endl;

    camera->configure(config.get());

    FrameBufferAllocator *allocator = new FrameBufferAllocator(camera);

    for (StreamConfiguration &cfg : *config) {
      int ret = allocator->allocate(cfg.stream());
      if (ret < 0) {
        std::cerr << "Can't alloc buffers" << std::endl;
        delete allocator;
        return -ENOMEM;
      }

      size_t allocated = allocator->buffers(cfg.stream()).size();
      std::cout << "Allocated " << allocated << " buffers for stream" << std::endl;
    }

    Stream *stream = streamConfig.stream();
    const std::vector<std::unique_ptr<FrameBuffer>> &buffers = allocator->buffers(stream);
    std::vector<std::unique_ptr<Request>> requests;

    for (unsigned int i = 0; i < buffers.size(); i++) {
      std::unique_ptr<Request> request = camera->createRequest();
      if (!request) {
        std::cerr << "Can't create request" << std::endl;
        return -ENOMEM;
      }

      const std::unique_ptr<FrameBuffer> &buffer = buffers[i];

      int ret = request->addBuffer(stream, buffer.get());
      if (ret < 0) {
        std::cerr << "Can't set buffer for reqeust" << std::endl;
        return ret;
      }

      requests.push_back(std::move(request));
    }

    camera->requestCompleted.connect(requestComplete);

    camera->start();

    capInterval = (capInterval > 0) ? capInterval : CAP_INTERVAL;
    timelapseLength = (timelapseLength > 0) ? timelapseLength : TIMELAPSE_LENGTH;

    const int totalFrames = (timelapseLength * 60 * 1000) / capInterval;
    for (int i = 0; i < totalFrames && !shouldRecordStop.load(); i++) {

      auto startTime = std::chrono::steady_clock::now();

      if (i > 0) {
        std::unique_lock<std::mutex> lock(reqCompleteMutex);
        reqCompleteCV.wait(lock, []{ return requestCompleted.load(); });

        if (shouldRecordStop.load()) break;

        requestCompleted.store(false);
        requests[0]->reuse(Request::ReuseBuffers);
      }

      camera->queueRequest(requests[0].get());

      auto timeSince = std::chrono::steady_clock::now() - startTime;
      auto timeLeft = std::chrono::milliseconds(capInterval) - timeSince;

      if (timeLeft > 0ms) std::this_thread::sleep_for(timeLeft);
    }

    if (shouldRecordStop.load()) {
      std::cout << "\nInterrupt received, finishing current frame..." << std::endl;
    }

    shouldRecordStop.store(true);

    std::this_thread::sleep_for(300ms);

    camera->requestCompleted.disconnect(requestComplete);
    requests.clear();

    camera->stop();

    allocator->free(stream);
    delete allocator;
  }

  camera->release();
  camera.reset();

  cm->stop();

  return 0;
}


enum class Preset : int {
  Medium = 1,
  Faster = 2,
  VeryFast = 3
};


const std::string getPreset(Preset preset) {
  switch (preset) {
    case Preset::Medium: return "medium";
    case Preset::Faster: return "faster";
    case Preset::VeryFast: return "veryfast";
  }

  throw std::invalid_argument("Invalid preset");
}


int createTimelapseHandler(int fps, int preset, int crf) {

  // set parameters to defaults if invalid
  fps = (fps > 0) ? fps : 60;
  preset = (preset > 0 && preset <= 3) ? preset : 2;
  crf = (crf > -1 && crf <= 51) ? crf : 23;

  std::string fpsStr = std::to_string(fps);
  std::string presetStr = getPreset(static_cast<Preset>(preset));
  std::string crfStr = std::to_string(crf);

  std::string frameInputPattern = (FRAME_PATH / "frame_%06d.jpg").string();

  // get current time to identify timelapse
  auto time = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());

  std::ostringstream oss;
  oss << "timelapse_" << std::put_time(std::localtime(&time), "%m_%d_%Y_%H_%M_%S") << ".mp4";

  std::string timelapseOutputPath = (TIMELAPSE_PATH / oss.str()).string();

  std::cout << "Creating timelapse: " << timelapseOutputPath << std::endl;
  std::cout << "Settings: fps=" << fps << ", preset=" << presetStr << ", crf=" << crf << std::endl;

  pid_t pid = fork();
  
  if (pid < 0) {
    std::cerr << "Unable to fork process" << std::endl;
    return -1;
  }

  // child process executes ffmpeg command
  if (pid == 0) {
    execl("/usr/bin/ffmpeg",
        "ffmpeg",
        "-framerate", fpsStr.c_str(),
        "-i", frameInputPattern.c_str(),
        "-c:v", "libx264",
        "-preset", presetStr.c_str(),
        "-crf", crfStr.c_str(),
        "-pix_fmt", "yuv420p",
        timelapseOutputPath.c_str(),
        nullptr
    );

    std::cerr << "Exec'ing ffmpeg command failed: " << std::strerror(errno) << std::endl;
    _exit(1);
  }

  int childStatus;
  while (true) {

    pid_t result = waitpid(pid, &childStatus, WNOHANG);

    if (result < 0) {
      std::cerr << "waitpid failed: " << std::strerror(errno) << std::endl;
      return -1;
    } 

    // child process finished
    if (result > 0) {

      // if ended on its own
      if (WIFEXITED(childStatus)) {

        int err = WEXITSTATUS(childStatus);
        if (!err) {
          std::cout << "Timelapse successfully created: " << timelapseOutputPath << std::endl;
          return 0;
        } else {
          std::cerr << "ffmpeg exited with code " << err << std::endl;
          return err;
        }

      } else if (WIFSIGNALED(childStatus)) { // if signaled/interrupted
        std::cout << "ffmpeg killed by signal " << WTERMSIG(childStatus) << std::endl;
        return -1;
      }
    }

    // check if flagged to stop
    if (shouldCreateStop.load()) {

      std::cout << "Stopping timelapse creation..." << std::endl;

      kill(pid, SIGTERM);

      // allow for ffmpeg to shut down
      std::this_thread::sleep_for(2000ms);

      // check if ffmpeg still running and force kill it if it was not shut down gracefully
      int running = waitpid(pid, &childStatus, WNOHANG);
      if (!running) {
        std::cout << "Force killing ffmpeg" << std::endl;
        kill(pid, SIGKILL);
        waitpid(pid, &childStatus, 0);
      }

      shouldCreateStop.store(false);
      return -1;
    }

    std::this_thread::sleep_for(400ms);
  }
}
