// SPDX-License-Identifier: AGPL-3.0-only
#include "../../src/app/juce/spectrogram_window.cpp" // Exercise the private production spectrogram without opening a window.
#include <iostream>
#include <numbers>
#include <stdexcept>
#include <chrono>
using namespace mgstc;
namespace s = app::spectrum;
static void require(bool condition, const char* text) { if (!condition) throw std::runtime_error(text); }
static void requireNear(double actual, double expected, double tolerance, const char* text) {
    if (std::abs(actual - expected) > tolerance) {
        std::cerr << text << ": " << actual << " expected " << expected << "\n";
        throw std::runtime_error(text);
    }
}
static void chipWrite(engine::ChipRack& a, engine::ChipRack& b, engine::ChipId chip,
                      std::uint8_t port, std::uint8_t address, std::uint8_t value) {
    const engine::RegisterWrite w{.chip=chip, .port=port, .address=address, .value=value};
    require(a.apply(w) && b.apply(w), "chip write");
}
static void capturePreservesAudio() {
    auto a = std::make_unique<engine::ChipRack>(), b = std::make_unique<engine::ChipRack>();
    for (std::uint8_t ch = 0; ch < 3; ++ch) {
        chipWrite(*a,*b,engine::ChipId::Psg,0,static_cast<std::uint8_t>(ch*2),static_cast<std::uint8_t>(100+ch*23));
        chipWrite(*a,*b,engine::ChipId::Psg,0,static_cast<std::uint8_t>(8+ch),8);
    }
    chipWrite(*a,*b,engine::ChipId::Psg,0,7,0x38);
    for (std::uint8_t i = 0; i < 128; ++i)
        chipWrite(*a,*b,engine::ChipId::Scc,0,i,
                  static_cast<std::uint8_t>(static_cast<std::int8_t>(std::sin(i*2.0*std::numbers::pi/32)*80)));
    for (std::uint8_t ch = 0; ch < 5; ++ch) {
        chipWrite(*a,*b,engine::ChipId::Scc,1,static_cast<std::uint8_t>(ch*2),static_cast<std::uint8_t>(100+ch*13));
        chipWrite(*a,*b,engine::ChipId::Scc,2,ch,4);
    }
    chipWrite(*a,*b,engine::ChipId::Scc,3,0,31);
    for (std::uint8_t ch = 0; ch < 9; ++ch) {
        chipWrite(*a,*b,engine::ChipId::Opll,0,static_cast<std::uint8_t>(0x30+ch),0x1a);
        chipWrite(*a,*b,engine::ChipId::Opll,0,static_cast<std::uint8_t>(0x10+ch),static_cast<std::uint8_t>(120+ch*10));
        chipWrite(*a,*b,engine::ChipId::Opll,0,static_cast<std::uint8_t>(0x20+ch),0x17);
    }
    for (int i = 0; i < 12000; ++i) {
        if (i == 6000) chipWrite(*a,*b,engine::ChipId::Opll,0,0x0e,0x3f);
        const auto plain = a->renderSample();
        const auto tapped = b->renderSample(i >= 1000);
        require(plain.psg == tapped.psg && plain.scc == tapped.scc && plain.opll == tapped.opll,
                "capture changed audible PCM");
        if (i >= 1100) {
            std::array<double,3> sums{};
            for (std::size_t ch=0;ch<22;++ch) sums[engine::spectrumSource(ch)] += tapped.channels[ch];
            requireNear(sums[0],tapped.psg,1e-6,"PSG resampled contribution sum");
            requireNear(sums[1],tapped.scc,1e-6,"SCC contribution sum");
            requireNear(sums[2],tapped.opll,1e-6,"OPLL melody/rhythm contribution sum");
        }
    }
}
static void captureBlocksAndGain() {
    auto host=std::make_unique<engine::RealtimeEngineHost>();
    host->setSpectrumCaptureEnabled(true);
    host->setSpectrumOutputGain(0.5F);
    std::vector<float> audio(4096*2);
    require(host->render(audio).ok(),"large render");
    auto block=std::make_unique<engine::SpectrumCaptureBlock>();
    int count=0;
    while(host->pollSpectrumCapture(*block)) {
        require(block->first_sample == static_cast<std::uint64_t>(count)*512,"lost block within one render");
        requireNear(block->output_gain[0],0.5,0,"captured output volume");
        ++count;
    }
    require(count==8,"all completed blocks must be delivered");
    host->setSpectrumCaptureEnabled(false);
    require(host->render(audio).ok(),"disabled render");
    require(!host->pollSpectrumCapture(*block),"capture not opt in");
    host->setSpectrumCaptureEnabled(true);
    require(host->render(audio).ok(),"resumed render");
    require(host->pollSpectrumCapture(*block) && block->first_sample==8192,"resume compressed time");
}
static std::unique_ptr<s::Frame> signal(s::Analyzer& analyzer, std::uint64_t context,
                                      std::uint64_t epoch, double frequency, bool inverted=false,
                                      std::uint64_t start=0) {
    auto b=std::make_unique<engine::SpectrumCaptureBlock>();
    auto result=std::make_unique<s::Frame>();
    b->context=context; b->pcm_epoch=epoch;
    for(std::size_t block=0;block<12;++block) {
        b->first_sample=start+block*512;
        for(std::size_t i=0;i<512;++i) {
            const float v=static_cast<float>(0.125*std::sin(2*std::numbers::pi*frequency*(b->first_sample+i)/s::sampleRate));
            b->channels[0][i]=v; b->channels[1][i]=inverted ? -v : v;
            b->sources[0][i]=b->channels[0][i]+b->channels[1][i];
            b->mixed[i]=b->sources[0][i];
            b->output_gain[i]=1;
            b->notes[i]={60,0,true};
        }
        analyzer.processBlock(*b);
        while(analyzer.pollFrame(*result)) {}
    }
    return result;
}
static const s::Peak& strongest(const s::Trace& trace) {
    require(trace.peak_count>0,"expected peak");
    return *std::max_element(trace.peaks.begin(),trace.peaks.begin()+trace.peak_count,
                            [](auto a,auto b){return a.db<b.db;});
}
static void signalsAndTiming() {
    auto host=std::make_unique<engine::RealtimeEngineHost>();
    auto analyzer=std::make_unique<s::Analyzer>(*host);
    auto f=signal(*analyzer,15,1,750);
    requireNear(strongest(f->mixed).frequency,750,0.01,"bin centered frequency");
    requireNear(strongest(f->mixed).db,20*std::log10(0.25),0.01,"Hann peak amplitude dBFS");
    requireNear(strongest(f->mixed).db-strongest(f->channel[0]).db,20*std::log10(2),0.03,"coherent sum +6.02 dB");
    require(f->info.sample==6144-1024,"FFT window center timestamp");
    auto cancelled=signal(*analyzer,15,2,750,true,8192);
    require(!cancelled->mixed.audible,"opposite phase must cancel mixed spectrum");
    require(cancelled->channel[0].audible && cancelled->channel[1].audible,"individual opposite-phase traces remain");
    auto fractional=signal(*analyzer,15,3,223,false,16384);
    requireNear(strongest(fractional->mixed).frequency,223,0.6,"sub-bin peak frequency");
    requireNear(strongest(fractional->mixed).frequency/220,223.0/220,0.003,"non-integer guide ratio");
    s::Model model;
    model.ingest(*f); model.ingest(*cancelled);
    require(!model.live.nearest(8192),"missing audio must not be replaced by nearest distant frame");
    std::cout<<"analysis maximum ms (representative signal): "<<analyzer->maximumAnalysisMs()<<"\n";
}
static void historyAndSnapshots() {
    s::Model model;
    auto f=std::make_unique<s::Frame>();
    f->info.context=15;
    for(std::uint64_t i=1;i<=700;++i) {
        f->info.id=i; f->info.sample=i*512;
        f->mixed.power[20]=static_cast<float>(i)*1e-6F;
        model.ingest(*f);
    }
    require(model.live.size()==s::historyCapacity,"history must remain bounded");
    requireNear(model.availableSeconds(),5,0,"five seconds retained");
    const auto* older=model.live.nearest(model.front()->info.sample-5*s::sampleRate);
    require(older && older->info.id==231,"nearest five-second frame");
    model.setPaused(true);
    const auto frozen_id=model.front()->info.id;
    const auto frozen_value=model.front()->mixed.power[20];
    model.selected_seconds=1;
    require(model.front() && !model.channelFrame(),"past selection must hide present channels");
    model.hold();
    const auto ref_id=model.reference->info.id;
    model.selected_seconds=0;
    for(std::uint64_t i=701;i<=1500;++i) {
        f->info.id=i; f->info.sample=i*512; f->mixed.power[20]=0;
        model.ingest(*f);
    }
    require(model.front()->info.id==frozen_id && model.front()->mixed.power[20]==frozen_value,
            "paused history changed after live ring overwrite");
    require(model.reference->info.id==ref_id,"comparison replaced by new audio");
    model.setPaused(false);
    require(model.front()->info.id==1500,"resume must jump to latest");
    model.setPaused(true);
    f->info.context=23; ++f->info.id; f->info.sample+=512; model.ingest(*f);
    require(!model.paused && model.live.size()==1 && model.reference->info.id==ref_id,"context transition");
}


