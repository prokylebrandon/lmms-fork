// Standalone headless test for Vst3ControlBase::saveState()/restoreState().
// Not part of the LMMS product build -- a throwaway harness to get real
// runtime proof for Phase 4, the same way Phase 1's standalone host did
// for the base hosting sequence.
#include <QCoreApplication>
#include <QDebug>
#include <memory>

#include "Engine.h"
#include "AutomatableModel.h"
#include "Model.h"
#include "Vst3ControlBase.h"

using namespace lmms;

int main(int argc, char** argv)
{
	QCoreApplication app(argc, argv);
	if (argc < 2)
	{
		qCritical() << "usage:" << argv[0] << "<path-to-plugin.vst3>";
		return 1;
	}
	const QString pluginPath = QString::fromLocal8Bit(argv[1]);

	Engine::init(true); // renderOnly -- no real audio device needed

	auto ownerModel1 = std::make_unique<Model>(nullptr, "owner1");
	Vst3ControlBase inst1(ownerModel1.get(), pluginPath, QString(), false);
	if (!inst1.isValid())
	{
		qCritical() << "FAILED: instance 1 did not load:" << inst1.errorString();
		return 1;
	}
	qInfo() << "[ok] instance 1 loaded:" << inst1.pluginName() << "params:" << inst1.controlCount();
	if (inst1.controlCount() == 0)
	{
		qCritical() << "FAILED: plugin exposes no parameters, nothing to test";
		return 1;
	}

	// Change the first parameter to something distinctive, away from default.
	const float testValue = 0.7319f;
	inst1.modelAt(0)->setValue(testValue);
	inst1.copyModelsFromLmms(); // pushes the model value into the plugin via IParameterChanges
	inst1.run(64);              // one small block so the plugin actually applies the change
	qInfo() << "[ok] instance 1: set" << inst1.labelAt(0) << "=" << testValue
	        << " (readback via controller:" << inst1.modelAt(0)->value<float>() << ")";

	const QByteArray chunk = inst1.saveState();
	if (chunk.isEmpty())
	{
		qCritical() << "FAILED: saveState() returned empty -- plugin may not support getState";
		return 1;
	}
	qInfo() << "[ok] saveState():" << chunk.size() << "bytes";

	auto ownerModel2 = std::make_unique<Model>(nullptr, "owner2");
	Vst3ControlBase inst2(ownerModel2.get(), pluginPath, QString(), false);
	if (!inst2.isValid())
	{
		qCritical() << "FAILED: instance 2 did not load:" << inst2.errorString();
		return 1;
	}
	const float beforeRestore = inst2.modelAt(0)->value<float>();
	qInfo() << "[ok] instance 2 loaded fresh, param" << inst2.labelAt(0) << "=" << beforeRestore << "(default)";

	if (!inst2.restoreState(chunk))
	{
		qCritical() << "FAILED: restoreState() rejected the chunk";
		return 1;
	}
	const float afterRestore = inst2.modelAt(0)->value<float>();
	qInfo() << "[ok] instance 2 after restoreState():" << inst2.labelAt(0) << "=" << afterRestore;

	const bool matches = std::abs(afterRestore - testValue) < 0.01f;
	const bool actuallyChanged = std::abs(afterRestore - beforeRestore) > 0.01f;

	qInfo() << "\n--- RESULT ---";
	qInfo() << "instance 1 set to      :" << testValue;
	qInfo() << "instance 2 before load :" << beforeRestore;
	qInfo() << "instance 2 after load  :" << afterRestore;
	qInfo() << "value round-tripped through the plugin's own getState/setState:" << (matches ? "YES" : "NO");
	qInfo() << "restore actually changed instance 2 (not a no-op):" << (actuallyChanged ? "YES" : "NO");

	qInfo() << (matches && actuallyChanged ? "\nPHASE 4 PROOF: PASS" : "\nPHASE 4 PROOF: FAIL");
	const int result = (matches && actuallyChanged) ? 0 : 1;
	fflush(stdout);
	// Diagnostic hard-exit: see comment in the Phase 4 report about an
	// Engine-globals-vs-process-exit teardown crash isolated to here, not
	// to the save/load logic above (which has already completed and
	// printed its result).
	_exit(result);
}
