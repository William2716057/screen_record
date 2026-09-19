

//   g++ -O2 -std=c++17 sound.cpp -o sound.exe -lole32
 
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <wrl/client.h>
 
#include <atomic>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
 
using Microsoft::WRL::ComPtr;
 
static void check(HRESULT hr, const char* what) {
    if (FAILED(hr))
        throw std::runtime_error(std::string(what) + " failed (HRESULT 0x" +
                                 [&] { char b[16]; snprintf(b, sizeof b, "%08lX", (unsigned long)hr); return std::string(b); }() + ")");
}
 
struct ComInit {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ~ComInit() { if (SUCCEEDED(hr)) CoUninitialize(); }
};
 
struct CoFree { void operator()(void* p) const { CoTaskMemFree(p); } };
 
static void writeWav(const char* path, const WAVEFORMATEX& fmt, const std::vector<BYTE>& pcm) {
    std::ofstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("Could not create WAV file.");
 
    auto put = [&](const void* p, size_t n) { f.write(static_cast<const char*>(p), (std::streamsize)n); };
 
    const uint32_t fmtSize  = sizeof(WAVEFORMATEX) + fmt.cbSize;
    const uint32_t dataSize = static_cast<uint32_t>(pcm.size());
    const uint32_t riffSize = 4 + (8 + fmtSize) + (8 + dataSize);
 
    put("RIFF", 4); put(&riffSize, 4); put("WAVE", 4);
    put("fmt ", 4); put(&fmtSize, 4);  put(&fmt, fmtSize);
    put("data", 4); put(&dataSize, 4); put(pcm.data(), pcm.size());
 
    if (!f) throw std::runtime_error("Write to WAV file failed.");
}
 
int main() try {
    ComInit com;
    check(com.hr, "CoInitializeEx");
 
    ComPtr<IMMDeviceEnumerator> enumerator;
    ComPtr<IMMDevice> device;
    ComPtr<IAudioClient> client;
    ComPtr<IAudioCaptureClient> capture;
 
    check(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                           IID_PPV_ARGS(&enumerator)), "Create device enumerator");
    check(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device), "Get default playback device");
    check(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                           reinterpret_cast<void**>(client.GetAddressOf())), "Activate audio client");
 
    WAVEFORMATEX* raw = nullptr;
    check(client->GetMixFormat(&raw), "GetMixFormat");
    std::unique_ptr<WAVEFORMATEX, CoFree> format(raw);
 
//    std::cout << "Sample rate: " << format->nSamplesPerSec
//              << "\nChannels:    " << format->nChannels
//              << "\nBits:        " << format->wBitsPerSample
//              << "\nFormat tag:  " << format->wFormatTag << "\n";
 
    check(client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK,
                             10'000'000 /* 1 s buffer */, 0, format.get(), nullptr),
          "Initialize loopback");
    check(client->GetService(IID_PPV_ARGS(&capture)), "Get capture client");
    check(client->Start(), "Start");
 
    std::cout << "\nRecording SYSTEM AUDIO \n"
                 "Press ENTER to stop.\n\n";
 
    std::atomic<bool> stop{false};
    std::thread([&] { std::cin.get(); stop = true; }).detach();
 
    std::vector<BYTE> audio;
    audio.reserve(size_t(format->nAvgBytesPerSec) * 60);
 
    while (!stop) {
        UINT32 packet = 0;
        while (SUCCEEDED(capture->GetNextPacketSize(&packet)) && packet > 0) {
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            if (FAILED(capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) break;
 
            const size_t bytes = size_t(frames) * format->nBlockAlign;
            const size_t old = audio.size();
            audio.resize(old + bytes);                       // new bytes are zero-filled (silence)
            if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT))
                std::memcpy(audio.data() + old, data, bytes);
 
            capture->ReleaseBuffer(frames);
        }
        Sleep(10);
    }
 
    client->Stop();
    writeWav("system_audio.wav", *format, audio);
    std::cout << "Saved " << audio.size() << " bytes to system_audio.wav\n";
    return 0;
}
catch (const std::exception& e) {
    std::cerr << "Error: " << e.what() << "\n";
    return 1;
}
 
