// Spike for mxl-browser-source (SPEC §19, S1-S4): windowless CEF driven by external
// BeginFrames. Prints paints per BeginFrame, latency, alpha, renderer, audio clock, CPU.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include "include/base/cef_callback.h"
#include "include/cef_app.h"
#include "include/cef_audio_handler.h"
#include "include/cef_browser.h"
#include "include/cef_client.h"
#include "include/cef_render_handler.h"
#include "include/wrapper/cef_closure_task.h"

namespace
{
std::string env(char const* name, char const* fallback)
{
    char const* v = std::getenv(name);
    return v != nullptr ? v : fallback;
}

double nowMono()
{
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) / 1e9;
}

double nowTai()
{
    timespec ts{};
    clock_gettime(CLOCK_TAI, &ts);
    return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) / 1e9;
}

int const kWidth = 1920;
int const kHeight = 1080;

struct Stats
{
    std::mutex mu;
    std::vector<double> bfTimes;
    std::vector<double> paintTimes;
    int paintsSinceBf = 0;
    std::vector<int> paintsPerBf;
    std::vector<double> latency;
    unsigned char boxPixel[4] = {};
    unsigned char bgPixel[4] = {};
    bool sampled = false;
    std::string title;
    // audio
    int audioRate = 0;
    int audioChannels = 0;
    int audioFramesPerBuffer = 0;
    long long audioFrames = 0;
    double audioFirstMono = 0;
    double audioLastMono = 0;
    long long audioFirstPts = -1;
    long long audioLastPts = -1;
    int audioPackets = 0;
    float audioPeak = 0;
} gStats;

std::atomic<bool> gStop{false};
CefRefPtr<CefBrowser> gBrowser;
std::mutex gBrowserMu;

class Handler : public CefClient, public CefRenderHandler, public CefAudioHandler, public CefLifeSpanHandler, public CefDisplayHandler, public CefLoadHandler
{
public:
    CefRefPtr<CefRenderHandler> GetRenderHandler() override { return this; }
    CefRefPtr<CefAudioHandler> GetAudioHandler() override { return this; }
    CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }
    CefRefPtr<CefDisplayHandler> GetDisplayHandler() override { return this; }
    CefRefPtr<CefLoadHandler> GetLoadHandler() override { return this; }

    void GetViewRect(CefRefPtr<CefBrowser>, CefRect& rect) override { rect = CefRect(0, 0, kWidth, kHeight); }

    void OnPaint(CefRefPtr<CefBrowser>, PaintElementType type, RectList const&, void const* buffer, int width, int height) override
    {
        if (type != PET_VIEW)
        {
            return;
        }
        double const t = nowMono();
        std::lock_guard lock{gStats.mu};
        gStats.paintTimes.push_back(t);
        gStats.paintsSinceBf++;
        if (!gStats.bfTimes.empty())
        {
            gStats.latency.push_back(t - gStats.bfTimes.back());
        }
        if (width == kWidth && height == kHeight)
        {
            auto const* px = static_cast<unsigned char const*>(buffer);
            std::memcpy(gStats.boxPixel, px + (200 * kWidth + 200) * 4, 4);
            std::memcpy(gStats.bgPixel, px + (900 * kWidth + 1800) * 4, 4);
            gStats.sampled = true;
        }
    }

    bool GetAudioParameters(CefRefPtr<CefBrowser>, CefAudioParameters& params) override
    {
        params.channel_layout = CEF_CHANNEL_LAYOUT_STEREO;
        params.sample_rate = 48000;
        params.frames_per_buffer = 480;
        return true;
    }
    void OnAudioStreamStarted(CefRefPtr<CefBrowser>, CefAudioParameters const& params, int channels) override
    {
        std::lock_guard lock{gStats.mu};
        gStats.audioRate = params.sample_rate;
        gStats.audioChannels = channels;
        gStats.audioFramesPerBuffer = params.frames_per_buffer;
        std::printf("audio started: rate=%d channels=%d frames_per_buffer=%d layout=%d\n", params.sample_rate, channels,
            params.frames_per_buffer, static_cast<int>(params.channel_layout));
    }
    void OnAudioStreamPacket(CefRefPtr<CefBrowser>, float const** data, int frames, int64_t pts) override
    {
        double const t = nowMono();
        std::lock_guard lock{gStats.mu};
        if (gStats.audioFirstPts < 0)
        {
            gStats.audioFirstPts = pts;
            gStats.audioFirstMono = t;
        }
        gStats.audioLastPts = pts;
        gStats.audioLastMono = t;
        gStats.audioFrames += frames;
        gStats.audioPackets++;
        for (int i = 0; i < frames; ++i)
        {
            gStats.audioPeak = std::max(gStats.audioPeak, std::abs(data[0][i]));
        }
    }
    void OnAudioStreamStopped(CefRefPtr<CefBrowser>) override { std::printf("audio stopped\n"); }
    void OnAudioStreamError(CefRefPtr<CefBrowser>, CefString const& message) override { std::printf("audio error: %s\n", message.ToString().c_str()); }

    void OnTitleChange(CefRefPtr<CefBrowser>, CefString const& title) override
    {
        std::lock_guard lock{gStats.mu};
        gStats.title = title.ToString();
    }
    void OnLoadEnd(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame> frame, int code) override
    {
        if (frame->IsMain())
        {
            std::printf("load end %d at %.3f\n", code, nowMono());
        }
    }
    void OnLoadError(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>, ErrorCode code, CefString const& text, CefString const& url) override
    {
        std::printf("load error %d %s %s\n", static_cast<int>(code), text.ToString().c_str(), url.ToString().c_str());
    }

    void OnAfterCreated(CefRefPtr<CefBrowser> browser) override
    {
        std::lock_guard lock{gBrowserMu};
        gBrowser = browser;
    }
    void OnBeforeClose(CefRefPtr<CefBrowser>) override
    {
        {
            std::lock_guard lock{gBrowserMu};
            gBrowser = nullptr;
        }
        CefQuitMessageLoop();
    }

    IMPLEMENT_REFCOUNTING(Handler);
};

