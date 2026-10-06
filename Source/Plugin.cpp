// Ultrasaw - VST3 synthesizer (JUCE 8). Single-file first pass.
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_dsp/juce_dsp.h>
#include <set>
#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>
using namespace juce;

namespace {
constexpr int kNumVoices = 8, kMaxUni = 9, kNumDests = 13;

const StringArray kFilterNames { "Ladder 24", "Ladder 12", "SVF LP 12", "SVF HP 12", "SVF BP 12", "Peak",
                                 "Notch", "LP Acid 24", "LP FM 12", "Chorus-Synth 4P A", "Chorus-Synth 4P B" };
const StringArray kLfoWaves { "Sine", "Triangle", "Saw Up", "Saw Down", "Square", "S&H", "Triple Peak", "Smooth Random" };
const StringArray kDests { "Filter", "Pan", "Amp", "HPF", "Resonance", "Drive", "Spread", "Pitch Saw", "Pitch Pulse",
                           "Pitch Sub", "FM Amount", "Noise Color", "Pitch Env" };
const StringArray kDivs { "4/1", "2/1", "1/1", "1/2", "1/4", "1/8.", "1/8", "1/8T", "1/16.", "1/16", "1/16T", "1/32" };
const float kBeats[12] = { 16.f, 8.f, 4.f, 2.f, 1.f, 0.75f, 0.5f, 1.f / 3.f, 0.375f, 0.25f, 1.f / 6.f, 0.125f };
const float kClassic7[7] = { -1.f, -0.5715f, -0.1774f, 0.f, 0.1810f, 0.5650f, 0.9766f };

struct Chord { const char* name; std::vector<int> iv; };
const std::vector<Chord> kChords = {
    { "Major", { 0, 4, 7 } }, { "Minor", { 0, 3, 7 } }, { "Sus2", { 0, 2, 7 } }, { "Sus4", { 0, 5, 7 } },
    { "Dim", { 0, 3, 6 } }, { "Aug", { 0, 4, 8 } }, { "Maj7", { 0, 4, 7, 11 } }, { "Min7", { 0, 3, 7, 10 } },
    { "Dom7", { 0, 4, 7, 10 } }, { "Dim7", { 0, 3, 6, 9 } }, { "m7b5", { 0, 3, 6, 10 } }, { "mMaj7", { 0, 3, 7, 11 } },
    { "6", { 0, 4, 7, 9 } }, { "m6", { 0, 3, 7, 9 } }, { "Add9", { 0, 4, 7, 14 } }, { "mAdd9", { 0, 3, 7, 14 } },
    { "Maj9", { 0, 4, 7, 11, 14 } }, { "Min9", { 0, 3, 7, 10, 14 } }, { "Dom9", { 0, 4, 7, 10, 14 } },
    { "7sus4", { 0, 5, 7, 10 } }, { "Power", { 0, 7 } }, { "Power+Oct", { 0, 7, 12 } }, { "Octaves", { 0, 12, 24 } },
    { "Min11", { 0, 3, 7, 10, 14, 17 } } };

String destId (int i) { return "lfo_" + kDests[i].toLowerCase().replace (" ", "_"); }

AudioProcessorValueTreeState::ParameterLayout makeLayout()
{
    std::vector<std::unique_ptr<RangedAudioParameter>> v;
    auto F = [&] (const String& id, const String& n, float lo, float hi, float def, float skew = 1.f)
    { v.push_back (std::make_unique<AudioParameterFloat> (ParameterID { id, 1 }, n, NormalisableRange<float> (lo, hi, 0.f, skew), def)); };
    auto I = [&] (const String& id, const String& n, int lo, int hi, int def)
    { v.push_back (std::make_unique<AudioParameterInt> (ParameterID { id, 1 }, n, lo, hi, def)); };
    auto C = [&] (const String& id, const String& n, const StringArray& s, int def)
    { v.push_back (std::make_unique<AudioParameterChoice> (ParameterID { id, 1 }, n, s, def)); };
    auto B = [&] (const String& id, const String& n, bool def)
    { v.push_back (std::make_unique<AudioParameterBool> (ParameterID { id, 1 }, n, def)); };

    // Oscillators
    F ("saw_mix", "Saw Mix", 0, 1, 0.8f);  I ("saw_semi", "Saw Semitone", -24, 24, 0);
    I ("uni_voices", "Unison Voices", 1, 9, 7);  F ("uni_detune", "Unison Detune (cents)", 0, 700, 40, 0.4f);
    F ("uni_blend", "Unison Blend", 0, 1, 0.7f);  B ("classic7", "Classic 7-Voice Mode", false);
    F ("pulse_mix", "Pulse Mix", 0, 1, 0);  I ("pulse_semi", "Pulse Semitone", -24, 24, 0);
    F ("pulse_width", "Pulse Width", 0.05f, 0.95f, 0.5f);  F ("pwm_depth", "PWM Depth", 0, 1, 0);
    F ("pwm_rate", "PWM Rate", 0.05f, 10, 1, 0.5f);  B ("pwm_sync", "PWM Sync (cycles/bar)", false);
    F ("sub_mix", "Sub Mix", 0, 1, 0.3f);  I ("sub_semi", "Sub Semitone", -24, 24, 0);
    C ("sub_shape", "Sub Shape", { "Sine", "Triangle", "Square", "Saw", "Pulse 25%" }, 0);
    F ("noise_mix", "Noise Mix", 0, 1, 0);  F ("noise_color", "Noise Color", 0, 1, 0.5f);
    // Filter
    C ("f_type", "Filter Type", kFilterNames, 0);  F ("cutoff", "Cutoff", 20, 20000, 8000, 0.3f);
    F ("res", "Resonance", 0, 1, 0.2f);  F ("drive", "Drive", 0, 1, 0);  F ("hpf", "HPF", 20, 2000, 20, 0.4f);
    F ("keytrack", "Key Track", 0, 1, 0);  F ("fenv_amt", "Filter Env Amount", -1, 1, 0.4f);
    F ("fm_amt", "FM Amount", 0, 1, 0);
    // LFO
    C ("lfo_wave", "LFO Wave", kLfoWaves, 0);  F ("lfo_rate", "LFO Rate (Hz)", 0.05f, 30, 4, 0.4f);
    B ("lfo_sync", "LFO Tempo Sync", false);  C ("lfo_div", "LFO Division", kDivs, 4);
    F ("lfo_fade", "LFO Fade (+in / -out, s)", -5, 5, 0);  F ("mod_depth", "Mod Wheel LFO Depth", 0, 1, 0.5f);
    for (int i = 0; i < kNumDests; ++i) F (destId (i), "LFO > " + kDests[i], -1, 1, 0);
    // Envelopes
    const char* en[2] = { "ae", "fe" }; const char* nn[2] = { "Amp", "Filter" };
    for (int e = 0; e < 2; ++e)
    {
        String id (en[e]), nm (nn[e]);
        F (id + "_a", nm + " Attack", 0.001f, 10, 0.005f, 0.3f);  F (id + "_d", nm + " Decay", 0.001f, 10, 0.3f, 0.3f);
        F (id + "_s", nm + " Sustain", 0, 1, e == 0 ? 0.8f : 0.4f);  F (id + "_r", nm + " Release", 0.001f, 10, 0.25f, 0.3f);
    }
    F ("pe_a", "Pitch Env Attack", 0.001f, 5, 0.001f, 0.3f);  F ("pe_d", "Pitch Env Decay", 0.001f, 5, 0.1f, 0.3f);
    F ("pe_depth", "Pitch Env Depth (semi)", -48, 48, 0);  C ("pe_target", "Pitch Env Target", { "All", "Saw", "Pulse", "Sub" }, 0);
    // FX
    C ("chorus_mode", "Chorus Mode", { "Off", "I", "II", "I+II" }, 2);
    F ("chorus_mix", "Chorus Mix", 0, 1, 1.f);  B ("bass_boost", "Bass Boost", false);
    F ("dly_time", "Delay Time (ms)", 1, 1000, 350, 0.5f);  F ("dly_fb", "Delay Feedback", 0, 0.95f, 0.35f);
    F ("dly_tone", "Delay Tone", 0, 1, 0.6f);  F ("dly_pp", "Delay Ping-Pong", 0, 1, 1);  F ("dly_mix", "Delay Mix", 0, 1, 0);
    F ("rev_size", "Reverb Size", 0, 1, 0.5f);  F ("rev_mix", "Reverb Mix", 0, 1, 0);  F ("rev_duck", "Reverb Ducking", 0, 1, 0.5f);
    // Play
    StringArray chordNames { "Off" }; for (auto& c : kChords) chordNames.add (c.name);
    C ("chord", "Chord Memory", chordNames, 0);
    C ("voice_mode", "Voice Mode", { "Poly", "Mono" }, 0);  F ("porta_time", "Portamento (s)", 0, 2, 0, 0.4f);
    C ("porta_mode", "Portamento Mode", { "Legato", "Always" }, 0);
    B ("arp_on", "Arp On", false);  C ("arp_div", "Arp Rate", kDivs, 9);  F ("arp_gate", "Arp Gate", 0.05f, 1, 0.8f);
    C ("arp_pat", "Arp Pattern", { "Up", "Down", "Up-Down", "Random" }, 0);  I ("arp_oct", "Arp Octaves", 1, 4, 1);
    F ("width", "Stereo Width", 0, 2, 1);  F ("out_bass", "Output Bass (dB)", -12, 12, 0);
    F ("out_treble", "Output Treble (dB)", -12, 12, 0);  F ("drift", "Vintage Drift (cents)", 0, 100, 0);
    C ("osc_reset", "Osc Reset", { "Random (VCO)", "Zero (DCO)" }, 0);  F ("master", "Master", 0, 1, 0.7f);
    return { v.begin(), v.end() };
}

struct P
{
    float sawMix, sawSemi, uniVoices, uniDetune, uniBlend, pulMix, pulSemi, pw, pwmDepth, pwmRate, subMix, subSemi,
          noiseMix, noiseColor, cutoff, res, drive, hpf, keytrack, fenvAmt, fmAmt, lfoRate, lfoFade, modDepth,
          dest[kNumDests], ae[4], fe[4], peA, peD, peDepth, chMix, dlyTime, dlyFb, dlyTone, dlyPP, dlyMix, revSize,
          revMix, revDuck, portaTime, arpGate, width, outBass, outTreble, drift, master;
    bool classic, pwmSync, lfoSync, bassBoost, arpOn;
    int subShape, ftype, lfoWave, lfoDiv, peTarget, chMode, chord, voiceMode, portaMode, arpDiv, arpPat, arpOct, oscReset;
};

struct Mod { float cut, res, drv, detMul, pan, amp, pSaw, pPul, pSub, fm, nc, penv, pwm; };

float polyBlep (float t, float dt)
{
    if (t < dt) { t /= dt; return t + t - t * t - 1.f; }
    if (t > 1.f - dt) { t = (t - 1.f) / dt; return t * t + t + t + 1.f; }
    return 0.f;
}
float sawBL (float ph, float inc) { return 2.f * ph - 1.f - polyBlep (ph, inc); }
float pulseBL (float ph, float inc, float pw) { float p2 = ph + pw; if (p2 >= 1.f) p2 -= 1.f; return 0.7f * (sawBL (ph, inc) - sawBL (p2, inc)); }

struct Adsr
{
    enum St { Idle, A, D, S, R } st = Idle;
    float v = 0.f;
    void on() { st = A; }
    void off() { if (st != Idle) st = R; }
    float tick (float a, float d, float s, float r, float fs)
    {
        switch (st)
        {
            case A: v += 1.f / (a * fs); if (v >= 1.f) { v = 1.f; st = D; } break;
            case D: v = s + (v - s) * std::exp (-4.6f / (d * fs)); if (std::abs (v - s) < 1e-4f) { v = s; st = S; } break;
            case S: v = s; break;
            case R: v *= std::exp (-4.6f / (r * fs)); if (v < 1e-4f) { v = 0.f; st = Idle; } break;
            default: break;
        }
        return v;
    }
};

struct Filt
{
    float s1 = 0, s2 = 0, ls[4] = { 0, 0, 0, 0 }, y4 = 0, y2 = 0, prevIn = 0;
    float svf (float x, int mode, float fc, float res, float fs)
    {
        const float g = std::tan (MathConstants<float>::pi * fc / fs);
        const float k = 1.41f * (1.f - res) + 0.03f;
        const float a1 = 1.f / (1.f + g * (g + k));
        const float hp = (x - (g + k) * s1 - s2) * a1;
        const float bp = g * hp + s1; s1 = g * hp + bp;
        const float lp = g * bp + s2; s2 = g * bp + lp;
        switch (mode) { case 0: return lp; case 1: return hp; case 2: return bp * k; case 3: return lp + hp; default: return lp - hp; }
    }
    float ladder (float x, float fc, float res, float fs, int poles, bool stageSat, float kmax, float asym, float comp)
    {
        const float g = std::tan (MathConstants<float>::pi * fc / fs), G = g / (1.f + g), k = res * kmax;
        const float fb = poles == 4 ? y4 : y2;
        float in = std::tanh (x - k * fb + asym) - std::tanh (asym);
        for (int i = 0; i < poles; ++i)
        {
            const float xi = (stageSat && i > 0) ? std::tanh (in) : in;
            const float vv = (xi - ls[i]) * G, y = vv + ls[i];
            ls[i] = y + vv; in = y;
        }
        (poles == 4 ? y4 : y2) = in;
        return in * (1.f + k * comp);
    }
    float ladderOS (float x, float fc, float res, float fs, bool stageSat, float kmax, float comp)
    {
        float sum = 0.f;
        for (int i = 1; i <= 4; ++i)
            sum += ladder (prevIn + (x - prevIn) * 0.25f * (float) i, fc, res, fs * 4.f, 4, stageSat, kmax, 0.f, comp);
        prevIn = x;
        return sum * 0.25f;
    }
    float process (float x, int type, float fc, float res, float fs)
    {
        switch (type)
        {
            case 0: return ladder (x, fc, res, fs, 4, false, 4.0f, 0.f, 0.f);
            case 1: return ladder (x, fc, res, fs, 2, false, 2.2f, 0.f, 0.f);
            case 2: return svf (x, 0, fc, res, fs);
            case 3: return svf (x, 1, fc, res, fs);
            case 4: return svf (x, 2, fc, res, fs);
            case 5: return svf (x, 4, fc, res, fs);
            case 6: return svf (x, 3, fc, res, fs);
            case 7: return ladder (x, fc, res, fs, 4, true, 3.7f, 0.2f, 0.f);
            case 8: return svf (x, 0, fc, res, fs);
            case 9: return ladderOS (x, fc, res, fs, true, 4.1f, 0.35f);
            default: return ladderOS (x, fc, res, fs, true, 3.8f, 0.7f);
        }
    }
};

struct Voice
{
    bool active = false, held = false;
    int note = -1, src = -1;
    uint64 age = 0;
    float vel = 0, freq = 440, target = 440, sawPh[kMaxUni] {}, pulPh[kMaxUni] {}, subPh = 0, fmPh = 0, nlp[2] {}, hpS[2] {};
    Adsr ae, fe, pe;
    Filt fl[2];
};

// Juno-60 style BBD chorus. Mode I = slow triangle (0.513 Hz), II = slightly faster (0.863 Hz),
// I+II = fast shallow sine (9.75 Hz). Left/right delays move in opposite directions (the Juno stereo trick).
// The wet path is low-passed like a bucket-brigade line and very gently saturated.
struct Chorus
{
    std::vector<float> buf[2]; int wp = 0, size = 1; double ph = 0; float lp[2][2] { { 0, 0 }, { 0, 0 } };
    void prepare (double fs)
    {
        size = (int) (fs * 0.05) + 8;
        for (auto& b : buf) b.assign ((size_t) size, 0.f);
        wp = 0; ph = 0; for (auto& a : lp) { a[0] = 0.f; a[1] = 0.f; }
    }
    void process (float& l, float& r, int mode, float mix, double fs)
    {
        if (mode == 0) return;
        //                          rate Hz, centre ms, depth ms
        static const float T[4][3] = { { 0, 0, 0 }, { 0.513f, 3.345f, 1.805f }, { 0.863f, 3.345f, 1.805f }, { 9.75f, 3.345f, 0.55f } };
        const auto* t = T[mode];
        ph += t[0] / fs; ph -= std::floor (ph);
        buf[0][(size_t) wp] = l; buf[1][(size_t) wp] = r;
        const float m = mode == 3 ? (float) std::sin (MathConstants<double>::twoPi * ph) : (float) (4.0 * std::abs (ph - 0.5) - 1.0);
        const float a = 1.f - std::exp (-MathConstants<float>::twoPi * 7500.f / (float) fs);
        float wet[2];
        for (int c = 0; c < 2; ++c)
        {
            const float dms = t[1] + t[2] * (c == 0 ? m : -m);
            float rp = (float) wp - dms * 0.001f * (float) fs;
            while (rp < 0.f) rp += (float) size;
            const int i0 = (int) rp % size, i1 = (i0 + 1) % size; const float fr = rp - std::floor (rp);
            float w = buf[c][(size_t) i0] * (1.f - fr) + buf[c][(size_t) i1] * fr;
            lp[c][0] += a * (w - lp[c][0]); lp[c][1] += a * (lp[c][0] - lp[c][1]);
            w = std::tanh (lp[c][1] * 1.3f) / 1.3f;
            wet[c] = w;
        }
        wp = (wp + 1) % size;
        l = l * (1.f - 0.3f * mix) + wet[0] * 0.8f * mix;
        r = r * (1.f - 0.3f * mix) + wet[1] * 0.8f * mix;
    }
};

struct Delay
{
    std::vector<float> b[2]; int size = 1, wp = 0; float lp[2] { 0, 0 }, tSm = 350.f;
    void prepare (double fs) { size = (int) (fs * 1.2) + 4; for (auto& x : b) x.assign ((size_t) size, 0.f); wp = 0; }
    void process (float& l, float& r, const P& p, float fs)
    {
        tSm += (p.dlyTime - tSm) * 0.0005f;
        float rp = (float) wp - tSm * 0.001f * fs;
        while (rp < 0.f) rp += (float) size;
        const int i0 = (int) rp, i1 = (i0 + 1) % size; const float fr = rp - (float) i0;
        const float dl = b[0][(size_t) i0] * (1.f - fr) + b[0][(size_t) i1] * fr;
        const float dr = b[1][(size_t) i0] * (1.f - fr) + b[1][(size_t) i1] * fr;
        const float a = 0.03f + 0.97f * p.dlyTone * p.dlyTone;
        lp[0] += a * (dl - lp[0]); lp[1] += a * (dr - lp[1]);
        const float pp = p.dlyPP;
        b[0][(size_t) wp] = l + p.dlyFb * ((1.f - pp) * lp[0] + pp * lp[1]);
        b[1][(size_t) wp] = r + p.dlyFb * ((1.f - pp) * lp[1] + pp * lp[0]);
        wp = (wp + 1) % size;
        l += dl * p.dlyMix; r += dr * p.dlyMix;
    }
};
} // namespace

