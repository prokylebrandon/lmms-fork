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
#include "Vst3ResultLog.h"
#include "MidiEvent.h"

// VST3 SDK headers
#include <public.sdk/source/vst/hosting/module.h>
#include <pluginterfaces/base/ipluginbase.h>
#include <pluginterfaces/gui/iplugview.h>
#include <pluginterfaces/gui/iplugviewcontentscalesupport.h>
#include <pluginterfaces/vst/ivstcomponent.h>
#include <pluginterfaces/vst/ivstmessage.h>
#include <pluginterfaces/vst/ivstaudioprocessor.h>
#include <pluginterfaces/vst/ivsteditcontroller.h>
#include <pluginterfaces/vst/ivstparameterchanges.h>
#include <pluginterfaces/vst/ivstevents.h>
#include <pluginterfaces/vst/vsttypes.h>
#include <pluginterfaces/vst/ivstprocesscontext.h>
#include <pluginterfaces/vst/vstspeaker.h>
#include <pluginterfaces/base/funknownimpl.h>

#include <QCoreApplication>
#include <QDebug>
#include <QThread>
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

// ---------------------------------------------------------------------------
// Lock-free pending queues (Phase 3 performance item -- replaces the
// mutex-protected pending/active vector pairs from Phase 2)
// ---------------------------------------------------------------------------

/// One queued LMMS -> plugin parameter write, delivered to the processor on
/// the next processAudio() call.
struct PendingParamChange
{
    Vst3ParamID id;
    double      value;
};

/// One queued MIDI event, delivered to the processor on the next
/// processAudio() call.
struct PendingMidiEvent
{
    MidiEvent event;
    int       sampleOffset;
};

/// Bounded, lock-free, multi-producer multi-consumer ring buffer (the
/// classic Vyukov MPMC bounded queue: a per-slot sequence counter, rather
/// than a single head/tail pair, is what lets multiple producers and
/// multiple consumers race on the same slots safely without a lock).
///
/// Used for both PendingParamChange and PendingMidiEvent. @p Capacity must
/// be a power of two so "index & (Capacity - 1)" can replace a modulo.
///
/// Every slot's payload is a std::optional<T> rather than a bare T so that
/// the queue never needs T to be default-constructible -- only copy- (push)
/// and move-or-copy- (pop) capable, which is all callers already rely on
/// elsewhere (e.g. PendingMidiEvent is built from a MidiEvent copy in
/// queueMidiEvent()). Capacity is fixed at construction and never grows:
/// push() on a full queue fails rather than allocating.
template<typename T, std::size_t Capacity>
class Vst3LockFreeQueue
{
    static_assert(Capacity >= 2 && (Capacity& (Capacity - 1)) == 0,
                  "Vst3LockFreeQueue capacity must be a power of two");

public:
    Vst3LockFreeQueue() : m_cells(Capacity)
    {
        for (std::size_t i = 0; i < Capacity; ++i)
        {
            m_cells[i].sequence.store(i, std::memory_order_relaxed);
        }
        m_enqueuePos.store(0, std::memory_order_relaxed);
        m_dequeuePos.store(0, std::memory_order_relaxed);
    }

    Vst3LockFreeQueue(const Vst3LockFreeQueue&)            = delete;
    Vst3LockFreeQueue& operator=(const Vst3LockFreeQueue&) = delete;

    /// Never allocates, never blocks. Returns false (item not enqueued) if
    /// the queue is full -- the caller counts the drop; retrying here could
    /// spin a realtime thread against a stalled consumer.
    bool push(const T& item)
    {
        Cell* cell = nullptr;
        std::size_t pos = m_enqueuePos.load(std::memory_order_relaxed);
        for (;;)
        {
            cell = &m_cells[pos & kMask];
            const std::size_t seq = cell->sequence.load(std::memory_order_acquire);
            const auto diff = static_cast<std::intptr_t>(seq) - static_cast<std::intptr_t>(pos);
            if (diff == 0)
            {
                if (m_enqueuePos.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed))
                {
                    break;
                }
            }
            else if (diff < 0)
            {
                return false; // full
            }
            else
            {
                pos = m_enqueuePos.load(std::memory_order_relaxed);
            }
        }
        cell->data = item;
        cell->sequence.store(pos + 1, std::memory_order_release);
        return true;
    }

    /// Never allocates, never blocks. Returns std::nullopt if the queue is
    /// currently empty. Returning by value (rather than writing into a
    /// caller-supplied T&) means a caller never has to default-construct a
    /// T just to have somewhere to pop into -- only move/copy-construction
    /// is required, which push() already relies on.
    std::optional<T> pop()
    {
        Cell* cell = nullptr;
        std::size_t pos = m_dequeuePos.load(std::memory_order_relaxed);
        for (;;)
        {
            cell = &m_cells[pos & kMask];
            const std::size_t seq = cell->sequence.load(std::memory_order_acquire);
            const auto diff = static_cast<std::intptr_t>(seq) - static_cast<std::intptr_t>(pos + 1);
            if (diff == 0)
            {
                if (m_dequeuePos.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed))
                {
                    break;
                }
            }
            else if (diff < 0)
            {
                return std::nullopt; // empty
            }
            else
            {
                pos = m_dequeuePos.load(std::memory_order_relaxed);
            }
        }
        std::optional<T> result(std::move(cell->data));
        cell->data.reset();
        cell->sequence.store(pos + kMask + 1, std::memory_order_release);
        return result;
    }