static double benchmarkPaint(juce::Component& component) {
    // Warm font/glyph creation separately from steady 30 Hz drawing.
    for(int i=0;i<3;++i)
        static_cast<void>(component.createComponentSnapshot(component.getLocalBounds(),true,1.0F,juce::SoftwareImageType{}));
    const auto started=juce::Time::getMillisecondCounterHiRes();
    for(int i=0;i<16;++i)
        static_cast<void>(component.createComponentSnapshot(component.getLocalBounds(),true,1.0F,juce::SoftwareImageType{}));
    return (juce::Time::getMillisecondCounterHiRes()-started)/16;
}
static void spectrogramGaps() {
    s::GuideState guide;
    for(double zoom:{0.25,1.0,4.0}) {
        app::SpectrogramDisplay fast(guide), reference(guide);
        for(auto* display:{&fast,&reference}) {
            display->setVisible(true); display->setBounds(0,0,1200,700);
            display->setTimeScale(zoom);
        }
        s::Column column; column.context=15; column.guide_note=60;
        for(std::size_t bin=0;bin<256;++bin) column.psg[bin]=static_cast<std::uint8_t>(bin);
        for(std::uint64_t i=1;i<=1400;++i) {
            column.sequence=i; column.sample_position=i*512;
            fast.appendColumn(column); reference.appendColumn(column);
        }
        for(std::uint64_t gap:{4,8,5000}) {
            const auto start=column.sequence, finish=start+gap;
            const auto started=juce::Time::getMillisecondCounterHiRes();
            column.sequence=finish; column.sample_position=finish*512; fast.appendColumn(column);
            std::cout<<"spectrogram gap update ms, zoom "<<zoom<<", gap "<<gap<<": "
                     <<(juce::Time::getMillisecondCounterHiRes()-started)<<"\n";
            s::Column empty; empty.context=15; empty.guide_note=60;
            for(auto i=start+1;i<finish;++i) {
                empty.sequence=i; empty.sample_position=i*512; reference.appendColumn(empty);
            }
            reference.appendColumn(column);
            const auto a=fast.createComponentSnapshot(fast.getLocalBounds(),true,1.0F,juce::SoftwareImageType{});
            const auto b=reference.createComponentSnapshot(reference.getLocalBounds(),true,1.0F,juce::SoftwareImageType{});
            for(int y=0;y<a.getHeight();++y) for(int x=0;x<a.getWidth();++x)
                require(a.getPixelAt(x,y)==b.getPixelAt(x,y),"gap pixels differ from explicit empty time");
        }
        if(zoom==1.0) std::cout<<"spectrogram warm paint ms, 1200x700: "<<benchmarkPaint(fast)<<"\n";
    }
}