//==============================================================================
class UltrasawProcessor : public AudioProcessor
{
public:
    UltrasawProcessor()
        : AudioProcessor (BusesProperties().withOutput ("Output", AudioChannelSet::stereo(), true)),
          apvts (*this, nullptr, "STATE", makeLayout()) {}

    const String getName() const override { return "Ultrasaw"; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 4.0; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const String getProgramName (int) override { return {}; }
    void changeProgramName (int, const String&) override {}
    bool hasEditor() const override { return true; }
    AudioProcessorEditor* createEditor() override;
    AudioProcessorValueTreeState& getApvts() { return apvts; }
    bool isBusesLayoutSupported (const BusesLayout& l) const override { return l.getMainOutputChannelSet() == AudioChannelSet::stereo(); }

    void getStateInformation (MemoryBlock& d) override { if (auto xml = apvts.copyState().createXml()) copyXmlToBinary (*xml, d); }
    void setStateInformation (const void* data, int size) override
    {
        if (auto xml = getXmlFromBinary (data, size))
            if (xml->hasTagName (apvts.state.getType())) apvts.replaceState (ValueTree::fromXml (*xml));
    }

    void prepareToPlay (double sr, int block) override
    {
        fs = (float) sr;
        chorus.prepare (sr); delay.prepare (sr);
        reverb.setSampleRate (sr); reverb.reset();
        wetBuf.setSize (2, block);
        for (auto& v : voices) v = Voice();
        monoStack.clear(); heldNotes.clear(); sustained.clear(); arpNote = -1; lastBass = lastTreble = -99.f;
        for (auto& f : bb) f.reset();
    }
    void releaseResources() override {}

    void processBlock (AudioBuffer<float>& buf, MidiBuffer& midi) override
    {
        ScopedNoDenormals nd;
        const int n = buf.getNumSamples();
        buf.clear();
        P p; load (p);
        double bpm = 120.0, ppq = -1.0; bool playing = false;
        if (auto* ph = getPlayHead())
            if (auto pos = ph->getPosition())
            {
                if (auto b = pos->getBpm()) bpm = *b;
                if (auto q = pos->getPpqPosition()) ppq = *q;
                playing = pos->getIsPlaying();
            }
        cur = &p;
        if (arpWasOn && ! p.arpOn) { if (arpNote >= 0) releaseNote (arpNote); arpNote = -1; heldNotes.clear(); }
        arpWasOn = p.arpOn;

        // output EQ / bass boost coefficients (only when changed)
        if (p.outBass != lastBass || p.outTreble != lastTreble || p.bassBoost != lastBB)
        {
            lastBass = p.outBass; lastTreble = p.outTreble; lastBB = p.bassBoost;
            for (int c = 0; c < 2; ++c)
            {
                eqLo[c].coefficients = dsp::IIR::Coefficients<float>::makeLowShelf (fs, 150.f, 0.7f, Decibels::decibelsToGain (p.outBass));
                eqHi[c].coefficients = dsp::IIR::Coefficients<float>::makeHighShelf (fs, 6000.f, 0.7f, Decibels::decibelsToGain (p.outTreble));
                bb[c].coefficients = dsp::IIR::Coefficients<float>::makeLowShelf (fs, 110.f, 0.7f, p.bassBoost ? 2.0f : 1.0f);
            }
        }

        auto* L = buf.getWritePointer (0); auto* R = buf.getWritePointer (1);
        auto it = midi.begin();
        const float invFs = 1.f / fs;
        const float glideCoef = p.portaTime > 0.f ? 1.f - std::exp (-1.f / (p.portaTime * fs)) : 1.f;

        for (int s = 0; s < n; ++s)
        {
            while (it != midi.end() && (*it).samplePosition <= s) { handleMidi ((*it).getMessage()); ++it; }
            if (p.arpOn) arpTick (p, bpm);

            // --- LFO ---
            const float rateHz = p.lfoSync ? (float) (bpm / 60.0) / kBeats[p.lfoDiv] : p.lfoRate;
            lfoPh += rateHz * invFs;
            if (lfoPh >= 1.f) { lfoPh -= 1.f; shVal = rnd.nextFloat() * 2.f - 1.f; randPrev = randNext; randNext = rnd.nextFloat() * 2.f - 1.f; }
            float lv = 0.f;
            switch (p.lfoWave)
            {
                case 0: lv = std::sin (MathConstants<float>::twoPi * lfoPh); break;
                case 1: lv = 4.f * std::abs (lfoPh - 0.5f) - 1.f; break;
                case 2: lv = 2.f * lfoPh - 1.f; break;
                case 3: lv = 1.f - 2.f * lfoPh; break;
                case 4: lv = lfoPh < 0.5f ? 1.f : -1.f; break;
                case 5: lv = shVal; break;
                case 6: lv = 2.f * std::abs (std::sin (3.f * MathConstants<float>::pi * lfoPh)) - 1.f; break;
                default: lv = randPrev + (randNext - randPrev) * (0.5f - 0.5f * std::cos (MathConstants<float>::pi * lfoPh)); break;
            }
            lfoTime += invFs;
            float fade = 1.f;
            if (p.lfoFade > 0.f) fade = jmin (1.f, lfoTime / p.lfoFade);
            else if (p.lfoFade < 0.f) fade = jmax (0.f, 1.f + lfoTime / p.lfoFade);
            modPh += 5.5f * invFs; if (modPh >= 1.f) modPh -= 1.f;
            const float total = lv * fade + modWheel * p.modDepth * (1.f - 4.f * std::abs (modPh - 0.5f));

            // --- PWM LFO ---
            double pwmPhase;
            if (p.pwmSync && playing && ppq >= 0.0) pwmPhase = std::fmod ((ppq + s * bpm / (60.0 * fs)) / 4.0 * p.pwmRate, 1.0);
            else { pwmFree += p.pwmRate * (p.pwmSync ? (float) bpm / 240.f : 1.f) * invFs; pwmFree -= std::floor (pwmFree); pwmPhase = pwmFree; }

            // --- smoothed / modulated globals ---
            cutLog += (std::log2 (p.cutoff) - cutLog) * 0.002f; resSm += (p.res - resSm) * 0.002f; drvSm += (p.drive - drvSm) * 0.002f;
            auto D = [&] (int i) { return total * p.dest[i]; };
            Mod m;
            m.cut = std::exp2 (cutLog + D (0) * 4.f);
            m.pan = D (1); m.amp = D (2);
            m.res = jlimit (0.f, 1.f, resSm + D (4) * 0.5f);
            m.drv = jlimit (0.f, 1.f, drvSm + D (5) * 0.5f);
            m.detMul = jmax (0.f, 1.f + D (6));
            m.pSaw = D (7) * 12.f; m.pPul = D (8) * 12.f; m.pSub = D (9) * 12.f;
            m.fm = jlimit (0.f, 1.f, p.fmAmt + D (10) * 0.5f); m.nc = jlimit (0.f, 1.f, p.noiseColor + D (11) * 0.5f);
            m.penv = D (12) * 24.f;
            m.pwm = (float) std::sin (MathConstants<double>::twoPi * pwmPhase);
            hpfMod = std::exp2 (D (3) * 3.f);

            float oL = 0.f, oR = 0.f;
            for (auto& v : voices)
            {
                if (! v.active) continue;
                float vl, vr;
                renderVoice (v, p, m, glideCoef, vl, vr);
                oL += vl; oR += vr;
            }

            chorus.process (oL, oR, p.chMode, p.chMix, fs);
            oL = bb[0].processSample (oL); oR = bb[1].processSample (oR);
            if (p.dlyMix > 0.001f) delay.process (oL, oR, p, fs);
            L[s] = oL; R[s] = oR;
        }

        // --- reverb with ducking, output EQ, width, master ---
        wetBuf.setSize (2, n, false, false, true);
        wetBuf.copyFrom (0, 0, buf, 0, 0, n); wetBuf.copyFrom (1, 0, buf, 1, 0, n);
        Reverb::Parameters rp; rp.roomSize = p.revSize; rp.damping = 0.5f; rp.wetLevel = 1.f; rp.dryLevel = 0.f; rp.width = 1.f; rp.freezeMode = 0.f;
        reverb.setParameters (rp);
        if (p.revMix > 0.001f) reverb.processStereo (wetBuf.getWritePointer (0), wetBuf.getWritePointer (1), n);
        else wetBuf.clear();
        const float* wl = wetBuf.getReadPointer (0); const float* wr = wetBuf.getReadPointer (1);
        for (int s = 0; s < n; ++s)
        {
            float l = L[s], r = R[s];
            const float e = jmax (std::abs (l), std::abs (r));
            duckEnv += (e > duckEnv ? 0.01f : 0.0003f) * (e - duckEnv);
            const float wg = p.revMix * (1.f - p.revDuck * jmin (1.f, duckEnv * 4.f));
            l += wl[s] * wg; r += wr[s] * wg;
            l = eqHi[0].processSample (eqLo[0].processSample (l)); r = eqHi[1].processSample (eqLo[1].processSample (r));
            const float mid = 0.5f * (l + r), side = 0.5f * (l - r) * p.width;
            L[s] = jlimit (-4.f, 4.f, (mid + side) * p.master); R[s] = jlimit (-4.f, 4.f, (mid - side) * p.master);
        }
    }

private:
    AudioProcessorValueTreeState apvts;
    float fs = 44100.f;
    Voice voices[kNumVoices];
    Chorus chorus; Delay delay; Reverb reverb; AudioBuffer<float> wetBuf;
    dsp::IIR::Filter<float> bb[2], eqLo[2], eqHi[2];
    float lastBass = -99.f, lastTreble = -99.f; bool lastBB = false;
    Random rnd;
    const P* cur = nullptr;
    uint64 ageCounter = 0;
    float lfoPh = 0, shVal = 0, randPrev = 0, randNext = 0, lfoTime = 0, modPh = 0, modWheel = 0, bend = 0, pwmFree = 0,
          cutLog = 13.f, resSm = 0.2f, drvSm = 0, duckEnv = 0, hpfMod = 1.f, lastVel = 0.8f;
    std::vector<int> monoStack, heldNotes; std::set<int> sustained; bool sustain = false, arpWasOn = false;
    int arpNote = -1, arpIdx = 0; double arpCounter = 1e9;

    void load (P& p)
    {
        auto f = [&] (const char* id) { return apvts.getRawParameterValue (id)->load(); };
        auto i = [&] (const char* id) { return (int) std::lround (f (id)); };
        p.sawMix = f ("saw_mix"); p.sawSemi = f ("saw_semi"); p.uniVoices = f ("uni_voices"); p.uniDetune = f ("uni_detune");
        p.uniBlend = f ("uni_blend"); p.classic = f ("classic7") > 0.5f;
        p.pulMix = f ("pulse_mix"); p.pulSemi = f ("pulse_semi"); p.pw = f ("pulse_width"); p.pwmDepth = f ("pwm_depth");
        p.pwmRate = f ("pwm_rate"); p.pwmSync = f ("pwm_sync") > 0.5f;
        p.subMix = f ("sub_mix"); p.subSemi = f ("sub_semi"); p.subShape = i ("sub_shape");
        p.noiseMix = f ("noise_mix"); p.noiseColor = f ("noise_color");
        p.ftype = i ("f_type"); p.cutoff = f ("cutoff"); p.res = f ("res"); p.drive = f ("drive"); p.hpf = f ("hpf");
        p.keytrack = f ("keytrack"); p.fenvAmt = f ("fenv_amt"); p.fmAmt = f ("fm_amt");
        p.lfoWave = i ("lfo_wave"); p.lfoRate = f ("lfo_rate"); p.lfoSync = f ("lfo_sync") > 0.5f; p.lfoDiv = i ("lfo_div");
        p.lfoFade = f ("lfo_fade"); p.modDepth = f ("mod_depth");
        for (int d = 0; d < kNumDests; ++d) p.dest[d] = apvts.getRawParameterValue (destId (d))->load();
        p.ae[0] = f ("ae_a"); p.ae[1] = f ("ae_d"); p.ae[2] = f ("ae_s"); p.ae[3] = f ("ae_r");
        p.fe[0] = f ("fe_a"); p.fe[1] = f ("fe_d"); p.fe[2] = f ("fe_s"); p.fe[3] = f ("fe_r");
        p.peA = f ("pe_a"); p.peD = f ("pe_d"); p.peDepth = f ("pe_depth"); p.peTarget = i ("pe_target");
        p.chMode = i ("chorus_mode"); p.chMix = f ("chorus_mix"); p.bassBoost = f ("bass_boost") > 0.5f;
        p.dlyTime = f ("dly_time"); p.dlyFb = f ("dly_fb"); p.dlyTone = f ("dly_tone"); p.dlyPP = f ("dly_pp"); p.dlyMix = f ("dly_mix");
        p.revSize = f ("rev_size"); p.revMix = f ("rev_mix"); p.revDuck = f ("rev_duck");
        p.chord = i ("chord"); p.voiceMode = i ("voice_mode"); p.portaTime = f ("porta_time"); p.portaMode = i ("porta_mode");
        p.arpOn = f ("arp_on") > 0.5f; p.arpDiv = i ("arp_div"); p.arpGate = f ("arp_gate"); p.arpPat = i ("arp_pat"); p.arpOct = i ("arp_oct");
        p.width = f ("width"); p.outBass = f ("out_bass"); p.outTreble = f ("out_treble"); p.drift = f ("drift");
        p.oscReset = i ("osc_reset"); p.master = f ("master");
    }

    // ---------- MIDI / voice management ----------
    void handleMidi (const MidiMessage& m)
    {
        if (m.isNoteOn()) noteIn (m.getNoteNumber(), m.getFloatVelocity());
        else if (m.isNoteOff()) noteOffIn (m.getNoteNumber());
        else if (m.isPitchWheel()) bend = (float) (m.getPitchWheelValue() - 8192) / 8192.f;
        else if (m.isController())
        {
            if (m.getControllerNumber() == 1) modWheel = (float) m.getControllerValue() / 127.f;
            else if (m.getControllerNumber() == 64)
            {
                sustain = m.getControllerValue() >= 64;
                if (! sustain) { for (int n : sustained) noteOffNow (n); sustained.clear(); }
            }
            else if (m.getControllerNumber() == 123 || m.isAllNotesOff()) { for (auto& v : voices) { v.ae.off(); v.fe.off(); v.pe.off(); v.held = false; } monoStack.clear(); heldNotes.clear(); }
        }
    }
    void noteIn (int note, float vel)
    {
        lastVel = vel;
        if (cur->arpOn) { if (std::find (heldNotes.begin(), heldNotes.end(), note) == heldNotes.end()) heldNotes.push_back (note); }
        else triggerNote (note, vel);
    }
    void noteOffIn (int note)
    {
        if (sustain) { sustained.insert (note); return; }
        noteOffNow (note);
    }
    void noteOffNow (int note)
    {
        if (cur->arpOn) heldNotes.erase (std::remove (heldNotes.begin(), heldNotes.end(), note), heldNotes.end());
        else releaseNote (note);
    }
    void triggerNote (int note, float vel)
    {
        bool anyActive = false; for (auto& v : voices) anyActive |= v.active;
        if (! anyActive) { lfoTime = 0.f; lfoPh = 0.f; }
        if (cur->voiceMode == 1)
        {
            const bool legato = ! monoStack.empty() && voices[0].active;
            monoStack.push_back (note);
            startVoice (voices[0], note, note, vel, legato);
            return;
        }
        std::vector<int> iv { 0 };
        if (cur->chord > 0) iv = kChords[(size_t) cur->chord - 1].iv;
        for (int o : iv)
        {
            Voice* v = nullptr; uint64 oldest = std::numeric_limits<uint64>::max();
            for (auto& c : voices) { if (! c.active) { v = &c; break; } if (c.age < oldest) { oldest = c.age; v = &c; } }
            startVoice (*v, jlimit (0, 127, note + o), note, vel, false);
        }
    }
    void releaseNote (int note)
    {
        if (cur->voiceMode == 1)
        {
            monoStack.erase (std::remove (monoStack.begin(), monoStack.end(), note), monoStack.end());
            if (! monoStack.empty()) { voices[0].note = monoStack.back(); voices[0].src = monoStack.back(); voices[0].target = mtof (monoStack.back()); }
            else { voices[0].ae.off(); voices[0].fe.off(); voices[0].pe.off(); voices[0].held = false; }
            return;
        }
        for (auto& v : voices) if (v.active && v.held && v.src == note) { v.ae.off(); v.fe.off(); v.pe.off(); v.held = false; }
    }
    static float mtof (int n) { return 440.f * std::exp2 (((float) n - 69.f) / 12.f); }
    void startVoice (Voice& v, int note, int src, float vel, bool legato)
    {
        const bool wasActive = v.active;
        v.note = note; v.src = src; v.target = mtof (note); v.age = ++ageCounter;
        if (legato) { v.held = true; if (cur->portaMode == 0 && cur->portaTime <= 0.f) v.freq = v.target; return; }
        v.vel = vel; v.held = true; v.active = true;
        const bool glide = cur->voiceMode == 1 && cur->portaTime > 0.f && (cur->portaMode == 1 || false);
        if (! (glide && wasActive)) v.freq = v.target;
        if (cur->voiceMode == 1 && cur->portaTime > 0.f && cur->portaMode == 1 && ! wasActive && v.freq < 1.f) v.freq = v.target;
        v.ae.on(); v.fe.on(); v.pe.on();
        for (int i = 0; i < kMaxUni; ++i)
        {
            v.sawPh[i] = cur->oscReset == 0 ? rnd.nextFloat() : 0.f;
            v.pulPh[i] = cur->oscReset == 0 ? rnd.nextFloat() : 0.f;
        }
        v.subPh = 0.f;
        for (auto& f : v.fl) f = Filt();
    }

    // ---------- arpeggiator ----------
    void arpTick (const P& p, double bpm)
    {
        const double stepLen = kBeats[p.arpDiv] * 60.0 / bpm * fs;
        if (heldNotes.empty()) { if (arpNote >= 0) { releaseNote (arpNote); arpNote = -1; } arpCounter = stepLen; arpIdx = 0; return; }
        arpCounter += 1.0;
        if (arpCounter >= stepLen)
        {
            arpCounter = arpCounter > 2.0 * stepLen ? 0.0 : arpCounter - stepLen;
            std::vector<int> base = heldNotes; std::sort (base.begin(), base.end());
            std::vector<int> seq;
            for (int o = 0; o < p.arpOct; ++o) for (int nn : base) seq.push_back (nn + 12 * o);
            const int n = (int) seq.size(); int idx = 0;
            switch (p.arpPat)
            {
                case 0: idx = arpIdx % n; break;
                case 1: idx = n - 1 - arpIdx % n; break;
                case 2: { const int per = jmax (1, 2 * n - 2); const int k = arpIdx % per; idx = k < n ? k : per - k; break; }
                default: idx = rnd.nextInt (n); break;
            }
            ++arpIdx;
            if (arpNote >= 0) releaseNote (arpNote);
            arpNote = jlimit (0, 127, seq[(size_t) idx]);
            triggerNote (arpNote, lastVel);
        }
        if (arpNote >= 0 && arpCounter >= stepLen * p.arpGate) { releaseNote (arpNote); arpNote = -1; }
    }

    // ---------- voice DSP ----------
    void renderVoice (Voice& v, const P& p, const Mod& m, float glideCoef, float& outL, float& outR)
    {
        const float invFs = 1.f / fs;
        if (p.voiceMode == 1) v.freq += (v.target - v.freq) * glideCoef; else v.freq = v.target;
        const float ae = v.ae.tick (p.ae[0], p.ae[1], p.ae[2], p.ae[3], fs);
        const float fe = v.fe.tick (p.fe[0], p.fe[1], p.fe[2], p.fe[3], fs);
        const float pe = v.pe.tick (p.peA, p.peD, 0.f, p.peD, fs);
        if (v.ae.st == Adsr::Idle) { v.active = false; outL = outR = 0.f; return; }

        const float drift = p.drift * 0.01f * ((float) v.note - 60.f) / 60.f;
        const float penvS = pe * (p.peDepth + m.penv);
        auto tgt = [&] (int t) { return (p.peTarget == 0 || p.peTarget == t) ? penvS : 0.f; };
        const float base = v.freq * std::exp2 ((drift + bend * 2.f) / 12.f);
        const float fSaw = base * std::exp2 ((p.sawSemi + m.pSaw + tgt (1)) / 12.f);
        const float fPul = base * std::exp2 ((p.pulSemi + m.pPul + tgt (2)) / 12.f);
        const float fSub = base * 0.5f * std::exp2 ((p.subSemi + m.pSub + tgt (3)) / 12.f);

        const int n = (int) p.uniVoices;
        const float det = p.uniDetune * m.detMul;
        auto uniOff = [&] (int i) { return n == 1 ? 0.f : (p.classic && n == 7 ? kClassic7[i] : (float) i / (float) (n - 1) * 2.f - 1.f); };
        auto runUni = [&] (float* phs, float f, bool pulse, float pw, float& uL, float& uR)
        {
            float sumg2 = 0.f; uL = uR = 0.f;
            for (int i = 0; i < n; ++i)
            {
                const float o = uniOff (i);
                const float inc = jmin (0.45f, f * std::exp2 (o * det / 1200.f) * invFs);
                float& ph = phs[i]; ph += inc; if (ph >= 1.f) ph -= 1.f;
                const float g = std::abs (o) < 0.001f ? 1.f : jmax (0.02f, p.uniBlend);
                const float s = (pulse ? pulseBL (ph, inc, pw) : sawBL (ph, inc)) * g;
                const float pn = o * 0.85f;
                uL += s * std::sqrt (0.5f * (1.f - pn)); uR += s * std::sqrt (0.5f * (1.f + pn));
                sumg2 += g * g;
            }
            const float norm = 1.f / std::sqrt (sumg2);
            uL *= norm; uR *= norm;
        };

        float sL = 0, sR = 0, pL = 0, pR = 0, sub = 0, nL = 0, nR = 0;
        if (p.sawMix > 0.001f) runUni (v.sawPh, fSaw, false, 0.5f, sL, sR);
        if (p.pulMix > 0.001f)
            runUni (v.pulPh, fPul, true, jlimit (0.05f, 0.95f, p.pw + p.pwmDepth * m.pwm * 0.45f), pL, pR);
        if (p.subMix > 0.001f)
        {
            const float inc = jmin (0.45f, fSub * invFs);
            v.subPh += inc; if (v.subPh >= 1.f) v.subPh -= 1.f;
            const float ph = v.subPh;
            switch (p.subShape)
            {
                case 0: sub = std::sin (MathConstants<float>::twoPi * ph); break;
                case 1: sub = 4.f * std::abs (ph - 0.5f) - 1.f; break;
                case 2: sub = pulseBL (ph, inc, 0.5f); break;
                case 3: sub = sawBL (ph, inc); break;
                default: sub = pulseBL (ph, inc, 0.25f); break;
            }
        }
        if (p.noiseMix > 0.001f)
        {
            const float c = 0.02f + 0.98f * m.nc * m.nc, comp = 0.5f * std::sqrt ((2.f - c) / c);
            v.nlp[0] += c * ((rnd.nextFloat() * 2.f - 1.f) - v.nlp[0]); v.nlp[1] += c * ((rnd.nextFloat() * 2.f - 1.f) - v.nlp[1]);
            nL = v.nlp[0] * comp; nR = v.nlp[1] * comp;
        }
        float x[2] = { (sL * p.sawMix + pL * p.pulMix + sub * p.subMix + nL * p.noiseMix) * 0.3f,
                       (sR * p.sawMix + pR * p.pulMix + sub * p.subMix + nR * p.noiseMix) * 0.3f };

        // drive, HPF, filter
        if (m.drv > 0.001f) { const float dg = 1.f + 6.f * m.drv; for (auto& s : x) s = std::tanh (s * dg * 3.f) / 3.f; }
        const float hpfHz = p.hpf * hpfMod;
        if (hpfHz > 21.f)
        {
            const float a = 1.f - std::exp (-MathConstants<float>::twoPi * jmin (hpfHz, 0.4f * fs) * invFs);
            for (int c = 0; c < 2; ++c) { v.hpS[c] += a * (x[c] - v.hpS[c]); x[c] -= v.hpS[c]; }
        }
        float fc = m.cut * std::exp2 (p.fenvAmt * fe * 8.f);
        if (p.keytrack > 0.001f) fc *= std::exp2 (p.keytrack * std::log2 (v.freq / 261.63f));
        if (p.ftype == 8)
        {
            v.fmPh += v.freq * invFs; if (v.fmPh >= 1.f) v.fmPh -= 1.f;
            fc *= std::exp2 (m.fm * 4.f * std::sin (MathConstants<float>::twoPi * v.fmPh));
        }
        fc = jlimit (20.f, 0.45f * fs, fc);
        const float gain = ae * (0.2f + 0.8f * v.vel) * jlimit (0.f, 2.f, 1.f + 0.5f * m.amp);
        float y[2];
        for (int c = 0; c < 2; ++c) y[c] = v.fl[c].process (x[c], p.ftype, fc, m.res, fs) * gain;
        const float pan = jlimit (-1.f, 1.f, m.pan);
        outL = y[0] * (1.f - jmax (0.f, pan)); outR = y[1] * (1.f + jmin (0.f, pan));
    }
};


//==============================================================================
// GUI: Juno-60 / TAL-Pha inspired panel
//==============================================================================
namespace {
const Colour kAccents[4] = { Colour (0xffe8742c), Colour (0xffd23a2e), Colour (0xfff2b632), Colour (0xff3d86c6) };
const Colour kCream (0xfff0e6d2);
const char* kDestShort[kNumDests] = { "Filter", "Pan", "Amp", "HPF", "Reso", "Drive", "Spread", "Saw", "Pulse", "Sub", "FM", "Noise", "PEnv" };
}

class JunoLook : public LookAndFeel_V4
{
public:
    JunoLook()
    {
        setColour (Slider::textBoxTextColourId, Colour (0xffd9d4c7));
        setColour (Slider::textBoxOutlineColourId, Colours::transparentBlack);
        setColour (Slider::textBoxBackgroundColourId, Colours::transparentBlack);
        setColour (ComboBox::backgroundColourId, Colour (0xff141517));
        setColour (ComboBox::textColourId, kCream);
        setColour (ComboBox::outlineColourId, Colour (0xff6a6a70));
        setColour (ComboBox::arrowColourId, Colour (0xffe8742c));
        setColour (PopupMenu::backgroundColourId, Colour (0xff1c1d20));
        setColour (PopupMenu::textColourId, kCream);
        setColour (PopupMenu::highlightedBackgroundColourId, Colour (0xffe8742c));
        setColour (PopupMenu::highlightedTextColourId, Colours::black);
    }
    int getSliderThumbRadius (Slider&) override { return 7; }
    Font getLabelFont (Label& l) override
    {
        if (dynamic_cast<Slider*> (l.getParentComponent()) != nullptr) return Font (FontOptions (10.5f));
        return l.getFont();
    }
    Font getComboBoxFont (ComboBox&) override { return Font (FontOptions (12.f)); }
    void positionComboBoxText (ComboBox& box, Label& label) override
    {
        label.setBounds (4, 1, box.getWidth() - 22, box.getHeight() - 2);
        label.setFont (getComboBoxFont (box));
    }
    void drawLinearSlider (Graphics& g, int x, int y, int w, int h, float pos, float, float, Slider::SliderStyle, Slider& s) override
    {
        const float cx = (float) x + (float) w * 0.5f;
        const float top = (float) y + 6.f, bot = (float) (y + h) - 6.f;
        g.setColour (Colour (0xff09090a)); g.fillRoundedRectangle (cx - 3.f, top, 6.f, bot - top, 3.f);
        g.setColour (Colour (0xff55565b)); g.drawRoundedRectangle (cx - 3.f, top, 6.f, bot - top, 3.f, 1.f);
        g.setColour (Colour (0xffa09b90));
        for (int i = 0; i <= 10; ++i)
        {
            const float ty = top + 2.f + (bot - top - 4.f) * (float) i / 10.f;
            g.drawLine (cx - 15.f, ty, cx - (i % 5 == 0 ? 9.f : 11.f), ty, i % 5 == 0 ? 1.5f : 1.f);
        }
        Rectangle<float> cap (cx - 12.f, pos - 7.f, 24.f, 14.f);
        g.setColour (Colours::black.withAlpha (0.55f)); g.fillRoundedRectangle (cap.translated (1.5f, 2.f), 2.f);
        g.setGradientFill (ColourGradient (Colour (0xfff4f2ec), 0.f, cap.getY(), Colour (0xffb4b1a8), 0.f, cap.getBottom(), false));
        g.fillRoundedRectangle (cap, 2.f);
        g.setColour (s.findColour (Slider::thumbColourId));
        g.fillRect (cap.getX() + 2.f, pos - 1.f, cap.getWidth() - 4.f, 2.f);
        g.setColour (Colour (0xff1c1c1e)); g.drawRoundedRectangle (cap, 2.f, 1.f);
    }
    void drawToggleButton (Graphics& g, ToggleButton& b, bool highlighted, bool) override
    {
        auto r = b.getLocalBounds().toFloat().reduced (1.f);
        const bool on = b.getToggleState();
        g.setColour (on ? Colour (0xff3a3a3e) : Colour (0xff26262a)); g.fillRoundedRectangle (r, 3.f);
        g.setColour (Colour (0xff6a6a70)); g.drawRoundedRectangle (r, 3.f, 1.f);
        auto led = Rectangle<float> (r.getX() + 7.f, r.getCentreY() - 4.f, 8.f, 8.f);
        if (on) { g.setColour (Colour (0xffff5a2a).withAlpha (0.35f)); g.fillEllipse (led.expanded (3.f)); }
        g.setColour (on ? Colour (0xffff5a2a) : Colour (0xff3b1a14)); g.fillEllipse (led);
        g.setColour (highlighted ? Colours::white : kCream);
        g.setFont (Font (FontOptions (12.f, Font::bold)));
        g.drawText (b.getButtonText(), r.withTrimmedLeft (22.f).toNearestInt(), Justification::centredLeft);
    }
    void drawComboBox (Graphics& g, int w, int h, bool, int, int, int, int, ComboBox& box) override
    {
        auto r = Rectangle<float> (0.f, 0.f, (float) w, (float) h).reduced (0.5f);
        g.setColour (box.findColour (ComboBox::backgroundColourId)); g.fillRoundedRectangle (r, 3.f);
        g.setColour (box.findColour (ComboBox::outlineColourId)); g.drawRoundedRectangle (r, 3.f, 1.f);
        Path p; const float ax = (float) w - 11.f, ay = (float) h * 0.5f;
        p.addTriangle (ax - 4.f, ay - 2.f, ax + 4.f, ay - 2.f, ax, ay + 3.f);
        g.setColour (box.findColour (ComboBox::arrowColourId)); g.fillPath (p);
    }
};

class JunoPanel : public Component
{
public:
    static constexpr int headerH = 60;