private:
    static constexpr std::size_t kMask = Capacity - 1;

    struct Cell
    {
        std::atomic<std::size_t> sequence;
        std::optional<T>         data;
    };

    std::vector<Cell>          m_cells;
    std::atomic<std::size_t>   m_enqueuePos;
    std::atomic<std::size_t>   m_dequeuePos;
};

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
        if (auto* owner = m_owner.load())
        {
            owner->onRestartRequested(static_cast<int>(flags));
        }
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
        Vst3WarnIfFailed(comp->getState(&emptyStream), "IComponent::getState()");
        emptyStream.seek(0, Steinberg::IBStream::kIBSeekSet, nullptr);
        Vst3WarnIfFailed(ctrl->setComponentState(&emptyStream), "IEditController::setComponentState()");
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
        Vst3WarnIfFailed(ctrl->setComponentState(&stateStream), "IEditController::setComponentState()");
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
    Vst3WarnIfFailed(cast<Steinberg::Vst::IEditController>(m_controller)->setComponentHandler(handler), "IEditController::setComponentHandler()");
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
        Vst3WarnIfFailed(compConn->connect(ctrlConn), "IConnectionPoint::connect(component to controller)");
        Vst3WarnIfFailed(ctrlConn->connect(compConn), "IConnectionPoint::connect(controller to component)");
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

    // ---- Negotiate the main bus layout (before setupProcessing, per the VST3 spec) ----
    // Ask for stereo on the main audio buses. A plugin that refuses (mono-only,
    // surround-only) keeps its own layout; it is read back here so processAudio()
    // hands the plugin exactly the channel count it expects.
    {
        const int nIn  = comp->getBusCount(Steinberg::Vst::kAudio, Steinberg::Vst::kInput);
        const int nOut = comp->getBusCount(Steinberg::Vst::kAudio, Steinberg::Vst::kOutput);
        std::vector<Steinberg::Vst::SpeakerArrangement> ins(static_cast<std::size_t>(nIn), Steinberg::Vst::SpeakerArr::kStereo);
        std::vector<Steinberg::Vst::SpeakerArrangement> outs(static_cast<std::size_t>(nOut), Steinberg::Vst::SpeakerArr::kStereo);
        for (int i = 0; i < nIn; ++i)  { (void)proc->getBusArrangement(Steinberg::Vst::kInput,  i, ins[static_cast<std::size_t>(i)]); }
        for (int i = 0; i < nOut; ++i) { (void)proc->getBusArrangement(Steinberg::Vst::kOutput, i, outs[static_cast<std::size_t>(i)]); }
        if (nIn > 0)  { ins[0]  = Steinberg::Vst::SpeakerArr::kStereo; }
        if (nOut > 0) { outs[0] = Steinberg::Vst::SpeakerArr::kStereo; }
        // A refusal is expected for plugins with a fixed layout, so it is not logged as an error.
        (void)proc->setBusArrangements(nIn > 0 ? ins.data() : nullptr, nIn, nOut > 0 ? outs.data() : nullptr, nOut);

        Steinberg::Vst::SpeakerArrangement arr = Steinberg::Vst::SpeakerArr::kStereo;
        m_numInputChannels  = 2;
        m_numOutputChannels = 2;
        if (nIn > 0 && proc->getBusArrangement(Steinberg::Vst::kInput, 0, arr) == Steinberg::kResultOk)
        {
            m_numInputChannels = std::max(1, static_cast<int>(Steinberg::Vst::SpeakerArr::getChannelCount(arr)));
        }
        if (nOut > 0 && proc->getBusArrangement(Steinberg::Vst::kOutput, 0, arr) == Steinberg::kResultOk)
        {
            m_numOutputChannels = std::max(1, static_cast<int>(Steinberg::Vst::SpeakerArr::getChannelCount(arr)));
        }
    }

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
        Vst3WarnIfFailed(comp->activateBus(Steinberg::Vst::kAudio, Steinberg::Vst::kInput, i, i == 0 /*main bus only*/), "IComponent::activateBus(audio input)");

    for (int i = 0; i < numOutputBuses; ++i)
        Vst3WarnIfFailed(comp->activateBus(Steinberg::Vst::kAudio, Steinberg::Vst::kOutput, i, i == 0 /*main bus only*/), "IComponent::activateBus(audio output)");

    // Also activate event input bus (MIDI) if present
    const int numEventInputs = comp->getBusCount(Steinberg::Vst::kEvent,
                                                   Steinberg::Vst::kInput);
    if (numEventInputs > 0)
        Vst3WarnIfFailed(comp->activateBus(Steinberg::Vst::kEvent, Steinberg::Vst::kInput, 0, true), "IComponent::activateBus(event input)");

    if (!check(comp->setActive(true), "IComponent::setActive(true)", error))
        return false;

    // Pre-allocate scratch buffers for processAudio() — no allocation inside
    // the audio callback.
    m_inputBuf.assign(static_cast<std::size_t>(blockSize) * static_cast<std::size_t>(std::max(2, m_numInputChannels)), 0.0f);
    m_outputBuf.assign(static_cast<std::size_t>(blockSize) * static_cast<std::size_t>(std::max(2, m_numOutputChannels)), 0.0f);

    // VST3 uses planar (non-interleaved) channel pointers.
    // We'll split our interleaved LMMS buffer into L/R planes on each call.
    m_inputPtrs.resize(static_cast<std::size_t>(std::max(2, m_numInputChannels)));
    m_outputPtrs.resize(static_cast<std::size_t>(std::max(2, m_numOutputChannels)));

    // Allocate the lock-free pending queues once, up front: push()/pop() on
    // them never allocate, so everything they need is in place before the
    // first processAudio() call.
    m_paramQueue = new Vst3LockFreeQueue<PendingParamChange, kQueueCapacity>();
    m_controllerSyncQueue = new Vst3LockFreeQueue<PendingParamChange, kQueueCapacity>();
    m_midiQueue  = new Vst3LockFreeQueue<PendingMidiEvent, kQueueCapacity>();
    m_blockEventList = new Vst3EventList();
    m_blockParamChanges = new Vst3ParameterChanges();

    // Belt-and-suspenders alongside m_activeNotes's own NSDMI: explicitly
    // establish the known-empty starting state before this instance is
    // reachable from queueMidiEvent()/flushActiveNotes().
    for (auto& row : m_activeNotes)
        for (auto& note : row)
            note.store(false, std::memory_order_relaxed);

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
        p.isHidden       = (pi.flags & F::kIsHidden)       != 0;

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

    // Pending queues: nothing after this point pushes to or drains them.
    // delete on a null pointer is a no-op, so this is safe even for a
    // load() that failed before initAudioSetup() reached the allocation
    // below and never created them.
    delete static_cast<Vst3LockFreeQueue<PendingParamChange, kQueueCapacity>*>(m_paramQueue);
    delete static_cast<Vst3LockFreeQueue<PendingMidiEvent, kQueueCapacity>*>(m_midiQueue);
    m_paramQueue = nullptr;
    delete static_cast<Vst3LockFreeQueue<PendingParamChange, kQueueCapacity>*>(m_controllerSyncQueue);
    m_controllerSyncQueue = nullptr;
    m_midiQueue  = nullptr;
    delete static_cast<Vst3EventList*>(m_blockEventList);
    delete static_cast<Vst3ParameterChanges*>(m_blockParamChanges);
    m_blockEventList = nullptr;
    m_blockParamChanges = nullptr;

    // Disconnect IConnectionPoint
    if (m_compConnection && m_ctrlConnection)
    {
        auto* cc = cast<Steinberg::Vst::IConnectionPoint>(m_compConnection);
        auto* ck = cast<Steinberg::Vst::IConnectionPoint>(m_ctrlConnection);
        (void)cc->disconnect(ck); // teardown: nothing useful can be done if this fails
        (void)ck->disconnect(cc); // teardown: nothing useful can be done if this fails
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
        (void)ctrl->setComponentHandler(nullptr); // teardown: nothing useful can be done if this fails

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
            (void)ctrl->terminate(); // teardown: nothing useful can be done if this fails
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
        (void)cast<Steinberg::Vst::IComponent>(m_component)->setActive(false); // teardown: nothing useful can be done if this fails
        (void)cast<Steinberg::Vst::IComponent>(m_component)->terminate(); // teardown: nothing useful can be done if this fails
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
    buildMidiMapping(); // GUI thread, not processing yet: nothing else reads the table

    auto* proc = cast<Steinberg::Vst::IAudioProcessor>(m_audioProcessor);
    QString err;
    // kNotImplemented means "nothing to prepare" and is valid per the VST3 spec.
    const Steinberg::tresult procResult = proc->setProcessing(true);
    if (procResult != Steinberg::kNotImplemented
        && !check(procResult, "IAudioProcessor::setProcessing(true)", err))
    {
        if (errorOut) *errorOut = err;
        return false;
    }
    m_processing = true;
    return true;
}