static void maximumChannelsAndGaps() {
    auto host=std::make_unique<engine::RealtimeEngineHost>();
    auto analyzer=std::make_unique<s::Analyzer>(*host);
    auto b=std::make_unique<engine::SpectrumCaptureBlock>();
    auto f=std::make_unique<s::Frame>();
    b->context=15; b->pcm_epoch=1;
    s::Model model;
    std::vector<double> block_times;
    for(std::uint64_t block=0;block<480;++block) {
        b->first_sample=block*512;
        for(std::size_t i=0;i<512;++i) {
            const double t=(b->first_sample+i)/s::sampleRate;
            for(auto& source:b->sources) source[i]=0;
            b->mixed[i]=0; b->output_gain[i]=1;
            for(std::size_t ch=0;ch<s::channels;++ch) {
                const auto value=static_cast<float>(0.01*std::sin(2*std::numbers::pi*(120+ch*173)*t));
                b->channels[ch][i]=value;
                b->sources[engine::spectrumSource(ch)][i]+=value; b->mixed[i]+=value;
            }
        }
        const auto started=juce::Time::getMillisecondCounterHiRes();
        analyzer->processBlock(*b);
        block_times.push_back(juce::Time::getMillisecondCounterHiRes()-started);
        while(analyzer->pollFrame(*f)) model.ingest(*f);
    }
    std::sort(block_times.begin(),block_times.end());
    std::cout<<"22-channel processing median/p95 ms: "<<block_times[block_times.size()/2]<<" / "
             <<block_times[block_times.size()*95/100]<<"\n";
    for(const auto& channel:f->channel) require(channel.audible,"missing active channel");
    require(analyzer->droppedOutput()==0,"output loss while keeping up");
    std::cout<<"analysis maximum ms, 22 active channels: "<<analyzer->maximumAnalysisMs()<<"\n";
    s::GuideState guide;
    s::Display display(model,guide,{juce::Colour(0xffb990ff),juce::Colour(0xff53e3a6),juce::Colour(0xffffa75e)});
    display.setVisible(true); display.setBounds(0,0,1200,700);
    model.setPaused(true); model.hold();
    for(double seconds:{0.0,1.0,5.0}) {
        display.history_seconds=seconds; display.refresh();
        std::cout<<"22-channel + comparison warm paint average ms, history "<<seconds<<": "
                 <<benchmarkPaint(display)<<"\n";
    }
    display.history_seconds=0;
    for(int variant=0;variant<3;++variant) {
        display.channel_visible.fill(variant==0);
        model.reference_visible=variant!=2;
        display.refresh();
        const auto started=juce::Time::getMillisecondCounterHiRes();
        for(int i=0;i<12;++i) static_cast<void>(display.createComponentSnapshot(
            display.getLocalBounds(),true,1.0F,juce::SoftwareImageType{}));
        std::cout<<"paint profile "<<variant<<" (all, mix+ref, mix): "
                 <<(juce::Time::getMillisecondCounterHiRes()-started)/12<<"\n";
    }
    const auto individual_ffts=analyzer->channelFftCount();
    require(individual_ffts>0,"individual FFT instrumentation");
    const auto last_sample=f->info.sample;
    b->channels_valid.fill(false);
    const auto gram_start=juce::Time::getMillisecondCounterHiRes();
    for(int block=0;block<120;++block) {
        b->first_sample+=512;
        analyzer->processBlock(*b); require(analyzer->pollFrame(*f),"mode switch reset common FFT");
        require(!f->channels_available,"hidden individual channels remained live");
    }
    std::cout<<"spectrogram mode process average ms/block: "
             <<(juce::Time::getMillisecondCounterHiRes()-gram_start)/120<<"\n";
    require(analyzer->channelFftCount()==individual_ffts,"spectrogram mode ran individual FFT");
    require(f->info.sample==last_sample+120*512,"mode switch broke sample time");
    b->channels_valid.fill(true);
    for(int block=0;block<4;++block) {
        b->first_sample+=512; analyzer->processBlock(*b);
        require(analyzer->pollFrame(*f),"individual warmup reset mixed FFT");
        require(f->channels_available==(block==3),"individual window warmup");
    }
    require(analyzer->channelFftCount()>individual_ffts,"individual FFT failed to resume");
    // A discontinuity starts a fresh complete FFT window, without filling the gap.
    b->first_sample+=512*8;
    analyzer->processBlock(*b);
    require(!analyzer->pollFrame(*f),"FFT bridged missing PCM");
    // Logical UI layer aggregation happens in PCM, so opposite physical voices cancel.
    b->pcm_epoch=2; b->channel_map[1]=0;
    for(auto& channel:b->channels) channel.fill(0);
    for(auto& source:b->sources) source.fill(0);
    b->mixed.fill(0);
    for(int block=0;block<12;++block) {
        b->first_sample+=512;
        for(std::size_t i=0;i<512;++i) {
            const float x=static_cast<float>(0.1*std::sin(2*std::numbers::pi*750*(b->first_sample+i)/s::sampleRate));
            b->channels[0][i]=x; b->channels[1][i]=-x;
        }
        analyzer->processBlock(*b); while(analyzer->pollFrame(*f)) {}
    }
    require(!f->channel[0].audible && !f->channel[1].audible,"UI voice mapping must sum before FFT");
    // Full input queue drops blocks without waiting, and retains sample time.
    host->setSpectrumCaptureEnabled(true);
    std::vector<float> audio(40*512*2);
    require(host->render(audio).ok(),"overload render");
    require(host->spectrumDroppedBlocks()==8,"bounded capture queue");
    int count=0;
    while(host->pollSpectrumCapture(*b)) ++count;
    require(count==32,"capture queue capacity");
    require(host->render(std::span<float>(audio.data(),512*2)).ok(),"render after overload");
    require(host->pollSpectrumCapture(*b) && b->first_sample==40*512,"queue loss compressed sample clock");
}