    explicit JunoPanel (AudioProcessorValueTreeState& a) : apvts (a)
    {
        setLookAndFeel (&look);
        build();
    }
    ~JunoPanel() override { setLookAndFeel (nullptr); }

    // positions everything for a design width W, returns the total design height
    int layoutAll (int W)
    {
        const int margin = 12, gap = 8;
        int y = headerH + gap;
        for (auto& row : rows)
        {
            float sumW = 0.f;
            for (int si : row) sumW += secs[(size_t) si].weight;
            const int avail = W - 2 * margin - gap * ((int) row.size() - 1);
            std::vector<int> widths; int rowH = 0;
            for (int si : row)
            {
                const int w = (int) ((float) avail * secs[(size_t) si].weight / sumW);
                widths.push_back (w);
                rowH = jmax (rowH, neededHeight (secs[(size_t) si], w));
            }
            int x = margin;
            for (size_t k = 0; k < row.size(); ++k)
            {
                auto& s = secs[(size_t) row[k]];
                s.r = Rectangle<int> (x, y, widths[k], rowH);
                placeSection (s);
                x += widths[k] + gap;
            }
            y += rowH + gap;
        }
        return y + 4;
    }

    void paint (Graphics& g) override
    {
        const int W = getWidth();
        g.setGradientFill (ColourGradient (Colour (0xff303034), 0.f, 0.f, Colour (0xff222225), 0.f, (float) getHeight(), false));
        g.fillAll();
        // header
        g.setGradientFill (ColourGradient (Colour (0xff3d3d42), 0.f, 0.f, Colour (0xff1f1f22), 0.f, (float) headerH, false));
        g.fillRect (0, 0, W, headerH);
        g.setColour (Colour (0xfff2b632)); g.fillRect (0, headerH - 12, W, 4);
        g.setColour (Colour (0xffe8742c)); g.fillRect (0, headerH - 8, W, 4);
        g.setColour (Colour (0xffd23a2e)); g.fillRect (0, headerH - 4, W, 4);
        g.setColour (Colours::white);
        g.setFont (Font (FontOptions (30.f, Font::bold)));
        g.drawText ("ULTRASAW", 18, 4, 300, 34, Justification::centredLeft);
        g.setColour (Colour (0xffe8742c));
        g.setFont (Font (FontOptions (10.5f, Font::bold)));
        g.drawText ("POLYPHONIC SYNTHESIZER", 20, 36, 300, 12, Justification::centredLeft);

        for (auto& s : secs)
        {
            const auto r = s.r.toFloat();
            g.setColour (Colour (0xff1d1e21)); g.fillRoundedRectangle (r, 5.f);
            g.setColour (Colour (0xff3d3e44)); g.drawRoundedRectangle (r, 5.f, 1.f);
            g.setColour (s.col); g.fillRect (r.getX() + 8.f, r.getY() + 21.f, r.getWidth() - 16.f, 2.f);
            g.setColour (Colours::white);
            g.setFont (Font (FontOptions (12.5f, Font::bold)));
            g.drawText (s.title, (int) r.getX() + 8, (int) r.getY() + 3, (int) r.getWidth() - 16, 17, Justification::centred);
            g.setColour (Colour (0xffb9b3a5));
            g.setFont (Font (FontOptions (9.5f, Font::bold)));
            for (auto* c : s.sels)
                if (dynamic_cast<ComboBox*> (c) != nullptr)
                    g.drawText (c->getName().toUpperCase(), c->getX(), c->getY() - 12, c->getWidth(), 11, Justification::centredLeft);
        }
    }

private:
    struct Sec
    {
        String title; Colour col; float weight = 1.f; Rectangle<int> r;
        std::vector<Slider*> sl; std::vector<Label*> lb; std::vector<Component*> sels;
    };
    static constexpr int pad = 8, titleH = 26, sliderH = 96, labelH = 16, selH = 36, selGap = 6, selMinW = 112;