class App : public CefApp
{
public:
    void OnBeforeCommandLineProcessing(CefString const&, CefRefPtr<CefCommandLine> command_line) override
    {
        std::stringstream flags(env("SPIKE_FLAGS", ""));
        std::string flag;
        while (std::getline(flags, flag, ','))
        {
            if (flag.empty())
            {
                continue;
            }
            auto const eq = flag.find('=');
            if (eq == std::string::npos)
            {
                command_line->AppendSwitch(flag);
            }
            else
            {
                command_line->AppendSwitchWithValue(flag.substr(0, eq), flag.substr(eq + 1));
            }
        }
    }
    IMPLEMENT_REFCOUNTING(App);
};

CefRefPtr<CefBrowser> browser()
{
    std::lock_guard lock{gBrowserMu};
    return gBrowser;
}

void beginFrame(bool invalidate)
{
    // CEF may call handlers synchronously from these calls: never hold gBrowserMu across them.
    auto const b = browser();
    if (!b)
    {
        return;
    }
    {
        std::lock_guard s{gStats.mu};
        if (!gStats.bfTimes.empty())
        {
            gStats.paintsPerBf.push_back(gStats.paintsSinceBf);
        }
        gStats.paintsSinceBf = 0;
        gStats.bfTimes.push_back(nowMono());
    }
    if (invalidate)
    {
        b->GetHost()->Invalidate(PET_VIEW);
    }
    b->GetHost()->SendExternalBeginFrame();
}

void closeBrowser()
{
    if (auto const b = browser())
    {
        b->GetHost()->CloseBrowser(true);
    }
}

// CPU seconds of this process and every descendant (/proc).
double treeCpuSeconds()
{
    long const hz = sysconf(_SC_CLK_TCK);
    std::vector<int> pids{getpid()};
    std::vector<std::pair<int, int>> all;
    if (DIR* d = opendir("/proc"))
    {
        while (dirent* e = readdir(d))
        {
            int const pid = std::atoi(e->d_name);
            if (pid <= 0)
            {
                continue;
            }
            std::ifstream f("/proc/" + std::to_string(pid) + "/stat");
            std::string s;
            std::getline(f, s);
            auto const close = s.rfind(')');
            if (close == std::string::npos)
            {
                continue;
            }
            std::istringstream rest(s.substr(close + 2));
            char state;
            int ppid;
            rest >> state >> ppid;
            all.emplace_back(pid, ppid);
        }
        closedir(d);
    }
    for (std::size_t i = 0; i < pids.size(); ++i)
    {
        for (auto const& [pid, ppid] : all)
        {
            if (ppid == pids[i])
            {
                pids.push_back(pid);
            }
        }
    }
    double total = 0;
    for (int pid : pids)
    {
        std::ifstream f("/proc/" + std::to_string(pid) + "/stat");
        std::string s;
        std::getline(f, s);
        auto const close = s.rfind(')');
        if (close == std::string::npos)
        {
            continue;
        }
        std::istringstream rest(s.substr(close + 2));
        std::string field;
        long long utime = 0, stime = 0;
        for (int i = 3; i <= 15; ++i)
        {
            rest >> field;
            if (i == 14)
            {
                utime = std::atoll(field.c_str());
            }
            if (i == 15)
            {
                stime = std::atoll(field.c_str());
            }
        }
        total += static_cast<double>(utime + stime) / static_cast<double>(hz);
    }
    return total;
}

double pct(std::vector<double> v, double p)
{
    if (v.empty())
    {
        return 0;
    }
    std::sort(v.begin(), v.end());
    return v[static_cast<std::size_t>(p * static_cast<double>(v.size() - 1))];
}