void Vst3PluginInstance::setTransport(double tempoBpm, bool playing, double projectTimeQuarters, int timeSigNumerator, int timeSigDenominator, double projectTimeSeconds)
{
    m_transportTempo     = tempoBpm > 0.0 ? tempoBpm : 120.0;
    m_transportPlaying   = playing;
    m_transportQuarters  = projectTimeQuarters;
    m_transportSeconds   = projectTimeSeconds;
    m_transportSigNum    = timeSigNumerator > 0 ? timeSigNumerator : 4;
    m_transportSigDen    = timeSigDenominator > 0 ? timeSigDenominator : 4;
    m_transportValid     = true;
}

bool Vst3PluginInstance::reconfigure(double sampleRate, int blockSize, QString* errorOut, bool offline)
{
    auto fail = [&](const QString& why) { if (errorOut) { *errorOut = why; } return false; };
    if (!m_component || !m_audioProcessor) { return fail(QStringLiteral("VST3: reconfigure() on an instance that is not set up")); }
    if (sampleRate <= 0.0 || blockSize <= 0) { return fail(QStringLiteral("VST3: reconfigure() with an invalid sample rate or block size")); }
    if (sampleRate == m_sampleRate && blockSize == m_blockSize && offline == m_offline) { return true; }

    const bool wasProcessing = m_processing;
    stopProcessing();

    auto* comp = cast<Steinberg::Vst::IComponent>(m_component);
    auto* proc = cast<Steinberg::Vst::IAudioProcessor>(m_audioProcessor);

    // setupProcessing() is only allowed while the component is inactive.
    (void)comp->setActive(false);

    Steinberg::Vst::ProcessSetup setup{};
    setup.processMode        = offline ? Steinberg::Vst::kOffline : Steinberg::Vst::kRealtime;
    setup.symbolicSampleSize = Steinberg::Vst::kSample32;
    setup.maxSamplesPerBlock = blockSize;
    setup.sampleRate         = sampleRate;

    QString err;
    if (!check(proc->setupProcessing(setup), "setupProcessing (reconfigure)", err)) { return fail(err); }
    if (!check(comp->setActive(true), "IComponent::setActive(true) (reconfigure)", err)) { return fail(err); }

    m_inputBuf.assign(static_cast<std::size_t>(blockSize) * static_cast<std::size_t>(std::max(2, m_numInputChannels)), 0.0f);
    m_outputBuf.assign(static_cast<std::size_t>(blockSize) * static_cast<std::size_t>(std::max(2, m_numOutputChannels)), 0.0f);
    m_sampleRate = sampleRate;
    m_blockSize  = blockSize;
    m_offline = offline;

    if (wasProcessing) { return startProcessing(errorOut); }
    return true;
}

void Vst3PluginInstance::buildMidiMapping()
{
    m_midiMap.clear();
    m_pitchBendMap.clear();
    if (!m_controller) { return; }

    auto* ctrl = cast<Steinberg::Vst::IEditController>(m_controller);
    auto* mapping = Steinberg::FUnknownPtr<Steinberg::Vst::IMidiMapping>(ctrl).getInterface();
    {
        auto* nec = Steinberg::FUnknownPtr<Steinberg::Vst::INoteExpressionController>(ctrl).getInterface();
        int diagTypes = -1; bool diagTuning = false;
        if (nec)
        {
            diagTypes = nec->getNoteExpressionCount(0, 0);
            for (int i = 0; i < diagTypes; ++i)
            {
                Steinberg::Vst::NoteExpressionTypeInfo info{};
                if (nec->getNoteExpressionInfo(0, 0, i, info) == Steinberg::kResultOk && info.typeId == Steinberg::Vst::kTuningTypeID) { diagTuning = true; }
            }
        }
        m_hasTuningExpression = diagTuning;
        qWarning("Vst3PluginInstance: %s note expression: INoteExpressionController=%d, types=%d, tuning=%d, IMidiMapping=%d", qPrintable(m_name), nec ? 1 : 0, diagTypes, diagTuning ? 1 : 0, mapping ? 1 : 0);
    }
    if (!mapping) { return; }

    const auto none = static_cast<Vst3ParamID>(Steinberg::Vst::kNoParamId);
    std::vector<Vst3ParamID> table(static_cast<std::size_t>(MidiChannelCount) * 128, none);
    bool any = false;
    for (int channel = 0; channel < MidiChannelCount; ++channel)
    {
        for (int cc = 0; cc < 128; ++cc)
        {
            Steinberg::Vst::ParamID pid = Steinberg::Vst::kNoParamId;
            if (mapping->getMidiControllerAssignment(0, static_cast<Steinberg::int16>(channel),
                    static_cast<Steinberg::Vst::CtrlNumber>(cc), pid) == Steinberg::kResultTrue
                && pid != Steinberg::Vst::kNoParamId)
            {
                table[static_cast<std::size_t>(channel) * 128 + static_cast<std::size_t>(cc)] = static_cast<Vst3ParamID>(pid);
                any = true;
            }
        }
    }
    if (any) { m_midiMap = std::move(table); }

    // Pitch bend has its own controller number (kPitchBend) in IMidiMapping.
    bool anyBend = false;
    std::vector<Vst3ParamID> bendTable(static_cast<std::size_t>(MidiChannelCount), none);
    for (int channel = 0; channel < MidiChannelCount; ++channel)
    {
        Steinberg::Vst::ParamID pid = Steinberg::Vst::kNoParamId;
        if (mapping->getMidiControllerAssignment(0, static_cast<Steinberg::int16>(channel),
                Steinberg::Vst::kPitchBend, pid) == Steinberg::kResultTrue
            && pid != Steinberg::Vst::kNoParamId)
        {
            bendTable[static_cast<std::size_t>(channel)] = static_cast<Vst3ParamID>(pid);
            anyBend = true;
        }
    }
    if (anyBend) { m_pitchBendMap = std::move(bendTable); }
    {
        int diagBendChannels = 0;
        for (const auto bendId : m_pitchBendMap) { if (bendId != none) { ++diagBendChannels; } }
        m_hasChannelPitchBend = diagBendChannels > 1;
        qWarning("Vst3PluginInstance: %s IMidiMapping: pitch bend mapped on %d channel(s), CC table %s", qPrintable(m_name), diagBendChannels, m_midiMap.empty() ? "empty" : "present");
    }
}

