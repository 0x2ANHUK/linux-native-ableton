// Audio through PipeWire for AbletonLive12.
//
// Live's ALSA driver lists sound cards itself (snd_card_next,
// snd_ctl_pcm_next_device) and opens the chosen one as "hw:CARD=...,DEV=..."
// with mmap access. A hw device has one user, so while Live runs the desktop
// (PipeWire) loses that card, and Live fails with "Device or resource busy"
// when PipeWire already holds it. On the Push, Live owns the hardware alone.
//
// Live's hw opens are sent to the "pipewire" ALSA PCM instead (pipewire-alsa;
// it supports the mmap access Live uses). Live keeps its own device list and
// settings; the sound goes to PipeWire's default sink/source, which the
// desktop routes. LIVE_ALSA_DEVICE=<pcm> picks another ALSA PCM,
// LIVE_ALSA_EXCLUSIVE=1 keeps Live's direct hardware access.
//
// Sample format: Live offers S32_LE, S24_3LE, S16_LE and FLOAT_LE in one mask
// (0x2d625c6) and takes whatever snd_pcm_hw_params settles on, which is the
// lowest: S16_LE whenever the device has it, pipewire-alsa included. Its own
// mixing is float, so the output was cut to 16 bits and converted twice. The
// mask is narrowed to the best format the device takes: FLOAT_LE, then
// S32_LE, S24_3LE, S16_LE (LIVE_ALSA_FORMAT=device leaves the choice to ALSA).
//
// Channels: Live sets the channel count to snd_pcm_hw_params_get_channels_max
// (0x2d626ac). The pipewire PCM offers 1..128, so Live opened 128-channel
// streams and listed 64 stereo pairs in Input/Output Config, and PipeWire
// converted 128 channels every cycle. The count is capped at the channels of
// the hw device Live chose (opened briefly, or read from /proc/asound while
// PipeWire holds it; 2 when neither works). LIVE_ALSA_CHANNELS=<n> sets the cap.
#include "livebuild.h"
#include <alsa/asoundlib.h>
#include <dlfcn.h>
#include <link.h>
#include <sys/mman.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <atomic>
#include <initializer_list>
#include <string>
#include <vector>
#include "pwdevices.h"
#include "audiodrivers.h"
#include <QtDBus/QDBusConnection>
#include <QtDBus/QDBusConnectionInterface>
#include <QtDBus/QDBusMessage>
#include <QtDBus/QDBusVirtualObject>
#include <QtCore/QCoreApplication>
#include <mutex>
#include <thread>
#include <unistd.h>

void menufixTrace(const char *fmt, int a = 0, int b = 0, int c = 0);
void *nextSymbol(const char *name, const char *version); // embedded.cpp
// jackaudio.cpp: the JACK device's PCMs
int jackOpen(snd_pcm_t **pcm, snd_pcm_stream_t stream);
bool jackStream(snd_pcm_t *pcm);
int jackSetChannels(snd_pcm_t *pcm, snd_pcm_hw_params_t *params, unsigned int val);
int jackSetFloatFormat(snd_pcm_t *pcm, snd_pcm_hw_params_t *params); // Live 11