void driver()
{
    double const fps = std::atof(env("SPIKE_FPS", "50").c_str());
    double const seconds = std::atof(env("SPIKE_SECONDS", "20").c_str());
    bool const invalidate = env("SPIKE_INVALIDATE", "1") == "1";
    double const warmup = 5;
    for (int i = 0; i < 200; ++i)
    {
        {
            std::lock_guard lock{gBrowserMu};
            if (gBrowser)
            {
                break;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    double const period = 1.0 / fps;
    double next = nowMono();
    double const start = next;
    double cpu0 = 0, measureStart = 0;
    bool measuring = false;
    while (!gStop.load())
    {
        next += period;
        double const wait = next - nowMono();
        if (wait > 0)
        {
            std::this_thread::sleep_for(std::chrono::duration<double>(wait));
        }
        if (!measuring && nowMono() - start > warmup)
        {
            std::lock_guard s{gStats.mu};
            gStats.bfTimes.clear();
            gStats.paintTimes.clear();
            gStats.paintsPerBf.clear();
            gStats.latency.clear();
            gStats.audioFrames = 0;
            gStats.audioFirstPts = -1;
            gStats.audioPackets = 0;
            cpu0 = treeCpuSeconds();
            measureStart = nowMono();
            measuring = true;
        }
        if (measuring && nowMono() - measureStart > seconds)
        {
            break;
        }
        CefPostTask(TID_UI, base::BindOnce(&beginFrame, invalidate));
    }
    double const cpu = treeCpuSeconds() - cpu0;
    double const span = nowMono() - measureStart;
    {
        std::lock_guard s{gStats.mu};
        int ppb[4] = {0, 0, 0, 0};
        for (int n : gStats.paintsPerBf)
        {
            ppb[std::min(n, 3)]++;
        }
        std::printf("RESULT begin_frames=%zu paints=%zu paints_per_bf{0:%d 1:%d 2:%d 3+:%d}\n", gStats.bfTimes.size(), gStats.paintTimes.size(), ppb[0], ppb[1], ppb[2], ppb[3]);
        std::printf("RESULT latency_ms p50=%.2f p95=%.2f max=%.2f\n", 1000 * pct(gStats.latency, 0.5), 1000 * pct(gStats.latency, 0.95), 1000 * pct(gStats.latency, 1.0));
        std::printf("RESULT pixel box(rgba 255,0,0,0.5) bgra=%d,%d,%d,%d background bgra=%d,%d,%d,%d\n", gStats.boxPixel[0], gStats.boxPixel[1], gStats.boxPixel[2], gStats.boxPixel[3],
            gStats.bgPixel[0], gStats.bgPixel[1], gStats.bgPixel[2], gStats.bgPixel[3]);
        std::printf("RESULT title=%s\n", gStats.title.c_str());
        double const audioSpan = gStats.audioLastMono - gStats.audioFirstMono;
        double const audioSeconds = static_cast<double>(gStats.audioFrames) / std::max(1, gStats.audioRate);
        std::printf("RESULT audio rate=%d ch=%d packets=%d frames=%lld over %.3f s mono -> %.1f ppm, pts span %lld ms, peak %.3f\n", gStats.audioRate,
            gStats.audioChannels, gStats.audioPackets, gStats.audioFrames, audioSpan,
            audioSpan > 0 ? 1e6 * (audioSeconds - audioSpan) / audioSpan : 0.0, gStats.audioLastPts - gStats.audioFirstPts, gStats.audioPeak);
        std::printf("RESULT cpu=%.2f cores over %.1f s (all CEF processes)\n", cpu / span, span);
    }
    std::fflush(stdout);
    gStop.store(true);
    CefPostTask(TID_UI, base::BindOnce(&closeBrowser));
}
} // namespace

int main(int argc, char* argv[])
{
    CefMainArgs args(argc, argv);
    CefRefPtr<App> app(new App());
    int const code = CefExecuteProcess(args, app, nullptr);
    if (code >= 0)
    {
        return code;
    }
    CefSettings settings;
    settings.windowless_rendering_enabled = true;
    settings.no_sandbox = true;
    settings.log_severity = LOGSEVERITY_WARNING;
    CefString(&settings.root_cache_path).FromString("/tmp/spike-cache");
    if (!CefInitialize(args, settings, app, nullptr))
    {
        std::printf("CefInitialize failed\n");
        return 75;
    }
    CefWindowInfo info;
    info.SetAsWindowless(kNullWindowHandle);
    info.external_begin_frame_enabled = true;
    CefBrowserSettings browserSettings;
    browserSettings.windowless_frame_rate = 60;
    browserSettings.background_color = 0x00000000;
    CefRefPtr<Handler> handler(new Handler());
    CefBrowserHost::CreateBrowser(info, handler, env("SPIKE_URL", "file:///opt/spike/spike.html"), browserSettings, nullptr, nullptr);
    std::thread t(driver);
    CefRunMessageLoop();
    t.join();
    CefShutdown();
    std::printf("shutdown ok\n");
    return 0;
}
