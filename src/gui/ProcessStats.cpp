/*
 * ProcessStats.cpp - memory and CPU time of the running process
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

#include "ProcessStats.h"

#if defined(_WIN32)
#	ifndef WIN32_LEAN_AND_MEAN
#		define WIN32_LEAN_AND_MEAN
#	endif
#	ifndef NOMINMAX
#		define NOMINMAX
#	endif
#	include <windows.h>
#	include <psapi.h>
#else
#	include <sys/resource.h>
#	include <unistd.h>
#	if defined(__APPLE__)
#		include <mach/mach.h>
#	else
#		include <cstdio>
#	endif
#endif

namespace lmms::gui::ProcessStats
{

#if defined(_WIN32)

std::uint64_t residentBytes()
{
	// K32GetProcessMemoryInfo lives in kernel32.dll on Windows 7 and later.
	// Resolving it at run time avoids linking psapi.lib / -lpsapi.
	using GetMemoryInfoFn = BOOL(WINAPI*)(HANDLE, PROCESS_MEMORY_COUNTERS*, DWORD);
	static const GetMemoryInfoFn getMemoryInfo = []() -> GetMemoryInfoFn
	{
		const HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
		if (!kernel) { return nullptr; }
		return reinterpret_cast<GetMemoryInfoFn>(
			reinterpret_cast<void (*)()>(GetProcAddress(kernel, "K32GetProcessMemoryInfo")));
	}();

	if (!getMemoryInfo) { return 0; }

	PROCESS_MEMORY_COUNTERS counters{};
	counters.cb = sizeof(counters);
	if (!getMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters))) { return 0; }
	return static_cast<std::uint64_t>(counters.WorkingSetSize);
}


std::int64_t cpuTimeMicros()
{
	FILETIME creation, exit, kernel, user;
	if (!GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user)) { return -1; }

	const auto toTicks = [](const FILETIME& ft)
	{
		return (static_cast<std::uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
	};

	// FILETIME counts 100 ns intervals
	return static_cast<std::int64_t>((toTicks(kernel) + toTicks(user)) / 10);
}

#else

std::uint64_t residentBytes()
{
#	if defined(__APPLE__)
	mach_task_basic_info info{};
	mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
	if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info), &count) != KERN_SUCCESS)
	{
		return 0;
	}
	return static_cast<std::uint64_t>(info.resident_size);
#	else
	// /proc/self/statm: "size resident shared ..." measured in pages
	std::FILE* file = std::fopen("/proc/self/statm", "r");
	if (!file) { return 0; }

	unsigned long size = 0;
	unsigned long resident = 0;
	const int fields = std::fscanf(file, "%lu %lu", &size, &resident);
	std::fclose(file);
	if (fields != 2) { return 0; }

	const long pageSize = sysconf(_SC_PAGESIZE);
	return pageSize > 0 ? static_cast<std::uint64_t>(resident) * static_cast<std::uint64_t>(pageSize) : 0;
#	endif
}


std::int64_t cpuTimeMicros()
{
	rusage usage{};
	if (getrusage(RUSAGE_SELF, &usage) != 0) { return -1; }

	const auto toMicros = [](const timeval& tv)
	{
		return static_cast<std::int64_t>(tv.tv_sec) * 1000000 + static_cast<std::int64_t>(tv.tv_usec);
	};

	return toMicros(usage.ru_utime) + toMicros(usage.ru_stime);
}

#endif

} // namespace lmms::gui::ProcessStats