namespace {

using OpenFn = int (*)(snd_pcm_t **, const char *, snd_pcm_stream_t, int);

// Shared PCMs opened for Live, with the channel count of the device Live asked for.
struct SharedPcm {
    snd_pcm_t *pcm;
    unsigned channels;
};
SharedPcm sharedPcms[8];
unsigned nextShared;

// A closed PCM's memory is reused by the next open: closing playback and
// capture and opening them again (new buffer size, sample rate or device)
// swaps their addresses. Entries are looked up by address, so a stale entry
// capped the new playback stream at the old capture stream's channel count
// (a mono microphone made the output mono). Every open forgets its address
// before the new entry is added.
void forgetPcm(snd_pcm_t *pcm)
{
    for (SharedPcm &shared : sharedPcms)
        if (shared.pcm == pcm)
            shared = {nullptr, 0};
}

void rememberPcm(snd_pcm_t *pcm, unsigned channels)
{
    forgetPcm(pcm);
    sharedPcms[nextShared++ % 8] = {pcm, channels};
}

// "hw:CARD=Generic_1,DEV=0" or "hw:1,0" -> /proc/asound/<card>/pcm<dev><p|c>/sub0/hw_params
std::string hwParamsPath(const char *name, snd_pcm_stream_t stream)
{
    std::string spec = strchr(name, ':') + 1, card, device = "0";
    size_t comma = spec.find(',');
    card = spec.substr(0, comma);
    if (comma != std::string::npos)
        device = spec.substr(comma + 1);
    if (!card.compare(0, 5, "CARD="))
        card = card.substr(5);
    else
        card = "card" + card;
    if (!device.compare(0, 4, "DEV="))
        device = device.substr(4);
    return "/proc/asound/" + card + "/pcm" + device + (stream == SND_PCM_STREAM_PLAYBACK ? "p" : "c") + "/sub0/hw_params";
}

unsigned deviceChannels(OpenFn open, const char *name, snd_pcm_stream_t stream)
{
    if (const char *forced = getenv("LIVE_ALSA_CHANNELS"))
        return strtoul(forced, nullptr, 10);
    std::string hw = std::string("hw:") + (strchr(name, ':') + 1); // plughw converts any count; ask the hw device
    snd_pcm_t *device;
    unsigned channels = 0;
    if (open(&device, hw.c_str(), stream, SND_PCM_NONBLOCK) == 0) {
        snd_pcm_hw_params_t *params;
        snd_pcm_hw_params_alloca(&params);
        if (snd_pcm_hw_params_any(device, params) < 0 || snd_pcm_hw_params_get_channels_max(params, &channels) < 0)
            channels = 0;
        snd_pcm_close(device);
    } else if (FILE *file = fopen(hwParamsPath(hw.c_str(), stream).c_str(), "r")) {
        // busy: PipeWire has it open, with the channels it uses
        char line[128];
        while (fgets(line, sizeof line, file))
            if (sscanf(line, "channels: %u", &channels) == 1)
                break;
        fclose(file);
    }
    return channels ? channels : 2;
}

// Only Live's own opens: alsa-lib and its plug-ins open hw devices internally too.
bool calledFromLive(const void *returnAddress)
{
    Dl_info info;
    link_map *map = nullptr;
    if (!dladdr1(returnAddress, &info, reinterpret_cast<void **>(&map), RTLD_DL_LINKMAP) || !map)
        return false;
    return map->l_name == nullptr || map->l_name[0] == '\0'; // the main program
}

// LIVE_AUDIO_THREADS=<n> (optional, from PR #4; not set by default): Live sizes
// its pool of audio worker threads ("AudioCalc") from the online CPU count,
// count - 1 (11 on a 12-thread CPU). With the variable set, Live's own
// sysconf(_SC_NPROCESSORS_ONLN/CONF) calls see at most n, so it makes n - 1
// workers; Live's libraries, Qt and plug-ins still see the machine as it is.
// Nothing is pinned. Measured (Patience, 12-thread CPU): n = 4 used 1.07
// instead of 1.28 cores with no more dropouts; n = 2 had more dropouts.
// Options-desktop.txt can carry it as -_LiveAudioThreads=<n> (standalone.cpp).
extern "C" long sysconf(int name)
{
    static auto real = reinterpret_cast<long (*)(int)>(nextSymbol("sysconf", "GLIBC_2.2.5"));
    long result = real(name);
    if ((name == _SC_NPROCESSORS_ONLN || name == _SC_NPROCESSORS_CONF) && calledFromLive(__builtin_return_address(0))) {
        static const long cap = [] {
            const char *value = getenv("LIVE_AUDIO_THREADS");
            const long n = value ? strtol(value, nullptr, 10) : 0;
            return n > 0 ? n : 0L;
        }();
        if (cap && result > cap)
            result = cap;
    }
    return result;
}

// Driver type ALSA opens the sound card itself. PipeWire (WirePlumber) holds
// it, and gives it up on request through the device reservation protocol
// (org.freedesktop.ReserveDevice1.Audio<card>, as jackd does): ask the owner
// to release it, take the name, and drop the name when Live closes its last
// PCM of that card, so the desktop gets the card back.
std::mutex reserveMutex;
struct Reservation {
    int card;
    unsigned pcms;
    QObject *object; // the exported org.freedesktop.ReserveDevice1 object
};
std::vector<Reservation> reservations;
struct ExclusivePcm {
    snd_pcm_t *pcm;
    int card;
};
std::vector<ExclusivePcm> exclusivePcms;

// "hw:1,0", "plughw:1", "hw:CARD=Generic_1,DEV=0" -> 1
int cardIndex(const char *name)
{
    const char *spec = strchr(name, ':') + 1;
    if (!strncmp(spec, "CARD=", 5)) {
        std::string id(spec + 5, strcspn(spec + 5, ","));
        return snd_card_get_index(id.c_str());
    }
    return atoi(spec);
}

QString reserveName(int card)
{
    return QStringLiteral("org.freedesktop.ReserveDevice1.Audio%1").arg(card);
}

QString reservePath(int card)
{
    return QStringLiteral("/org/freedesktop/ReserveDevice1/Audio%1").arg(card);
}

constexpr int kReservePriority = 10; // above WirePlumber's -20

// The reservation object owners export: others (WirePlumber, wanting the card
// back) ask it to release the card; Live keeps it while a PCM is open, so the
// answer is no. Without an answer WirePlumber's own request hangs, and it then
// misses the name coming free and never takes the card back.
class ReserveObject : public QDBusVirtualObject {
public:
    explicit ReserveObject(int card) : card(card) {}
    QString introspect(const QString &) const override
    {
        return QStringLiteral("<interface name=\"org.freedesktop.ReserveDevice1\">"
                              "<method name=\"RequestRelease\"><arg name=\"priority\" type=\"i\" direction=\"in\"/>"
                              "<arg name=\"result\" type=\"b\" direction=\"out\"/></method>"
                              "<property name=\"Priority\" type=\"i\" access=\"read\"/>"
                              "<property name=\"ApplicationName\" type=\"s\" access=\"read\"/>"
                              "<property name=\"ApplicationDeviceName\" type=\"s\" access=\"read\"/></interface>");
    }
    bool handleMessage(const QDBusMessage &message, const QDBusConnection &connection) override
    {
        QVariant value;
        if (message.member() == QLatin1String("RequestRelease")) {
            value = false;
        } else if (message.member() == QLatin1String("Get") && message.arguments().size() == 2) {
            const QString property = message.arguments().at(1).toString();
            if (property == QLatin1String("Priority"))
                value = QVariant::fromValue(QDBusVariant(kReservePriority));
            else if (property == QLatin1String("ApplicationName"))
                value = QVariant::fromValue(QDBusVariant(QStringLiteral("Ableton Live")));
            else if (property == QLatin1String("ApplicationDeviceName"))
                value = QVariant::fromValue(QDBusVariant(QStringLiteral("hw:%1").arg(card)));
        }
        if (!value.isValid())
            return false;
        connection.send(message.createReply(value));
        return true;
    }

private:
    int card;
};

// When a reserved card was last handed back: WirePlumber then needs a moment to
// make its PipeWire devices again (pcmJustReleased, jackaudio.cpp's snd_pcm_hw_params).
std::atomic<long long> lastCardRelease{0};

bool reserveCard(int card)
{
    std::lock_guard<std::mutex> lock(reserveMutex);
    for (Reservation &r : reservations)
        if (r.card == card) {
            ++r.pcms;
            return true;
        }
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) {
        reservations.push_back({card, 1, nullptr}); // no session bus: nobody to ask
        return true;
    }
    // The object first: WirePlumber asks it for the card back as soon as the name is ours.
    // Its messages are handled in the main thread (Live's event loop).
    auto *object = new ReserveObject(card);
    if (QCoreApplication::instance())
        object->moveToThread(QCoreApplication::instance()->thread());
    bus.registerVirtualObject(reservePath(card), object);
    auto fail = [&](const char *why) {
        bus.unregisterObject(reservePath(card));
        object->deleteLater();
        menufixTrace(why, card);
        return false;
    };
    QDBusConnectionInterface *dbus = bus.interface();
    const QString name = reserveName(card);
    if (dbus->registerService(name, QDBusConnectionInterface::DontQueueService,
                              QDBusConnectionInterface::DontAllowReplacement)
        != QDBusConnectionInterface::ServiceRegistered) {
        QDBusMessage ask = QDBusMessage::createMethodCall(name, reservePath(card), QStringLiteral("org.freedesktop.ReserveDevice1"),
                                                          QStringLiteral("RequestRelease"));
        ask << kReservePriority;
        const QDBusMessage reply = bus.call(ask, QDBus::Block, 5000);
        if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().value(0).toBool() != true)
            return fail("alsaaudio: card %d: its owner would not release it\n");
        if (dbus->registerService(name, QDBusConnectionInterface::ReplaceExistingService,
                                  QDBusConnectionInterface::DontAllowReplacement)
            != QDBusConnectionInterface::ServiceRegistered)
            return fail("alsaaudio: card %d: reservation failed\n");
    }
    menufixTrace("alsaaudio: card %d reserved\n", card);
    reservations.push_back({card, 1, object});
    return true;
}

