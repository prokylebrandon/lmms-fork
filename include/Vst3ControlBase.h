/*
 * Vst3ControlBase.h - Vst3 control base class
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

#ifndef LMMS_VST3_CONTROL_BASE_H
#define LMMS_VST3_CONTROL_BASE_H

#include "lmmsconfig.h"

#ifdef LMMS_HAVE_VST3

#include <memory>
#include <vector>

#include <QByteArray>
#include <QString>

#include "public.sdk/source/vst/hosting/module.h"
#include "public.sdk/source/vst/hosting/processdata.h"
#include "public.sdk/source/vst/hosting/parameterchanges.h"
#include "public.sdk/source/vst/hosting/eventlist.h"
#include "public.sdk/source/vst/hosting/connectionproxy.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"

#include "lmms_export.h"
#include "LmmsTypes.h"

class QDomDocument;
class QDomElement;

namespace lmms
{

class AutomatableModel;
class Model;
class SampleFrame;

/**
	Common in-process host for a single loaded VST3 plugin instance.

	Deliberately NOT a Model/QObject subclass -- Instrument and
	EffectControls both already inherit QObject, and this class needs to
	sit alongside either of them via multiple inheritance without causing
	a QObject diamond. Vst3Instrument multi-inherits this directly
	(matching Lv2Instrument); Vst3Effect instead holds a Vst3FxControls
	that multi-inherits EffectControls + Vst3ControlBase (matching
	Lv2Effect/Lv2FxControls), since Effect and EffectControls are already
	split classes in LMMS's own design.

	Unlike Lv2ControlBase, this does not maintain a vector of per-channel
	processors for mono plugins run twice. VST3 buses can usually be
	renegotiated to stereo via setBusArrangements; if a plugin insists on
	mono, we fall back to feeding it the left channel only and duplicating
	its mono output to both LMMS output channels. That's a real,
	documented simplification versus Lv2's dual-instance approach -- fine
	for an experimental fork, called out explicitly here and in the
	phase report.
*/
class LMMS_EXPORT Vst3ControlBase
{
public:
	//! @param that the class inheriting this class (and inheriting Model)
	//! @param pluginPath path to the .vst3 bundle
	//! @param classId the specific class within the module's factory to
	//!   instantiate, as returned by Vst3SubPluginFeatures discovery
	//! @param isInstrument whether to request note/event input
	Vst3ControlBase(Model* that, const QString& pluginPath, const QString& classId, bool isInstrument);
	Vst3ControlBase(const Vst3ControlBase&) = delete;
	virtual ~Vst3ControlBase();
	Vst3ControlBase& operator=(const Vst3ControlBase&) = delete;

	//! False if the module/class failed to load or the mandatory hosting
	//! sequence (create/initialize/connect/activate) failed anywhere.
	//! Callers must check this before calling run()/copyBuffers*().
	bool isValid() const { return m_valid; }
	const QString& errorString() const { return m_errorString; }
	const QString& pluginName() const { return m_pluginName; }

	std::size_t controlCount() const { return m_ports.size(); }
	//! Accessors for the generic knob view (Phase 3) -- deliberately
	//! narrow (model + label only) rather than exposing Port/m_ports
	//! directly, so the GUI layer can't reach into hosting internals.
	AutomatableModel* modelAt(std::size_t i) const { return m_ports.at(i).model; }
	QString labelAt(std::size_t i) const;

	/*
		utils for the run thread -- must be called from virtuals in the
		child class, same contract as Lv2ControlBase
	*/

	//! Push any LMMS-side AutomatableModel changes into queued VST3
	//! parameter-change points for the next run()
	void copyModelsFromLmms();
	//! Reflect parameter changes the plugin itself reported (e.g. internal
	//! modulation, MIDI-learn) back onto the AutomatableModels
	void copyModelsToLmms();

	//! Copy LMMS' interleaved stereo buffer into our input bus
	void copyBuffersFromLmms(const SampleFrame* buf, f_cnt_t frames);
	//! Copy our output bus into LMMS' interleaved stereo buffer
	void copyBuffersToLmms(SampleFrame* buf, f_cnt_t frames) const;

	//! Queue a note-on/note-off for the next run() (instruments only;
	//! no-op if the plugin has no event input bus)
	void noteOn(int16_t pitch, float velocity, int32_t noteId);
	void noteOff(int16_t pitch, float velocity, int32_t noteId);

	//! Run the plugin for @param frames frames (must be <= the block size
	//! passed to init())
	void run(f_cnt_t frames);

	void saveSettings(QDomDocument& doc, QDomElement& that);
	void loadSettings(const QDomElement& that);
	static QString nodeName() { return "vst3controls"; }

	//! Raw IComponent state chunk (Phase 4). Empty if the plugin doesn't
	//! support/return one -- caller (saveSettings) falls back to the
	//! Phase 2 per-parameter save in that case, matching VstPlugin.cpp's
	//! own chunk-with-fallback convention.
	QByteArray saveState() const;
	//! Restores a chunk saved by saveState(), then resyncs this
	//! instance's AutomatableModels from the controller's post-restore
	//! values (a plugin's setState can change parameters the host never
	//! explicitly set). Returns false if the plugin rejected the chunk.
	bool restoreState(const QByteArray& data);

	bool hasNoteInput() const { return m_isInstrument && m_hasEventInput; }

private:
	void instantiate(const QString& pluginPath, const QString& classId);
	void teardown();
	void buildParameterModels(Model* that);
	void syncModelsFromController();
	void fail(const QString& step, int32_t tresultCode);

	bool m_isInstrument;
	bool m_valid = false;
	bool m_hasEventInput = false;
	QString m_errorString;
	QString m_pluginName;

	VST3::Hosting::Module::Ptr m_module;
	Steinberg::IPtr<Steinberg::Vst::IComponent> m_component;
	Steinberg::IPtr<Steinberg::Vst::IEditController> m_controller;
	bool m_singleComponent = false;
	Steinberg::IPtr<Steinberg::Vst::ConnectionProxy> m_componentCPProxy;
	Steinberg::IPtr<Steinberg::Vst::ConnectionProxy> m_controllerCPProxy;
	Steinberg::Vst::IAudioProcessor* m_processor = nullptr; // lifetime owned by m_component

	int32_t m_numChannelsIn = 0;
	int32_t m_numChannelsOut = 0;
	int32_t m_blockSize = 0;

	Steinberg::Vst::HostProcessData m_processData;
	Steinberg::Vst::ParameterChanges m_inputParamChanges;
	Steinberg::Vst::EventList m_inputEvents;

	//! One entry per plugin parameter that isn't the bypass toggle.
	//! Ordering is stable (factory enumeration order) so save/load by
	//! index round-trips even without persisting VST3 param IDs by name.
	struct Port
	{
		Steinberg::Vst::ParamID id;
		//! Raw, not unique_ptr: constructed with `that` as its QObject
		//! parent (see buildParameterModels), so Qt's parent-child
		//! ownership already deletes this -- a smart pointer on top would
		//! be a double-free. Confirmed the hard way: this was a
		//! unique_ptr in the first draft and it segfaulted on teardown.
		AutomatableModel* model;
		double lastSyncedValue;
	};
	std::vector<Port> m_ports;
};


} // namespace lmms

#endif // LMMS_HAVE_VST3

#endif // LMMS_VST3_CONTROL_BASE_H
