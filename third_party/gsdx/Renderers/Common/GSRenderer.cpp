/*
 *	Copyright (C) 2007-2009 Gabest
 *	http://www.gabest.org
 *
 *  This Program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This Program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with GNU Make; see the file COPYING.  If not, write to
 *  the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA USA.
 *  http://www.gnu.org/copyleft/gpl.html
 *
 */

#include "stdafx.h"
#include "GSRenderer.h"
#include <atomic>
#include <mutex>

// gs::request_dump (F10 in the port): record a raw dump starting at the next
// vsync, like PS2_GS_DUMP but at a moment chosen while playing.
std::mutex g_gsdx_dump_mutex;
std::string g_gsdx_dump_path;
std::atomic<long> g_gsdx_dump_vsyncs{0};
#if defined(__unix__)
#include <X11/keysym.h>
#endif

const unsigned int s_interlace_nb = 8;
const unsigned int s_post_shader_nb = 5;
const unsigned int s_aspect_ratio_nb = 3;
const unsigned int s_mipmap_nb = 3;

GSRenderer::GSRenderer()
	: m_shader(0)
	, m_shift_key(false)
	, m_control_key(false)
	, m_texture_shuffle(false)
	, m_real_size(0,0)
	, m_wnd()
	, m_dev(NULL)
{
	m_GStitleInfoBuffer[0] = 0;

	m_interlace   = theApp.GetConfigI("interlace") % s_interlace_nb;
	m_aspectratio = theApp.GetConfigI("AspectRatio") % s_aspect_ratio_nb;
	m_shader      = theApp.GetConfigI("TVShader") % s_post_shader_nb;
	m_vsync       = theApp.GetConfigI("vsync");
	m_aa1         = theApp.GetConfigB("aa1");
	m_fxaa        = theApp.GetConfigB("fxaa");
	m_shaderfx    = theApp.GetConfigB("shaderfx");
	m_shadeboost  = theApp.GetConfigB("ShadeBoost");
	m_sharpen     = theApp.GetConfigI("ps2_sharpen") > 0;
}

GSRenderer::~GSRenderer()
{
	/*if(m_dev)
	{
		m_dev->Reset(1, 1, GSDevice::Windowed);
	}*/

	delete m_dev;
}

bool GSRenderer::CreateDevice(GSDevice* dev)
{
	ASSERT(dev);
	ASSERT(!m_dev);

	if(!dev->Create(m_wnd))
	{
		return false;
	}

	m_dev = dev;
	m_dev->SetVSync(m_vsync);

	return true;
}

void GSRenderer::ResetDevice()
{
    if(m_dev) m_dev->Reset(1, 1);
}

