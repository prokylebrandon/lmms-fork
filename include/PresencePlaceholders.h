/*
 * PresencePlaceholders.h - placeholder registry and text templates for
 *                          Discord Rich Presence
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

#ifndef LMMS_GUI_PRESENCE_PLACEHOLDERS_H
#define LMMS_GUI_PRESENCE_PLACEHOLDERS_H

#include <functional>
#include <vector>

#include <QHash>
#include <QSet>
#include <QString>

namespace lmms::gui
{

//! Maps placeholder names (e.g. "bpm") to functions producing their current value.
//!
//! Adding a placeholder is a single add() call; nothing else needs to change,
//! the settings dialog lists whatever is registered.
class PlaceholderRegistry
{
public:
	using Resolver = std::function<QString()>;

	//! Where a value comes from. Polled values have no change event, so the
	//! presence manager has to refresh them on a timer (and only if used).
	enum class Source
	{
		Event,
		Polled
	};

	struct Entry
	{
		QString name;			//!< lower-case, without the surrounding '%'
		QString description;
		Resolver resolver;
		Source source;
	};

	//! Registers (or replaces) a placeholder. A resolver returns an empty
	//! string when the information is unavailable.
	void add(const QString& name, const QString& description, Resolver resolver, Source source = Source::Event);

	bool contains(const QString& name) const;
	QString resolve(const QString& name) const;
	Source sourceOf(const QString& name) const;
	const std::vector<Entry>& entries() const { return m_entries; }

private:
	std::vector<Entry> m_entries;
	QHash<QString, int> m_index;
};


//! A user-written text such as "Working on %projectname%", parsed once into
//! literal and placeholder segments so rendering needs no pattern matching.
//!
//! - Names are case-insensitive and consist of letters, digits and '_'.
//! - A '%' that does not form a complete "%name%" is kept as a literal '%'.
//! - A well-formed placeholder that the registry does not know is kept
//!   verbatim, so typos stay visible to the user.
//! - A known placeholder without a value renders as an empty string.
class PresenceTemplate
{
public:
	PresenceTemplate() = default;
	explicit PresenceTemplate(const QString& text) { setText(text); }

	void setText(const QString& text);
	const QString& text() const { return m_text; }

	//! Lower-case names of all well-formed placeholders in the text
	const QSet<QString>& placeholders() const { return m_names; }
	bool uses(const QString& name) const { return m_names.contains(name.toLower()); }

	QString render(const PlaceholderRegistry& registry) const;

private:
	struct Segment
	{
		QString raw;		//!< literal text, or the original "%Name%" spelling
		QString name;		//!< lower-case name; empty for literal segments
		bool isPlaceholder;
	};

	QString m_text;
	std::vector<Segment> m_segments;
	QSet<QString> m_names;
};


//! Makes text safe and tidy for a Discord presence field:
//! - control and invisible formatting characters are removed
//! - whitespace is collapsed
//! - separators left dangling by empty placeholders ("| 140 BPM") are removed
//! - the result is truncated on a character boundary to at most maxBytes of UTF-8
QString sanitizePresenceText(const QString& text, int maxBytes = 128);

} // namespace lmms::gui

#endif // LMMS_GUI_PRESENCE_PLACEHOLDERS_H