    JunoLook look;
    AudioProcessorValueTreeState& apvts;
    std::vector<Sec> secs;
    std::vector<std::vector<int>> rows;
    OwnedArray<Slider> sliders; OwnedArray<Label> labels; OwnedArray<ComboBox> combos; OwnedArray<ToggleButton> toggles;
    std::vector<std::unique_ptr<AudioProcessorValueTreeState::SliderAttachment>> sAtt;
    std::vector<std::unique_ptr<AudioProcessorValueTreeState::ComboBoxAttachment>> cAtt;
    std::vector<std::unique_ptr<AudioProcessorValueTreeState::ButtonAttachment>> bAtt;

    static int colsFor (int w) { return jmax (1, (w - 2 * pad + selGap) / (selMinW + selGap)); }
    static int neededHeight (const Sec& s, int w)
    {
        int h = titleH + pad;
        if (! s.sl.empty()) h += sliderH + labelH + 4;
        const int ns = (int) s.sels.size(), cols = colsFor (w);
        if (ns > 0) h += ((ns + cols - 1) / cols) * (selH + selGap);
        return h;
    }
    void placeSection (Sec& s)
    {
        int y = s.r.getY() + titleH;
        const int n = (int) s.sl.size();
        if (n > 0)
        {
            const int cw = (s.r.getWidth() - 2 * pad) / n;
            for (int i = 0; i < n; ++i)
            {
                const int x = s.r.getX() + pad + i * cw;
                s.sl[(size_t) i]->setTextBoxStyle (Slider::TextBoxBelow, false, jmin (cw, 58), 14);
                s.sl[(size_t) i]->setBounds (x, y, cw, sliderH);
                s.lb[(size_t) i]->setBounds (x, y + sliderH, cw, labelH);
            }
            y += sliderH + labelH + 4;
        }
        const int cols = colsFor (s.r.getWidth());
        const int selW = (s.r.getWidth() - 2 * pad - (cols - 1) * selGap) / cols;
        for (size_t i = 0; i < s.sels.size(); ++i)
        {
            const int cx = s.r.getX() + pad + (int) (i % (size_t) cols) * (selW + selGap);
            const int cy = y + (int) (i / (size_t) cols) * (selH + selGap);
            s.sels[i]->setBounds (cx, cy + 13, selW, selH - 13);
        }
    }