bool GSRenderer::Merge(int field)
{
	bool en[2];

	GSVector4i fr[2];
	GSVector4i dr[2];

	GSVector2i display_baseline = { INT_MAX, INT_MAX };
	GSVector2i frame_baseline = { INT_MAX, INT_MAX };

	for(int i = 0; i < 2; i++)
	{
		en[i] = IsEnabled(i);

		if(en[i])
		{
			fr[i] = GetFrameRect(i);
			dr[i] = GetDisplayRect(i);

			display_baseline.x = std::min(dr[i].left, display_baseline.x);
			display_baseline.y = std::min(dr[i].top, display_baseline.y);
			frame_baseline.x = std::min(fr[i].left, frame_baseline.x);
			frame_baseline.y = std::min(fr[i].top, frame_baseline.y);

			//printf("[%d]: %d %d %d %d, %d %d %d %d\n", i, fr[i].x,fr[i].y,fr[i].z,fr[i].w , dr[i].x,dr[i].y,dr[i].z,dr[i].w);
		}
	}

	if(!en[0] && !en[1])
	{
		return false;
	}

	GL_PUSH("Renderer Merge %d (0: enabled %d 0x%x, 1: enabled %d 0x%x)", s_n, en[0], m_regs->DISP[0].DISPFB.Block(), en[1], m_regs->DISP[1].DISPFB.Block());

	// try to avoid fullscreen blur, could be nice on tv but on a monitor it's like double vision, hurts my eyes (persona 4, guitar hero)
	//
	// NOTE: probably the technique explained in graphtip.pdf (Antialiasing by Supersampling / 4. Reading Odd/Even Scan Lines Separately with the PCRTC then Blending)

	bool samesrc =
		en[0] && en[1] &&
		m_regs->DISP[0].DISPFB.FBP == m_regs->DISP[1].DISPFB.FBP &&
		m_regs->DISP[0].DISPFB.FBW == m_regs->DISP[1].DISPFB.FBW &&
		m_regs->DISP[0].DISPFB.PSM == m_regs->DISP[1].DISPFB.PSM;

	if(samesrc /*&& m_regs->PMODE.SLBG == 0 && m_regs->PMODE.MMOD == 1 && m_regs->PMODE.ALP == 0x80*/)
	{
		// persona 4:
		//
		// fr[0] = 0 0 640 448
		// fr[1] = 0 1 640 448
		// dr[0] = 159 50 779 498
		// dr[1] = 159 50 779 497
		//
		// second image shifted up by 1 pixel and blended over itself
		//
		// god of war:
		//
		// fr[0] = 0 1 512 448
		// fr[1] = 0 0 512 448
		// dr[0] = 127 50 639 497
		// dr[1] = 127 50 639 498
		//
		// same just the first image shifted
		//
		// These kinds of cases are now fixed by the more generic frame_diff code below, as the code here was too specific and has become obsolete.
		// NOTE: Persona 4 and God Of War are not rare exceptions, many games have the same(or very similar) offsets.

		int topDiff = fr[0].top - fr[1].top;
		if (dr[0].eq(dr[1]) && (fr[0].eq(fr[1] + GSVector4i(0, topDiff, 0, topDiff)) || fr[1].eq(fr[0] + GSVector4i(0, topDiff, 0, topDiff))))
		{
			// dq5:
			//
			// fr[0] = 0 1 512 445
			// fr[1] = 0 0 512 444
			// dr[0] = 127 50 639 494
			// dr[1] = 127 50 639 494

			int top = std::min(fr[0].top, fr[1].top);
			int bottom = std::min(fr[0].bottom, fr[1].bottom);

			fr[0].top = fr[1].top = top;
			fr[0].bottom = fr[1].bottom = bottom;
		}
	}

	GSVector2i fs(0, 0);
	GSVector2i ds(0, 0);

	GSTexture* tex[3] = {NULL, NULL, NULL};
	int y_offset[3]   = {0, 0, 0};

	s_n++;

	bool feedback_merge = m_regs->EXTWRITE.WRITE == 1;

	if(samesrc && fr[0].bottom == fr[1].bottom && !feedback_merge)
	{
		tex[0]      = GetOutput(0, y_offset[0]);
		tex[1]      = tex[0]; // saves one texture fetch
		y_offset[1] = y_offset[0];
	}
	else
	{
		if(en[0]) tex[0] = GetOutput(0, y_offset[0]);
		if(en[1]) tex[1] = GetOutput(1, y_offset[1]);
		if(feedback_merge) tex[2] = GetFeedbackOutput();
	}

	GSVector4 src[2];
	GSVector4 src_hw[2];
	GSVector4 dst[2];

	for(int i = 0; i < 2; i++)
	{
		if(!en[i] || !tex[i]) continue;

		GSVector4i r = fr[i];
		GSVector4 scale = GSVector4(tex[i]->GetScale()).xyxy();

		// Upscaled: full-screen sprites drawn with the usual -0.5 offset end at
		// x = width - 0.5, so the rightmost upscaled columns are never written
		// and show stale colours (Transformers: a yellow/green line at the right
		// edge). Leave out the last native column.
		static const bool keep_edge = getenv("PS2_GS_KEEP_EDGE") != nullptr;
		if (!keep_edge && scale.x > 1.0f && r.width() > 64)
			r.z -= 1;

		src[i] = GSVector4(r) * scale / GSVector4(tex[i]->GetSize()).xyxy();
		src_hw[i] = (GSVector4(r) + GSVector4 (0, y_offset[i], 0, y_offset[i])) * scale / GSVector4(tex[i]->GetSize()).xyxy();

		GSVector2 off(0);
		GSVector2i display_diff(dr[i].left - display_baseline.x, dr[i].top - display_baseline.y);
		GSVector2i frame_diff(fr[i].left - frame_baseline.x, fr[i].top - frame_baseline.y);

		// Time Crisis 2/3 uses two side by side images when in split screen mode.
		// Though ignore cases where baseline and display rectangle offsets only differ by 1 pixel, causes blurring and wrong resolution output on FFXII
		if(display_diff.x > 2)
		{
			off.x = tex[i]->GetScale().x * display_diff.x;
		}
		// If the DX offset is too small then consider the status of frame memory offsets, prevents blurring on Tenchu: Fatal Shadows, Worms 3D
		else if(display_diff.x != frame_diff.x)
		{
			off.x = tex[i]->GetScale().x * frame_diff.x;
		}

		if(display_diff.y >= 4) // Shouldn't this be >= 2?
		{
			off.y = tex[i]->GetScale().y * display_diff.y;

			if(m_regs->SMODE2.INT && m_regs->SMODE2.FFMD)
			{
				off.y /= 2;
			}
		}
		else if(display_diff.y != frame_diff.y)
		{
			off.y = tex[i]->GetScale().y * frame_diff.y;
		}

		dst[i] = GSVector4(off).xyxy() + scale * GSVector4(r.rsize());

		fs.x = std::max(fs.x, (int)(dst[i].z + 0.5f));
		fs.y = std::max(fs.y, (int)(dst[i].w + 0.5f));
	}

	ds = fs;

	if(m_regs->SMODE2.INT && m_regs->SMODE2.FFMD)
	{
		ds.y *= 2;
	}
	m_real_size = ds;

	bool slbg = m_regs->PMODE.SLBG;

	if(tex[0] || tex[1])
	{
		if(tex[0] == tex[1] && !slbg && (src[0] == src[1] & dst[0] == dst[1]).alltrue())
		{
			// the two outputs are identical, skip drawing one of them (the one that is alpha blended)

			tex[0] = NULL;
		}

		GSVector4 c = GSVector4((int)m_regs->BGCOLOR.R, (int)m_regs->BGCOLOR.G, (int)m_regs->BGCOLOR.B, (int)m_regs->PMODE.ALP) / 255;

		m_dev->Merge(tex, src_hw, dst, fs, m_regs->PMODE, m_regs->EXTBUF, c);

		if(m_regs->SMODE2.INT && m_interlace > 0)
		{
			if(m_interlace == 7 && m_regs->SMODE2.FFMD) // Auto interlace enabled / Odd frame interlace setting
			{
				int field2 = 0;
				int mode = 2;
				m_dev->Interlace(ds, field ^ field2, mode, tex[1] ? tex[1]->GetScale().y : tex[0]->GetScale().y);
			}
			else
			{
				int field2 = 1 - ((m_interlace - 1) & 1);
				int mode = (m_interlace - 1) >> 1;
				m_dev->Interlace(ds, field ^ field2, mode, tex[1] ? tex[1]->GetScale().y : tex[0]->GetScale().y);
			}
		}

		if(m_shadeboost)
		{
			m_dev->ShadeBoost();
		}

		if(m_sharpen)
		{
			m_dev->Sharpen();
		}

		if(m_shaderfx)
		{
			m_dev->ExternalFX();
		}

		if(m_fxaa)
		{
			m_dev->FXAA();
		}
	}

	return true;
}

