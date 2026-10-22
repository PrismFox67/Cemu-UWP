#pragma once

// Interface language of the Xbox frontend. The texts are English in the code, Tr() looks them up in a small built-in table
// (Portuguese (Brazil) and Spanish to start), <user data>/translations/<code>.txt can add or replace texts
// (lines "English text<TAB>translation"). The idea follows danprice142/Cemu-UWP's UwpTranslation (MPL-2.0)

#include <string>
#include <vector>

namespace HostTranslation
{
	struct Language
	{
		const char* code; // "" follows the console language
		const char* name; // in its own language
	};
	const std::vector<Language>& GetLanguages();

	// the active language: the setting, or the console language when it is "" (English when there is no table for it)
	std::string GetActiveLanguage();
	const char* Tr(const char* english);
}

// translated text, the English text when there is no translation
#define _TR(text) HostTranslation::Tr(text)
