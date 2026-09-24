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
#include <pluginterfaces/gui/iplugview.h>
#include <pluginterfaces/vst/ivstcomponent.h>
#include <pluginterfaces/vst/ivstmessage.h>
#include <pluginterfaces/vst/ivstaudioprocessor.h>
#include <pluginterfaces/vst/ivsteditcontroller.h>
#include <pluginterfaces/vst/ivstparameterchanges.h>
#include <pluginterfaces/vst/ivstevents.h>
#include <pluginterfaces/vst/vsttypes.h>
#include <pluginterfaces/vst/ivstprocesscontext.h>
#include <pluginterfaces/base/funknownimpl.h>

#include <QDebug>
#include <atomic>
#include <cstdio>
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

/// The IPlugView platform-type constant for the platform we were built for.
/// Chosen at compile time; never hardcode one platform.
Steinberg::FIDString platformType()
{
#if defined(_WIN32)
    return Steinberg::kPlatformTypeHWND;
#elif defined(__APPLE__)
    return Steinberg::kPlatformTypeNSView;
#else
    return Steinberg::kPlatformTypeX11EmbedWindowID;
#endif
}

/// Sanity bounds for editor sizes reported by a plugin.  A plugin that
/// returns garbage must not be able to make the host create a 2-billion-pixel
/// window.
constexpr int kMaxEditorDimension = 16384;
constexpr int kDefaultEditorWidth  = 640;
constexpr int kDefaultEditorHeight = 480;

bool editorSizeIsSane(int w, int h)
{
    return w > 0 && h > 0 && w <= kMaxEditorDimension && h <= kMaxEditorDimension;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Host callback objects
//
// Both are tiny COM objects the plugin holds references to.  They keep only
// a back-pointer to the owning instance, and that pointer is cleared
// (detach()) before the instance dies, so a plugin that leaks a reference can
// never call into a destroyed host.
// ---------------------------------------------------------------------------

/// IComponentHandler: the channel through which the plugin's controller (and
/// therefore its native editor) tells the host that a parameter changed.
class Vst3ComponentHandlerImpl final : public Steinberg::Vst::IComponentHandler
{
public:
    explicit Vst3ComponentHandlerImpl(Vst3PluginInstance* owner) : m_owner(owner) {}

    void detach() { m_owner.store(nullptr); }

    // Gesture bracketing.  Not needed until parameter-automation *recording*
    // is implemented (Phase 3); accepted and ignored for now.
    Steinberg::tresult PLUGIN_API beginEdit(Steinberg::Vst::ParamID) override
    {
        return Steinberg::kResultOk;
    }

    Steinberg::tresult PLUGIN_API performEdit(Steinberg::Vst::ParamID id,
                                              Steinberg::Vst::ParamValue value) override
    {
        if (auto* owner = m_owner.load())
        {
            owner->onControllerEdit(static_cast<Vst3ParamID>(id), value);
        }
        return Steinberg::kResultOk;
    }

    Steinberg::tresult PLUGIN_API endEdit(Steinberg::Vst::ParamID) override
    {
        return Steinberg::kResultOk;
    }

    Steinberg::tresult PLUGIN_API restartComponent(Steinberg::int32 flags) override
    {
        // TODO(Phase 3): handle kParamValuesChanged / kReloadComponent / kIoChanged.
        qDebug("Vst3PluginInstance: restartComponent(flags=0x%x) requested but not handled yet",
               static_cast<unsigned>(flags));
        return Steinberg::kResultOk;
    }

    Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID iid, void** obj) override
    {
        QUERY_INTERFACE(iid, obj, Steinberg::FUnknown::iid, Steinberg::Vst::IComponentHandler)
        QUERY_INTERFACE(iid, obj, Steinberg::Vst::IComponentHandler::iid, Steinberg::Vst::IComponentHandler)
        *obj = nullptr;
        return Steinberg::kNoInterface;
    }

    Steinberg::uint32 PLUGIN_API addRef() override { return ++m_refs; }

    Steinberg::uint32 PLUGIN_API release() override
    {
        const auto remaining = --m_refs;
        if (remaining == 0)
        {
            delete this;
        }
        return remaining;
    }

private:
    std::atomic<Vst3PluginInstance*> m_owner;
    std::atomic<Steinberg::uint32>   m_refs{1};
};