void releaseCard(int card)
{
    std::lock_guard<std::mutex> lock(reserveMutex);
    for (size_t i = 0; i < reservations.size(); ++i) {
        if (reservations[i].card != card || --reservations[i].pcms)
            continue;
        QObject *object = reservations[i].object;
        reservations.erase(reservations.begin() + i);
        QDBusConnection bus = QDBusConnection::sessionBus();
        if (bus.isConnected()) {
            bus.interface()->unregisterService(reserveName(card));
            bus.unregisterObject(reservePath(card));
        }
        if (object)
            object->deleteLater();
        lastCardRelease.store(std::chrono::steady_clock::now().time_since_epoch().count());
        menufixTrace("alsaaudio: card %d released\n", card);
        return;
    }
}

} // namespace

bool waitForPipeWireDefault(bool playback, int ms); // pwdevices.cpp

bool cardJustReleased()
{
    const long long at = lastCardRelease.load();
    return at && std::chrono::steady_clock::now().time_since_epoch().count() - at
                     < std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::seconds(3)).count();
}

// jackaudio.cpp's snd_pcm_close, for every PCM that is not a JACK one.
void alsaPcmClosed(snd_pcm_t *pcm)
{
    int card = -1;
    {
        std::lock_guard<std::mutex> lock(reserveMutex);
        for (size_t i = 0; i < exclusivePcms.size(); ++i)
            if (exclusivePcms[i].pcm == pcm) {
                card = exclusivePcms[i].card;
                exclusivePcms.erase(exclusivePcms.begin() + i);
                break;
            }
    }
    if (card >= 0)
        releaseCard(card);
}