bool Vst3PluginInstance::mapMidiToParameter(const MidiEvent& event, Vst3ParamID& id, double& value) const
{
    // Audio thread: table lookup only, no allocation, no controller call.
    if (event.type() == MidiPitchBend)
    {
        const int bendChannel = event.channel();
        if (m_pitchBendMap.empty() || bendChannel < 0 || bendChannel >= MidiChannelCount) { return false; }
        const Vst3ParamID bendId = m_pitchBendMap[static_cast<std::size_t>(bendChannel)];
        if (bendId == static_cast<Vst3ParamID>(Steinberg::Vst::kNoParamId)) { return false; }
        id = bendId;
        // LMMS carries pitch bend as an unsigned 14-bit value: 0..16383, centre 8192.
        value = event.pitchBend() == 8192 ? 0.5 : std::min(1.0, std::max(0.0, static_cast<double>(event.pitchBend()) / 16383.0)); // 8192 is exact centre
        return true;
    }
    if (m_midiMap.empty() || event.type() != MidiControlChange) { return false; }
    const int channel = event.channel();
    const int cc = event.controllerNumber();
    if (channel < 0 || channel >= MidiChannelCount || cc < 0 || cc >= 128) { return false; }
    const Vst3ParamID mapped = m_midiMap[static_cast<std::size_t>(channel) * 128 + static_cast<std::size_t>(cc)];
    if (mapped == static_cast<Vst3ParamID>(Steinberg::Vst::kNoParamId)) { return false; }
    id = mapped;
    value = static_cast<double>(event.controllerValue()) / 127.0;
    return true;
}

void Vst3PluginInstance::stopProcessing()
{
    if (!m_processing) return;

    auto* proc = cast<Steinberg::Vst::IAudioProcessor>(m_audioProcessor);
    (void)proc->setProcessing(false); // teardown: nothing useful can be done if this fails (kNotImplemented is valid)
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
    // Lock-free drain: pop() never blocks, so a producer racing this loop
    // just shows up on the NEXT processAudio() call instead of this one,
    // same as it would have after a mutex unlock in the old implementation.
    auto* paramQueue = static_cast<Vst3LockFreeQueue<PendingParamChange, kQueueCapacity>*>(m_paramQueue);
    auto* midiQueue  = static_cast<Vst3LockFreeQueue<PendingMidiEvent, kQueueCapacity>*>(m_midiQueue);

    // ---- Build per-block parameter changes ----
    auto& paramChanges = *static_cast<Vst3ParameterChanges*>(m_blockParamChanges); paramChanges.clear();
    while (auto pc = paramQueue->pop())
        paramChanges.addChange(pc->id, pc->value);

    // ---- Build per-block event list ----
    auto& eventList = *static_cast<Vst3EventList*>(m_blockEventList); eventList.clear();
    while (auto pe = midiQueue->pop())
    {
        // CC reaches a VST3 plugin as a parameter change through its IMidiMapping,
        // not as an event (kLegacyMIDICCOutEvent is plugin -> host and is ignored).
        Vst3ParamID mappedId = 0;
        double mappedValue = 0.0;
        if (mapMidiToParameter(pe->event, mappedId, mappedValue))
        {
            paramChanges.addChange(mappedId, mappedValue);
        }
        else
        {
            eventList.addMidiEvent(pe->event, pe->sampleOffset);
        }
    }

    // ---- De-interleave input (L R L R …) → planar (L… R…) ----
    // ---- Per-note tuning (piano-roll pitch bend): one note-expression event per changed note ----
    // Each tuning event sits at the offset of its own NoteOn (0 when the note began in an earlier block),
    // after that NoteOn in the list; the sort below keeps the whole list in ascending offset order.
    for (int noteId = 0; noteId <= MidiMaxKey; ++noteId)
    {
        if (m_noteTuningDirty[noteId].load(std::memory_order_relaxed) && m_noteTuningDirty[noteId].exchange(false, std::memory_order_acquire))
        {
            eventList.addNoteTuning(noteId, m_noteTuning[noteId].load(std::memory_order_relaxed), eventList.noteOnOffset(noteId));
        }
    }

    eventList.sortBySampleOffset();

    const std::size_t nf = static_cast<std::size_t>(numFrames);
    const std::size_t inCh  = static_cast<std::size_t>(m_numInputChannels);
    const std::size_t outCh = static_cast<std::size_t>(m_numOutputChannels);
    const std::size_t inPlanes  = std::max<std::size_t>(2, inCh);
    const std::size_t outPlanes = std::max<std::size_t>(2, outCh);
    float* inL = m_inputBuf.data();
    float* inR = m_inputBuf.data() + nf;
    float* outL = m_outputBuf.data();
    float* outR = m_outputBuf.data() + nf;

    // LMMS hands over interleaved stereo. Planes 0 and 1 take L and R; a mono
    // input bus gets L+R mixed down; any extra planes (a surround layout the
    // plugin insisted on) are fed silence.
    std::fill(m_inputBuf.data(), m_inputBuf.data() + nf * inPlanes, 0.0f);
    if (inputs)
    {
        if (inCh == 1)
        {
            for (std::size_t i = 0; i < nf; ++i) { inL[i] = 0.5f * (inputs[i * 2] + inputs[i * 2 + 1]); }
        }
        else
        {
            for (std::size_t i = 0; i < nf; ++i) { inL[i] = inputs[i * 2]; inR[i] = inputs[i * 2 + 1]; }
        }
    }
    std::fill(m_outputBuf.data(), m_outputBuf.data() + nf * outPlanes, 0.0f);

    for (std::size_t p = 0; p < inCh; ++p)  { m_inputPtrs[p]  = m_inputBuf.data()  + p * nf; }
    for (std::size_t p = 0; p < outCh; ++p) { m_outputPtrs[p] = m_outputBuf.data() + p * nf; }
    // ---- Assemble VST3 AudioBusBuffers ----
    Steinberg::Vst::AudioBusBuffers inputBusBuffers{};
    inputBusBuffers.numChannels         = static_cast<Steinberg::int32>(inCh);
    inputBusBuffers.channelBuffers32    = m_inputPtrs.data();
    inputBusBuffers.silenceFlags        = 0;

    Steinberg::Vst::AudioBusBuffers outputBusBuffers{};
    outputBusBuffers.numChannels        = static_cast<Steinberg::int32>(outCh);
    outputBusBuffers.channelBuffers32   = m_outputPtrs.data();
    outputBusBuffers.silenceFlags       = 0;

    // ---- Assemble ProcessData ----
    Vst3OutputParamChanges outParamChanges; // the plugin writes meter etc. values here
    // ---- Transport context: tempo, play state, position, time signature ----
    Steinberg::Vst::ProcessContext transportContext{};
    if (m_transportValid)
    {
        transportContext.state = Steinberg::Vst::ProcessContext::kTempoValid
                               | Steinberg::Vst::ProcessContext::kTimeSigValid
                               | Steinberg::Vst::ProcessContext::kProjectTimeMusicValid
                               | (m_transportPlaying ? Steinberg::Vst::ProcessContext::kPlaying : 0u);
        transportContext.sampleRate         = m_sampleRate;
        transportContext.tempo              = m_transportTempo;
        transportContext.timeSigNumerator   = m_transportSigNum;
        transportContext.timeSigDenominator = m_transportSigDen;
        transportContext.projectTimeMusic   = m_transportQuarters;
        transportContext.projectTimeSamples = static_cast<Steinberg::Vst::TSamples>(
            m_transportSeconds >= 0.0
                ? m_transportSeconds * m_sampleRate
                : m_transportQuarters * 60.0 / m_transportTempo * m_sampleRate);
    }
    Steinberg::Vst::ProcessData pd{};
    pd.processMode         = m_offline ? Steinberg::Vst::kOffline : Steinberg::Vst::kRealtime;
    pd.symbolicSampleSize  = Steinberg::Vst::kSample32;
    pd.numSamples          = numFrames;
    pd.numInputs           = m_hasInputBus ? 1 : 0;
    pd.numOutputs          = 1;
    pd.inputs              = m_hasInputBus ? &inputBusBuffers : nullptr;
    pd.outputs             = &outputBusBuffers;
    pd.inputParameterChanges  = &paramChanges;
    pd.outputParameterChanges = &outParamChanges; // plugin -> host values (meters)
    pd.inputEvents            = &eventList;
    pd.outputEvents           = nullptr;
    pd.processContext         = m_transportValid ? &transportContext : nullptr;

    // A C++ exception escaping a plugin's process() on the audio thread would terminate the whole program,
    // so a throwing plugin is treated like one whose process() failed (handled just below).
    Steinberg::tresult processResult = Steinberg::kResultFalse;
    try { processResult = proc->process(pd); }
    catch (...) { processResult = Steinberg::kResultFalse; }
    if (processResult != Steinberg::kResultOk)
    {
        // The VST3 contract leaves the output buffers undefined when
        // process() fails, so play silence rather than whatever is in them.
        // Counted here, logged later from the GUI thread (logging allocates).
        m_processErrors.fetch_add(1, std::memory_order_relaxed);
        std::fill(outL, outL + nf, 0.0f);
        std::fill(outR, outR + nf, 0.0f);
    }

    // ---- Hand plugin-reported parameter values (meters etc.) to the GUI ----
    // Allocation-free and lock-free: only atomic stores into
    // m_outputParamSlots. The GUI thread applies them in
    // pollOutputParameters().
    for (Steinberg::int32 i = 0, n = (processResult == Steinberg::kResultOk) ? outParamChanges.getParameterCount() : 0; i < n; ++i)
    {
        auto* q = outParamChanges.getParameterData(i);
        const Steinberg::int32 points = q ? q->getPointCount() : 0;
        if (points <= 0) { continue; }

        Steinberg::int32 sampleOffset = 0;
        Steinberg::Vst::ParamValue value = 0.0;
        if (q->getPoint(points - 1, sampleOffset, value) == Steinberg::kResultOk)
        {
            m_outputParamSlots.publish(static_cast<Vst3ParamID>(q->getParameterId()), value);
        }
    }

    // ---- Re-interleave planar output → interleaved (L R L R …) ----
    for (std::size_t i = 0; i < nf; ++i)
    {
        outputs[i * 2]     = outL[i];
        outputs[i * 2 + 1] = (outCh == 1) ? outL[i] : outR[i];
    }
}

