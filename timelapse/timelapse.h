#ifndef TIMELAPSE_H
#define TIMELAPSE_H

#include <atomic>
#include <mutex>
#include <condition_variable>
#include <filesystem>
#include <string>

extern std::atomic<bool> shouldRecordStop;
extern std::atomic<bool> shouldCreateStop;

extern std::filesystem::path FRAME_PATH;
extern std::filesystem::path TIMELAPSE_PATH;

// to be used by server state
extern std::atomic<int> currFrameNo;
extern std::atomic<int> totalFrames;

struct ProgressBlock {
  std::string frame;
  std::string fps;
  std::string bitrate;
  std::string totalSize;
  std::string speed;
};

/**
 * Captures timelapse using system camera and writes frames to specified path.
 * @param timelapseLength Length of timelapse in minutes (default is 0 which evaluates to 24 hours)
 * @param capInterval Interval of frame capture in milliseconds (default is 0 which evaluates to 500 milliseconds)
 * @return 0 on success, non-zero on error
 */
int recordTimelapseHandler(int timelapseLength, int capInterval);

/**
 * Creates timelapse using ffmpeg command and writers final mp4 to specified path.
 * @param fps Framerate used in ffpmeg command (default is 0 which evaluates to 60)
 * @param preset Speed preset corresponding to presets in ffmpeg command (default is 0 which evaluates to 2). Used to index enum (1 - medium, 2 - faster, 3 - veryfast)
 * @param crf Encoding mode that determines visual quality and file size (default is -1 which evaluates to 23)
 * @param requestedFilename The name of the output file that the timelapse will be written to (default is an empty string which evaluates to the exact time the timelapse creation started)
 * @return 0 on success, non-zero on error
 */
int createTimelapseHandler(int fps, int preset, int crf, std::string requestedFilename);

/**
 * Reads most recent (at time of calling) timelapse creation progress from file at PROGRESS_PATH and populates struct with progress dat.
 * @param ProgressBlock An empty ProgressBlock struct to be populated by function
 * @return 0 on success, non-zero on error
 */
int readCreationProgress(ProgressBlock& block);

#endif
