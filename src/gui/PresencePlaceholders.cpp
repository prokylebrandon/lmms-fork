/*
 * PresencePlaceholders.cpp - placeholder registry and text templates for
 *                            Discord Rich Presence
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

#include "PresencePlaceholders.h"

#include <QByteArray>
#include <QChar>
#include <QRegularExpression>
#include <utility>

namespace lmms::gui
{

namespace
{

bool isNameChar(QChar c)
{
	return c.unicode() < 128 && (c.isLetterOrNumber() || c == QLatin1Char('_'));
}

} // namespace


void PlaceholderRegistry::add(const QString& name, const QString& description, Resolver resolver, Source source)
{
	Entry entry{name.toLower(), description, std::move(resolver), source};

	const auto it = m_index.constFind(entry.name);
	if (it != m_index.constEnd())
	{
		m_entries[static_cast<std::size_t>(it.value())] = std::move(entry);
		return;
	}

	m_index.insert(entry.name, static_cast<int>(m_entries.size()));
	m_entries.push_back(std::move(entry));
}


bool PlaceholderRegistry::contains(const QString& name) const
{
	return m_index.contains(name.toLower());
}


QString PlaceholderRegistry::resolve(const QString& name) const
{
	const auto it = m_index.constFind(name.toLower());
	if (it == m_index.constEnd()) { return {}; }

	const Resolver& resolver = m_entries[static_cast<std::size_t>(it.value())].resolver;
	return resolver ? resolver() : QString{};
}


PlaceholderRegistry::Source PlaceholderRegistry::sourceOf(const QString& name) const
{
	const auto it = m_index.constFind(name.toLower());
	if (it == m_index.constEnd()) { return Source::Event; }
	return m_entries[static_cast<std::size_t>(it.value())].source;
}


void PresenceTemplate::setText(const QString& text)
{
	m_text = text;
	m_segments.clear();
	m_names.clear();

	QString literal;
	const int size = text.size();
	int i = 0;

	const auto flushLiteral = [&]()
	{
		if (literal.isEmpty()) { return; }
		m_segments.push_back({literal, QString{}, false});
		literal.clear();
	};

	while (i < size)
	{
		if (text[i] != QLatin1Char('%'))
		{
			literal += text[i];
			++i;
			continue;
		}

		int j = i + 1;
		while (j < size && isNameChar(text[j])) { ++j; }

		if (j < size && j > i + 1 && text[j] == QLatin1Char('%'))
		{
			flushLiteral();
			const QString name = text.mid(i + 1, j - i - 1).toLower();
			m_segments.push_back({text.mid(i, j - i + 1), name, true});
			m_names.insert(name);
			i = j + 1;
		}
		else
		{
			literal += QLatin1Char('%');
			++i;
		}
	}

	flushLiteral();
}


QString PresenceTemplate::render(const PlaceholderRegistry& registry) const
{
	QString out;
	out.reserve(m_text.size() + 16);

	for (const Segment& segment : m_segments)
	{
		if (!segment.isPlaceholder) { out += segment.raw; }
		else if (registry.contains(segment.name)) { out += registry.resolve(segment.name); }
		else { out += segment.raw; }
	}

	return out;
}


QString sanitizePresenceText(const QString& text, int maxBytes)
{
	// Control characters become spaces, invisible formatting characters
	// (bidi overrides, zero-width spaces, ...) are dropped. U+200D is kept
	// because emoji sequences need it.
	QString cleaned;
	cleaned.reserve(text.size());
	for (const QChar c : text)
	{
		const auto category = c.category();
		if (category == QChar::Other_Control || category == QChar::Separator_Line
			|| category == QChar::Separator_Paragraph)
		{
			cleaned += QLatin1Char(' ');
		}
		else if (category == QChar::Other_Format && c.unicode() != 0x200D)
		{
			continue;
		}
		else
		{
			cleaned += c;
		}
	}

	// Collapses all whitespace runs to a single space and trims the ends
	QString result = cleaned.simplified();

	// "A |  | B" (an empty placeholder between two separators) -> "A | B"
	static const QRegularExpression repeated(
		QStringLiteral(R"(\s*([|\x{2022}\x{00B7}])(?:\s*\1)+\s*)"));
	result.replace(repeated, QStringLiteral(" \\1 "));

	// Separators dangling at either end because a placeholder was empty
	static const QRegularExpression leading(
		QStringLiteral(R"(^[\s|\x{2022}\x{00B7}/\\,;:\-\x{2013}\x{2014}]+)"));
	static const QRegularExpression trailing(
		QStringLiteral(R"([\s|\x{2022}\x{00B7}/\\,;:\-\x{2013}\x{2014}]+$)"));
	result.remove(leading);
	result.remove(trailing);
	result = result.simplified();

	// Truncate on a code point boundary so the UTF-8 form fits maxBytes
	constexpr int ellipsisBytes = 3; // U+2026 is three bytes in UTF-8
	const QByteArray utf8 = result.toUtf8();
	if (maxBytes > ellipsisBytes && utf8.size() > maxBytes)
	{
		int cut = maxBytes - ellipsisBytes;
		while (cut > 0 && (static_cast<unsigned char>(utf8[cut]) & 0xC0) == 0x80) { --cut; }
		result = QString::fromUtf8(utf8.constData(), cut).trimmed() + QChar(0x2026);
	}

	return result;
}

} // namespace lmms::gui
