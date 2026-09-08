/*
 * Vst3ControlBase.cpp - Vst3 control base class
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

#include "Vst3ControlBase.h"

#ifdef LMMS_HAVE_VST3

#include <QDebug>
#include <QDomDocument>
#include <QDomElement>

#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/vst/ivstevents.h"
#include "public.sdk/source/common/memorystream.h"
#include "public.sdk/source/vst/hosting/hostclasses.h"

#include "AudioEngine.h"
#include "AutomatableModel.h"
#include "Engine.h"
#include "Model.h"
#include "SampleFrame.h"

using namespace Steinberg;
using namespace Steinberg::Vst;

namespace lmms
{


Vst3ControlBase::Vst3ControlBase(Model* that, const QString& pluginPath, const QString& classId, bool isInstrument) :
	m_isInstrument(isInstrument),
	m_inputEvents(64)
{
	instantiate(pluginPath, classId);
	if (m_valid) { buildParameterModels(that); }
}


Vst3ControlBase::~Vst3ControlBase()
{
	teardown();
}


void Vst3ControlBase::fail(const QString& step, int32_t tresultCode)
{
	m_valid = false;
	m_errorString = QString("%1 (tresult=%2)").arg(step).arg(tresultCode);
	qCritical() << "Vst3ControlBase:" << m_errorString;
}


void Vst3ControlBase::instantiate(const QString& pluginPath, const QString& classId)
{
	// 1) module load
	std::string error;
	m_module = VST3::Hosting::Module::create(pluginPath.toStdString(), error);
	if (!m_module)
	{
		fail(QString("module load: %1").arg(QString::fromStdString(error)), 0);
		return;
	}
	m_pluginName = QString::fromStdString(m_module->getName());

	// 2) GetPluginFactory, find the requested class (falls back to the
	// first audio-effect-category class if classId is empty or not found --
	// keeps this usable even if a caller only has a bare module path)
	auto& factory = m_module->getFactory();
	VST3::Hosting::ClassInfo effectClass;
	bool found = false;
	for (auto& ci : factory.classInfos())
	{
		if (QString::fromStdString(ci.ID().toString()) == classId) { effectClass = ci; found = true; break; }
	}
	if (!found && classId.isEmpty())
	{
		for (auto& ci : factory.classInfos())
		{
			if (ci.category() == kVstAudioEffectClass) { effectClass = ci; found = true; break; }
		}
	}
	if (!found) { fail("no matching class in factory", 0); return; }

	static HostApplication s_hostApp;

	// 3) create + initialize IComponent
	m_component = factory.createInstance<IComponent>(effectClass.ID());
	if (!m_component) { fail("factory.createInstance<IComponent>", 0); return; }
	if (m_component->initialize(&s_hostApp) != kResultOk) { fail("component->initialize", 0); return; }

	// 4) find/create + initialize IEditController
	FUnknownPtr<IEditController> sameObjectController(m_component);
	m_singleComponent = sameObjectController.getInterface() != nullptr;
	if (m_singleComponent)
	{
		m_controller = IPtr<IEditController>(sameObjectController);
	}
	else
	{
		TUID controllerCID{};
		if (m_component->getControllerClassId(controllerCID) == kResultTrue)
		{
			m_controller = factory.createInstance<IEditController>(VST3::UID::fromTUID(controllerCID));
			if (m_controller && m_controller->initialize(&s_hostApp) != kResultOk)
			{
				fail("controller->initialize", 0);
				return;
			}
		}
	}

	// 5) connect via IConnectionPoint (only meaningful for separate objects)
	if (m_controller && !m_singleComponent)
	{
		FUnknownPtr<IConnectionPoint> compCP(m_component);
		FUnknownPtr<IConnectionPoint> ctrlCP(m_controller);
		if (compCP && ctrlCP)
		{
			m_componentCPProxy = owned(new ConnectionProxy(compCP));
			m_controllerCPProxy = owned(new ConnectionProxy(ctrlCP));
			m_componentCPProxy->connect(ctrlCP);
			m_controllerCPProxy->connect(compCP);
		}
	}

	// 6) IAudioProcessor lives on the component
	FUnknownPtr<IAudioProcessor> processorPtr(m_component);
	if (!processorPtr) { fail("component does not implement IAudioProcessor", 0); return; }
	m_processor = processorPtr;

	// 7) bus discovery. Try to negotiate stereo on both main buses if the
	// plugin reports something else -- most effects/instruments accept
	// this; if it's refused we fall back to whatever the plugin reports
	// (mono-only plugins get their single channel duplicated to L/R on
	// output, and only the left LMMS channel on input -- see
	// copyBuffers*Lmms; a real dual-mono-instance approach like Lv2's
	// would be more correct but is more than this experimental fork's
	// Phase 2 needs).
	const int32 numAudioIn = m_component->getBusCount(kAudio, kInput);
	const int32 numAudioOut = m_component->getBusCount(kAudio, kOutput);
	if (numAudioOut < 1 && !m_isInstrument) { fail("effect exposes no audio output bus", 0); return; }

	if (numAudioOut >= 1)
	{
		SpeakerArrangement stereoIn = SpeakerArr::kStereo, stereoOut = SpeakerArr::kStereo;
		m_processor->setBusArrangements(numAudioIn > 0 ? &stereoIn : nullptr, numAudioIn > 0 ? 1 : 0, &stereoOut, 1);
	}

	if (numAudioIn > 0)
	{
		BusInfo inInfo{};
		m_component->getBusInfo(kAudio, kInput, 0, inInfo);
		m_component->activateBus(kAudio, kInput, 0, true);
		m_numChannelsIn = inInfo.channelCount;
	}
	if (numAudioOut > 0)
	{
		BusInfo outInfo{};
		m_component->getBusInfo(kAudio, kOutput, 0, outInfo);
		m_component->activateBus(kAudio, kOutput, 0, true);
		m_numChannelsOut = outInfo.channelCount;
	}

	// Event (MIDI) input bus, for instruments
	const int32 numEventIn = m_component->getBusCount(kEvent, kInput);
	if (numEventIn > 0)
	{
		m_component->activateBus(kEvent, kInput, 0, true);
		m_hasEventInput = true;
	}

	// 8) setupProcessing / setActive / setProcessing
	const auto* audioEngine = Engine::audioEngine();
	m_blockSize = static_cast<int32>(audioEngine->framesPerPeriod());
	ProcessSetup setup{};
	setup.processMode = kRealtime;
	setup.symbolicSampleSize = kSample32;
	setup.maxSamplesPerBlock = m_blockSize;
	setup.sampleRate = audioEngine->outputSampleRate();
	if (m_processor->setupProcessing(setup) != kResultOk) { fail("setupProcessing", 0); return; }
	if (m_component->setActive(true) != kResultOk) { fail("component->setActive(true)", 0); return; }
	m_processor->setProcessing(true); // optional notification, see Phase 1 report

	if (!m_processData.prepare(*m_component, m_blockSize, kSample32)) { fail("HostProcessData::prepare", 0); return; }
	m_inputParamChanges.setMaxParameters(256);

	m_valid = true;
}


void Vst3ControlBase::teardown()
{
	if (!m_component) { return; }
	if (m_processor) { m_processor->setProcessing(false); }
	m_component->setActive(false);
	if (m_componentCPProxy) { m_componentCPProxy->disconnect(); }
	if (m_controllerCPProxy) { m_controllerCPProxy->disconnect(); }
	if (m_controller && !m_singleComponent) { m_controller->terminate(); }
	m_component->terminate();
	m_ports.clear();
	m_controller.reset();
	m_component.reset();
	m_module.reset();
}


void Vst3ControlBase::buildParameterModels(Model* that)
{
	if (!m_controller) { return; }
	const int32 count = m_controller->getParameterCount();
	m_ports.reserve(count);
	for (int32 i = 0; i < count; ++i)
	{
		ParameterInfo pi{};
		if (m_controller->getParameterInfo(i, pi) != kResultOk) { continue; }
		if (pi.flags & ParameterInfo::kIsBypass) { continue; }
		if (pi.flags & ParameterInfo::kIsProgramChange) { continue; }
		if (pi.flags & ParameterInfo::kIsReadOnly) { continue; }

		const QString displayName = QString::fromUtf16(reinterpret_cast<const char16_t*>(pi.title));
		const double initial = m_controller->getParamNormalized(pi.id);
		// VST3 parameters are always normalized [0,1] from the host's
		// perspective; the plugin maps that internally to whatever
		// physical range/curve it wants (see getParamStringByValue for
		// display, which is a Phase 3 GUI concern, not needed here).
		auto* model = new FloatModel(
			static_cast<float>(initial), 0.0f, 1.0f, 0.001f, that, displayName);
		m_ports.push_back(Port{pi.id, model, initial});
	}
}


QString Vst3ControlBase::labelAt(std::size_t i) const
{
	return m_ports.at(i).model->displayName();
}


void Vst3ControlBase::copyModelsFromLmms()
{
	m_inputParamChanges.clearQueue();
	for (auto& port : m_ports)
	{
		const double v = port.model->value<float>();
		if (v != port.lastSyncedValue)
		{
			int32 queueIndex = 0;
			IParamValueQueue* q = m_inputParamChanges.addParameterData(port.id, queueIndex);
			if (q)
			{
				int32 pointIndex = 0;
				q->addPoint(0, v, pointIndex);
			}
			port.lastSyncedValue = v;
		}
	}
	m_processData.inputParameterChanges = &m_inputParamChanges;
}


void Vst3ControlBase::copyModelsToLmms()
{
	if (!m_processData.outputParameterChanges) { return; }
	IParameterChanges* out = m_processData.outputParameterChanges;
	const int32 n = out->getParameterCount();
	for (int32 i = 0; i < n; ++i)
	{
		IParamValueQueue* q = out->getParameterData(i);
		if (!q) { continue; }
		const ParamID id = q->getParameterId();
		const int32 pointCount = q->getPointCount();
		if (pointCount < 1) { continue; }
		int32 sampleOffset = 0;
		ParamValue value = 0.0;
		if (q->getPoint(pointCount - 1, sampleOffset, value) != kResultOk) { continue; }
		for (auto& port : m_ports)
		{
			if (port.id == id)
			{
				port.model->setValue(static_cast<float>(value));
				port.lastSyncedValue = value;
				break;
			}
		}
	}
}


void Vst3ControlBase::syncModelsFromController()
{
	if (!m_controller) { return; }
	for (auto& port : m_ports)
	{
		const double v = m_controller->getParamNormalized(port.id);
		port.model->setValue(static_cast<float>(v));
		port.lastSyncedValue = v;
	}
}


QByteArray Vst3ControlBase::saveState() const
{
	if (!m_valid) { return {}; }
	MemoryStream stream;
	if (m_component->getState(&stream) != kResultOk) { return {}; }
	return QByteArray(stream.getData(), static_cast<int>(stream.getSize()));
}


bool Vst3ControlBase::restoreState(const QByteArray& data)
{
	if (!m_valid || data.isEmpty()) { return false; }

	// setState reads from the current cursor, so a fresh stream per call
	// (not reused) -- MemoryStream's ctor here wraps our bytes without
	// copying/taking ownership.
	{
		MemoryStream stream(const_cast<char*>(data.constData()), data.size());
		if (m_component->setState(&stream) != kResultOk) { return false; }
	}

	// Per IEditController::setComponentState's own doc comment, the
	// controller doesn't learn about a processor-side state restore
	// automatically -- the host must forward the same data. Separate
	// stream instance since setState may have consumed/repositioned the
	// first one's cursor.
	if (m_controller && !m_singleComponent)
	{
		MemoryStream stream(const_cast<char*>(data.constData()), data.size());
		m_controller->setComponentState(&stream);
	}

	// The plugin's own setState can change parameter values the host
	// never explicitly set (that's the whole point of a chunk vs. just
	// replaying parameter values) -- pull the controller's new idea of
	// each value back into our AutomatableModels.
	syncModelsFromController();
	return true;
}


void Vst3ControlBase::copyBuffersFromLmms(const SampleFrame* buf, f_cnt_t frames)
{
	if (m_numChannelsIn <= 0) { return; }
	float** in = m_processData.inputs[0].channelBuffers32;
	for (f_cnt_t i = 0; i < frames; ++i)
	{
		in[0][i] = buf[i].left();
		if (m_numChannelsIn > 1) { in[1][i] = buf[i].right(); }
	}
}


void Vst3ControlBase::copyBuffersToLmms(SampleFrame* buf, f_cnt_t frames) const
{
	if (m_numChannelsOut <= 0) { return; }
	float** out = m_processData.outputs[0].channelBuffers32;
	for (f_cnt_t i = 0; i < frames; ++i)
	{
		const float l = out[0][i];
		const float r = m_numChannelsOut > 1 ? out[1][i] : l; // duplicate mono -> stereo
		buf[i] = SampleFrame(l, r);
	}
}


void Vst3ControlBase::noteOn(int16_t pitch, float velocity, int32_t noteId)
{
	if (!m_hasEventInput) { return; }
	Event e{};
	e.busIndex = 0;
	e.sampleOffset = 0;
	e.type = Event::kNoteOnEvent;
	e.noteOn.channel = 0;
	e.noteOn.pitch = pitch;
	e.noteOn.tuning = 0.f;
	e.noteOn.velocity = velocity;
	e.noteOn.length = 0;
	e.noteOn.noteId = noteId;
	m_inputEvents.addEvent(e);
}


void Vst3ControlBase::noteOff(int16_t pitch, float velocity, int32_t noteId)
{
	if (!m_hasEventInput) { return; }
	Event e{};
	e.busIndex = 0;
	e.sampleOffset = 0;
	e.type = Event::kNoteOffEvent;
	e.noteOff.channel = 0;
	e.noteOff.pitch = pitch;
	e.noteOff.tuning = 0.f;
	e.noteOff.velocity = velocity;
	e.noteOff.noteId = noteId;
	m_inputEvents.addEvent(e);
}


void Vst3ControlBase::run(f_cnt_t frames)
{
	if (!m_valid) { return; }
	m_processData.numSamples = static_cast<int32>(frames);
	if (m_hasEventInput) { m_processData.inputEvents = &m_inputEvents; }
	m_processor->process(m_processData);
	m_inputEvents.clear();
}


void Vst3ControlBase::saveSettings(QDomDocument& doc, QDomElement& that)
{
	that.setAttribute("plugin", m_pluginName);

	const QByteArray chunk = saveState();
	if (!chunk.isEmpty())
	{
		that.setAttribute("chunk", QString::fromLatin1(chunk.toBase64()));
		return;
	}

	// Plugin doesn't support/return a state chunk -- fall back to saving
	// each parameter's normalized value individually (Phase 2).
	for (std::size_t i = 0; i < m_ports.size(); ++i)
	{
		QDomElement paramNode = doc.createElement(QString("param%1").arg(i));
		paramNode.setAttribute("id", static_cast<qulonglong>(m_ports[i].id));
		paramNode.setAttribute("value", m_ports[i].model->value<float>());
		that.appendChild(paramNode);
	}
}


void Vst3ControlBase::loadSettings(const QDomElement& that)
{
	if (that.hasAttribute("chunk"))
	{
		const QByteArray chunk = QByteArray::fromBase64(that.attribute("chunk").toLatin1());
		if (restoreState(chunk)) { return; }
		// Chunk present but rejected (e.g. incompatible plugin version) --
		// fall through and see if per-parameter values are also there.
	}

	for (std::size_t i = 0; i < m_ports.size(); ++i)
	{
		QDomElement paramNode = that.firstChildElement(QString("param%1").arg(i));
		if (paramNode.isNull()) { continue; }
		const float v = paramNode.attribute("value").toFloat();
		m_ports[i].model->setValue(v);
		m_ports[i].lastSyncedValue = v;
	}
}


} // namespace lmms

#endif // LMMS_HAVE_VST3
