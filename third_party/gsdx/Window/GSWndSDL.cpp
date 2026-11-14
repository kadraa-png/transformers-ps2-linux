#include "stdafx.h"
#include "GSWndSDL.h"
namespace {
unsigned char* capture_pixels = nullptr;
int capture_width = 0;
int capture_height = 0;
bool capture_done = false;
}

void GSSDL_CaptureNextFlip(unsigned char* pixels, int width, int height) {
    capture_pixels = pixels;
    capture_width = width;
    capture_height = height;
    capture_done = false;
}

bool GSSDL_FinishCapture() {
    capture_pixels = nullptr; // GSvsync may return without presenting a disabled display.
    const bool done = capture_done;
    capture_done = false;
    return done;
}


void GSWndSDL::CreateContext(int major, int minor) {
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, major);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, minor);
    context = SDL_GL_CreateContext(window);
    if (!context) {
        fprintf(stderr, "GSdx SDL OpenGL context: %s\n", SDL_GetError());
        throw GSDXRecoverableError();
    }
}

bool GSWndSDL::Attach(void* handle, bool managed) {
    window = static_cast<SDL_Window*>(handle);
    if (!window) throw GSDXRecoverableError();
    m_managed = managed;
    FullContextInit();
    return true;
}

void GSWndSDL::Detach() {
    DetachContext();
    if (context) SDL_GL_DestroyContext(context);
    context = nullptr;
    window = nullptr;
}

GSVector4i GSWndSDL::GetClientRect() {
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(window, &w, &h);
    return GSVector4i(0, 0, w, h);
}

void GSWndSDL::AttachContext() {
    if (SDL_GL_MakeCurrent(window, context)) m_ctx_attached = true;
    else throw GSDXRecoverableError();
}

void GSWndSDL::DetachContext() {
    if (m_ctx_attached) SDL_GL_MakeCurrent(window, nullptr);
    m_ctx_attached = false;
}

void* GSWndSDL::GetProcAddress(const char* name, bool opt) {
    void* ptr = reinterpret_cast<void*>(SDL_GL_GetProcAddress(name));
    if (!ptr && !opt) {
        fprintf(stderr, "GSdx missing GL symbol: %s\n", name);
        throw GSDXRecoverableError();
    }
    return ptr;
}

void GSWndSDL::SetSwapInterval() { SDL_GL_SetSwapInterval(m_vsync); }
void GSWndSDL::Flip() {
    if (m_vsync_change_requested.exchange(false)) SetSwapInterval();
    if (capture_pixels) {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        glReadBuffer(GL_BACK);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, capture_width, capture_height, GL_RGBA, GL_UNSIGNED_BYTE, capture_pixels);
        capture_done = glGetError() == GL_NO_ERROR;
        capture_pixels = nullptr;
    }
    SDL_GL_SwapWindow(window);
}
