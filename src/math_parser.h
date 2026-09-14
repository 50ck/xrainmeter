/* Copyright (C) 2011 Rainmeter Project Developers
 *
 * This Source Code Form is subject to the terms of the GNU General Public
 * License; either version 2 of the License, or (at your option) any later
 * version. If a copy of the GPL was not distributed with this file, You can
 * obtain one at <https://www.gnu.org/licenses/gpl-2.0.html>. */

// Adapted from Rainmeter 4.5.26 Common/MathParser at commit
// 5a124b6a09e2f7f67f8be9232718c489100e6173 for portable C++17 types.

// Heavily based on ccalc 0.5.1 by Walery Studennikov <hqsoftware@mail.ru>

#ifndef RM_COMMON_MATHPARSER_H_
#define RM_COMMON_MATHPARSER_H_

#include <cwchar>

namespace MathParser
{
	typedef bool (*GetValueFunc)(const wchar_t* str, int len, double* value, void* context);

	const wchar_t* Check(const wchar_t* formula);
	const wchar_t* CheckedParse(const wchar_t* formula, double* result);
	const wchar_t* Parse(
		const wchar_t* formula, double* result,
		GetValueFunc getValue = nullptr, void* getValueContext = nullptr);

	bool IsDelimiter(wchar_t ch);
};

#endif
