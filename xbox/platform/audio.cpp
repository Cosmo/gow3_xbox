#include "audio.h"
#include <windows.h>
#include <mmreg.h>
#include <xaudio2.h>
#include <winrt/base.h>
#include <atomic>
#include <cstdio>
#include <deque>
#include <mutex>
#include <vector>

namespace {
IXAudio2* engine=nullptr;
IXAudio2MasteringVoice* master=nullptr;
std::mutex engine_mutex;
bool engine_failed=false;
int device_channels=2;
}
struct Gow3UwpAudioStream final : IXAudio2VoiceCallback {
    IXAudio2SourceVoice* voice=nullptr;
    std::mutex mutex;
    std::deque<std::vector<unsigned char>> buffers;
    std::atomic<int> queued{0};
    int bytes_per_second=0;
    std::atomic<bool> failed{false};
    void STDMETHODCALLTYPE OnVoiceProcessingPassStart(UINT32) noexcept override {}
    void STDMETHODCALLTYPE OnVoiceProcessingPassEnd() noexcept override {}
    void STDMETHODCALLTYPE OnStreamEnd() noexcept override {}
    void STDMETHODCALLTYPE OnBufferStart(void*) noexcept override {}
    void STDMETHODCALLTYPE OnLoopEnd(void*) noexcept override {}
    void STDMETHODCALLTYPE OnBufferEnd(void*) noexcept override {
        std::scoped_lock lock(mutex);
        if(!buffers.empty()) {queued-=int(buffers.front().size());buffers.pop_front();}
    }
    void STDMETHODCALLTYPE OnVoiceError(void*,HRESULT error) noexcept override {
        if(!failed.exchange(true)) std::fprintf(stderr,"UWP XAudio2 voice error: 0x%08x\n",unsigned(error));
    }
};
extern "C" int gow3_uwp_audio_init() {
    thread_local bool apartment=false;
    if(!apartment) {
        try { winrt::init_apartment(winrt::apartment_type::multi_threaded); }
        catch(winrt::hresult_error const& e) { if(e.code()!=RPC_E_CHANGED_MODE) return 0; }
        apartment=true;
    }
    std::scoped_lock lock(engine_mutex);
    if(engine||engine_failed) return engine!=nullptr;
    HRESULT result=XAudio2Create(&engine,0,XAUDIO2_DEFAULT_PROCESSOR);
    if(SUCCEEDED(result)) result=engine->CreateMasteringVoice(&master);
    if(FAILED(result)) {
        std::fprintf(stderr,"UWP XAudio2 device unavailable: 0x%08x\n",unsigned(result));
        if(engine) engine->Release();engine=nullptr;engine_failed=true;return 0;
    }
    XAUDIO2_VOICE_DETAILS details{};master->GetVoiceDetails(&details);
    device_channels=int(details.InputChannels);
    std::printf("UWP XAudio2 output: %u Hz, %u channels\n",details.InputSampleRate,details.InputChannels);
    return 1;
}
extern "C" int gow3_uwp_audio_channels() { return device_channels; }
extern "C" Gow3UwpAudioStream* gow3_uwp_audio_open(int channels,int is_float) {
    if(!gow3_uwp_audio_init()) return nullptr;
    auto stream=new Gow3UwpAudioStream;
    WAVEFORMATEXTENSIBLE format{};
    format.Format.wFormatTag=WAVE_FORMAT_EXTENSIBLE;
    format.Format.nChannels=WORD(channels);
    format.Format.nSamplesPerSec=48000;
    format.Format.wBitsPerSample=is_float ? 32 : 16;
    format.Format.nBlockAlign=WORD(channels*format.Format.wBitsPerSample/8);
    format.Format.nAvgBytesPerSec=48000*format.Format.nBlockAlign;
    format.Format.cbSize=sizeof(WAVEFORMATEXTENSIBLE)-sizeof(WAVEFORMATEX);
    format.Samples.wValidBitsPerSample=format.Format.wBitsPerSample;
    constexpr DWORD surround_7_1=0x63f,stereo=3,mono=4;
    format.dwChannelMask=channels==8 ? surround_7_1 : channels==2 ? stereo : mono;
    format.SubFormat={is_float ? 3u : 1u,0,0x10,{0x80,0,0,0xaa,0,0x38,0x9b,0x71}};
    stream->bytes_per_second=int(format.Format.nAvgBytesPerSec);
    HRESULT result=engine->CreateSourceVoice(&stream->voice,&format.Format,0,XAUDIO2_DEFAULT_FREQ_RATIO,stream);
    if(SUCCEEDED(result)) result=stream->voice->Start();
    if(FAILED(result)) {
        std::fprintf(stderr,"UWP XAudio2 stream failed: 0x%08x\n",unsigned(result));
        if(stream->voice) stream->voice->DestroyVoice();delete stream;return nullptr;
    }
    return stream;
}
extern "C" void gow3_uwp_audio_close(Gow3UwpAudioStream* stream) {
    if(!stream) return;
    stream->voice->DestroyVoice();delete stream;
}
extern "C" int gow3_uwp_audio_queued(Gow3UwpAudioStream* stream) { return stream->queued.load(); }
extern "C" void gow3_uwp_audio_put(Gow3UwpAudioStream* stream,const void* data,int bytes) {
    if(bytes<=0||stream->failed.load()) return;
    std::scoped_lock lock(stream->mutex);
    if(stream->queued.load()>stream->bytes_per_second||stream->buffers.size()>=XAUDIO2_MAX_QUEUED_BUFFERS) return;
    auto& storage=stream->buffers.emplace_back(static_cast<const unsigned char*>(data),static_cast<const unsigned char*>(data)+bytes);
    XAUDIO2_BUFFER buffer{};buffer.AudioBytes=UINT32(bytes);buffer.pAudioData=storage.data();
    stream->queued+=bytes;
    const HRESULT result=stream->voice->SubmitSourceBuffer(&buffer);
    if(FAILED(result)) {
        stream->queued-=bytes;stream->buffers.pop_back();
        if(!stream->failed.exchange(true)) std::fprintf(stderr,"UWP XAudio2 submit failed: 0x%08x\n",unsigned(result));
    }
}
