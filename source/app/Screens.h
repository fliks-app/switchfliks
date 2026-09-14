#pragma once

#include <memory>
#include <string>

#include "app/Screen.h"

namespace app {

struct PlayRequest {
    int mediaId = 0;
    int mediaFileId = 0;
    int episodeId = 0;
    double startAtSeconds = 0;
    std::string title;
    std::string subtitle;
};

std::unique_ptr<Screen> makeServerScreen();
std::unique_ptr<Screen> makeLoginScreen();
std::unique_ptr<Screen> makeHomeScreen();
std::unique_ptr<Screen> makeLibraryScreen(int libraryId, std::string name);
std::unique_ptr<Screen> makeDetailScreen(int mediaId);
std::unique_ptr<Screen> makeSearchScreen();
std::unique_ptr<Screen> makePersonScreen(int personId);
std::unique_ptr<Screen> makeSettingsScreen();
std::unique_ptr<Screen> makePlayerScreen(PlayRequest request);

} // namespace app