GSVector2i GSRenderer::GetInternalResolution()
{
	return m_real_size;
}

void GSRenderer::SetVSync(int vsync)
{
	m_vsync = vsync;

	if(m_dev) m_dev->SetVSync(m_vsync);
}

void GSRenderer::VSync(int field)
{
	GSPerfMonAutoTimer pmat(&m_perfmon);

	GS_TIME(GST_VSYNC);
	m_perfmon.Put(GSPerfMon::Frame);

	Flush();
	static const bool tc_trace = getenv("GSDX_DUMP_TRIGGER") != nullptr; // TEMP-DEBUG (revert)
	if (tc_trace) fprintf(stderr, "[tc] vsync s_n %d\n", s_n);
	// TEMP-DEBUG (revert): file-triggered draw dump.
	if((m_perfmon.GetFrame() & 7) == 0)
	{
		static const char* trig = getenv("GSDX_DUMP_TRIGGER");
		if(trig && access(trig, F_OK) == 0)
		{
			unlink(trig);
			GSmkdir(root_hw.c_str());
			GSmkdir(root_sw.c_str());
			s_dump = 1;
			s_save = getenv("GSDX_DUMP_RT") != nullptr;
			s_savet = getenv("GSDX_DUMP_TEX") != nullptr;
			s_savez = getenv("GSDX_DUMP_Z") != nullptr;
			s_saven = s_n;
			s_savel = getenv("GSDX_DUMP_LEN") ? atoi(getenv("GSDX_DUMP_LEN")) : 3000;
			fprintf(stderr, "[gsdx] dump armed at s_n %d frame %lld\n", s_n, (long long)m_perfmon.GetFrame());
		}
	}

	if(s_dump && s_n >= s_saven)
	{
		m_regs->Dump(root_sw + format("%05d_f%lld_gs_reg.txt", s_n, m_perfmon.GetFrame()));
	}

	if(!m_dev->IsLost(true))
	{
		if(!Merge(field ? 1 : 0))
		{
			return;
		}
	}
	else
	{
		ResetDevice();
	}

	m_dev->AgePool();

	// osd

	if((m_perfmon.GetFrame() & 0x1f) == 0)
	{
		m_perfmon.Update();

		double fps = 1000.0f / m_perfmon.Get(GSPerfMon::Frame);

		// PS2_GS_STATS=1: GS work summed over each 60 vsyncs (GSPerfMon is
		// compiled out, so its frame counter does not pace this block).
		static const bool gs_stats = getenv("PS2_GS_STATS") && *getenv("PS2_GS_STATS") == '1';
		static int gs_stats_vsyncs = 0;
		if(gs_stats && ++gs_stats_vsyncs >= 60)
		{
			gs_stats_vsyncs = 0;
			extern uint64 g_gs_draw_count, g_gs_vertex_count;
			fprintf(stderr, "[gsdx] per 60 vsyncs: %.0f draws, %.0f vertices\n",
				double(g_gs_draw_count), double(g_gs_vertex_count));
			g_gs_draw_count = g_gs_vertex_count = 0;
			extern uint64 g_gs_flush_sites[4096];
			std::string sites;
			for (int i = 0; i < 4096; i++)
				if (g_gs_flush_sites[i]) sites += format(" %d:%llu", i, (unsigned long long)g_gs_flush_sites[i]);
			fprintf(stderr, "[gsdx] draws by GSState.cpp flush line:%s\n", sites.c_str());
			memset(g_gs_flush_sites, 0, sizeof(g_gs_flush_sites));
			const auto v = [](GSOpCounter c) { return double(g_gs_ops[c]); };
			fprintf(stderr, "[gsdx] per 60 vsyncs: %.0f GL draws, %.0f stage changes, %.0f UBO uploads, %.0f FBO binds, "
				"%.0f barriers, %.0f texture uploads (%.0f KB), %.1f textures created, %.1f copies, %.1f stretches\n",
				v(GSOP_GL_DRAW), v(GSOP_STAGES), v(GSOP_UBO), v(GSOP_FBO), v(GSOP_BARRIER), v(GSOP_TEX_UPDATE),
				v(GSOP_TEX_BYTES) / 1024, v(GSOP_TEX_CREATE), v(GSOP_COPY), v(GSOP_STRETCH));
			memset(g_gs_ops, 0, sizeof(g_gs_ops));
			// TSC rate from the wall clock between two reports.
			static uint64 last_tsc = 0;
			static auto last_wall = std::chrono::steady_clock::now();
			const uint64 tsc = __rdtsc();
			const auto wall = std::chrono::steady_clock::now();
			const double secs = std::chrono::duration<double>(wall - last_wall).count();
			if (last_tsc && secs > 0) {
				const double ms_per_tick = secs * 1000.0 / double(tsc - last_tsc);
				const auto t = [&](GSTimeCounter c) { return g_gs_time[c] * ms_per_tick; };
				fprintf(stderr, "[gsdx] ms per 60 vsyncs: transfer %.2f (hw draw %.2f: prims %.2f [gl draw %.2f, ubo %.2f, pipeline %.2f], tex flush %.2f) vsync %.2f\n",
					t(GST_TRANSFER), t(GST_HW_DRAW), t(GST_PRIMS), t(GST_GL_DRAW), t(GST_UBO), t(GST_PIPELINE), t(GST_TEX_FLUSH), t(GST_VSYNC));
			}
			last_tsc = tsc;
			last_wall = wall;
			memset(g_gs_time, 0, sizeof(g_gs_time));
		}

		std::string s;

#ifdef GSTITLEINFO_API_FORCE_VERBOSE
		if(1)//force verbose reply
#else
		if(m_wnd->IsManaged())
#endif
		{
			//GSdx owns the window's title, be verbose.

			std::string s2 = m_regs->SMODE2.INT ? (std::string("Interlaced ") + (m_regs->SMODE2.FFMD ? "(frame)" : "(field)")) : "Progressive";

			s = format(
				"%lld | %d x %d | %.2f fps (%d%%) | %s - %s | %s | %d S/%d P/%d D | %d%% CPU | %.2f | %.2f",
				m_perfmon.GetFrame(), GetInternalResolution().x, GetInternalResolution().y, fps, (int)(100.0 * fps / GetTvRefreshRate()),
				s2.c_str(),
				theApp.m_gs_interlace[m_interlace].name.c_str(),
				theApp.m_gs_aspectratio[m_aspectratio].name.c_str(),
				(int)m_perfmon.Get(GSPerfMon::SyncPoint),
				(int)m_perfmon.Get(GSPerfMon::Prim),
				(int)m_perfmon.Get(GSPerfMon::Draw),
				m_perfmon.CPU(),
				m_perfmon.Get(GSPerfMon::Swizzle) / 1024,
				m_perfmon.Get(GSPerfMon::Unswizzle) / 1024
			);

			double fillrate = m_perfmon.Get(GSPerfMon::Fillrate);

			if(fillrate > 0)
			{
				s += format(" | %.2f mpps", fps * fillrate / (1024 * 1024));

				int sum = 0;

				for(int i = 0; i < 16; i++)
				{
					sum += m_perfmon.CPU(GSPerfMon::WorkerDraw0 + i);
				}

				s += format(" | %d%% CPU", sum);
			}
		}
		else
		{
			// Satisfy PCSX2's request for title info: minimal verbosity due to more external title text

			s = format("%dx%d | %s", GetInternalResolution().x, GetInternalResolution().y, theApp.m_gs_interlace[m_interlace].name.c_str());
		}

		if(m_capture.IsCapturing())
		{
			s += " | Recording...";
		}

		if(m_wnd->IsManaged())
		{
			m_wnd->SetWindowText(s.c_str());
		}
		else
		{
			// note: do not use TryEnterCriticalSection.  It is unnecessary code complication in
			// an area that absolutely does not matter (even if it were 100 times slower, it wouldn't
			// be noticeable).  Besides, these locks are extremely short -- overhead of conditional
			// is way more expensive than just waiting for the CriticalSection in 1 of 10,000,000 tries. --air

			std::lock_guard<std::mutex> lock(m_pGSsetTitle_Crit);

			strncpy(m_GStitleInfoBuffer, s.c_str(), countof(m_GStitleInfoBuffer) - 1);

			m_GStitleInfoBuffer[sizeof(m_GStitleInfoBuffer) - 1] = 0; // make sure null terminated even if text overflows
		}
	}
	else
	{
		// [TODO]
		// We don't have window title rights, or the window has no title,
		// so let's use actual OSD!
	}

	// Port: a vblank without a guest flip still merges and deinterlaces (the
	// blend/weave buffers keep the other field), but does not swap.
	extern bool g_gs_skip_swap;
	if(m_frameskip || g_gs_skip_swap)
	{
		return;
	}

	// present

	// This will scale the OSD to the window's size.
	// Will maintiain the font size no matter what size the window is.
	GSVector4i window_size = m_wnd->GetClientRect();
	m_dev->m_osd.m_real_size.x = window_size.v[2];
	m_dev->m_osd.m_real_size.y = window_size.v[3];

	m_dev->Present(m_wnd->GetClientRect().fit(m_aspectratio), m_shader);

	// PS2_GS_DUMP=<file>:<first vsync>:<vsyncs> records a raw GSdx dump of
	// that many vsyncs (runtime/tests/gs_replay.cpp replays it).
	if(const long want = g_gsdx_dump_vsyncs.exchange(0); want > 0 && !m_dump)
	{
		std::string path;
		{
			std::lock_guard<std::mutex> lock(g_gsdx_dump_mutex);
			path = g_gsdx_dump_path;
		}
		GSFreezeData fd = {0, nullptr};
		Freeze(&fd, true);
		fd.data = new uint8[fd.size];
		Freeze(&fd, false);
		m_dump = std::unique_ptr<GSDumpBase>(new GSDump(path, m_crc, fd, m_regs));
		delete [] fd.data;
		m_requested_dump_left = want;
		fprintf(stderr, "[gsdx] recording %ld vsyncs to %s\n", want, path.c_str());
	}
	else if(m_requested_dump_left > 0 && m_dump)
	{
		m_dump->VSync(field, true, m_regs);
		if(--m_requested_dump_left == 0)
		{
			m_dump.reset();
			fprintf(stderr, "[gsdx] dump done\n");
		}
	}
	static const char* dump_env = getenv("PS2_GS_DUMP");
	if(dump_env)
	{
		static std::string dump_path;
		static long dump_first = -1, dump_vsyncs = 0, vsync_n = 0;
		if(dump_first < 0)
		{
			std::string e(dump_env);
			const size_t a = e.find(':'), b = e.find(':', a + 1);
			dump_path = e.substr(0, a);
			dump_first = atol(e.c_str() + a + 1);
			dump_vsyncs = b == std::string::npos ? 4 : atol(e.c_str() + b + 1);
		}
		if(vsync_n == dump_first)
		{
			GSFreezeData fd = {0, nullptr};
			Freeze(&fd, true);
			fd.data = new uint8[fd.size];
			Freeze(&fd, false);
			m_dump = std::unique_ptr<GSDumpBase>(new GSDump(dump_path, m_crc, fd, m_regs));
			delete [] fd.data;
			fprintf(stderr, "[gsdx] recording %ld vsyncs to %s\n", dump_vsyncs, dump_path.c_str());
		}
		else if(m_dump)
		{
			m_dump->VSync(field, true, m_regs);
			if(vsync_n >= dump_first + dump_vsyncs)
			{
				m_dump.reset();
				fprintf(stderr, "[gsdx] dump done\n");
			}
		}
		++vsync_n;
	}

	// snapshot

	if(!m_snapshot.empty())
	{
		if(!m_dump && m_shift_key)
		{
			GSFreezeData fd = {0, nullptr};
			Freeze(&fd, true);
			fd.data = new uint8[fd.size];
			Freeze(&fd, false);

			if (m_control_key)
				m_dump = std::unique_ptr<GSDumpBase>(new GSDump(m_snapshot, m_crc, fd, m_regs));
			else
				m_dump = std::unique_ptr<GSDumpBase>(new GSDumpXz(m_snapshot, m_crc, fd, m_regs));

			delete [] fd.data;
		}

		if(GSTexture* t = m_dev->GetCurrent())
		{
			t->Save(m_snapshot + ".bmp");
		}

		m_snapshot.clear();
	}
	else if(m_dump && !dump_env)
	{
		if(m_dump->VSync(field, !m_control_key, m_regs))
			m_dump.reset();
	}

	// capture

	if(m_capture.IsCapturing())
	{
		if(GSTexture* current = m_dev->GetCurrent())
		{
			GSVector2i size = m_capture.GetSize();

			if(GSTexture* offscreen = m_dev->CopyOffscreen(current, GSVector4(0, 0, 1, 1), size.x, size.y))
			{
				GSTexture::GSMap m;

				if(offscreen->Map(m))
				{
					m_capture.DeliverFrame(m.bits, m.pitch, !m_dev->IsRBSwapped());

					offscreen->Unmap();
				}

				m_dev->Recycle(offscreen);
			}
		}
	}
}