/// IPlugFrame: lets the plugin's view ask the host to resize its window.
class Vst3PlugFrameImpl final : public Steinberg::IPlugFrame
{
public:
    explicit Vst3PlugFrameImpl(Vst3PluginInstance* owner) : m_owner(owner) {}

    void detach() { m_owner.store(nullptr); }

    Steinberg::tresult PLUGIN_API resizeView(Steinberg::IPlugView* /*view*/,
                                             Steinberg::ViewRect*  newSize) override
    {
        auto* owner = m_owner.load();
        if (!owner || !newSize)
        {
            return Steinberg::kInvalidArgument;
        }
        owner->onEditorResizeRequested(newSize->getWidth(), newSize->getHeight());
        return Steinberg::kResultOk;
    }

    Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID iid, void** obj) override
    {
        QUERY_INTERFACE(iid, obj, Steinberg::FUnknown::iid, Steinberg::IPlugFrame)
        QUERY_INTERFACE(iid, obj, Steinberg::IPlugFrame::iid, Steinberg::IPlugFrame)
        *obj = nullptr;
        return Steinberg::kNoInterface;
    }

    Steinberg::uint32 PLUGIN_API addRef() override { return ++m_refs; }

    Steinberg::uint32 PLUGIN_API release() override
    {
        const auto remaining = --m_refs;
        if (remaining == 0)
        {
            delete this;
        }
        return remaining;
    }

private:
    std::atomic<Vst3PluginInstance*> m_owner;
    std::atomic<Steinberg::uint32>   m_refs{1};
};

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

    // classIndex is the index into the factory's FULL class list, matching
    // Vst3ClassInfo::classIndex from discoverClasses().  (It used to be
    // counted among audio classes only, which disagreed with discoverClasses()
    // for any bundle that lists a non-audio class before the audio class.)
    bool found = false;

    const int nClasses = factory->countClasses();
    for (int i = 0; i < nClasses; ++i)
    {
        Steinberg::PClassInfo ci{};
        if (factory->getClassInfo(i, &ci) != Steinberg::kResultOk)
            continue;

        if (std::string(ci.category) != kVstAudioEffectClass)
            continue;

        if (i == classIndex)
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
        installComponentHandler();

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

    // Take ownership, then hand the controller its IComponentHandler before
    // any state is pushed into it.
    m_controller = ctrl;
    installComponentHandler();

    // Sync component state into controller
    Vst3MemoryStream stateStream;
    if (comp->getState(&stateStream) == Steinberg::kResultOk)
    {
        stateStream.seek(0, Steinberg::IBStream::kIBSeekSet, nullptr);
        ctrl->setComponentState(&stateStream);
    }

    return true;
}

void Vst3PluginInstance::installComponentHandler()
{
    if (!m_controller || m_componentHandler)
        return;

    // Refcount starts at 1: that reference is owned by this instance and
    // released in the destructor.  The controller takes its own reference.
    auto* handler = new Vst3ComponentHandlerImpl(this);
    m_componentHandler = handler;
    cast<Steinberg::Vst::IEditController>(m_controller)->setComponentHandler(handler);
}