// ---------------------------------------------------------------------------
// MIDI / event queue
// ---------------------------------------------------------------------------

void Vst3PluginInstance::queueMidiEvent(const MidiEvent& event, int sampleOffset)
{
    // Active-note tracking, feeding flushActiveNotes() (see its doc
    // comment and m_activeNotes' comment in the header). Mirrors
    // Vst3EventList::addMidiEvent()'s own "velocity-0 NoteOn is treated as
    // NoteOff by convention" so tracking agrees with what the plugin
    // actually receives.
    const bool isNoteOn  = event.type() == MidiNoteOn && event.velocity() > 0;
    const bool isNoteOff = event.type() == MidiNoteOff
        || (event.type() == MidiNoteOn && event.velocity() == 0);

    const int channel = event.channel();
    const int key      = event.key();
    const bool inRange = channel >= 0 && channel < MidiChannelCount
        && key >= 0 && key <= MidiMaxKey;

    auto* queue = static_cast<Vst3LockFreeQueue<PendingMidiEvent, kQueueCapacity>*>(m_midiQueue);
    const bool pushed = queue && queue->push({ event, sampleOffset });

    if (inRange && isNoteOn)
    {
        // Optimistic: mark active even if the push above was dropped.
        // Worst case is one harmless extra NoteOff later, from
        // flushActiveNotes(), for a note the plugin never actually
        // started -- see the header comment for the reasoning and why
        // the NoteOff side below is NOT symmetric with this.
        m_activeNotes[channel][key].store(true, std::memory_order_relaxed);
    }
    else if (inRange && isNoteOff && pushed)
    {
        // Only clear on a CONFIRMED delivery: if this NoteOff itself was
        // dropped, the plugin may still believe the note is held, so
        // flushActiveNotes() must still account for it.
        m_activeNotes[channel][key].store(false, std::memory_order_relaxed);
    }

    if (!pushed)
    {
        // Either called before initAudioSetup() allocated the queue (should
        // not happen -- every public entry point that reaches here implies
        // a constructed instance), or the queue is full. Count, don't log:
        // this can be called from the audio thread, where a qWarning() call
        // would itself be a realtime-safety violation.
        m_droppedMidiEvents.fetch_add(1, std::memory_order_relaxed);
    }
}