// Live's reference is versioned ALSA_0.9 (shim/qt5.map).
extern "C" int snd_pcm_hw_params_set_format_mask(snd_pcm_t *pcm, snd_pcm_hw_params_t *params, snd_pcm_format_mask_t *mask)
{
    using SetMaskFn = int (*)(snd_pcm_t *, snd_pcm_hw_params_t *, snd_pcm_format_mask_t *);
    static auto real = reinterpret_cast<SetMaskFn>(nextSymbol("snd_pcm_hw_params_set_format_mask", "ALSA_0.9"));
    if (liveBuild() == LiveBuild::Live11_3_43 && jackStream(pcm) && calledFromLive(__builtin_return_address(0)))
        return jackSetFloatFormat(pcm, params); // Live 11 has no float format; jackaudio.cpp converts
    const char *choice = getenv("LIVE_ALSA_FORMAT");
    if ((choice && !strcmp(choice, "device")) || !calledFromLive(__builtin_return_address(0)))
        return real(pcm, params, mask);
    for (snd_pcm_format_t format : {SND_PCM_FORMAT_FLOAT_LE, SND_PCM_FORMAT_S32_LE, SND_PCM_FORMAT_S24_3LE}) {
        if (!snd_pcm_format_mask_test(mask, format) || snd_pcm_hw_params_test_format(pcm, params, format) != 0)
            continue;
        snd_pcm_format_mask_t *best;
        snd_pcm_format_mask_alloca(&best);
        snd_pcm_format_mask_none(best);
        snd_pcm_format_mask_set(best, format);
        if (real(pcm, params, best) == 0) {
            menufixTrace("alsaaudio: sample format %d\n", format);
            return 0;
        }
    }
    return real(pcm, params, mask);
}