    // ----- builders -----
    void beginRow() { rows.emplace_back(); }
    void section (const String& title, float weight)
    {
        secs.emplace_back();
        auto& s = secs.back();
        s.title = title.toUpperCase(); s.weight = weight; s.col = kAccents[(secs.size() - 1) % 4];
        rows.back().push_back ((int) secs.size() - 1);
    }
    void slider (const String& id, const String& name)
    {
        auto* s = sliders.add (new Slider (Slider::LinearVertical, Slider::TextBoxBelow));
        s->setColour (Slider::thumbColourId, secs.back().col);
        addAndMakeVisible (s);
        auto* l = labels.add (new Label (String(), name.toUpperCase()));
        l->setJustificationType (Justification::centred);
        l->setFont (Font (FontOptions (10.f, Font::bold)));
        l->setColour (Label::textColourId, kCream);
        l->setInterceptsMouseClicks (false, false);
        addAndMakeVisible (l);
        sAtt.push_back (std::make_unique<AudioProcessorValueTreeState::SliderAttachment> (apvts, id, *s));
        secs.back().sl.push_back (s); secs.back().lb.push_back (l);
    }
    void combo (const String& id, const String& caption)
    {
        auto* c = combos.add (new ComboBox (caption));
        if (auto* pc = dynamic_cast<AudioParameterChoice*> (apvts.getParameter (id))) c->addItemList (pc->choices, 1);
        addAndMakeVisible (c);
        cAtt.push_back (std::make_unique<AudioProcessorValueTreeState::ComboBoxAttachment> (apvts, id, *c));
        secs.back().sels.push_back (c);
    }
    void toggle (const String& id, const String& text)
    {
        auto* b = toggles.add (new ToggleButton (text));
        addAndMakeVisible (b);
        bAtt.push_back (std::make_unique<AudioProcessorValueTreeState::ButtonAttachment> (apvts, id, *b));
        secs.back().sels.push_back (b);
    }

