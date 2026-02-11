#ifndef TIMELAPSE_H
#define TIMELAPSE_H

#include <atomic>
#include <mutex>
#include <condition_variable>
#include <filesystem>

extern std::atomic<bool> shouldRecordStop;
extern std::atomic<bool> shouldCreateStop;

extern std::filesystem::path FRAME_PATH;
extern std::filesystem::path TIMELAPSE_PATH;

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
 * @return 0 on success, non-zero on error
 */
int createTimelapseHandler(int fps, int preset, int crf);

#endif