// Live's reference is versioned ALSA_0.9; shim/qt5.map gives this definition that version.
extern "C" int snd_pcm_open(snd_pcm_t **pcm, const char *name, snd_pcm_stream_t stream, int mode)
{
    static auto real = reinterpret_cast<OpenFn>(nextSymbol("snd_pcm_open", "ALSA_0.9"));
    const bool hw = name && (!strncmp(name, "hw:", 3) || !strncmp(name, "plughw:", 7));
    if (hw && !getenv("LIVE_ALSA_EXCLUSIVE") && calledFromLive(__builtin_return_address(0))) {
        // Driver types (audiodrivers.cpp): the card number says which one listed the device.
        const int card = cardIndex(name);
        if (driverTypesActive() && card == kJackCard) {
            // alsa-plugins' JACK PCM (jackaudio.cpp), with the buffer of 2 periods below
            const int result = jackOpen(pcm, stream);
            if (result == 0)
                rememberPcm(*pcm, jackChannels(stream == SND_PCM_STREAM_PLAYBACK));
            return result;
        }
        if (driverTypesActive() && card >= 0 && card < kFirstVirtualCard) {
            if (!reserveCard(card))
                return -EBUSY;
            // WirePlumber answers RequestRelease before its own stream on the card is
            // closed (seen while JACK was playing to it): busy for a moment more.
            int result = real(pcm, name, stream, mode);
            for (int waited = 0; result == -EBUSY && waited < 2000; waited += 50) {
                usleep(50000);
                result = real(pcm, name, stream, mode);
                if (result != -EBUSY)
                    menufixTrace("alsaaudio: card %d free after %d ms\n", card, waited + 50);
            }
            if (result == 0) {
                std::lock_guard<std::mutex> lock(reserveMutex);
                exclusivePcms.push_back({*pcm, card});
            } else {
                menufixTrace("alsaaudio: opening card %d failed (%d)\n", card, result);
                releaseCard(card);
            }
            return result;
        }
        VirtualDevice device;
        if (virtualDevicesEnabled() && virtualDevice(atoi(strchr(name, ':') + 1), device)) {
            if (device.node.empty() && cardJustReleased())
                waitForPipeWireDefault(stream == SND_PCM_STREAM_PLAYBACK, 2000);
            // a PipeWire device from Live's menu (pwdevices.cpp)
            const std::string target = device.node.empty() ? "pipewire" : "pipewire:NODE=\"" + device.node + "\"";
            // A device chosen by name that goes away is not replaced by the
            // desktop's default (the laptop's speakers and microphone, which feed
            // back while an input is monitored): the stream fails and Live shows
            // No Device, as for an unplugged interface. node.dont-fallback would
            // also do that, but WirePlumber then refused to link to sound cards
            // at all (Live stuck with "Audio queue timeout").
            const char *props = getenv("PIPEWIRE_ALSA");
            const std::string saved = props ? props : "";
            if (!device.node.empty() && !saved.empty() && saved.back() == '}')
                setenv("PIPEWIRE_ALSA", (saved.substr(0, saved.size() - 1) + "node.dont-reconnect = true }").c_str(), 1);
            const int result = real(pcm, target.c_str(), stream, mode);
            if (props)
                setenv("PIPEWIRE_ALSA", saved.c_str(), 1);
            if (result == 0) {
                const unsigned channels = stream == SND_PCM_STREAM_PLAYBACK ? device.playbackChannels : device.captureChannels;
                rememberPcm(*pcm, channels ? channels : 2);
            } else {
                menufixTrace("alsaaudio: opening the PipeWire device failed (%d)\n", result);
            }
            return result;
        }
        const char *shared = getenv("LIVE_ALSA_DEVICE") ? getenv("LIVE_ALSA_DEVICE") : "pipewire";
        const unsigned channels = deviceChannels(real, name, stream);
        const int result = real(pcm, shared, stream, mode);
        if (result == 0) {
            rememberPcm(*pcm, channels);
            return 0;
        }
        menufixTrace("alsaaudio: opening the shared PCM failed (%d), using the hardware\n", result);
    }
    const int result = real(pcm, name, stream, mode);
    if (result == 0 && calledFromLive(__builtin_return_address(0)))
        forgetPcm(*pcm); // Live's own hardware PCM: no cap
    return result;
}

