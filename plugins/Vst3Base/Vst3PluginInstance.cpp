/*
 * Vst3PluginInstance.cpp - In-process VST3 plugin host core implementation
 *
 * Copyright (c) 2024 LMMS contributors
 *
 * This file is part of LMMS - https://lmms.io
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program (see COPYING); if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301 USA.
 *
 */

#include "Vst3PluginInstance.h"
#include "Vst3MemoryStream.h"
#include "Vst3EventList.h"
#include "Vst3ParameterChanges.h"
#include "MidiEvent.h"

// VST3 SDK headers
#include <public.sdk/source/vst/hosting/module.h>
#include <pluginterfaces/base/ipluginbase.h>
#include <pluginterfaces/vst/ivstcomponent.h>
#include <pluginterfaces/vst/ivstconnectionpoint.h>
#include <pluginterfaces/vst/ivstaudioprocessor.h>
#include <pluginterfaces/vst/ivsteditcontroller.h>
#include <pluginterfaces/vst/ivstparameterchanges.h>
#include <pluginterfaces/vst/ivstevents.h>
#include <pluginterfaces/vst/vsttypes.h>
#include <pluginterfaces/vst/ivstprocesscontext.h>
#include <pluginterfaces/base/funknownimpl.h>

#include <QDebug>
#include <cstring>
#include <algorithm>

