/*
 * ProjectJournal.cpp - implementation of ProjectJournal
 *
 * Copyright (c) 2006-2014 Tobias Doerffel <tobydox/at/users.sourceforge.net>
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

#include <cstdlib>
#include <QDomElement>

#include "ProjectJournal.h"
#include "Engine.h"
#include "JournallingObject.h"
#include "lmms_math.h"
#include "Song.h"
#include "AutomationClip.h"

namespace lmms
{

//! Avoid clashes between loaded IDs (have the bit cleared)
//! and newly created IDs (have the bit set)
static const int EO_ID_MSB = 1 << 23;

const int ProjectJournal::MAX_UNDO_STATES = 100; // TODO: make this configurable in settings

ProjectJournal::ProjectJournal() :
	m_joIDs(),
	m_undoCheckPoints(),
	m_redoCheckPoints(),
	m_journalling( false )
{
}




namespace
{

//! Disables journalling on a ProjectJournal and restores the previous state on
//! every exit path (including exceptions), so the flag cannot get stuck off.
class JournallingSuspender
{
public:
	explicit JournallingSuspender(ProjectJournal& journal)
		: m_journal(journal)
		, m_previous(journal.isJournalling())
	{
		m_journal.setJournalling(false);
	}

	~JournallingSuspender() { m_journal.setJournalling(m_previous); }

	JournallingSuspender(const JournallingSuspender&) = delete;
	JournallingSuspender& operator=(const JournallingSuspender&) = delete;

private:
	ProjectJournal& m_journal;
	bool m_previous;
};

//! Sets a bool for the lifetime of the object and restores the old value afterwards.
class ScopedFlag
{
public:
	ScopedFlag(bool& flag, bool value)
		: m_flag(flag)
		, m_previous(flag)
	{
		m_flag = value;
	}

	~ScopedFlag() { m_flag = m_previous; }

	ScopedFlag(const ScopedFlag&) = delete;
	ScopedFlag& operator=(const ScopedFlag&) = delete;

private:
	bool& m_flag;
	bool m_previous;
};

} // namespace


bool ProjectJournal::stepCheckPoints(CheckPointStack& from, CheckPointStack& to)
{
	// An undo/redo started while another one is still restoring would corrupt both stacks.
	if (m_restoring) { return false; }

	// Never rewrite project state while a project is loading or being exported.
	if (const auto* song = Engine::getSong(); song && (song->isLoadingProject() || song->isExporting()))
	{
		return false;
	}

	while (!from.isEmpty())
	{
		CheckPoint c = from.pop();

		// journallingObject() looks up without inserting; operator[] on the id
		// map would add a null entry for every unknown id.
		JournallingObject* jo = journallingObject(c.joID);
		const QDomElement saved = c.data.content().firstChildElement();

		// Entries whose object is gone, or that hold no state, cannot be restored
		// (restoring an empty element could reset the object to defaults).
		// Drop them and try the next older entry.
		if (!jo || saved.isNull()) { continue; }

		// Remember what we are about to overwrite so the opposite action can bring it back.
		DataFile previousState(DataFile::Type::JournalData);
		jo->saveState(previousState, previousState.content());
		const bool canReverse = !previousState.content().firstChildElement().isNull();

		try
		{
			const JournallingSuspender suspender(*this);
			const ScopedFlag restoring(m_restoring, true);
			jo->restoreState(saved);
		}
		catch (...)
		{
			// Put the entry back so a failed restore does not lose it. The guards
			// above have already restored the journalling flag. Do not let an
			// exception escape into a Qt slot.
			from.push(c);
			return false;
		}

		if (canReverse)
		{
			to.push(CheckPoint(c.joID, previousState));
			if (to.size() > MAX_UNDO_STATES) { to.remove(0, to.size() - MAX_UNDO_STATES); }
		}
		else
		{
			qWarning("ProjectJournal: could not save the current state before restoring; "
					 "the opposite action is not available for this step");
		}

		if (auto* song = Engine::getSong()) { song->setModified(); }

		// Loading AutomationClip connections correctly (needed for undo AND redo)
		if (!c.data.content().elementsByTagName("automationclip").isEmpty())
		{
			AutomationClip::resolveAllIDs();
		}
		return true;
	}
	return false;
}


bool ProjectJournal::undo()
{
	return stepCheckPoints(m_undoCheckPoints, m_redoCheckPoints);
}


bool ProjectJournal::redo()
{
	return stepCheckPoints(m_redoCheckPoints, m_undoCheckPoints);
}

bool ProjectJournal::canUndo() const
{
	return !m_undoCheckPoints.isEmpty();
}

bool ProjectJournal::canRedo() const
{
	return !m_redoCheckPoints.isEmpty();
}



void ProjectJournal::addJournalCheckPoint( JournallingObject *jo )
{
	if (!jo || !isJournalling() || m_restoring) { return; }

	DataFile dataFile( DataFile::Type::JournalData );
	jo->saveState( dataFile, dataFile.content() );

	// saveState() produces nothing when the object has journalling disabled. An
	// empty checkpoint would make a later undo press a silent no-op, and clearing
	// the redo stack for it would throw away valid redo steps.
	if (dataFile.content().firstChildElement().isNull()) { return; }

	m_redoCheckPoints.clear();
	m_undoCheckPoints.push( CheckPoint( jo->id(), dataFile ) );
	if( m_undoCheckPoints.size() > MAX_UNDO_STATES )
	{
		m_undoCheckPoints.remove( 0, m_undoCheckPoints.size() - MAX_UNDO_STATES );
	}
}

jo_id_t ProjectJournal::allocID(JournallingObject* obj)
{
	jo_id_t id = generateId();
	m_unregisteredIds.erase(id);
	m_joIDs[id] = obj;
	return id;
}

jo_id_t ProjectJournal::generateId()
{
	jo_id_t id;
	for (jo_id_t tid = fastRand();
		m_joIDs.contains(id = tid % EO_ID_MSB | EO_ID_MSB) || m_unregisteredIds.contains(id);
		tid++) {}
	m_unregisteredIds.insert(id);
	return id;
}

void ProjectJournal::reallocID( const jo_id_t _id, JournallingObject * _obj )
{
	//printf("realloc %d %d\n", _id, _obj );
//	if( m_joIDs.contains( _id ) )
	{
		m_unregisteredIds.erase(_id);
		m_joIDs[_id] = _obj;
	}
}




jo_id_t ProjectJournal::idToSave( jo_id_t id )
{
	return id & ~EO_ID_MSB;
}

jo_id_t ProjectJournal::idFromSave( jo_id_t id )
{
	return id | EO_ID_MSB;
}




void ProjectJournal::clearJournal()
{
	m_undoCheckPoints.clear();
	m_redoCheckPoints.clear();

	for( JoIdMap::Iterator it = m_joIDs.begin(); it != m_joIDs.end(); )
	{
		if( it.value() == nullptr )
		{
			it = m_joIDs.erase( it );
		}
		else
		{
			++it;
		}
	}
}

void ProjectJournal::stopAllJournalling()
{
	for( JoIdMap::Iterator it = m_joIDs.begin(); it != m_joIDs.end(); ++it)
	{
		if( it.value() != nullptr )
		{
			it.value()->setJournalling(false);
		}
	}
	setJournalling(false);
}



} // namespace lmms
