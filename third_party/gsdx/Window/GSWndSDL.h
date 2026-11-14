#pragma once
#include "GSWnd.h"
#include <SDL3/SDL.h>
// Used only by the explicit PNG screenshot path; ordinary frames never read back.
void GSSDL_CaptureNextFlip(unsigned char* pixels, int width, int height);
bool GSSDL_FinishCapture();

class GSWndSDL final : public GSWndGL {
    SDL_Window* window = nullptr;
    SDL_GLContext context = nullptr;
    void PopulateWndGlFunction() override {}
    void CreateContext(int major, int minor) override;
    void SetSwapInterval() override;
    bool HasLateVsyncSupport() override { return false; }
public:
    ~GSWndSDL() override { Detach(); }
    bool Create(const std::string&, int, int) override { return false; }
    bool Attach(void* handle, bool managed = true) override;
    void Detach() override;
    void* GetDisplay() override { return window; }
    void* GetHandle() override { return window; }
    GSVector4i GetClientRect() override;
    bool SetWindowText(const char* title) override { return SDL_SetWindowTitle(window, title); }
    void AttachContext() override;
    void DetachContext() override;
    void* GetProcAddress(const char* name, bool opt = false) override;
    void Show() override { SDL_ShowWindow(window); }
    void Hide() override { SDL_HideWindow(window); }
    void HideFrame() override {}
    void Flip() override;
};
