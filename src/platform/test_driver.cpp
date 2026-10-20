#include "platform/test_driver.h"

#include <SDL3/SDL.h>
#if defined(MSCHARGED_HOST_SCREENSHOTS)
#include <aurora/video.h>
#endif

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>

namespace mscharged::platform
{
namespace
{
using Clock = std::chrono::steady_clock;

struct Command
{
    enum Kind { Key, Wait, Snap, Point } kind;
    float x = 0, y = 0;
    bool aim = false;
    SDL_Scancode key = SDL_SCANCODE_UNKNOWN;
    int ms = 0;
    std::string name;
};

struct Driver
{
    std::filesystem::path directory;
    std::filesystem::file_time_type seen{};
    std::deque<Command> queue;
    bool running = false;
    Clock::time_point until{};
    SDL_Scancode held = SDL_SCANCODE_UNKNOWN;
    bool aiming = false;
    float x = 0, y = 0;
    int scripts = 0;
    std::mutex mutex;
};

Driver* Get()
{
    static Driver* driver = [] {
        const char* path = std::getenv("RECHARGED_TEST_DRIVER");
        if (!path || !*path) return static_cast<Driver*>(nullptr);
        auto* created = new Driver;
        created->directory = path;
        std::error_code error;
        std::filesystem::create_directories(created->directory, error);
        return created;
    }();
    return driver;
}

void Load(Driver& driver)
{
    std::ifstream file(driver.directory / "script");
    std::string line;
    while (std::getline(file, line))
    {
        if (const auto hash = line.find('#'); hash != std::string::npos) line.erase(hash);
        std::istringstream words(line);
        std::string verb;
        if (!(words >> verb)) continue;
        Command command{Command::Wait};
        if (verb == "key")
        {
            std::string key;
            words >> key >> command.ms;
            command.kind = Command::Key;
            command.key = SDL_GetScancodeFromName(key.c_str());
            if (command.key == SDL_SCANCODE_UNKNOWN) std::fprintf(stderr, "Test driver: unknown key %s\n", key.c_str());
            driver.queue.push_back(command);
            // Released a moment before the next command, so that two presses are two.
            command = {Command::Wait};
            command.ms = 60;
        }
        else if (verb == "wait")
            words >> command.ms;
        else if (verb == "point")
        {
            command.kind = Command::Point;
            std::string first;
            words >> first;
            command.aim = first != "off";
            if (command.aim)
            {
                command.x = std::strtof(first.c_str(), nullptr);
                words >> command.y;
            }
        }
        else if (verb == "snap")
        {
            command.kind = Command::Snap;
            words >> command.name;
        }
        else
        {
            std::fprintf(stderr, "Test driver: unknown command %s\n", verb.c_str());
            continue;
        }
        driver.queue.push_back(command);
    }
    driver.running = true;
}

void Finish(Driver& driver)
{
    driver.running = false;
    ++driver.scripts;
    std::ofstream(driver.directory / "status") << "done " << driver.scripts << "\n";
}
} // namespace

bool TestDriverActive()
{
    return Get() != nullptr;
}

void ServiceTestDriver()
{
    Driver* driver = Get();
    if (!driver) return;
    std::lock_guard lock(driver->mutex);
    const auto now = Clock::now();
    if (!driver->running)
    {
        std::error_code error;
        const auto time = std::filesystem::last_write_time(driver->directory / "script", error);
        if (error || time == driver->seen) return;
        driver->seen = time;
        Load(*driver);
    }
    if (now < driver->until) return;
    driver->held = SDL_SCANCODE_UNKNOWN;
    while (!driver->queue.empty())
    {
        const Command command = driver->queue.front();
        driver->queue.pop_front();
        switch (command.kind)
        {
        case Command::Key:
            driver->held = command.key;
            driver->until = now + std::chrono::milliseconds(command.ms);
            return;
        case Command::Wait:
            driver->until = now + std::chrono::milliseconds(command.ms);
            return;
        case Command::Point:
            driver->aiming = command.aim;
            driver->x = command.x;
            driver->y = command.y;
            continue;
        case Command::Snap:
        {
#if defined(MSCHARGED_HOST_SCREENSHOTS)
            const auto utf8 = (driver->directory / (command.name + ".png")).u8string();
            const std::string path(utf8.begin(), utf8.end());
            if (!aurora_request_video_screenshot(path.c_str()))
                std::fprintf(stderr, "Test driver: screenshot %s skipped\n", command.name.c_str());
#endif
            // The screenshot waits for the next presented frame.
            driver->until = now + std::chrono::milliseconds(100);
            return;
        }
        }
    }
    Finish(*driver);
}

void ApplyTestDriverKeys(bool* keys, std::size_t count)
{
    Driver* driver = Get();
    if (!driver) return;
    std::lock_guard lock(driver->mutex);
    if (driver->held != SDL_SCANCODE_UNKNOWN && std::size_t(driver->held) < count) keys[driver->held] = true;
}
bool TestDriverPointer(float& x, float& y)
{
    Driver* driver = Get();
    if (!driver) return false;
    std::lock_guard lock(driver->mutex);
    x = driver->x;
    y = driver->y;
    return driver->aiming;
}
} // namespace mscharged::platform
