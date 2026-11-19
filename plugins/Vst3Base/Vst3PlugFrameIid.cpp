// IPlugFrame's storage (Steinberg::IPlugFrame::iid) isn't provided by
// vstinitiids.cpp -- that file only covers Steinberg::Vst::* interfaces.
// DECLARE_CLASS_IID in pluginterfaces/gui/iplugview.h only creates the
// compile-time UID constant; DEF_CLASS_IID is what gives IPlugFrame::iid
// actual linkable storage. Vst3PluginInstance implements IPlugFrame
// itself (host-side resize callback for the native editor), so this
// host needs that storage directly rather than getting it for free from
// Steinberg's public.sdk VSTGUI helper classes.
//
// Do not add DEF_CLASS_IID for other interfaces here speculatively --
// only add one if a future link error names it specifically. Others
// (IPluginFactory, IBStream, etc.) already resolve elsewhere in this
// vendored tree, and duplicating them causes an ODR "multiple
// definition" link error instead of this one.

#include "pluginterfaces/gui/iplugview.h"

namespace Steinberg {
DEF_CLASS_IID(IPlugFrame)
}