    void build()
    {
        beginRow();
        section ("Saw Osc", 5.5f);
        slider ("saw_mix", "Mix"); slider ("saw_semi", "Semi"); slider ("uni_voices", "Voices");
        slider ("uni_detune", "Detune"); slider ("uni_blend", "Blend"); toggle ("classic7", "Classic 7-Voice");
        section ("Pulse Osc", 5.5f);
        slider ("pulse_mix", "Mix"); slider ("pulse_semi", "Semi"); slider ("pulse_width", "Width");
        slider ("pwm_depth", "PWM"); slider ("pwm_rate", "Rate"); toggle ("pwm_sync", "PWM Tempo Sync");
        section ("Sub / Noise", 4.5f);
        slider ("sub_mix", "Sub"); slider ("sub_semi", "Semi"); slider ("noise_mix", "Noise"); slider ("noise_color", "Color");
        combo ("sub_shape", "Sub Shape");
        section ("Filter", 7.5f);
        slider ("hpf", "HPF"); slider ("cutoff", "Cutoff"); slider ("res", "Reso"); slider ("drive", "Drive");
        slider ("keytrack", "Key"); slider ("fenv_amt", "Env"); slider ("fm_amt", "FM");
        combo ("f_type", "Filter Type");

        beginRow();
        section ("LFO", 5.f);
        slider ("lfo_rate", "Rate"); slider ("lfo_fade", "Fade"); slider ("mod_depth", "Wheel");
        combo ("lfo_wave", "Wave"); combo ("lfo_div", "Sync Division"); toggle ("lfo_sync", "LFO Tempo Sync");
        section ("LFO Destinations", 13.f);
        for (int i = 0; i < kNumDests; ++i) slider (destId (i), kDestShort[i]);
        section ("Chorus", 3.2f);
        slider ("chorus_mix", "Mix"); combo ("chorus_mode", "Chorus"); toggle ("bass_boost", "Bass Boost");

        beginRow();
        section ("Amp Env", 4.2f);
        slider ("ae_a", "A"); slider ("ae_d", "D"); slider ("ae_s", "S"); slider ("ae_r", "R");
        section ("Filter Env", 4.2f);
        slider ("fe_a", "A"); slider ("fe_d", "D"); slider ("fe_s", "S"); slider ("fe_r", "R");
        section ("Pitch Env", 3.5f);
        slider ("pe_a", "A"); slider ("pe_d", "D"); slider ("pe_depth", "Depth"); combo ("pe_target", "Target");
        section ("Delay", 5.f);
        slider ("dly_time", "Time"); slider ("dly_fb", "Fdbk"); slider ("dly_tone", "Tone"); slider ("dly_pp", "Ping"); slider ("dly_mix", "Mix");
        section ("Reverb", 3.5f);
        slider ("rev_size", "Size"); slider ("rev_mix", "Mix"); slider ("rev_duck", "Duck");

        beginRow();
        section ("Play / Arp", 5.f);
        slider ("porta_time", "Glide"); slider ("arp_gate", "Gate"); slider ("arp_oct", "Oct");
        combo ("chord", "Chord"); combo ("voice_mode", "Voices"); combo ("porta_mode", "Glide Mode");
        toggle ("arp_on", "Arp On"); combo ("arp_div", "Arp Rate"); combo ("arp_pat", "Arp Pattern");
        section ("Output", 6.f);
        slider ("width", "Width"); slider ("out_bass", "Bass"); slider ("out_treble", "Treble");
        slider ("drift", "Drift"); slider ("master", "Master");
        combo ("osc_reset", "Osc Reset");
    }
};

class UltrasawEditor : public AudioProcessorEditor
{
public:
    explicit UltrasawEditor (UltrasawProcessor& p) : AudioProcessorEditor (p), panel (p.getApvts())
    {
        addAndMakeVisible (panel);
        const int ph = panel.layoutAll (designW);
        panel.setBounds (0, 0, designW, ph);
        setResizeLimits (640, (int) (640.0 * ph / designW), 1920, (int) (1920.0 * ph / designW));
        setResizable (true, true);
        if (auto* c = getConstrainer()) c->setFixedAspectRatio ((double) designW / (double) ph);
        setSize ((int) (designW * 0.9), (int) (ph * 0.9));
    }
    void resized() override { panel.setTransform (AffineTransform::scale ((float) getWidth() / (float) designW)); }
private:
    static constexpr int designW = 1280;
    JunoPanel panel;
};

AudioProcessorEditor* UltrasawProcessor::createEditor() { return new UltrasawEditor (*this); }

AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new UltrasawProcessor(); }