bool Vst3PluginInstance::connectComponentController(QString& error)
{
    Q_UNUSED(error)
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

    // A synth with no audio input bus must be given numInputs == 0 in
    // ProcessData, not a phantom stereo input.
    m_hasInputBus = numInputBuses > 0;

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

    // Reserve the event/parameter queues up front so that pushing from the
    // GUI/MIDI threads and swapping on the audio thread never allocate in
    // the common case.
    m_pendingParamChanges.reserve(1024);
    m_pendingMidiEvents.reserve(1024);
    m_activeParamChanges.reserve(1024);
    m_activeMidiEvents.reserve(1024);

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
    // The editor view holds references into the controller, so it has to be
    // torn down before anything else.
    closeEditor();

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

        // Stop the controller calling back into a host object that is about
        // to be destroyed.
        ctrl->setComponentHandler(nullptr);

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

    // Our own reference to the component handler.  detach() first so that a
    // plugin which kept a stray reference can no longer reach this object.
    if (m_componentHandler)
    {
        auto* handler = static_cast<Vst3ComponentHandlerImpl*>(m_componentHandler);
        handler->detach();
        handler->release();
        m_componentHandler = nullptr;
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
// processAudio  (audio thread)
// ---------------------------------------------------------------------------

void Vst3PluginInstance::processAudio(const float* inputs,
                                       float*       outputs,
                                       int          numFrames)
{
    if (!m_processing || !m_audioProcessor || !outputs || numFrames <= 0) return;

    // The planar scratch buffers were sized for m_blockSize in
    // initAudioSetup().  A larger block would write past their end, so
    // refuse it and emit silence.  (Mid-session buffer-size changes are a
    // Phase 3 item; until then this turns a heap overrun into a dropout.)
    if (numFrames > m_blockSize)
    {
        std::fill(outputs, outputs + static_cast<std::size_t>(numFrames) * 2, 0.0f);
        return;
    }

    auto* proc = cast<Steinberg::Vst::IAudioProcessor>(m_audioProcessor);

    // ---- Take everything queued since the last block ----
    // The lock covers two vector swaps only.  Producers (MIDI thread, GUI
    // thread, the plugin editor's performEdit) push under the same mutex.
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        m_activeParamChanges.swap(m_pendingParamChanges);
        m_activeMidiEvents.swap(m_pendingMidiEvents);
    }

    // ---- Build per-block parameter changes ----
    Vst3ParameterChanges paramChanges;
    for (const auto& pc : m_activeParamChanges)
        paramChanges.addChange(pc.id, pc.value);
    m_activeParamChanges.clear();

    // ---- Build per-block event list ----
    Vst3EventList eventList;
    for (const auto& pe : m_activeMidiEvents)
        eventList.addMidiEvent(pe.event, pe.sampleOffset);
    m_activeMidiEvents.clear();

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
    pd.numInputs           = m_hasInputBus ? 1 : 0;
    pd.numOutputs          = 1;
    pd.inputs              = m_hasInputBus ? &inputBusBuffers : nullptr;
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
    std::lock_guard<std::mutex> lock(m_queueMutex);
    m_pendingMidiEvents.push_back({ event, sampleOffset });
}

void Vst3PluginInstance::clearMidiQueue()
{
    std::lock_guard<std::mutex> lock(m_queueMutex);
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

void Vst3PluginInstance::queueForProcessor(Vst3ParamID id, double value)
{
    std::lock_guard<std::mutex> lock(m_queueMutex);
    m_pendingParamChanges.push_back({ id, value });
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
    queueForProcessor(id, normalisedValue);
}

void Vst3PluginInstance::setParameterEditedCallback(ParameterEditedCallback cb)
{
    m_paramEditedCb = std::move(cb);
}

void Vst3PluginInstance::onControllerEdit(Vst3ParamID id, double value)
{
    // The plugin's controller is the origin of this change, so it already
    // holds the new value.  Deliberately do NOT call setParamNormalized()
    // here: that would re-enter the controller from inside its own
    // performEdit() and is the first half of a feedback loop.  Just make
    // sure the processor hears about it.
    queueForProcessor(id, value);

    if (m_paramEditedCb)
        m_paramEditedCb(id, value);
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
// Native editor (IPlugView)
// ---------------------------------------------------------------------------

bool Vst3PluginInstance::createEditor(int*                widthOut,
                                      int*                heightOut,
                                      EditorResizeCallback onResize,
                                      QString*            errorOut)
{
    const auto fail = [errorOut](const char* userMessage) -> bool
    {
        if (errorOut)
            *errorOut = QString::fromLatin1(userMessage);
        return false;
    };

    // At most one live view per instance.
    closeEditor();

    if (!m_controller)
    {
        qWarning("Vst3PluginInstance: '%s' has no controller, so no editor", qPrintable(m_name));
        return fail("Plugin editor is unavailable");
    }

    auto* ctrl = cast<Steinberg::Vst::IEditController>(m_controller);

    // createView() returns a view we own (refcount 1), or nullptr when the
    // plugin has no editor.
    Steinberg::IPlugView* view = ctrl->createView(Steinberg::Vst::ViewType::kEditor);
    if (!view)
    {
        qDebug("Vst3PluginInstance: '%s' returned no editor view", qPrintable(m_name));
        return fail("Plugin editor is unavailable");
    }

    if (view->isPlatformTypeSupported(platformType()) != Steinberg::kResultTrue)
    {
        qWarning("Vst3PluginInstance: '%s' editor does not support platform type '%s'",
                 qPrintable(m_name), platformType());
        view->release();
        return fail("Plugin editor is unavailable");
    }

    // Validate the size instead of trusting it.
    Steinberg::ViewRect rect{};
    int width  = kDefaultEditorWidth;
    int height = kDefaultEditorHeight;
    if (view->getSize(&rect) == Steinberg::kResultOk
        && editorSizeIsSane(rect.getWidth(), rect.getHeight()))
    {
        width  = rect.getWidth();
        height = rect.getHeight();
    }
    else
    {
        qWarning("Vst3PluginInstance: '%s' reported an unusable editor size; using %dx%d",
                 qPrintable(m_name), width, height);
    }

    // The frame must be installed before attached() so the plugin can request
    // resizes during attach.
    auto* frame = new Vst3PlugFrameImpl(this); // refcount 1: owned by us
    view->setFrame(frame);

    m_plugView         = view;
    m_plugFrame        = frame;
    m_editorAttached   = false;
    m_editorResizeCb   = std::move(onResize);

    if (widthOut)  *widthOut  = width;
    if (heightOut) *heightOut = height;
    return true;
}

bool Vst3PluginInstance::attachEditor(void* nativeParent, QString* errorOut)
{
    if (!m_plugView || m_editorAttached || !nativeParent)
    {
        if (errorOut)
            *errorOut = QStringLiteral("Plugin editor is unavailable");
        return false;
    }

    auto* view = cast<Steinberg::IPlugView>(m_plugView);
    const Steinberg::tresult r = view->attached(nativeParent, platformType());
    if (r != Steinberg::kResultOk)
    {
        qWarning("Vst3PluginInstance: '%s' editor attached() failed (tresult=%d)",
                 qPrintable(m_name), static_cast<int>(r));
        if (errorOut)
            *errorOut = QStringLiteral("Plugin editor is unavailable");
        return false;
    }

    m_editorAttached = true;
    return true;
}

void Vst3PluginInstance::closeEditor()
{
    if (!m_plugView)
        return;

    // Stop resize callbacks first: nothing from here on may reach the GUI.
    m_editorResizeCb = nullptr;

    auto* view = cast<Steinberg::IPlugView>(m_plugView);
    m_plugView = nullptr;

    // Order matters: detach from the native parent, drop the frame, release.
    if (m_editorAttached)
    {
        view->removed();
        m_editorAttached = false;
    }
    view->setFrame(nullptr);
    view->release();

    if (m_plugFrame)
    {
        auto* frame = static_cast<Vst3PlugFrameImpl*>(m_plugFrame);
        frame->detach();
        frame->release();
        m_plugFrame = nullptr;
    }
}

void Vst3PluginInstance::onEditorResizeRequested(int width, int height)
{
    // Guard against a plugin that asks for a nonsense size, and against
    // re-entrancy: onSize() can itself provoke another resizeView().
    if (m_inEditorResize || !m_plugView || !editorSizeIsSane(width, height))
        return;

    m_inEditorResize = true;

    // Per the IPlugView contract: the host resizes its window first, then, in
    // the same call stack, tells the view its new size.
    if (m_editorResizeCb)
        m_editorResizeCb(width, height);

    Steinberg::ViewRect rect(0, 0, width, height);
    cast<Steinberg::IPlugView>(m_plugView)->onSize(&rect);

    m_inEditorResize = false;
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
