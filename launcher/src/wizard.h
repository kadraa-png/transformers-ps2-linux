#pragma once
// Which wizard screen shows when: the launcher's whole navigation, testable
// without a window.
namespace launcher {

enum class Screen { Disc, Check, Building, QuickSetup, Home, Settings, Controls, Error };

enum class Event {
    IsoAccepted, IsoRejected, StartBuild, Back, BuildSucceeded, BuildFailed, BuildCancelled,
    Finish, OpenSettings, OpenControls, ChangeDisc, Close, GameCrashed, Retry
};

struct LaunchState {
    bool iso_set = false;
    bool iso_exists = false;
    bool game_built = false;         // binary for the current cache key exists
    bool build_in_progress = false;  // its .tmp work dir exists
    bool setup_done = false;
};

struct Start {
    Screen screen;
    bool resume;  // Check screen offers "Resume build"
};

Start initial_screen(const LaunchState& state);
Screen next_screen(Screen from, Event event, const LaunchState& state);

}  // namespace launcher
