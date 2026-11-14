/* SPU2-X native runtime integration. DSP and hardware register algorithms are
 * retained from PCSX2 1.6 (LGPL-3.0-or-later; see LGPL.txt).
 */
#pragma once
#include "ps2/types.h"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#define __POSIX__ 1

#define __forceinline inline __attribute__((always_inline))
#define __fastcall
#define pxAssume(x) assert(x)
#define pxAssertMsg(x, msg) assert(x)
template <typename T>
inline T GetClamped(T src, T min, T max) { return std::min(std::max(src, min), max); }
template <typename T>
inline void Clampify(T& src, T min, T max) { src = GetClamped(src, min, max); }
#define jNO_DEFAULT default: break;

// Plugin debug/configuration features do not participate in SPU2 hardware state.
inline constexpr bool IsDevBuild = false;
inline constexpr bool EffectsDisabled = false;
inline constexpr bool postprocess_filter_dealias = false;
inline constexpr int Interpolation = 4; // SPU2-X default Catmull-Rom
inline constexpr int SndOutVolumeShift = 12;
inline constexpr float FinalVolume = 1.0f;
inline constexpr unsigned delayCycles = 4;
extern bool psxmode;
inline bool MsgToConsole() { return false; }
inline bool MsgCache() { return false; }
inline bool MsgDMA() { return false; }
inline bool MsgAutoDMA() { return false; }
inline bool MsgVoiceOff() { return false; }
inline bool MsgKeyOnOff() { return false; }
#define ConLog(...) ((void)0)
#define FileLog(...) ((void)0)
#define DMALog() false
inline FILE* OpenBinaryLog(const wchar_t*) { return nullptr; }
inline void safe_fclose(FILE*& fp) { if (fp) fclose(fp); fp = nullptr; }
inline constexpr const wchar_t* DMA4LogFileName = L"";
inline constexpr const wchar_t* DMA7LogFileName = L"";

struct StereoOut16;
struct StereoOutFloat;
#include "defs.h"
#include "regs.h"

// Plugin-independent interrupt and audio sink bindings supplied by spu2.cpp.
extern void (*dma4callback)();
extern void (*dma7callback)();
extern u32 lClocks;
extern void (*spu2_irq_for_core)(int);
extern unsigned spu2_pending_irq;
namespace SndBuffer { void Write(const StereoOut32& sample); }
namespace WaveDump { inline void WriteCore(int, int, const StereoOut32&) {} }
enum { CoreSrc_DryVoiceMix, CoreSrc_WetVoiceMix, CoreSrc_PreReverb,
       CoreSrc_PostReverb, CoreSrc_Input, CoreSrc_External };