namespace lmms
{

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

namespace
{

/// Cast a void* stored field back to a VST3 interface pointer.
template<typename T>
T* cast(void* p) { return static_cast<T*>(p); }

/// Wrap a tresult check with a human-readable label for error messages.
bool check(Steinberg::tresult r, const char* label, QString& error)
{
    if (r != Steinberg::kResultOk && r != Steinberg::kResultTrue)
    {
        error = QString("VST3: %1 failed (tresult=%2)")
                    .arg(QString::fromLatin1(label))
                    .arg(static_cast<int>(r));
        return false;
    }
    return true;
}

/// True if the sub-category string marks an instrument.
bool subCatIsInstrument(const std::string& subCat)
{
    return subCat.find("Instrument") != std::string::npos ||
           subCat.find("Synth")      != std::string::npos;
}

/// Release a VST3 COM interface pointer and null it out.
template<typename T>
void safeRelease(void*& ptr)
{
    if (ptr)
    {
        static_cast<T*>(ptr)->release();
        ptr = nullptr;
    }
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Vst3PluginInstance::discoverClasses
// ---------------------------------------------------------------------------

std::vector<Vst3ClassInfo>
Vst3PluginInstance::discoverClasses(const QString& bundlePath, QString* errorOut)
{
    std::vector<Vst3ClassInfo> result;
    std::string err;

    auto module = VST3::Hosting::Module::create(bundlePath.toStdString(), err);
    if (!module)
    {
        if (errorOut)
            *errorOut = QString("VST3: cannot open bundle '%1': %2")
                            .arg(bundlePath)
                            .arg(QString::fromStdString(err));
        return result;
    }

    auto factory = module->getFactory();
    const auto& factoryInfo = factory.info();
    (void)factoryInfo; // available if needed

    const auto classInfoList = factory.classInfos();
    for (size_t i = 0; i < classInfoList.size(); ++i)
    {
        const auto& ci = classInfoList[i];

        // Only enumerate audio processor classes
        if (std::string(ci.category()) != kVstAudioEffectClass)
            continue;

        Vst3ClassInfo info;
        info.classIndex    = static_cast<int>(i);
        info.name          = QString::fromStdString(ci.name());
        info.vendor        = QString::fromStdString(factoryInfo.vendor());
        info.category      = QString::fromStdString(ci.category());
        info.subCategories = QString::fromStdString(ci.subCategoriesString());
        info.isInstrument  = subCatIsInstrument(ci.subCategoriesString());

        // Format the 16-byte UID as a hex string for stable project identity
        const auto& uid = ci.ID();
        char hex[33] = {};
        for (int b = 0; b < 16; ++b)
            snprintf(hex + b*2, 3, "%02X",
                     static_cast<unsigned char>(uid.data()[b]));
        info.cid = QString::fromLatin1(hex);

        result.push_back(std::move(info));
    }

    return result;
}

// ---------------------------------------------------------------------------
// Vst3PluginInstance::load
// ---------------------------------------------------------------------------

Vst3PluginInstance::LoadResult
Vst3PluginInstance::load(const QString& bundlePath,
                          int            classIndex,
                          double         sampleRate,
                          int            blockSize)
{
    LoadResult res;
    auto inst = std::unique_ptr<Vst3PluginInstance>(new Vst3PluginInstance());
    inst->m_sampleRate = sampleRate;
    inst->m_blockSize  = blockSize;

    QString err;
    if (!inst->initFactory(bundlePath, err)          ||
        !inst->initComponent(classIndex, err)         ||
        !inst->initController(err)                    ||
        !inst->connectComponentController(err)        ||
        !inst->initAudioSetup(sampleRate, blockSize, err))
    {
        res.error = err;
        return res;
    }

    inst->loadParameters();
    res.instance = std::move(inst);
    return res;
}

// ---------------------------------------------------------------------------
// Private init helpers
// ---------------------------------------------------------------------------

bool Vst3PluginInstance::initFactory(const QString& bundlePath, QString& error)
{
    std::string err;
    auto module = VST3::Hosting::Module::create(bundlePath.toStdString(), err);
    if (!module)
    {
        error = QString("VST3: cannot open bundle '%1': %2")
                    .arg(bundlePath)
                    .arg(QString::fromStdString(err));
        return false;
    }

    // Keep the module alive for the lifetime of this instance.
    // VST3::Hosting::Module is a shared_ptr typedef; heap-allocate a copy
    // so the .so stays mapped until we explicitly delete it in the destructor.
    m_moduleHandle = new VST3::Hosting::Module::Ptr(module);

    // module->getFactory() returns a PluginFactory **by value** — a thin
    // wrapper that itself holds a ref-counted IPluginFactory*.  Calling
    // .get() on the temporary would yield a dangling pointer the moment
    // the temporary is destroyed (end of the full-expression).  Instead,
    // heap-allocate the wrapper so its lifetime matches the instance, then
    // extract the raw pointer from the stable copy.
    auto* storedWrapper = new VST3::Hosting::PluginFactory(module->getFactory());
    // m_moduleHandle already keeps the module (and thus the factory's
    // backing data) alive; storedWrapper is a second owner for cleanup.
    // We reuse the m_factory void* slot to carry the wrapper pointer.
    // initComponent() will downcast it back to PluginFactory* to obtain
    // the raw IPluginFactory*.
    if (!storedWrapper->get())
    {
        delete storedWrapper;
        error = "VST3: module returned null factory";
        return false;
    }
    m_factory = storedWrapper; // void* carrying PluginFactory*
    return true;
}

bool Vst3PluginInstance::initComponent(int classIndex, QString& error)
{
    // m_factory carries a VST3::Hosting::PluginFactory*; extract the raw ptr.
    auto* factory = cast<VST3::Hosting::PluginFactory>(m_factory)->get().get();

    Steinberg::PFactoryInfo factInfo{};
    factory->getFactoryInfo(&factInfo);
    m_vendor = QString::fromLatin1(factInfo.vendor);

    // Locate the class at classIndex (audio effect classes only)
    int audioIdx = 0;
    bool found   = false;

    const int nClasses = factory->countClasses();
    for (int i = 0; i < nClasses; ++i)
    {
        Steinberg::PClassInfo ci{};
        if (factory->getClassInfo(i, &ci) != Steinberg::kResultOk)
            continue;

        if (std::string(ci.category) != kVstAudioEffectClass)
            continue;

        if (audioIdx == classIndex)
        {
            m_name        = QString::fromLatin1(ci.name);
            m_isInstrument = false; // refined below with PClassInfo2

            // Try to get extended info for sub-categories
            if (auto* factory2 = Steinberg::FUnknownPtr<Steinberg::IPluginFactory2>(factory).getInterface())
            {
                Steinberg::PClassInfo2 ci2{};
                if (factory2->getClassInfo2(i, &ci2) == Steinberg::kResultOk)
                {
                    m_category     = QString::fromLatin1(ci2.subCategories);
                    m_isInstrument = subCatIsInstrument(ci2.subCategories);
                }
            }

            // Instantiate the component
            Steinberg::IPtr<Steinberg::Vst::IComponent> comp;
            if (!check(factory->createInstance(ci.cid,
                                               Steinberg::Vst::IComponent::iid,
                                               reinterpret_cast<void**>(&comp)),
                        "createInstance(IComponent)", error))
                return false;

            if (!comp)
            {
                error = "VST3: createInstance returned null component";
                return false;
            }

            // IComponent::initialize — host context can be null for Phase 1
            if (!check(comp->initialize(nullptr), "IComponent::initialize", error))
                return false;

            comp->addRef();
            m_component = comp.get();
            found = true;
            break;
        }
        ++audioIdx;
    }

    if (!found)
    {
        error = QString("VST3: no audio class at index %1").arg(classIndex);
        return false;
    }

    // Obtain IAudioProcessor from component
    auto* comp = cast<Steinberg::Vst::IComponent>(m_component);
    Steinberg::Vst::IAudioProcessor* proc = nullptr;
    if (!check(comp->queryInterface(Steinberg::Vst::IAudioProcessor::iid,
                                     reinterpret_cast<void**>(&proc)),
                "queryInterface(IAudioProcessor)", error))
        return false;

    m_audioProcessor = proc;
    return true;
}

bool Vst3PluginInstance::initController(QString& error)
{
    auto* comp = cast<Steinberg::Vst::IComponent>(m_component);

    // First try: does the component itself implement IEditController?
    Steinberg::Vst::IEditController* ctrl = nullptr;
    if (comp->queryInterface(Steinberg::Vst::IEditController::iid,
                              reinterpret_cast<void**>(&ctrl)) == Steinberg::kResultOk
        && ctrl)
    {
        // Single-object plugin — component IS the controller.
        // initialize() was already called above; call setComponentState()
        // with an empty stream so the controller knows the component is ready.
        m_controller = ctrl;
        Vst3MemoryStream emptyStream;
        comp->getState(&emptyStream);
        emptyStream.seek(0, Steinberg::IBStream::kIBSeekSet, nullptr);
        ctrl->setComponentState(&emptyStream);
        return true;
    }

    // Second try: separate controller class via IComponent::getControllerClassId
    Steinberg::TUID controllerCID{};
    if (comp->getControllerClassId(controllerCID) != Steinberg::kResultOk)
    {
        // No separate controller — some plugins are valid without one.
        // Log a warning but don't fail.
        qWarning("Vst3PluginInstance: no separate IEditController for '%s'",
                 qPrintable(m_name));
        return true;
    }

    auto* factory = cast<VST3::Hosting::PluginFactory>(m_factory)->get().get();
    if (!check(factory->createInstance(controllerCID,
                                       Steinberg::Vst::IEditController::iid,
                                       reinterpret_cast<void**>(&ctrl)),
                "createInstance(IEditController)", error))
        return false;

    if (!ctrl)
    {
        error = "VST3: separate controller createInstance returned null";
        return false;
    }

    if (!check(ctrl->initialize(nullptr), "IEditController::initialize", error))
    {
        ctrl->release();
        return false;
    }

    // Sync component state into controller
    Vst3MemoryStream stateStream;
    if (comp->getState(&stateStream) == Steinberg::kResultOk)
    {
        stateStream.seek(0, Steinberg::IBStream::kIBSeekSet, nullptr);
        ctrl->setComponentState(&stateStream);
    }

    m_controller = ctrl;
    return true;
}

bool Vst3PluginInstance::connectComponentController(QString& error)
{
    if (!m_controller) return true; // no controller — skip

    auto* comp = cast<Steinberg::Vst::IComponent>(m_component);
    auto* ctrl = cast<Steinberg::Vst::IEditController>(m_controller);

    // Check if they are the same object — if so, don't connect them to
    // themselves (some SDK implementations reject self-connection).
    Steinberg::FUnknown* compUnk = nullptr;
    Steinberg::FUnknown* ctrlUnk = nullptr;
    comp->queryInterface(Steinberg::FUnknown::iid, reinterpret_cast<void**>(&compUnk));
    ctrl->queryInterface(Steinberg::FUnknown::iid, reinterpret_cast<void**>(&ctrlUnk));
    const bool sameObject = (compUnk == ctrlUnk);
    if (compUnk) compUnk->release();
    if (ctrlUnk) ctrlUnk->release();

    if (sameObject)
        return true;

    // Wire IConnectionPoint if both sides support it
    Steinberg::Vst::IConnectionPoint* compConn = nullptr;
    Steinberg::Vst::IConnectionPoint* ctrlConn = nullptr;

    comp->queryInterface(Steinberg::Vst::IConnectionPoint::iid,
                          reinterpret_cast<void**>(&compConn));
    ctrl->queryInterface(Steinberg::Vst::IConnectionPoint::iid,
                          reinterpret_cast<void**>(&ctrlConn));

    if (compConn && ctrlConn)
    {
        compConn->connect(ctrlConn);
        ctrlConn->connect(compConn);
        m_compConnection = compConn;
        m_ctrlConnection = ctrlConn;
    }
    else
    {
        // Not all plugins implement IConnectionPoint — that's fine.
        if (compConn) compConn->release();
        if (ctrlConn) ctrlConn->release();
    }

    return true;
}

bool Vst3PluginInstance::initAudioSetup(double sampleRate, int blockSize, QString& error)
{
    auto* proc = cast<Steinberg::Vst::IAudioProcessor>(m_audioProcessor);
    auto* comp = cast<Steinberg::Vst::IComponent>(m_component);

    // Set up process setup
    Steinberg::Vst::ProcessSetup setup{};
    setup.processMode        = Steinberg::Vst::kRealtime;
    setup.symbolicSampleSize = Steinberg::Vst::kSample32;
    setup.maxSamplesPerBlock = blockSize;
    setup.sampleRate         = sampleRate;

    if (!check(proc->setupProcessing(setup), "setupProcessing", error))
        return false;

    // Activate audio buses.  We activate only the first main stereo I/O pair.
    // Additional buses are left inactive (Phase 1 limitation).
    const int numInputBuses  = comp->getBusCount(Steinberg::Vst::kAudio,
                                                   Steinberg::Vst::kInput);
    const int numOutputBuses = comp->getBusCount(Steinberg::Vst::kAudio,
                                                   Steinberg::Vst::kOutput);

    for (int i = 0; i < numInputBuses; ++i)
        comp->activateBus(Steinberg::Vst::kAudio, Steinberg::Vst::kInput, i,
                          i == 0 /*main bus only*/);

    for (int i = 0; i < numOutputBuses; ++i)
        comp->activateBus(Steinberg::Vst::kAudio, Steinberg::Vst::kOutput, i,
                          i == 0 /*main bus only*/);

    // Also activate event input bus (MIDI) if present
    const int numEventInputs = comp->getBusCount(Steinberg::Vst::kEvent,
                                                   Steinberg::Vst::kInput);
    if (numEventInputs > 0)
        comp->activateBus(Steinberg::Vst::kEvent, Steinberg::Vst::kInput, 0, true);

    if (!check(comp->setActive(true), "IComponent::setActive(true)", error))
        return false;

    // Pre-allocate scratch buffers for processAudio() — no allocation inside
    // the audio callback.
    m_inputBuf.assign(static_cast<std::size_t>(blockSize) * 2, 0.0f);
    m_outputBuf.assign(static_cast<std::size_t>(blockSize) * 2, 0.0f);

    // VST3 uses planar (non-interleaved) channel pointers.
    // We'll split our interleaved LMMS buffer into L/R planes on each call.
    m_inputPtrs.resize(2);
    m_outputPtrs.resize(2);

    return true;
}

void Vst3PluginInstance::loadParameters()
{
    if (!m_controller) return;

    auto* ctrl = cast<Steinberg::Vst::IEditController>(m_controller);
    const int count = ctrl->getParameterCount();
    m_parameters.reserve(static_cast<std::size_t>(count));

    for (int i = 0; i < count; ++i)
    {
        Steinberg::Vst::ParameterInfo pi{};
        if (ctrl->getParameterInfo(i, pi) != Steinberg::kResultOk)
            continue;

        Vst3Parameter p;
        p.id = static_cast<Vst3ParamID>(pi.id);

        // Convert Steinberg TChar (char16) to QString
        p.title      = QString::fromUtf16(reinterpret_cast<const char16_t*>(pi.title));
        p.shortTitle = QString::fromUtf16(reinterpret_cast<const char16_t*>(pi.shortTitle));
        p.units      = QString::fromUtf16(reinterpret_cast<const char16_t*>(pi.units));

        p.defaultNormalisedValue = pi.defaultNormalizedValue;
        p.stepCount              = pi.stepCount;

        using F = Steinberg::Vst::ParameterInfo;
        p.isAutomatable  = (pi.flags & F::kCanAutomate)    != 0;
        p.isReadOnly     = (pi.flags & F::kIsReadOnly)     != 0;
        p.isBypass       = (pi.flags & F::kIsBypass)       != 0;
        p.isProgramChange= (pi.flags & F::kIsProgramChange)!= 0;

        m_parameters.push_back(p);
    }
}

// ---------------------------------------------------------------------------
// Vst3PluginInstance destructor
// ---------------------------------------------------------------------------

Vst3PluginInstance::~Vst3PluginInstance()
{
    // Ensure processing is stopped before tearing down
    if (m_processing)
        stopProcessing();

    // Disconnect IConnectionPoint
    if (m_compConnection && m_ctrlConnection)
    {
        auto* cc = cast<Steinberg::Vst::IConnectionPoint>(m_compConnection);
        auto* ck = cast<Steinberg::Vst::IConnectionPoint>(m_ctrlConnection);
        cc->disconnect(ck);
        ck->disconnect(cc);
    }
    safeRelease<Steinberg::Vst::IConnectionPoint>(m_compConnection);
    safeRelease<Steinberg::Vst::IConnectionPoint>(m_ctrlConnection);

    // Controller
    if (m_controller)
    {
        // Only terminate if it's a separate object (not the same as component)
        auto* comp = cast<Steinberg::Vst::IComponent>(m_component);
        Steinberg::Vst::IEditController* ctrl =
            cast<Steinberg::Vst::IEditController>(m_controller);

        Steinberg::FUnknown* compUnk = nullptr;
        Steinberg::FUnknown* ctrlUnk = nullptr;
        if (comp) comp->queryInterface(Steinberg::FUnknown::iid,
                                        reinterpret_cast<void**>(&compUnk));
        ctrl->queryInterface(Steinberg::FUnknown::iid,
                              reinterpret_cast<void**>(&ctrlUnk));
        const bool sameObj = (compUnk == ctrlUnk);
        if (compUnk) compUnk->release();
        if (ctrlUnk) ctrlUnk->release();

        if (!sameObj)
            ctrl->terminate();
        safeRelease<Steinberg::Vst::IEditController>(m_controller);
    }

    // Audio processor — release before component
    safeRelease<Steinberg::Vst::IAudioProcessor>(m_audioProcessor);

    // Component
    if (m_component)
    {
        cast<Steinberg::Vst::IComponent>(m_component)->setActive(false);
        cast<Steinberg::Vst::IComponent>(m_component)->terminate();
        safeRelease<Steinberg::Vst::IComponent>(m_component);
    }

    // Factory wrapper (PluginFactory*, heap-allocated in initFactory)
    if (m_factory)
    {
        delete static_cast<VST3::Hosting::PluginFactory*>(m_factory);
        m_factory = nullptr;
    }

    // Module (last — keeps the shared library open until we're fully done)
    if (m_moduleHandle)
    {
        delete static_cast<VST3::Hosting::Module::Ptr*>(m_moduleHandle);
        m_moduleHandle = nullptr;
    }
}

// ---------------------------------------------------------------------------
// Processing lifecycle
// ---------------------------------------------------------------------------

bool Vst3PluginInstance::startProcessing(QString* errorOut)
{
    if (m_processing) return true;

    auto* proc = cast<Steinberg::Vst::IAudioProcessor>(m_audioProcessor);
    QString err;
    if (!check(proc->setProcessing(true), "IAudioProcessor::setProcessing(true)", err))
    {
        if (errorOut) *errorOut = err;
        return false;
    }
    m_processing = true;
    return true;
}

void Vst3PluginInstance::stopProcessing()
{
    if (!m_processing) return;

    auto* proc = cast<Steinberg::Vst::IAudioProcessor>(m_audioProcessor);
    proc->setProcessing(false);
    m_processing = false;
}

// ---------------------------------------------------------------------------
// processAudio  (audio thread — no heap allocation)
// ---------------------------------------------------------------------------

void Vst3PluginInstance::processAudio(const float* inputs,
                                       float*       outputs,
                                       int          numFrames)
{
    if (!m_processing || !m_audioProcessor) return;

    auto* proc = cast<Steinberg::Vst::IAudioProcessor>(m_audioProcessor);

    // ---- Build per-block parameter changes ----
    Vst3ParameterChanges paramChanges;
    for (const auto& pc : m_pendingParamChanges)
        paramChanges.addChange(pc.id, pc.value);
    m_pendingParamChanges.clear();

    // ---- Build per-block event list ----
    Vst3EventList eventList;
    for (const auto& pe : m_pendingMidiEvents)
        eventList.addMidiEvent(pe.event, pe.sampleOffset);
    m_pendingMidiEvents.clear();

    // ---- De-interleave input (L R L R …) → planar (L… R…) ----
    const std::size_t nf = static_cast<std::size_t>(numFrames);
    float* inL = m_inputBuf.data();
    float* inR = m_inputBuf.data() + nf;
    float* outL = m_outputBuf.data();
    float* outR = m_outputBuf.data() + nf;

    if (inputs)
    {
        for (std::size_t i = 0; i < nf; ++i)
        {
            inL[i] = inputs[i * 2];
            inR[i] = inputs[i * 2 + 1];
        }
    }
    else
    {
        std::fill(inL, inL + nf, 0.0f);
        std::fill(inR, inR + nf, 0.0f);
    }
    std::fill(outL, outL + nf, 0.0f);
    std::fill(outR, outR + nf, 0.0f);

    m_inputPtrs[0]  = inL;
    m_inputPtrs[1]  = inR;
    m_outputPtrs[0] = outL;
    m_outputPtrs[1] = outR;

    // ---- Assemble VST3 AudioBusBuffers ----
    Steinberg::Vst::AudioBusBuffers inputBusBuffers{};
    inputBusBuffers.numChannels         = 2;
    inputBusBuffers.channelBuffers32    = m_inputPtrs.data();
    inputBusBuffers.silenceFlags        = 0;

    Steinberg::Vst::AudioBusBuffers outputBusBuffers{};
    outputBusBuffers.numChannels        = 2;
    outputBusBuffers.channelBuffers32   = m_outputPtrs.data();
    outputBusBuffers.silenceFlags       = 0;

    // ---- Assemble ProcessData ----
    Steinberg::Vst::ProcessData pd{};
    pd.processMode         = Steinberg::Vst::kRealtime;
    pd.symbolicSampleSize  = Steinberg::Vst::kSample32;
    pd.numSamples          = numFrames;
    pd.numInputs           = 1;
    pd.numOutputs          = 1;
    pd.inputs              = &inputBusBuffers;
    pd.outputs             = &outputBusBuffers;
    pd.inputParameterChanges  = &paramChanges;
    pd.outputParameterChanges = nullptr; // we don't consume output param changes
    pd.inputEvents            = &eventList;
    pd.outputEvents           = nullptr;
    pd.processContext         = nullptr; // transport context not wired in Phase 1

    proc->process(pd);

    // ---- Re-interleave planar output → interleaved (L R L R …) ----
    for (std::size_t i = 0; i < nf; ++i)
    {
        outputs[i * 2]     = outL[i];
        outputs[i * 2 + 1] = outR[i];
    }
}

// ---------------------------------------------------------------------------
// MIDI / event queue
// ---------------------------------------------------------------------------

void Vst3PluginInstance::queueMidiEvent(const MidiEvent& event, int sampleOffset)
{
    m_pendingMidiEvents.push_back({ event, sampleOffset });
}

void Vst3PluginInstance::clearMidiQueue()
{
    m_pendingMidiEvents.clear();
}

// ---------------------------------------------------------------------------
// Parameters
// ---------------------------------------------------------------------------

const Vst3Parameter* Vst3PluginInstance::findParameter(Vst3ParamID id) const
{
    for (const auto& p : m_parameters)
        if (p.id == id) return &p;
    return nullptr;
}

std::optional<double> Vst3PluginInstance::getParameterNormalized(Vst3ParamID id) const
{
    if (!m_controller) return std::nullopt;
    auto* ctrl = cast<Steinberg::Vst::IEditController>(m_controller);
    const double v = ctrl->getParamNormalized(static_cast<Steinberg::Vst::ParamID>(id));
    if (findParameter(id) == nullptr) return std::nullopt;
    return v;
}

void Vst3PluginInstance::queueParameterChange(Vst3ParamID id, double normalisedValue)
{
    // Also inform the controller so its internal state stays in sync
    if (m_controller)
    {
        auto* ctrl = cast<Steinberg::Vst::IEditController>(m_controller);
        ctrl->setParamNormalized(static_cast<Steinberg::Vst::ParamID>(id),
                                  normalisedValue);
    }
    m_pendingParamChanges.push_back({ id, normalisedValue });
}

QString Vst3PluginInstance::parameterDisplayString(Vst3ParamID id,
                                                    double normalisedValue) const
{
    if (!m_controller) return {};
    auto* ctrl = cast<Steinberg::Vst::IEditController>(m_controller);

    Steinberg::Vst::String128 display{};
    if (ctrl->getParamStringByValue(static_cast<Steinberg::Vst::ParamID>(id),
                                     normalisedValue, display) == Steinberg::kResultOk)
        return QString::fromUtf16(reinterpret_cast<const char16_t*>(display));
    return {};
}

// ---------------------------------------------------------------------------
// State serialisation
// ---------------------------------------------------------------------------

QByteArray Vst3PluginInstance::saveState() const
{
    if (!m_component) return {};

    auto* comp = cast<Steinberg::Vst::IComponent>(m_component);

    // Format: [4 bytes compLen][comp bytes][4 bytes ctrlLen][ctrl bytes]
    Vst3MemoryStream compStream;
    comp->getState(&compStream);

    QByteArray ctrlBytes;
    if (m_controller)
    {
        // If component and controller are the same object, we only save component state
        auto* ctrl = cast<Steinberg::Vst::IEditController>(m_controller);
        Steinberg::FUnknown* compUnk = nullptr;
        Steinberg::FUnknown* ctrlUnk = nullptr;
        comp->queryInterface(Steinberg::FUnknown::iid,
                              reinterpret_cast<void**>(&compUnk));
        ctrl->queryInterface(Steinberg::FUnknown::iid,
                              reinterpret_cast<void**>(&ctrlUnk));
        const bool sameObj = (compUnk == ctrlUnk);
        if (compUnk) compUnk->release();
        if (ctrlUnk) ctrlUnk->release();

        if (!sameObj)
        {
            Vst3MemoryStream ctrlStream;
            ctrl->getState(&ctrlStream);
            ctrlBytes = ctrlStream.buffer();
        }
    }

    const QByteArray compBytes = compStream.buffer();
    const uint32_t compLen = static_cast<uint32_t>(compBytes.size());
    const uint32_t ctrlLen = static_cast<uint32_t>(ctrlBytes.size());

    QByteArray out;
    out.resize(8 + compLen + ctrlLen);
    char* d = out.data();
    std::memcpy(d,     &compLen, 4); d += 4;
    std::memcpy(d, compBytes.constData(), compLen); d += compLen;
    std::memcpy(d,     &ctrlLen, 4); d += 4;
    if (ctrlLen > 0)
        std::memcpy(d, ctrlBytes.constData(), ctrlLen);

    return out;
}

bool Vst3PluginInstance::restoreState(const QByteArray& data)
{
    if (!m_component) return false;
    if (data.size() < 8) return false;

    const char* d = data.constData();
    uint32_t compLen = 0, ctrlLen = 0;
    std::memcpy(&compLen, d, 4); d += 4;

    if (static_cast<int>(compLen) > data.size() - 8) return false;

    QByteArray compBytes(d, static_cast<int>(compLen)); d += compLen;
    std::memcpy(&ctrlLen, d, 4); d += 4;
    QByteArray ctrlBytes(d, static_cast<int>(ctrlLen));

    auto* comp = cast<Steinberg::Vst::IComponent>(m_component);

    Vst3MemoryStream compStream(compBytes);
    if (comp->setState(&compStream) != Steinberg::kResultOk)
        return false;

    // Sync controller with new component state
    if (m_controller)
    {
        auto* ctrl = cast<Steinberg::Vst::IEditController>(m_controller);
        Vst3MemoryStream syncStream(compBytes);
        ctrl->setComponentState(&syncStream);

        if (ctrlLen > 0)
        {
            Vst3MemoryStream ctrlStream(ctrlBytes);
            ctrl->setState(&ctrlStream);
        }
    }

    return true;
}

} // namespace lmms