bool GSRenderer::MakeSnapshot(const std::string& path)
{
	if(m_snapshot.empty())
	{
		time_t cur_time = time(nullptr);
		static time_t prev_snap;
		// The variable 'n' is used for labelling the screenshots when multiple screenshots are taken in
		// a single second, we'll start using this variable for naming when a second screenshot request is detected
		// at the same time as the first one. Hence, we're initially setting this counter to 2 to imply that
		// the captured image is the 2nd image captured at this specific time.
		static int n = 2;
		char local_time[16];

		if (strftime(local_time, sizeof(local_time), "%Y%m%d%H%M%S", localtime(&cur_time)))
		{
			if (cur_time == prev_snap)
			{
				m_snapshot = format("%s_%s_(%d)", path.c_str(), local_time, n++);
			}
			else
			{
				n = 2;
				m_snapshot = format("%s_%s", path.c_str(), local_time);
			}
			prev_snap = cur_time;
		}
	}

	return true;
}

bool GSRenderer::BeginCapture()
{
	GSVector4i disp = m_wnd->GetClientRect().fit(m_aspectratio);
	float aspect = (float)disp.width() / std::max(1, disp.height());

	return m_capture.BeginCapture(GetTvRefreshRate(), GetInternalResolution(), aspect);
}

void GSRenderer::EndCapture()
{
	m_capture.EndCapture();
}