// Live's reference is versioned ALSA_0.9 (shim/qt5.map).
extern "C" int snd_pcm_hw_params_set_channels(snd_pcm_t *pcm, snd_pcm_hw_params_t *params, unsigned int val)
{
    using SetChannelsFn = int (*)(snd_pcm_t *, snd_pcm_hw_params_t *, unsigned int);
    static auto real = reinterpret_cast<SetChannelsFn>(nextSymbol("snd_pcm_hw_params_set_channels", "ALSA_0.9"));
    if (jackStream(pcm))
        return jackSetChannels(pcm, params, val);
    if (calledFromLive(__builtin_return_address(0))) {
        for (const SharedPcm &shared : sharedPcms) {
            if (shared.pcm != pcm || val <= shared.channels)
                continue;
            menufixTrace("alsaaudio: %d channels instead of %d\n", shared.channels, val);
            val = shared.channels;
            break;
        }
    }
    return real(pcm, params, val);
}

// Live 11 knows no float format: the JACK PCM's FLOAT_LE, converted by jackaudio.cpp, is
// S32_LE to Live. Live references this as ALSA_0.9.0rc4 (shim/qt5.map).
extern "C" int snd_pcm_hw_params_get_format(const snd_pcm_hw_params_t *params, snd_pcm_format_t *val)
{
    using GetFormatFn = int (*)(const snd_pcm_hw_params_t *, snd_pcm_format_t *);
    static auto real = reinterpret_cast<GetFormatFn>(nextSymbol("snd_pcm_hw_params_get_format", "ALSA_0.9.0rc4"));
    const int result = real(params, val);
    if (result >= 0 && *val == SND_PCM_FORMAT_FLOAT_LE && liveBuild() == LiveBuild::Live11_3_43
        && calledFromLive(__builtin_return_address(0)))
        *val = SND_PCM_FORMAT_S32_LE;
    return result;
}

// Live 11 never sets a channel count (it imports neither snd_pcm_hw_params_set_channels nor
// get_channels_max; the Push 3 build had one fixed device). snd_pcm_hw_params then installs
// the minimum: the pipewire PCM (1..128) gave a mono stream, Live's left channel only, with
// its output config listing one channel. Called from jackaudio.cpp's snd_pcm_hw_params before
// the install, this sets what Live 12 sets: the maximum, capped for the shared PCMs as in
// snd_pcm_hw_params_set_channels above. The JACK PCM has one count (its ports) already.
void setLive11Channels(snd_pcm_t *pcm, snd_pcm_hw_params_t *params, const void *returnAddress)
{
    if (liveBuild() != LiveBuild::Live11_3_43 || jackStream(pcm) || !calledFromLive(returnAddress))
        return;
    using SetChannelsFn = int (*)(snd_pcm_t *, snd_pcm_hw_params_t *, unsigned int);
    static auto real = reinterpret_cast<SetChannelsFn>(nextSymbol("snd_pcm_hw_params_set_channels", "ALSA_0.9"));
    unsigned channels = 0;
    if (snd_pcm_hw_params_get_channels_max(params, &channels) < 0 || !channels)
        return;
    for (const SharedPcm &shared : sharedPcms)
        if (shared.pcm == pcm && shared.channels && shared.channels < channels)
            channels = shared.channels;
    const int result = real(pcm, params, channels);
    menufixTrace(result == 0 ? "alsaaudio: %d channels (Live 11)\n" : "alsaaudio: setting %d channels failed (%d)\n", channels, result);
}