void Vst3PluginInstance::flushActiveNotes()
{
    // See the header's doc comment for the full contract, in particular
    // the threading requirement this relies on the CALLER to provide.
    if (!m_processing)
        return;

    bool anyQueued = false;
    for (int channel = 0; channel < MidiChannelCount; ++channel)
    {
        for (int key = 0; key <= MidiMaxKey; ++key)
        {
            if (m_activeNotes[channel][key].load(std::memory_order_relaxed))
            {
                // Routed through queueMidiEvent() itself (not pushed to
                // the queue directly) so the same tracking logic above
                // clears the flag on confirmed delivery, and so a plugin
                // with a genuinely full queue right now still gets the
                // same drop-and-count treatment as any other MIDI event
                // rather than a silently different code path here.
                queueMidiEvent(MidiEvent(MidiNoteOff, static_cast<int8_t>(channel),
                                          static_cast<int16_t>(key), 0),
                               0);
                anyQueued = true;
            }
        }
    }

    if (!anyQueued)
        return;

    // Deliver the queued NoteOffs into the processor now, synchronously,
    // rather than waiting for a processAudio() call that will never come
    // once the caller proceeds to stopProcessing(). Output is discarded;
    // this is silence-in, silence-out from LMMS's point of view -- the
    // point is delivering the events, not the audio this block produces.
    std::vector<float> silence(static_cast<std::size_t>(m_blockSize) * 2, 0.0f);
    std::vector<float> discard(static_cast<std::size_t>(m_blockSize) * 2, 0.0f);
    processAudio(m_hasInputBus ? silence.data() : nullptr, discard.data(), m_blockSize);
}

void Vst3PluginInstance::setNoteTuning(int noteId, float semitones)
{
    if (noteId < 0 || noteId > MidiMaxKey) { return; }
    // Value first, flag second: processAudio() consumes the flag, then reads the value.
    m_noteTuning[noteId].store(semitones, std::memory_order_relaxed);
    m_noteTuningDirty[noteId].store(true, std::memory_order_release);
}

void Vst3PluginInstance::clearMidiQueue()
{
    auto* queue = static_cast<Vst3LockFreeQueue<PendingMidiEvent, kQueueCapacity>*>(m_midiQueue);
    if (!queue)
        return;
    while (queue->pop())
    {
        // Discard: this is a deliberate flush (e.g. transport stop), not a
        // drain into the processor.
    }
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
    auto* queue = static_cast<Vst3LockFreeQueue<PendingParamChange, kQueueCapacity>*>(m_paramQueue);
    if (!queue || !queue->push({ id, value }))
    {
        // See queueMidiEvent()'s matching comment: counted, not logged,
        // because this can run on the audio thread (automation applied
        // during playback ends up here via Vst3ParameterModel::
        // onModelChanged() -> queueParameterChange() -> here).
        m_droppedParamChanges.fetch_add(1, std::memory_order_relaxed);
    }
}

void Vst3PluginInstance::queueParameterChange(Vst3ParamID id, double normalisedValue)
{
    // IEditController is GUI-thread only. Automation applied during playback
    // reaches this on the audio thread, so there the controller is not touched:
    // the change goes to m_controllerSyncQueue and pollOutputParameters()
    // applies it on the GUI thread. A dropped sync only delays what the
    // plugin's own editor shows; the processor still gets the change below.
    if (m_controller)
    {
        const QCoreApplication* app = QCoreApplication::instance();
        if (app && QThread::currentThread() == app->thread())
        {
            auto* ctrl = cast<Steinberg::Vst::IEditController>(m_controller);
            (void)ctrl->setParamNormalized(static_cast<Steinberg::Vst::ParamID>(id), normalisedValue);
        }
        else if (auto* syncQueue = static_cast<Vst3LockFreeQueue<PendingParamChange, kQueueCapacity>*>(m_controllerSyncQueue))
        {
            (void)syncQueue->push({ id, normalisedValue });
        }
    }
    queueForProcessor(id, normalisedValue);
}

void Vst3PluginInstance::pollOutputParameters()
{
    // Report audio-thread process() failures (counted, not logged, there).
    const std::uint64_t processErrors = m_processErrors.load(std::memory_order_relaxed);
    if (processErrors != m_reportedProcessErrors)
    {
        qWarning("Vst3PluginInstance: IAudioProcessor::process() failed; %llu block(s) output as silence so far",
                 static_cast<unsigned long long>(processErrors));
        m_reportedProcessErrors = processErrors;
    }

    // GUI thread. The plugin is the origin of these values, so the model is
    // updated through the edited-parameter callback (which must not write back
    // into the plugin) and nothing is queued for the processor.
    auto* ctrl = m_controller ? cast<Steinberg::Vst::IEditController>(m_controller) : nullptr;

    // Parameter changes made off the GUI thread (automation during playback) are
    // applied to the controller here; IEditController is GUI-thread only.
    auto* syncQueue = static_cast<Vst3LockFreeQueue<PendingParamChange, kQueueCapacity>*>(m_controllerSyncQueue);
    if (ctrl && syncQueue)
    {
        while (auto pc = syncQueue->pop())
        {
            (void)ctrl->setParamNormalized(static_cast<Steinberg::Vst::ParamID>(pc->id), pc->value);
        }
    }
    m_outputParamSlots.drain([&](Vst3ParamID id, double value)
    {
        if (ctrl)
        {
            // Moves the controller's parameter, which is what the plugin's own
            // editor reads its meters from.
            (void)ctrl->setParamNormalized(static_cast<Steinberg::Vst::ParamID>(id), value); // an id the controller does not know is not an error worth reporting
        }
        if (m_paramEditedCb)
        {
            m_paramEditedCb(id, value);
        }
    });
}

void Vst3PluginInstance::setParameterEditedCallback(ParameterEditedCallback cb)
{
    m_paramEditedCb = std::move(cb);
}

void Vst3PluginInstance::setParametersChangedCallback(ParametersChangedCallback cb)
{
    m_paramsChangedCb = std::move(cb);
}

void Vst3PluginInstance::onRestartRequested(int flags)
{
    if (flags & Steinberg::Vst::kParamValuesChanged)
    {
        // The plugin changed parameter values itself (typically: it just
        // loaded one of its own presets) without a performEdit() per
        // parameter, so every host-side mirror of those values is now stale
        // until it re-reads the controller.
        //
        // The VST3 spec puts restartComponent() on the UI thread. If a
        // plugin calls it from elsewhere, do NOT run the callback there: it
        // touches GUI-thread models. Dropping the notification is safe
        // because the owners also re-read the controller when saving.
        const QCoreApplication* app = QCoreApplication::instance();
        if (app && QThread::currentThread() != app->thread())
        {
            qWarning("Vst3PluginInstance: '%s' reported kParamValuesChanged from a non-GUI thread; "
                     "ignoring (parameter values are re-read when the project is saved)",
                     qPrintable(m_name));
        }
        else if (m_paramsChangedCb)
        {
            m_paramsChangedCb();
        }
    }

    // kReloadComponent / kIoChanged / kLatencyChanged / ... would need the
    // processor torn down and re-initialised; not implemented. Say so rather
    // than silently accepting them.
    const int unhandled = flags & ~Steinberg::Vst::kParamValuesChanged;
    if (unhandled)
    {
        qDebug("Vst3PluginInstance: restartComponent(flags=0x%x) requested but not handled",
               static_cast<unsigned>(unhandled));
    }
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
    Vst3WarnIfFailed(view->setFrame(frame), "IPlugView::setFrame()");

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
        (void)view->removed(); // teardown: nothing useful can be done if this fails
        m_editorAttached = false;
    }
    (void)view->setFrame(nullptr); // teardown: nothing useful can be done if this fails
    view->release();

    if (m_plugFrame)
    {
        auto* frame = static_cast<Vst3PlugFrameImpl*>(m_plugFrame);
        frame->detach();
        frame->release();
        m_plugFrame = nullptr;
    }
}