void GSRenderer::KeyEvent(GSKeyEventData* e)
{
#ifdef _WIN32
	m_shift_key = !!(::GetAsyncKeyState(VK_SHIFT) & 0x8000);
	m_control_key = !!(::GetAsyncKeyState(VK_CONTROL) & 0x8000);
#else
	switch(e->key)
	{
		case XK_Shift_L:
		case XK_Shift_R:
			m_shift_key = (e->type == KEYPRESS);
			return;
		case XK_Control_L:
		case XK_Control_R:
			m_control_key = (e->type == KEYPRESS);
			return;
	}
#endif

	if(e->type == KEYPRESS)
	{

		int step = m_shift_key ? -1 : 1;

#if defined(__unix__)
#define VK_F5 XK_F5
#define VK_F6 XK_F6
#define VK_F7 XK_F7
#define VK_DELETE XK_Delete
#define VK_INSERT XK_Insert
#define VK_PRIOR XK_Prior
#define VK_HOME XK_Home
#endif

		switch(e->key)
		{
		case VK_F5:
			m_interlace = (m_interlace + s_interlace_nb + step) % s_interlace_nb;
			theApp.SetConfig("interlace", m_interlace);
			printf("GSdx: Set deinterlace mode to %d (%s).\n", m_interlace, theApp.m_gs_interlace.at(m_interlace).name.c_str());
			return;
		case VK_F6:
			if( m_wnd->IsManaged() )
				m_aspectratio = (m_aspectratio + s_aspect_ratio_nb + step) % s_aspect_ratio_nb;
			return;
		case VK_F7:
			m_shader = (m_shader + s_post_shader_nb + step) % s_post_shader_nb;
			theApp.SetConfig("TVShader", m_shader);
			printf("GSdx: Set shader to: %d.\n", m_shader);
			return;
		case VK_DELETE:
			m_aa1 = !m_aa1;
			theApp.SetConfig("aa1", m_aa1);
			printf("GSdx: (Software) Edge anti-aliasing is now %s.\n", m_aa1 ? "enabled" : "disabled");
			return;
		case VK_INSERT:
			m_mipmap = (m_mipmap + s_mipmap_nb + step) % s_mipmap_nb;
			theApp.SetConfig("mipmap_hw", m_mipmap);
			printf("GSdx: Mipmapping is now %s.\n", theApp.m_gs_hack.at(m_mipmap).name.c_str());
			return;
		case VK_PRIOR:
			m_fxaa = !m_fxaa;
			theApp.SetConfig("fxaa", m_fxaa);
			printf("GSdx: FXAA anti-aliasing is now %s.\n", m_fxaa ? "enabled" : "disabled");
			return;
		case VK_HOME:
			m_shaderfx = !m_shaderfx;
			theApp.SetConfig("shaderfx", m_shaderfx);
			printf("GSdx: External post-processing is now %s.\n", m_shaderfx ? "enabled" : "disabled");
			return;
		}

	}
}

void GSRenderer::PurgePool()
{
	m_dev->PurgePool();
}