// Live's ALSA device reports its output latency as 2 periods (0x2d64230: 2 x period /
// rate, vtable 0x9550638 slot +0x248); the outer latency figures and the offset that keeps
// recordings in place are built on it. With the buffer floor below, the real output
// latency is longer by the extra ring frames, measured exactly (+7.8 ms at a forced 512
// against 128). The slot is pointed at outputLatency(), which adds that extra while the
// floor is active on the playback stream (0 otherwise), so Live's Output and Overall
// Latency and its recording offset follow the real buffer. Checked: the function's first
// bytes and the slot's value; another build leaves it off.
namespace {
std::atomic<float> extraOutputMs {0.0f};
using LatencyFn = float (*)(void *);
LatencyFn originalOutputLatency;
float outputLatency(void *device)
{
    return originalOutputLatency(device) + extraOutputMs.load(std::memory_order_relaxed);
}
} // namespace

__attribute__((constructor)) static void installOutputLatencyHook()
{
    // Live 12.4.7: vtable 0x9550638 slot +0x248 -> 0x2d64230. Live 11.3.43: AAlsaAudioDevice
    // vtable 0x7c80fd0, same slot -> 0x25ddc90 (the same 2 x period / rate in ms, without 12's
    // first test).
    const bool live11 = liveBuild() == LiveBuild::Live11_3_43;
    if (liveBuild() != LiveBuild::Live12_4_7 && !live11)
        return;
    const uintptr_t kSlot = liveAddress(0x9550880, 0x7c81218), kFunction = liveAddress(0x2d64230, 0x25ddc90);
    static const unsigned char prologue12[] = {0x53, 0x48, 0x83, 0xec, 0x10, 0x48, 0x89, 0xfb, 0x48, 0x8b, 0x07, 0xff, 0x90, 0x00, 0x02, 0x00, 0x00};
    static const unsigned char prologue11[] = {0x50, 0x8b, 0x87, 0x00, 0x01, 0x00, 0x00, 0x01, 0xc0, 0xc5, 0xfa, 0x2a, 0xc0, 0xc5, 0xfa, 0x59, 0x05};
    const unsigned char *prologue = live11 ? prologue11 : prologue12;
    uintptr_t base = 0;
    dl_iterate_phdr([](dl_phdr_info *info, size_t, void *out) { *static_cast<uintptr_t *>(out) = info->dlpi_addr; return 1; }, &base);
    auto *slot = reinterpret_cast<uintptr_t *>(base + kSlot);
    if (!base || memcmp(reinterpret_cast<void *>(base + kFunction), prologue, sizeof prologue12) || *slot != base + kFunction) {
        menufixTrace("alsaaudio: output latency hook: not this Live build, off\n");
        return;
    }
    const long pageSize = sysconf(_SC_PAGESIZE);
    void *page = reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(slot) & ~uintptr_t(pageSize - 1));
    if (mprotect(page, pageSize, PROT_READ | PROT_WRITE) != 0)
        return;
    originalOutputLatency = reinterpret_cast<LatencyFn>(*slot);
    *slot = reinterpret_cast<uintptr_t>(&outputLatency);
    mprotect(page, pageSize, PROT_READ);
}