namespace
{

// Asks the view for IPlugViewContentScaleSupport; the caller releases the
// result.  The interface is looked up by its literal TUID rather than through
// IPlugViewContentScaleSupport::iid: like IPlugFrame (see Vst3PlugFrameIid.cpp)
// that member has no linkable storage in this build, and adding a
// DEF_CLASS_IID for it is not needed just to call the interface.
Steinberg::IPlugViewContentScaleSupport* queryContentScaleSupport(Steinberg::IPlugView* view)
{
    static const Steinberg::TUID kIid = INLINE_UID(0x65ED9690, 0x8AC44525, 0x8AADEF7A, 0x72EA703F);

    void* obj = nullptr;
    if (!view || view->queryInterface(kIid, &obj) != Steinberg::kResultOk)
    {
        return nullptr;
    }
    return static_cast<Steinberg::IPlugViewContentScaleSupport*>(obj);
}

} // namespace

bool Vst3PluginInstance::editorSupportsContentScale() const
{
    if (!m_plugView)
        return false;

    auto* support = queryContentScaleSupport(cast<Steinberg::IPlugView>(m_plugView));
    if (!support)
        return false;

    support->release();
    return true;
}

bool Vst3PluginInstance::setEditorContentScale(float factor)
{
    if (!m_plugView || !(factor > 0.0f))
        return false;

    auto* support = queryContentScaleSupport(cast<Steinberg::IPlugView>(m_plugView));
    if (!support)
        return false;

    // A view that handles the factor answers kResultTrue and then asks the
    // host to resize through IPlugFrame::resizeView() (onEditorResizeRequested
    // below), so there is nothing else to do here.
    const Steinberg::tresult r = support->setContentScaleFactor(factor);
    support->release();

    if (r != Steinberg::kResultTrue)
    {
        qDebug("Vst3PluginInstance: '%s' editor declined content scale %.2f (tresult=%d)",
               qPrintable(m_name), static_cast<double>(factor), static_cast<int>(r));
        return false;
    }
    return true;
}

bool Vst3PluginInstance::editorViewSize(int* widthOut, int* heightOut) const
{
    if (!m_plugView)
        return false;

    Steinberg::ViewRect rect{};
    if (cast<Steinberg::IPlugView>(m_plugView)->getSize(&rect) != Steinberg::kResultOk
        || !editorSizeIsSane(rect.getWidth(), rect.getHeight()))
    {
        return false;
    }

    if (widthOut)  *widthOut  = rect.getWidth();
    if (heightOut) *heightOut = rect.getHeight();
    return true;
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
    (void)cast<Steinberg::IPlugView>(m_plugView)->onSize(&rect); // the plugin may decline a size it cannot honour; the host window is already resized, so there is nothing to recover

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
    const Steinberg::tresult compGetResult = comp->getState(&compStream);
    if (compGetResult != Steinberg::kResultOk)
    {
        // Diagnostics for Task A (PRESTIGE-Phase-4c-State-Fix-and-Knobs.md):
        // getState() failing here was previously silent -- saveState()
        // went on to write whatever partial/empty bytes ended up in
        // compStream into the project as if it had succeeded, so a later
        // restoreState() call would legitimately fail against a genuinely
        // bad blob with no record of why. Not a confirmed cause of the
        // reported symptom (inference only -- no plugin in this session
        // has been observed to fail getState()), but silently swallowing
        // a real SDK failure here made it uninvestigable, so it is now
        // logged and stops the save rather than proceeding.
        qWarning("Vst3PluginInstance: IComponent::getState failed for '%s' (tresult=%d)",
                 qPrintable(m_name), static_cast<int>(compGetResult));
        return {};
    }

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
            const Steinberg::tresult ctrlGetResult = ctrl->getState(&ctrlStream);
            if (ctrlGetResult != Steinberg::kResultOk)
            {
                qWarning("Vst3PluginInstance: IEditController::getState failed for '%s' (tresult=%d)",
                         qPrintable(m_name), static_cast<int>(ctrlGetResult));
                return {};
            }
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

    // Compare as a common 64-bit unsigned type rather than casting compLen
    // down to (signed) int first. The previous
    // "static_cast<int>(compLen) > data.size() - 8" truncated compLen to
    // 32 bits *before* comparing: a compLen at or above 2^31 (impossible
    // for any real preset, but reachable from corrupted/foreign data)
    // reinterprets as negative and can pass a bounds check it should
    // fail, reading past the buffer in the memcpy below. Widening both
    // sides to uint64_t before comparing removes the wraparound instead
    // of relying on realistic preset sizes staying under the danger
    // threshold. This was found during Task A's review of
    // PRESTIGE-Phase-4c-State-Fix-and-Knobs.md; not confirmed as the
    // cause of the reported "state could not be restored" message (a
    // normal preset's compLen is nowhere near 2^31), but it is a real
    // out-of-bounds read for malformed input and is fixed alongside the
    // root cause in Vst3ParameterModel::loadSettings().
    const auto availableAfterHeader = static_cast<std::uint64_t>(data.size()) - 8u;
    if (static_cast<std::uint64_t>(compLen) > availableAfterHeader)
    {
        qWarning("Vst3PluginInstance: restoreState for '%s' rejected -- component length %u "
                 "exceeds available %llu bytes", qPrintable(m_name), compLen,
                 static_cast<unsigned long long>(availableAfterHeader));
        return false;
    }

    QByteArray compBytes(d, static_cast<int>(compLen)); d += compLen;

    // ctrlLen is read from immediately after the component bytes; make
    // sure that 4-byte length field itself is actually present before
    // reading it, and that the ctrl bytes it claims are present too --
    // the pre-fix code trusted both unconditionally.
    const auto availableAfterComp = availableAfterHeader - compLen;
    if (availableAfterComp < 4u)
    {
        qWarning("Vst3PluginInstance: restoreState for '%s' rejected -- missing controller length field",
                 qPrintable(m_name));
        return false;
    }
    std::memcpy(&ctrlLen, d, 4); d += 4;
    const auto availableForCtrl = availableAfterComp - 4u;
    if (static_cast<std::uint64_t>(ctrlLen) > availableForCtrl)
    {
        qWarning("Vst3PluginInstance: restoreState for '%s' rejected -- controller length %u "
                 "exceeds available %llu bytes", qPrintable(m_name), ctrlLen,
                 static_cast<unsigned long long>(availableForCtrl));
        return false;
    }
    QByteArray ctrlBytes(d, static_cast<int>(ctrlLen));

    auto* comp = cast<Steinberg::Vst::IComponent>(m_component);

    Vst3MemoryStream compStream(compBytes);
    const Steinberg::tresult compSetResult = comp->setState(&compStream);
    if (compSetResult != Steinberg::kResultOk)
    {
        qWarning("Vst3PluginInstance: IComponent::setState failed for '%s' (tresult=%d, "
                 "compLen=%u, ctrlLen=%u, hasSeparateControllerState=%d)",
                 qPrintable(m_name), static_cast<int>(compSetResult), compLen, ctrlLen,
                 hasSeparateControllerState() ? 1 : 0);
        return false;
    }

    // Sync controller with new component state
    if (m_controller)
    {
        auto* ctrl = cast<Steinberg::Vst::IEditController>(m_controller);
        Vst3MemoryStream syncStream(compBytes);
        const Steinberg::tresult syncResult = ctrl->setComponentState(&syncStream);
        if (syncResult != Steinberg::kResultOk && syncResult != Steinberg::kNotImplemented)
        {
            // Logged, not fatal: IEditController::setComponentState is
            // documented as optional (kNotImplemented is a legitimate
            // answer for a plugin whose controller doesn't need syncing
            // from component state), and the component state above
            // already applied successfully, so a non-OK/non-
            // kNotImplemented result here is downgraded to a diagnostic
            // rather than failing the whole restore -- per hypothesis 2
            // in Task A, treating every non-OK optional-call result as a
            // hard failure was itself a candidate root cause. This is
            // NOT the confirmed root cause (the confirmed one is in
            // Vst3ParameterModel::loadSettings()); logged here so it can
            // be ruled in or out against a real plugin if presets still
            // misbehave after that fix.
            qWarning("Vst3PluginInstance: IEditController::setComponentState returned "
                     "tresult=%d for '%s' (component state already applied; continuing)",
                     static_cast<int>(syncResult), qPrintable(m_name));
        }

        if (ctrlLen > 0)
        {
            Vst3MemoryStream ctrlStream(ctrlBytes);
            const Steinberg::tresult ctrlSetResult = ctrl->setState(&ctrlStream);
            if (ctrlSetResult != Steinberg::kResultOk)
            {
                qWarning("Vst3PluginInstance: IEditController::setState returned tresult=%d "
                         "for '%s' (component state already applied; continuing)",
                         static_cast<int>(ctrlSetResult), qPrintable(m_name));
            }
        }
    }

    return true;
}

