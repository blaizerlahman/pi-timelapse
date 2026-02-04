#ifndef TIMELAPSE_H
#define TIMELAPSE_H

#include <atomic>
#include <filesystem>

extern std::atomic<bool> shouldStop;
extern std::filesystem::path FRAME_PATH;

/**
 * Captures timelapse using system camera and writes frames to specified path.
 * @param timelapseLength Length of timelapse in minutes (default is 0 which evaluates to 24 hours)
 * @return 0 on success, non-zero on error
 */
int timelapseHandler(int timelapseLength);

#endif