// Live asks for a buffer of 3 periods (0x2d62683). Through pipewire-alsa the
// round trip is then one period longer than the latency Live reports and
// compensates recordings with (4 periods measured, 3 reported). The shared PCM
// gets 2 periods: the round trip matches Live's figure (8.00 ms at 128) and is
// one period shorter, with no more dropouts in the tests. LIVE_ALSA_PERIODS=<n>
// sets another count (3: Live's own). Periods under 64 frames keep 3: PipeWire
// runs 64-frame cycles then, which 2 periods of 32 cannot cover.
extern "C" int snd_pcm_hw_params_set_buffer_size(snd_pcm_t *pcm, snd_pcm_hw_params_t *params, snd_pcm_uframes_t val)
{
    using SetBufferFn = int (*)(snd_pcm_t *, snd_pcm_hw_params_t *, snd_pcm_uframes_t);
    static auto real = reinterpret_cast<SetBufferFn>(nextSymbol("snd_pcm_hw_params_set_buffer_size", "ALSA_0.9"));
    const snd_pcm_uframes_t period = val / 3; // Live's period: it asks for 3
    int periods = getenv("LIVE_ALSA_PERIODS") ? atoi(getenv("LIVE_ALSA_PERIODS")) : 2;
    // JACK capture: 4 periods of room. Its latency does not grow with it: jackaudio.cpp's
    // alignCapture keeps Live on the previous cycle's input; the room only absorbs a late Live.
    if (snd_pcm_stream(pcm) == SND_PCM_STREAM_CAPTURE && jackStream(pcm) && !getenv("LIVE_JACK_CAPTURE_PERIODS"))
        periods = 4;
    else if (snd_pcm_stream(pcm) == SND_PCM_STREAM_CAPTURE && jackStream(pcm))
        periods = atoi(getenv("LIVE_JACK_CAPTURE_PERIODS"));
    if (periods >= 2 && periods != 3 && val / 3 >= 64 && calledFromLive(__builtin_return_address(0))) {
        for (const SharedPcm &shared : sharedPcms) {
            if (shared.pcm != pcm)
                continue;
            val = val / 3 * periods;
            menufixTrace("alsaaudio: buffer %d frames\n", int(val));
            break;
        }
    }
    // Buffer floor (pwgraph.cpp): when PipeWire's cycle cannot follow Live's period
    // (clock.force-quantum set, or clock.min-quantum above the period), the ring must
    // hold a whole cycle plus a period, in whole periods (512 against 128: 640 frames).
    // Live's period, so its CPU use, stays as set. Live computes its latency from its own
    // buffer setting: outputLatency() above adds the extra, so recordings stay in place.
    // LIVE_ALSA_MINRING=<frames> sets the floor (0: none).
    if (calledFromLive(__builtin_return_address(0))) {
        for (const SharedPcm &shared : sharedPcms) {
            if (shared.pcm != pcm)
                continue;
            snd_pcm_uframes_t floorFrames = 0;
            unsigned cycle = 0;
            if (const char *ring = getenv("LIVE_ALSA_MINRING")) {
                floorFrames = strtoul(ring, nullptr, 10);
            } else if (period) {
                unsigned force = 0, minimum = 0, graphRate = 0;
                if (pipewireClock(force, minimum, graphRate)) {
                    cycle = force ? force : minimum;
                    unsigned rate = 0;
                    int dir = 0;
                    if (cycle && graphRate && snd_pcm_hw_params_get_rate(params, &rate, &dir) == 0 && rate && rate != graphRate)
                        cycle = unsigned((uint64_t(cycle) * rate + graphRate - 1) / graphRate); // frames at the stream's rate
                    if (cycle > period)
                        floorFrames = (cycle + 2 * period - 1) / period * period;
                }
            }
            const bool playback = snd_pcm_stream(pcm) == SND_PCM_STREAM_PLAYBACK;
            if (floorFrames && val < floorFrames) {
                unsigned rate = 0;
                int dir = 0;
                const unsigned extra = unsigned(floorFrames - val);
                const float ms = snd_pcm_hw_params_get_rate(params, &rate, &dir) == 0 && rate ? extra * 1000.0f / rate : 0.0f;
                if (playback)
                    extraOutputMs.store(ms, std::memory_order_relaxed);
                menufixTrace("alsaaudio: PipeWire cycle %d frames > period: ring %d frames", int(cycle), int(floorFrames));
                menufixTrace(" instead of %d, output latency +%d us\n", int(val), int(ms * 1000));
                val = floorFrames;
            } else if (playback) {
                extraOutputMs.store(0.0f, std::memory_order_relaxed);
            }
            break;
        }
    }
    return real(pcm, params, val);
}