bool Vst3PluginInstance::hasSeparateControllerState() const
{
    if (!m_component || !m_controller) return false;

    auto* comp = cast<Steinberg::Vst::IComponent>(m_component);
    auto* ctrl = cast<Steinberg::Vst::IEditController>(m_controller);

    Steinberg::FUnknown* compUnk = nullptr;
    Steinberg::FUnknown* ctrlUnk = nullptr;
    comp->queryInterface(Steinberg::FUnknown::iid, reinterpret_cast<void**>(&compUnk));
    ctrl->queryInterface(Steinberg::FUnknown::iid, reinterpret_cast<void**>(&ctrlUnk));
    const bool sameObj = (compUnk == ctrlUnk);
    if (compUnk) compUnk->release();
    if (ctrlUnk) ctrlUnk->release();

    return !sameObj;
}

QByteArray Vst3PluginInstance::saveComponentState() const
{
    if (!m_component) return {};

    auto* comp = cast<Steinberg::Vst::IComponent>(m_component);
    Vst3MemoryStream stream;
    const Steinberg::tresult getResult = comp->getState(&stream);
    if (getResult != Steinberg::kResultOk)
    {
        // A failed (possibly partial) state must not be written into the
        // project as if it were the plugin's real state.
        Vst3WarnIfFailed(getResult, "IComponent::getState() while saving");
        return {};
    }
    return stream.buffer();
}

QByteArray Vst3PluginInstance::saveControllerState() const
{
    // Deliberately empty for a single-object plugin: there is nothing
    // distinct from the component's own state to save (see the header's
    // doc comment), and an empty return is exactly what
    // restoreControllerState() below refuses to accept, keeping the two
    // symmetric.
    if (!hasSeparateControllerState()) return {};

    auto* ctrl = cast<Steinberg::Vst::IEditController>(m_controller);
    Vst3MemoryStream stream;
    const Steinberg::tresult getResult = ctrl->getState(&stream);
    if (getResult != Steinberg::kResultOk)
    {
        Vst3WarnIfFailed(getResult, "IEditController::getState() while saving");
        return {};
    }
    return stream.buffer();
}

bool Vst3PluginInstance::restoreComponentState(const QByteArray& data)
{
    if (!m_component || data.isEmpty()) return false;

    auto* comp = cast<Steinberg::Vst::IComponent>(m_component);
    Vst3MemoryStream compStream(data);
    const Steinberg::tresult compSetResult = comp->setState(&compStream);
    if (compSetResult != Steinberg::kResultOk)
    {
        qWarning("Vst3PluginInstance: IComponent::setState (separate-format restore) failed "
                 "for '%s' (tresult=%d, dataLen=%d)",
                 qPrintable(m_name), static_cast<int>(compSetResult), data.size());
        return false;
    }

    // Sync controller with the just-restored component state, same as
    // restoreState() already does for the combined-blob path above.
    if (m_controller)
    {
        auto* ctrl = cast<Steinberg::Vst::IEditController>(m_controller);
        Vst3MemoryStream syncStream(data);
        const Steinberg::tresult syncResult = ctrl->setComponentState(&syncStream);
        if (syncResult != Steinberg::kResultOk && syncResult != Steinberg::kNotImplemented)
        {
            // See the matching comment in restoreState(): optional call,
            // logged rather than treated as a hard failure.
            qWarning("Vst3PluginInstance: IEditController::setComponentState returned "
                     "tresult=%d for '%s' (component state already applied; continuing)",
                     static_cast<int>(syncResult), qPrintable(m_name));
        }
    }
    return true;
}

bool Vst3PluginInstance::restoreControllerState(const QByteArray& data)
{
    if (!hasSeparateControllerState() || data.isEmpty()) return false;

    auto* ctrl = cast<Steinberg::Vst::IEditController>(m_controller);
    Vst3MemoryStream stream(data);
    const Steinberg::tresult result = ctrl->setState(&stream);
    if (result != Steinberg::kResultOk)
    {
        qWarning("Vst3PluginInstance: IEditController::setState (separate-format restore) "
                 "failed for '%s' (tresult=%d, dataLen=%d)",
                 qPrintable(m_name), static_cast<int>(result), data.size());
        return false;
    }
    return true;
}

} // namespace lmms
