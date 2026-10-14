// 돼굴돼굴 발화자 구분 (dwg-diar.exe)
//  · sherpa-onnx(Apache-2.0)의 오프라인 발화자 구분 기능을 씀 — 인터넷 사용 없음, 이 PC 안에서만
//  · 입력: 16kHz · 한 채널 · 16비트 wav (돼굴돼굴 녹음 형식. 다른 파일은 돼굴돼굴이 먼저 바꿔서 넘김)
//  · 녹음 전체를 한 번에 구분해서 화자 번호가 처음부터 끝까지 이어짐
//
//  dwg-diar --in 녹음.wav --seg segmentation.onnx --emb embedding.onnx --out 결과.화자
//           [--threads N] [--threshold 0.5] [--speakers N(모르면 0)] [--parent-pid N]
//  표준 출력 (돼굴돼굴이 읽음):
//    DUR <ms>   P <한 것> <전체>   SPK <화자 수>   DONE <걸린 ms>   ERR <내용>
//  결과 파일: 한 줄에 '시작ms 끝ms 화자번호(0부터)' — 시작 순서

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "sherpa-onnx/c-api/c-api.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
typedef std::wstring pstr;
static std::string to_utf8(const pstr & w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0'); if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}
static FILE * open_read(const pstr & p) { return _wfopen(p.c_str(), L"rb"); }
static FILE * open_write(const pstr & p) { return _wfopen(p.c_str(), L"wb"); }
#else
#include <signal.h>
#include <cerrno>
typedef std::string pstr;
static std::string to_utf8(const pstr & s) { return s; }
static FILE * open_read(const pstr & p) { return fopen(p.c_str(), "rb"); }
static FILE * open_write(const pstr & p) { return fopen(p.c_str(), "wb"); }
#endif

static void out(const char * fmt, ...) {
    va_list ap; va_start(ap, fmt); vfprintf(stdout, fmt, ap); va_end(ap);
    fputc('\n', stdout); fflush(stdout);
}

static uint32_t le32(const unsigned char * p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint16_t le16(const unsigned char * p) { return (uint16_t)(p[0] | (p[1] << 8)); }

// 16kHz · 한 채널 · 16비트 wav 읽기 (녹음 중 끊긴 파일처럼 data 길이가 틀려도 파일 끝까지 읽음)
static int read_wav(const pstr & path, std::vector<float> & pcm) {
    FILE * f = open_read(path);
    if (!f) return 1;
    unsigned char h[12];
    if (fread(h, 1, 12, f) != 12 || memcmp(h, "RIFF", 4) != 0 || memcmp(h + 8, "WAVE", 4) != 0) { fclose(f); return 2; }
    bool fmt_ok = false;
    for (;;) {
        unsigned char c[8];
        if (fread(c, 1, 8, f) != 8) { fclose(f); return 2; }
        uint32_t size = le32(c + 4);
        if (memcmp(c, "fmt ", 4) == 0) {
            std::vector<unsigned char> b(size);
            if (size < 16 || fread(b.data(), 1, size, f) != size) { fclose(f); return 2; }
            uint16_t format = le16(&b[0]), ch = le16(&b[2]), bits = le16(&b[14]);
            uint32_t rate = le32(&b[4]);
            if (!((format == 1 || format == 0xFFFE) && ch == 1 && bits == 16 && rate == 16000)) { fclose(f); return 3; }
            fmt_ok = true;
            if (size & 1) fseek(f, 1, SEEK_CUR);
        } else if (memcmp(c, "data", 4) == 0) {
            if (!fmt_ok) { fclose(f); return 2; }
            std::vector<int16_t> buf(1 << 16);
            size_t got;
            while ((got = fread(buf.data(), 2, buf.size(), f)) > 0)
                for (size_t i = 0; i < got; i++) pcm.push_back(buf[i] / 32768.0f);
            fclose(f);
            return 0;
        } else {
#ifdef _WIN32
            _fseeki64(f, size + (size & 1), SEEK_CUR);
#else
            fseeko(f, size + (size & 1), SEEK_CUR);
#endif
        }
    }
}

static int32_t on_progress(int32_t done, int32_t total, void *) {
    static auto last = std::chrono::steady_clock::now() - std::chrono::seconds(1);
    auto now = std::chrono::steady_clock::now();
    if (done == total || now - last >= std::chrono::milliseconds(500)) { out("P %d %d", done, total); last = now; }
    return 0;
}

#ifdef _WIN32
int wmain(int argc, wchar_t ** argv) {
    SetConsoleOutputCP(CP_UTF8);
#else
int main(int argc, char ** argv) {
#endif
    pstr in, seg, emb, outp;
    int threads = std::max(1u, std::thread::hardware_concurrency() / 2);
    float threshold = 0.5f; int speakers = 0; unsigned long parent_pid = 0;
    for (int i = 1; i + 1 < argc; i += 2) {
        pstr k = argv[i], v = argv[i + 1];
        std::string ks = to_utf8(k), vs = to_utf8(v);
        if (ks == "--in") in = v; else if (ks == "--seg") seg = v; else if (ks == "--emb") emb = v; else if (ks == "--out") outp = v;
        else if (ks == "--threads") threads = std::max(1, atoi(vs.c_str()));
        else if (ks == "--threshold") threshold = (float)atof(vs.c_str());
        else if (ks == "--speakers") speakers = std::max(0, atoi(vs.c_str()));
        else if (ks == "--parent-pid") parent_pid = strtoul(vs.c_str(), nullptr, 10);
    }
    if (in.empty() || seg.empty() || emb.empty() || outp.empty()) { out("ERR 사용법: dwg-diar --in 녹음.wav --seg 모델 --emb 모델 --out 결과"); return 2; }

    // 돼굴돼굴이 꺼지면 같이 끝냄
    if (parent_pid > 0) {
        std::thread([parent_pid]() {
#ifdef _WIN32
            HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, parent_pid);
            if (!h) _exit(9);
            WaitForSingleObject(h, INFINITE);
            _exit(9);
#else
            for (;;) { std::this_thread::sleep_for(std::chrono::milliseconds(500)); if (kill((pid_t)parent_pid, 0) != 0 && errno == ESRCH) _exit(9); }
#endif
        }).detach();
    }

    auto t0 = std::chrono::steady_clock::now();
    std::vector<float> pcm;
    int r = read_wav(in, pcm);
    if (r == 1) { out("ERR 녹음 파일을 열지 못했어요"); return 3; }
    if (r == 3) { out("ERR 16kHz · 한 채널 wav만 돼요"); return 3; }
    if (r != 0) { out("ERR wav 파일 모양이 아니에요"); return 3; }
    long long dur = (long long)pcm.size() * 1000 / 16000;
    out("DUR %lld", dur);
    if (pcm.size() < 16000 * 2) { out("ERR 녹음이 너무 짧아요"); return 3; }
    if (pcm.size() > 0x7FFFFFFFull) { out("ERR 녹음이 너무 길어요"); return 3; }

    std::string seg8 = to_utf8(seg), emb8 = to_utf8(emb);   // sherpa-onnx는 UTF-8 경로를 받음 (윈도우에서 한글 폴더 가능)
    SherpaOnnxOfflineSpeakerDiarizationConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.segmentation.pyannote.model = seg8.c_str();
    // 창 이동 간격: 짧은 녹음은 촘촘히(1초), 긴 녹음은 덩어리 수를 4000개 안팎으로 묶어 메모리 · 시간을 줄임
    double sec = dur / 1000.0;
    cfg.segmentation.pyannote.window_shift_ratio = (float)std::min(0.5, std::max(0.1, sec / (10.0 * 4000)));
    cfg.segmentation.num_threads = threads;
    cfg.segmentation.provider = "cpu";
    cfg.embedding.model = emb8.c_str();
    cfg.embedding.num_threads = threads;
    cfg.embedding.provider = "cpu";
    cfg.clustering.num_clusters = speakers > 0 ? speakers : -1;
    cfg.clustering.threshold = threshold;
    cfg.min_duration_on = 0.3f;
    cfg.min_duration_off = 0.5f;

    const SherpaOnnxOfflineSpeakerDiarization * sd = SherpaOnnxCreateOfflineSpeakerDiarization(&cfg);
    if (!sd) { out("ERR 발화자 구분 모델을 읽지 못했어요"); return 4; }

    const SherpaOnnxOfflineSpeakerDiarizationResult * res =
        SherpaOnnxOfflineSpeakerDiarizationProcessWithCallback(sd, pcm.data(), (int32_t)pcm.size(), on_progress, nullptr);
    std::vector<float>().swap(pcm);
    if (!res) { out("ERR 발화자를 구분하지 못했어요"); SherpaOnnxDestroyOfflineSpeakerDiarization(sd); return 5; }

    int n = SherpaOnnxOfflineSpeakerDiarizationResultGetNumSegments(res);
    int ns = SherpaOnnxOfflineSpeakerDiarizationResultGetNumSpeakers(res);
    const SherpaOnnxOfflineSpeakerDiarizationSegment * s = n > 0 ? SherpaOnnxOfflineSpeakerDiarizationResultSortByStartTime(res) : nullptr;

    FILE * fo = open_write(outp);
    if (!fo) { out("ERR 결과 파일을 만들지 못했어요"); return 6; }
    bool ok = true;
    for (int i = 0; i < n; i++)
        if (fprintf(fo, "%lld %lld %d\n", (long long)llround(s[i].start * 1000.0), (long long)llround(s[i].end * 1000.0), s[i].speaker) < 0) ok = false;
    if (fclose(fo) != 0) ok = false;
    if (s) SherpaOnnxOfflineSpeakerDiarizationDestroySegment(s);
    SherpaOnnxOfflineSpeakerDiarizationDestroyResult(res);
    SherpaOnnxDestroyOfflineSpeakerDiarization(sd);
    if (!ok) { out("ERR 결과 파일을 저장하지 못했어요"); return 6; }

    out("SPK %d", ns);
    out("DONE %lld", (long long)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count());
    return 0;
}