static void displayAndCache(const juce::File& imageDirectory) {
    auto host=std::make_unique<engine::RealtimeEngineHost>();
    auto analyzer=std::make_unique<s::Analyzer>(*host);
    s::Model model;
    auto b=std::make_unique<engine::SpectrumCaptureBlock>();
    auto f=std::make_unique<s::Frame>();
    b->context=15; b->pcm_epoch=1;
    std::vector<double> block_times;
    for(std::uint64_t block=0;block<480;++block) {
        b->first_sample=block*512;
        for(std::size_t i=0;i<512;++i) {
            const double t=(b->first_sample+i)/s::sampleRate;
            const float x=static_cast<float>((0.08+0.03*std::sin(t*3))*(std::sin(2*std::numbers::pi*220*t)
                +0.5*std::sin(2*std::numbers::pi*660*t)+0.2*std::sin(2*std::numbers::pi*1123*t)));
            b->sources[0][i]=x; b->channels[0][i]=x;
            b->mixed[i]=x; b->output_gain[i]=1; b->notes[i]={57,0,true};
        }
        const auto started=juce::Time::getMillisecondCounterHiRes();
        analyzer->processBlock(*b);
        block_times.push_back(juce::Time::getMillisecondCounterHiRes()-started);
        while(analyzer->pollFrame(*f)) model.ingest(*f);
    }
    s::GuideState guide;
    s::Display display(model,guide,{juce::Colour(0xffb990ff),juce::Colour(0xff53e3a6),juce::Colour(0xffffa75e)});
    display.setVisible(true);
    display.setBounds(0,0,1100,650);
    display.refresh();
    auto builds=display.pathBuilds();
    guide.click(220); display.refresh();
    require(display.pathBuilds()==builds,"guide rebuilt spectral paths");
    display.setBounds(0,0,1200,700);
    require(display.pathBuilds()==builds,"resize rebuilt normalized spectral paths");
    require(display.keyPressed(juce::KeyPress(juce::KeyPress::escapeKey)) && !guide.fixed(),"Esc releases guide");
    model.setPaused(true); display.refresh();
    const auto before=display.createComponentSnapshot(display.getLocalBounds(), true, 1.0F, juce::SoftwareImageType{});
    int coloured_pixels=0;
    // Restrict to the plot, excluding the new text legend.
    for(int y=50;y<450;++y) for(int x=60;x<1000;++x) {
        const auto pixel=before.getPixelAt(x,y);
        if(pixel.getBlue()>pixel.getGreen()+12 && pixel.getRed()>pixel.getGreen()+8) ++coloured_pixels;
    }
    require(coloured_pixels>100,"white mix hid the individual channel trace");
    for(std::uint64_t i=0;i<600;++i) { ++f->info.id; f->info.sample+=512; model.ingest(*f); }
    display.refresh();
    const auto after=display.createComponentSnapshot(display.getLocalBounds(), true, 1.0F, juce::SoftwareImageType{});
    require(before.getWidth()==after.getWidth(),"snapshot dimension");
    for(int y=0;y<before.getHeight();++y)
        for(int x=0;x<before.getWidth();++x)
            require(before.getPixelAt(x,y)==after.getPixelAt(x,y),"frozen view changed as live audio advanced");
    std::array<double,3> lengths{0,1,5};
    for(double length:lengths) {
        display.history_seconds=length; display.refresh();
        std::cout<<"single-channel warm paint average ms, history "<<length<<": "
                 <<benchmarkPaint(display)<<"\n";
        if(imageDirectory!=juce::File{}) {
            require(imageDirectory.createDirectory().wasOk(),"image output directory");
            juce::FileOutputStream output(imageDirectory.getChildFile("spectrum-"+juce::String(length,0)+".png"));
            require(output.openedOk(),"PNG stream");
            juce::PNGImageFormat png;
            require(png.writeImageToStream(display.createComponentSnapshot(display.getLocalBounds(), true, 1.0F, juce::SoftwareImageType{}),output),"PNG write");
        }
    }
    model.hold(); model.selected_seconds=0.43; display.refresh();
    if(imageDirectory!=juce::File{}) {
        juce::FileOutputStream output(imageDirectory.getChildFile("spectrum-comparison.png"));
        juce::PNGImageFormat png;
        require(png.writeImageToStream(display.createComponentSnapshot(display.getLocalBounds(), true, 1.0F, juce::SoftwareImageType{}),output),"comparison PNG");
    }
    std::cout<<"Frame bytes: "<<sizeof(s::Frame)<<"; history spectra bytes (live+stopped): "
             <<2*s::historyCapacity*sizeof(s::HistoryFrame)<<"\n";
}
int main(int argc,char** argv) {
    juce::ScopedJuceInitialiser_GUI gui;
    try {
        capturePreservesAudio(); captureBlocksAndGain(); signalsAndTiming(); historyAndSnapshots(); maximumChannelsAndGaps(); spectrogramGaps();
        displayAndCache(argc>1 ? juce::File(juce::String::fromUTF8(argv[1])) : juce::File{});
        std::cout<<"spectrum tests passed\n";
        return 0;
    } catch(const std::exception& e) { std::cerr<<"spectrum test failed: "<<e.what()<<"\n"; return 1; }
}
