#include "hw/ipu.h"
#include "hw/hw.h"
#include "ps2/ee_mem.h"
#include "ps2/intc.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <string>
#include <vector>
#ifdef IPU_FFMPEG
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
}
#ifndef IPU_SAMPLE_MOVIE
#define IPU_SAMPLE_MOVIE "iso/extract/FMV/PMH_LOGO.PSS"
#endif
#ifndef IPU_OUTPUT_DIR
#define IPU_OUTPUT_DIR "build-ipu"
#endif
#endif

namespace {
hw::DmaChan channels[10];
unsigned ipu_interrupts = 0;
unsigned dma_interrupts[10]{};
void require(bool condition, const char* message) {
    if (!condition) { std::fprintf(stderr, "ipu_tests: %s\n", message); std::exit(1); }
}
void reset() {
    channels[3] = channels[4] = {};
    ipu::init();
    ipu_interrupts = 0;
    dma_interrupts[3] = dma_interrupts[4] = 0;
}
void normal_input(u32 address, unsigned qwc) {
    auto& ch = channels[4];
    ch = {};
    ch.chcr = 0x101;
    ch.madr = address;
    ch.qwc = qwc;
    ipu::dma_start(4);
}
void normal_output(u32 address, unsigned qwc) {
    auto& ch = channels[3];
    ch = {};
    ch.chcr = 0x100;
    ch.madr = address;
    ch.qwc = qwc;
    ipu::dma_start(3);
}
void test_fifo_and_chain() {
    reset();
    constexpr u32 source = 0x20000, tags = 0x21000;
    for (int i = 0; i < 48; ++i) ee_host(source)[i] = u8(i * 31 + 17);
    // REF(2) followed by END(1), both fetched from guest RAM as ordinary DMAC tags.
    ee_write32_ram(tags, (3u << 28) | 2); ee_write32_ram(tags + 4, source);
    ee_write32_ram(tags + 16, (7u << 28) | 1);
    std::memcpy(ee_host(tags + 32), ee_host(source + 32), 16);
    ipu::write_reg(0, 0);  // BCLR
    auto& ch = channels[4];
    ch = {}; ch.chcr = 0x105; ch.tadr = tags;
    ipu::dma_start(4);
    require(dma_interrupts[4] == 1 && ch.qwc == 0, "REF/END chain completion");
    require(ch.madr == tags + 48 && ch.tadr == tags + 48, "chain register advancement");
    require((ipu::read_reg(0x20) >> 8 & 15) == 3, "input FIFO IFC 3");
    ipu::write_reg(0, (4u << 28) | 5);
    require(!(ipu::read_reg(0x10) & 0x80000000u), "FDEC completes with enough input");
    u32 value = 0;
    for (unsigned j = 0; j < 32; ++j) {
        const unsigned bit = j + 5;
        value = (value << 1) | ((ee_host(source)[bit / 8] >> (7 - bit % 8)) & 1);
    }
    require(ipu::read_reg(0) == value && ipu::read_reg(0x30) == value, "FDEC MSB-first unaligned peek");
    const u32 bp = ipu::read_reg(0x20);
    require((bp & 127) == 5 && ((bp >> 16) & 3) == 2 && ((bp >> 8) & 15) == 1,
            "FDEC BP/FP/IFC accounting");
    require(ipu_interrupts == 2, "one IRQ each for BCLR and FDEC");
}
void test_fdec_crosses_qword_dma_boundary() {
    reset();
    constexpr u32 source = 0x2c000;
    std::memset(ee_host(source), 0, 32);
    ee_host(source)[14] = 0x12;
    ee_host(source)[15] = 0x34;
    ee_host(source)[16] = 0x56;
    ee_host(source)[17] = 0x78;
    // BCLR accepts a seven-bit BP; FDEC then needs a second input qword.
    ipu::write_reg(0, 112);
    normal_input(source, 1);
    ipu::write_reg(0, 4u << 28);
    require(ipu::read_reg(0x10) & 0x80000000u, "FDEC waits for next qword at BP112");
    normal_input(source + 16, 1);
    require(!(ipu::read_reg(0x10) & 0x80000000u), "toIPU resumes stalled FDEC");
    require(ipu::read_reg(0) == 0x12345678u, "cross-qword FDEC preserves MPEG byte order");
    require((ipu::read_reg(0x20) & 127) == 112, "FDEC peek preserves bit pointer");
}
void test_dma_call_ret_and_irq() {
    reset();
    constexpr u32 root = 0x30000, branch = 0x30100, payload = 0x30200;
    ee_write32_ram(root, (5u << 28) | 1); ee_write32_ram(root + 4, branch);
    ee_write32_ram(root + 32, 0x80000000u | (3u << 28) | 1);
    ee_write32_ram(root + 36, payload);
    ee_write32_ram(branch, (6u << 28) | 1);
    std::memset(ee_host(root + 16), 0x11, 16);
    std::memset(ee_host(branch + 16), 0x22, 16);
    std::memset(ee_host(payload), 0x33, 16);
    auto& ch = channels[4];
    ch = {}; ch.chcr = 0x185; ch.tadr = root;  // STR, chain, direction, TIE.
    ipu::dma_start(4);
    require(dma_interrupts[4] == 1 && ch.qwc == 0, "IRQ/TIE ends source chain after REF");
    require(ch.asr0 == root + 32 && ((ch.chcr >> 4) & 3) == 0,
            "CALL/RET restores ASR stack depth and return PC");
    require(ch.tadr == root + 48 && ch.madr == payload + 16,
            "IRQ REF advances tag and payload registers");
    require((ipu::read_reg(0x20) >> 8 & 15) == 3, "CALL/RET delivered three payload qwords");
}
void test_command_stall_and_setiq() {
    reset();
    ipu::write_reg(0, 0);
    ipu::write_reg(0, 5u << 28);
    require((ipu::read_reg(0x10) & 0x80000000u) != 0, "SETIQ waits for input");
    constexpr u32 address = 0x22000;
    for (unsigned i = 0; i < 64; ++i) ee_host(address)[i] = u8(i + 1);
    normal_input(address, 4);
    require(channels[4].qwc == 0 && dma_interrupts[4] == 1, "SETIQ releases input DMA");
    require((ipu::read_reg(0x10) & 0x80000000u) == 0, "SETIQ completes");
    require(ipu_interrupts == 2, "SETIQ completion interrupt");
}
void test_bdec_macroblock() {
    reset();
    // One MPEG-2 I macroblock with DC differential 0 and EOB for all six blocks.
    // Luma DC-size=0 VLC 100, chroma DC-size=0 VLC 00, EOB VLC 10.
    constexpr u32 source = 0x23000, output = 0x24000;
    std::memset(ee_host(source), 0, 256);
    unsigned bitpos = 0;
    auto bits = [&](unsigned value, unsigned length) {
        for (unsigned i = length; i--;) {
            const unsigned bit = bitpos++;
            ee_host(source)[bit >> 3] |= u8(((value >> i) & 1u) << (7 - (bit & 7)));
        }
    };
    for (unsigned i = 0; i < 4; ++i) { bits(4, 3); bits(2, 2); }
    for (unsigned i = 0; i < 2; ++i) { bits(0, 2); bits(2, 2); }
    std::memset(ee_host(output), 0x55, 768);
    ipu::write_reg(0, 0);
    // BDEC: quantizer scale=8, DCR=1, macroblock intra=1.
    ipu::write_reg(0, (2u << 28) | (1u << 27) | (1u << 26) | (8u << 16));
    normal_input(source, 4);
    require(ipu::read_reg(0x10) & 0x80000000u, "BDEC stalls on full output FIFO");
    normal_output(output, 48);
    require(!(ipu::read_reg(0x10) & 0x80000000u), "BDEC resumes when fromIPU starts");
    require(dma_interrupts[3] == 1 && channels[3].qwc == 0, "BDEC outputs 48 qwords");
    const u16* pixel = reinterpret_cast<const u16*>(ee_host(output));
    for (unsigned i = 0; i < 384; ++i)
        require(pixel[i] == 128, "zero-coefficient I macroblock reconstructs neutral YCbCr");
}
void test_predicted_picture_residual() {
    reset();
    constexpr u32 source = 0x29000, output = 0x2a000, matrix_addr = 0x2b000;
    std::memset(ee_host(matrix_addr), 16, 64);  // MPEG default non-intra matrix.
    ipu::write_reg(0, 0);
    ipu::write_reg(0, (5u << 28) | (1u << 27));
    normal_input(matrix_addr, 4);
    require(!(ipu::read_reg(0x10) & 0x80000000u), "non-intra SETIQ completes");
    ipu::write_reg(0, 0);
    std::memset(ee_host(source), 0, 256);
    unsigned bitpos = 0;
    auto bits = [&](unsigned value, unsigned length) {
        for (unsigned i = length; i--; ++bitpos)
            ee_host(source)[bitpos >> 3] |= u8(((value >> i) & 1) << (7 - (bitpos & 7)));
    };
    bits(0b001100, 6);  // coded_block_pattern=63 (all six blocks).
    for (unsigned i = 0; i < 6; ++i) {
        bits(0b10, 2);  // first coefficient: run=0, level=1, sign=0
        bits(0b10, 2);  // end of block
    }
    ipu::write_reg(0, (2u << 28) | (8u << 16));  // BDEC P/B non-intra residual
    normal_output(output, 48);
    normal_input(source, 4);
    require(!(ipu::read_reg(0x10) & 0x80000000u), "P/B residual BDEC completes");
    require(channels[3].qwc == 0 && ((ipu::read_reg(0x10) >> 8) & 63) == 63,
            "P/B residual writes 48 qwords and coded block pattern");
    const u16* pixel = reinterpret_cast<const u16*>(ee_host(output));
    require(pixel[0] == 3 && pixel[256] == 3 && pixel[320] == 3,
            "non-intra dequantization and IDCT produce +3 residual in YCbCr");
}
void test_csc_pack_and_threshold() {
    reset();
    constexpr u32 source = 0x26000, output = 0x27000, palette_addr = 0x28000;
    std::memset(ee_host(source), 235, 256);
    ee_host(source)[0] = 16;
    std::memset(ee_host(source + 256), 128, 128);
    ipu::write_reg(0, (9u << 28) | 20u | (80u << 16));
    ipu::write_reg(0, (7u << 28) | 1u);
    normal_output(output, 64);
    normal_input(source, 24);
    require(!(ipu::read_reg(0x10) & 0x80000000u) && channels[3].qwc == 0,
            "CSC transfers RGB32 macroblock under FIFO backpressure");
    const u8* rgb = ee_host(output);
    require(rgb[0] == 0 && rgb[1] == 0 && rgb[2] == 0 && rgb[3] == 0,
            "CSC thresholds dark pixel to transparent");
    require(rgb[4] >= 250 && rgb[5] >= 250 && rgb[6] >= 250 && rgb[7] == 0x80,
            "CSC converts YCbCr white to RGB32");

    // Enable ordered dithering and an alpha threshold in RGB16 mode.
    ipu::write_reg(0, 0);
    std::memset(ee_host(source), 22, 256);
    std::memset(ee_host(source + 256), 128, 128);
    ipu::write_reg(0, (9u << 28) | (50u << 16));
    ipu::write_reg(0, (7u << 28) | (1u << 27) | (1u << 26) | 1u);
    normal_output(output, 32);
    normal_input(source, 24);
    require(!(ipu::read_reg(0x10) & 0x80000000u) && channels[3].qwc == 0,
            "CSC transfers RGB16 with dithering");
    const auto* rgb16 = reinterpret_cast<const u16*>(ee_host(output));
    require((rgb16[0] & 0x8000u) && (rgb16[3] & 0x8000u) && rgb16[0] != rgb16[3],
            "RGB16 alpha threshold and ordered dither vary pixel quantization");

    std::memset(ee_host(palette_addr), 0, 32);
    ee_host(palette_addr)[2] = 0xff;
    ee_host(palette_addr)[3] = 0x7f;
    ipu::write_reg(0, 6u << 28);
    normal_input(palette_addr, 2);
    require(!(ipu::read_reg(0x10) & 0x80000000u), "SETVQ accepts 16 RGB555 palette entries");
    std::memset(ee_host(source), 255, 1024);
    ipu::write_reg(0, (8u << 28) | 1u);
    normal_output(output, 8);
    normal_input(source, 64);
    require(!(ipu::read_reg(0x10) & 0x80000000u) && channels[3].qwc == 0,
            "PACK transfers 128-byte indexed macroblock");
    for (unsigned i = 0; i < 128; ++i)
        require(ee_host(output)[i] == 0x11, "PACK maps white RGB pixels to palette index 1");
}
#ifdef IPU_FFMPEG
void save_png(const char* filename, const std::vector<u8>& rgba, int width, int height) {
    const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_PNG);
    require(codec != nullptr, "FFmpeg PNG encoder available");
    AVCodecContext* ctx = avcodec_alloc_context3(codec);
    ctx->width = width; ctx->height = height; ctx->pix_fmt = AV_PIX_FMT_RGBA; ctx->time_base = {1, 25};
    require(avcodec_open2(ctx, codec, nullptr) == 0, "open PNG encoder");
    AVFrame* frame = av_frame_alloc();
    frame->width = width; frame->height = height; frame->format = AV_PIX_FMT_RGBA;
    frame->data[0] = const_cast<u8*>(rgba.data()); frame->linesize[0] = width * 4;
    require(avcodec_send_frame(ctx, frame) == 0, "submit PNG image");
    AVPacket* pkt = av_packet_alloc();
    require(avcodec_receive_packet(ctx, pkt) == 0, "encode PNG image");
    FILE* file = std::fopen(filename, "wb");
    require(file != nullptr, "open PNG output");
    require(std::fwrite(pkt->data, 1, pkt->size, file) == size_t(pkt->size), "write PNG output");
    std::fclose(file);
    // Reopen the on-disk PNG and decode those bytes (not the in-memory encoder
    // packet) to prove screenshots and PSNR use the same pixels.
    file = std::fopen(filename, "rb");
    require(file != nullptr, "reopen saved PNG");
    std::vector<u8> encoded(size_t(pkt->size) + AV_INPUT_BUFFER_PADDING_SIZE);
    require(std::fread(encoded.data(), 1, pkt->size, file) == size_t(pkt->size),
            "read saved PNG bytes");
    require(std::fgetc(file) == EOF, "saved PNG contains exactly the encoded packet");
    std::fclose(file);
    AVPacket saved{};
    saved.data = encoded.data();
    saved.size = pkt->size;
    const AVCodec* png_decoder = avcodec_find_decoder(AV_CODEC_ID_PNG);
    AVCodecContext* verify = avcodec_alloc_context3(png_decoder);
    require(avcodec_open2(verify, png_decoder, nullptr) == 0, "open PNG verification decoder");
    require(avcodec_send_packet(verify, &saved) == 0, "decode saved PNG");
    AVFrame* recovered = av_frame_alloc();
    require(avcodec_receive_frame(verify, recovered) == 0 &&
            recovered->width == width && recovered->height == height &&
            recovered->format == AV_PIX_FMT_RGBA, "roundtrip PNG dimensions and RGBA format");
    for (int row = 0; row < height; ++row)
        require(std::memcmp(recovered->data[0] + row * recovered->linesize[0],
                            rgba.data() + size_t(row) * width * 4, width * 4) == 0,
                "saved PNG pixels match the compared RGBA buffer");
    av_frame_free(&recovered); avcodec_free_context(&verify);
    av_packet_free(&pkt); av_frame_free(&frame); avcodec_free_context(&ctx);
}
struct StartCode { size_t offset; unsigned code; };
std::vector<StartCode> start_codes(const std::vector<u8>& video) {
    std::vector<StartCode> codes;
    for (size_t i = 0; i + 4 <= video.size(); ++i)
        if (video[i] == 0 && video[i + 1] == 0 && video[i + 2] == 1) {
            codes.push_back({i, video[i + 3]});
            i += 3;
        }
    return codes;
}
unsigned get_bits(const u8* bytes, unsigned& bit, unsigned length) {
    unsigned result = 0;
    for (unsigned j = 0; j < length; ++j, ++bit)
        result = (result << 1) | ((bytes[bit / 8] >> (7 - bit % 8)) & 1u);
    return result;
}
void test_i_picture(const char* movie_name, unsigned target_i, const char* label) {
    const auto movie = std::filesystem::path(IPU_SAMPLE_MOVIE).parent_path() / movie_name;
    if (!std::filesystem::exists(movie)) {
        const auto extract = movie.parent_path().parent_path();
        const auto iso = extract.parent_path() / "transformers.iso";
        const std::string command = "7z x -y -o\"" + extract.string() + "\" \"" +
                                    iso.string() + "\" FMV/" + movie_name + " > /dev/null";
        const int rc = std::system(command.c_str());
        require(rc == 0 && std::filesystem::exists(movie), "extract PSS sample from game ISO");
    }
    AVFormatContext* format = nullptr;
    require(avformat_open_input(&format, movie.c_str(), nullptr, nullptr) == 0, "open game PSS via libavformat");
    require(avformat_find_stream_info(format, nullptr) >= 0, "find PSS video stream");
    const int video_stream = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    require(video_stream >= 0, "PSS contains MPEG video");
    std::vector<u8> video;
    AVPacket* pkt = av_packet_alloc();
    size_t selected_picture = 0, picture_end = 0, scanned = 0;
    unsigned i_count = 0;
    while (!picture_end && av_read_frame(format, pkt) >= 0) {
        if (pkt->stream_index == video_stream) {
            video.insert(video.end(), pkt->data, pkt->data + pkt->size);
            for (size_t i = scanned; i + 6 <= video.size(); ++i) {
                if (video[i] || video[i + 1] || video[i + 2] != 1 || video[i + 3] != 0) continue;
                if (selected_picture) { picture_end = i; break; }
                if (((video[i + 5] >> 3) & 7) == 1 && ++i_count == target_i)
                    selected_picture = i;
            }
            scanned = video.size() > 5 ? video.size() - 5 : 0;
        }
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt); avformat_close_input(&format);
    require(picture_end > selected_picture && selected_picture != 0,
            "parse chosen MPEG I-picture after three seconds");
    video.resize(picture_end);
    const auto all_codes = start_codes(video);
    size_t sequence = 0, first_picture_after_sequence = 0;
    for (size_t i = 0; i < all_codes.size(); ++i) {
        if (all_codes[i].code == 0xb3 && all_codes[i].offset < selected_picture) {
            sequence = all_codes[i].offset;
            first_picture_after_sequence = 0;
        }
        if (!first_picture_after_sequence && all_codes[i].code == 0 &&
            all_codes[i].offset >= sequence)
            first_picture_after_sequence = all_codes[i].offset;
    }
    require(first_picture_after_sequence > sequence, "I-picture has preceding MPEG sequence header");
    std::vector<u8> isolated;
    isolated.insert(isolated.end(), video.begin() + sequence, video.begin() + first_picture_after_sequence);
    isolated.insert(isolated.end(), video.begin() + selected_picture, video.end());
    video = std::move(isolated);
    const auto codes = start_codes(video);
    require(codes.size() > 25 && codes.front().code == 0xb3,
            "sequence header and I-picture slices in isolated stream");
    unsigned width = 0, height = 0, ctrl = 0, frame_pred = 1;
    for (auto c : codes) {
        if (c.code == 0xb3) {
            const u8* header = video.data() + c.offset + 4;
            width = unsigned(header[0]) * 16 + (header[1] >> 4);
            height = ((header[1] & 15u) << 8) | header[2];
        }
        if (c.code == 0xb5 && (video[c.offset + 4] >> 4) == 8) {
            unsigned bit = 4 + 16;
            const u8* payload = video.data() + c.offset + 4;
            const unsigned idp = get_bits(payload, bit, 2);
            const unsigned structure = get_bits(payload, bit, 2);
            (void)structure;
            get_bits(payload, bit, 1);  // top_field_first
            frame_pred = get_bits(payload, bit, 1);
            get_bits(payload, bit, 1);  // concealment_motion_vectors
            const unsigned qst = get_bits(payload, bit, 1);
            const unsigned ivf = get_bits(payload, bit, 1);
            const unsigned alt = get_bits(payload, bit, 1);
            ctrl = (idp << 16) | (alt << 20) | (ivf << 21) | (qst << 22);
        }
    }
    require(width == 640 && height == 384, "PAL MPEG picture dimensions");
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_MPEG2VIDEO);
    AVCodecContext* decoder = avcodec_alloc_context3(codec);
    require(avcodec_open2(decoder, codec, nullptr) == 0, "open reference MPEG-2 decoder");
    AVPacket* frame_packet = av_packet_alloc();
    require(av_new_packet(frame_packet, int(video.size())) == 0, "allocate reference picture packet");
    std::memcpy(frame_packet->data, video.data(), video.size());
    require(avcodec_send_packet(decoder, frame_packet) == 0, "submit reference I-picture");
    av_packet_free(&frame_packet);
    avcodec_send_packet(decoder, nullptr);
    AVFrame* frame = av_frame_alloc();
    require(avcodec_receive_frame(decoder, frame) == 0, "decode reference I-picture");
    require(frame->width == int(width) && frame->height == int(height), "reference picture dimensions");
    std::vector<u8> reference(width * height * 4), actual(reference.size());
    SwsContext* sws = sws_getContext(width, height, AVPixelFormat(frame->format),
                                     width, height, AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr);
    require(sws != nullptr, "allocate reference YUV-to-RGB conversion");
    // PCSX2 IPU CSC implements MPEG studio-range BT.601 YCbCr to full-range RGB.
    // Make the reference matrix/range explicit instead of relying on swscale defaults.
    const int* bt601 = sws_getCoefficients(SWS_CS_ITU601);
    require(sws_setColorspaceDetails(sws, bt601, 0, bt601, 1, 0, 1 << 16, 1 << 16) == 0,
            "set MPEG limited-range BT.601 to full-range RGB reference");
    u8* planes[] = {reference.data(), nullptr, nullptr, nullptr};
    int strides[] = {int(width * 4), 0, 0, 0};
    sws_scale(sws, frame->data, frame->linesize, 0, int(height), planes, strides);
    sws_freeContext(sws); av_frame_free(&frame); avcodec_free_context(&decoder);

    reset();
    ipu::write_reg(0x10, ctrl);
    constexpr u32 source = 0x40000, output = 0x100000, tag = 0x180000, matrix = 0x190000;
    for (auto c : codes) if (c.code == 0xb3) {
        const u8* header = video.data() + c.offset + 4;
        if (header[7] & 1u) {
            std::memcpy(ee_host(matrix), header + 8, 64);
            ipu::write_reg(0, 5u << 28);
            normal_input(matrix, 4);
            require(!(ipu::read_reg(0x10) & 0x80000000u), "load sequence intra quantizer matrix");
        }
    }
    unsigned rows = 0;
    for (size_t ci = 0; ci < codes.size(); ++ci) {
        if (codes[ci].code < 1 || codes[ci].code > height / 16) continue;
        const unsigned row = codes[ci].code - 1;
        const size_t begin = codes[ci].offset + 4;
        const size_t end = (ci + 1 < codes.size()) ? codes[ci + 1].offset : video.size();
        require(end > begin && end - begin < 0xffffu * 16u, "slice bitstream boundaries");
        const unsigned qwc = unsigned((end - begin + 31) / 16);
        std::memset(ee_host(source), 0, qwc * 16);
        std::memcpy(ee_host(source), video.data() + begin, end - begin);
        // Append a start code to let IDEC detect end-of-slice and fill IPU_TOP.
        ee_host(source)[end - begin + 0] = 0;
        ee_host(source)[end - begin + 1] = 0;
        ee_host(source)[end - begin + 2] = 1;
        ee_host(source)[end - begin + 3] = 0xb7;
        ee_write32_ram(tag, (3u << 28) | qwc);
        ee_write32_ram(tag + 4, source);
        ipu::write_reg(0, 0);  // BCLR before arming chain.
        auto& to = channels[4];
        to = {}; to.chcr = 0x105; to.tadr = tag;
        ipu::dma_start(4);
        // Sony libmpeg consumes the slice's quantizer/extra bit, then VDEC's
        // macroblock_address_increment before giving macroblock_type to IDEC.
        ipu::write_reg(0, (3u << 28) | 6);
        require(!(ipu::read_reg(0x10) & 0x80000000u), "VDEC decodes slice macroblock increment");
        const u32 increment = ipu::read_reg(0);
        require((increment & 0xffffu) == 1, "first macroblock is at slice start");
        const unsigned row_qwc = (width / 16) * 64;
        std::memset(ee_host(output), 0x55, row_qwc * 16);
        normal_output(output, row_qwc);
        const unsigned scale = ee_host(source)[0] >> 3;
        ipu::write_reg(0, (1u << 28) | (scale << 16) | ((!frame_pred) << 24));
        require(!(ipu::read_reg(0x10) & 0x80000000u), "IDEC finishes picture slice");
        if (channels[3].qwc != 0)
            std::fprintf(stderr, "%s I #%u slice %u: emitted %u/%u macroblocks, ctrl=%08x bp=%08x top=%08x scale=%u\n",
                         label, target_i, row, (row_qwc - channels[3].qwc) / 64, row_qwc / 64,
                         ipu::read_reg(0x10), ipu::read_reg(0x20), ipu::read_reg(0x30), scale);
        require(channels[3].qwc == 0, "IDEC emits complete macroblock row");
        require((ipu::read_reg(0x10) & 0xc000u) == 0x8000u,
                "IDEC flags start-code boundary without decode error");
        for (unsigned x = 0; x < width / 16; ++x) {
            const u8* mb = ee_host(output) + x * 1024;
            for (unsigned y = 0; y < 16; ++y)
                std::memcpy(actual.data() + ((row * 16 + y) * width + x * 16) * 4,
                            mb + y * 16 * 4, 16 * 4);
        }
        ++rows;
    }
    require(rows == height / 16, "decoded every first-picture slice");
    double mse = 0;
    for (size_t i = 0; i < actual.size(); i += 4)
        for (int channel = 0; channel < 3; ++channel) {
            const double delta = double(actual[i + channel]) - reference[i + channel];
            mse += delta * delta;
        }
    mse /= width * height * 3;
    const double psnr = mse ? 10.0 * std::log10(255.0 * 255.0 / mse) : std::numeric_limits<double>::infinity();
    double mean = 0, variance = 0;
    for (size_t i = 0; i < reference.size(); i += 4)
        mean += (double(reference[i]) + reference[i + 1] + reference[i + 2]) / 3;
    mean /= width * height;
    for (size_t i = 0; i < reference.size(); i += 4) {
        const double y = (double(reference[i]) + reference[i + 1] + reference[i + 2]) / 3;
        variance += (y - mean) * (y - mean);
    }
    variance /= width * height;
    require(variance > 300, "picture has substantive visible content, not flat gray or black");
    std::filesystem::create_directories(IPU_OUTPUT_DIR);
    const std::string ipu_path = std::string(IPU_OUTPUT_DIR) + "/ipu_" + label + ".png";
    const std::string ffmpeg_path = std::string(IPU_OUTPUT_DIR) + "/ffmpeg_" + label + ".png";
    save_png(ipu_path.c_str(), actual, width, height);
    save_png(ffmpeg_path.c_str(), reference, width, height);
    std::printf("%s I-picture #%u: %ux%u %u slices, luma variance %.1f, RGB PSNR %.2f dB\n",
                label, target_i, width, height, rows, variance, psnr);
    require(psnr > 35.0, "IPU RGB matches FFmpeg reference at PSNR > 35 dB");
}
#endif
} // namespace

namespace hw {
DmaChan& dma_chan(u32 ch) { return channels[ch]; }
void dma_done(u32 ch) { channels[ch].chcr &= ~0x100u; ++dma_interrupts[ch]; }
} // namespace hw
void intc_raise(u32 line) { if (line == INTC_IPU) ++ipu_interrupts; }

int main() {
    ee_mem_init();
    test_fifo_and_chain();
    test_fdec_crosses_qword_dma_boundary();
    test_dma_call_ret_and_irq();
    test_command_stall_and_setiq();
    test_bdec_macroblock();
    test_predicted_picture_residual();
    test_csc_pack_and_threshold();
#ifdef IPU_FFMPEG
    test_i_picture("PATARI.PSS", 7, "patari_i");
    test_i_picture("NINTRO.PSS", 18, "nintro_i");
#endif
    std::puts("ipu_tests: FIFO, DMA, FDEC/VDEC, SETIQ/SETVQ, BDEC/IDEC, CSC/PACK passed");
}
