#include "wizard.h"

namespace launcher {

Start initial_screen(const LaunchState& s) {
    if (!s.iso_set || !s.iso_exists) return {Screen::Disc, false};
    if (!s.game_built) return {Screen::Check, s.build_in_progress};
    if (!s.setup_done) return {Screen::QuickSetup, false};
    return {Screen::Home, false};
}

Screen next_screen(Screen from, Event e, const LaunchState& s) {
    switch (from) {
    case Screen::Disc:
        // A disc whose game is already built (moved ISO, "Change disc" back to
        // the same one) needs no second build.
        if (e == Event::IsoAccepted && s.game_built) return s.setup_done ? Screen::Home : Screen::QuickSetup;
        if (e == Event::IsoAccepted) return Screen::Check;
        break;
    case Screen::Check:
        if (e == Event::StartBuild) return Screen::Building;
        if (e == Event::Back) return Screen::Disc;
        break;
    case Screen::Building:
        if (e == Event::BuildSucceeded) return s.setup_done ? Screen::Home : Screen::QuickSetup;
        if (e == Event::BuildFailed) return Screen::Error;
        if (e == Event::BuildCancelled) return Screen::Check;
        break;
    case Screen::QuickSetup:
        if (e == Event::Finish) return Screen::Home;
        break;
    case Screen::Home:
        if (e == Event::OpenSettings) return Screen::Settings;
        if (e == Event::OpenControls) return Screen::Controls;
        if (e == Event::ChangeDisc) return Screen::Disc;
        if (e == Event::GameCrashed) return Screen::Error;
        break;
    case Screen::Settings:
    case Screen::Controls:
        if (e == Event::Close) return Screen::Home;
        break;
    case Screen::Error:
        // After a game crash the build is fine: Retry goes back to Play.
        if (e == Event::Retry) return s.game_built ? Screen::Home : Screen::Check;
        if (e == Event::Back) return Screen::Disc;
        break;
    }
    return from;
}

}  // namespace launcher
