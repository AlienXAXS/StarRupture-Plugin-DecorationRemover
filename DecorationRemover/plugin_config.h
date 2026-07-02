#pragma once

#include "plugin_interface.h"

namespace DecorationRemoverConfig
{
	// One toggleable decoration group: a config key under [Removal], the
	// species-name substrings (lowercase) it covers, and its default state.
	struct DecorationGroup
	{
		const char* key;          // config key under [Removal]
		const char* description;  // label shown in the plugin config UI
		const char* defaultValue; // "true"/"false"
		const char* const* filters;   // lowercase species-name substrings
		int         filterCount;
	};

	namespace Filters
	{
		static const char* const TundraGrass[]  = { "tundragrass" };
		static const char* const CottonGrass[]  = { "cottongrass" };
		static const char* const Sedge[]        = { "carex" };
		static const char* const Moss[]         = { "moss" };
		static const char* const DwarfBirch[]   = { "dwarfbirch" };
		static const char* const Crowberries[]  = { "crowberr" };
		static const char* const GoldFruitTree[]= { "goldfruittree" };
		static const char* const Thornfruit[]   = { "thornfruit" };
		static const char* const Oxallop[]      = { "oxallop" };
		static const char* const Aggressive[]   = { "aggressiveplant" };
		static const char* const DeadBush[]     = { "deadbush" };
		static const char* const SmallRocks[]   = { "gravel_rock", "gravel_sparse", "jaggedgravel", "rocksnatural", "rocksangular" };
		static const char* const Cliffs[]       = { "cliff_" };
		static const char* const OreGravel[]    = { "wolframgravel", "heliumgravel", "calciumgravel", "sulphur_gravel" };
	}

	#define DECO_GROUP(key, desc, def, arr) { key, desc, def, arr, sizeof(arr) / sizeof(arr[0]) }

	// Everything defaults to kept — users opt in per group via the config UI.
	// Polifruit and Hydrobulb have no group at all: they are food sources and
	// must never be removed.
	static const DecorationGroup DECORATION_GROUPS[] = {
		DECO_GROUP("RemoveGoldFruitTrees",   "Gold fruit trees (incl. fallen branches)",       "false", Filters::GoldFruitTree),
		DECO_GROUP("RemoveDwarfBirch",       "Dwarf birch bushes",                             "false", Filters::DwarfBirch),
		DECO_GROUP("RemoveThornfruit",       "Thornfruit plants (base, stems, vines)",         "false", Filters::Thornfruit),
		DECO_GROUP("RemoveAggressivePlants", "Aggressive plants (roots and branches)",         "false", Filters::Aggressive),
		DECO_GROUP("RemoveDeadBush",         "Dead bushes (Sulheart roots)",                   "false", Filters::DeadBush),
		DECO_GROUP("RemoveOxallop",          "Oxallop plants (may be gatherable)",             "false", Filters::Oxallop),
		DECO_GROUP("RemoveCrowberries",      "Crowberry shrubs (low ground cover)",            "false", Filters::Crowberries),
		DECO_GROUP("RemoveTundraGrass",      "Tundra grass",                                   "false", Filters::TundraGrass),
		DECO_GROUP("RemoveCottonGrass",      "Cotton grass",                                   "false", Filters::CottonGrass),
		DECO_GROUP("RemoveSedgeGrass",       "Sedge grass (Carex)",                            "false", Filters::Sedge),
		DECO_GROUP("RemoveMoss",             "Moss clusters and moss detail",                  "false", Filters::Moss),
		DECO_GROUP("RemoveSmallRocks",       "Small rocks and loose gravel",                   "false", Filters::SmallRocks),
		DECO_GROUP("RemoveCliffDecorations", "Cliff decorations (moss/gravel on cliff faces)", "false", Filters::Cliffs),
		DECO_GROUP("RemoveOreGravel",        "Ore surface gravel (marks ore veins!)",          "false", Filters::OreGravel),
	};

	#undef DECO_GROUP

	static const int DECORATION_GROUP_COUNT = sizeof(DECORATION_GROUPS) / sizeof(DECORATION_GROUPS[0]);

	// General entries + one boolean per decoration group, built at static init.
	static const ConfigEntry GENERAL_ENTRIES[] = {
		{
			"General",
			"Enabled",
			ConfigValueType::Boolean,
			"true",
			"Enable or disable the Decoration Remover plugin"
		},
		{
			"General",
			"LogSpecies",
			ConfigValueType::Boolean,
			"false",
			"Log each decoration species (and its mesh assets) the first time it is seen.  This will make your game lag."
		},
		{
			"Removal",
			"CustomFilters",
			ConfigValueType::String,
			"",
			"Extra comma-separated, case-insensitive species-name substrings to remove (for species not covered by the toggles)"
		}
	};

	static const int GENERAL_ENTRY_COUNT = sizeof(GENERAL_ENTRIES) / sizeof(GENERAL_ENTRIES[0]);

	// Type-safe config accessor class
	class Config
	{
	public:
		static void Initialize(IPluginSelf* self)
		{
			s_self = self;
			if (!s_self)
				return;

			BuildSchema();
			s_self->config->InitializeFromSchema(s_self, &s_schema);
		}

		static bool IsEnabled()
		{
			return s_self ? s_self->config->ReadBool(s_self, "General", "Enabled", true) : true;
		}

		static bool ShouldLogSpecies()
		{
			return s_self ? s_self->config->ReadBool(s_self, "General", "LogSpecies", true) : true;
		}

		static bool IsGroupEnabled(const DecorationGroup& group)
		{
			bool def = group.defaultValue[0] == 't';
			return s_self ? s_self->config->ReadBool(s_self, "Removal", group.key, def) : def;
		}

		static const char* GetCustomFilters()
		{
			static char buffer[1024];
			if (s_self && s_self->config->ReadString(s_self, "Removal", "CustomFilters", buffer, sizeof(buffer), ""))
			{
				return buffer;
			}
			return "";
		}

	private:
		static void BuildSchema()
		{
			int n = 0;
			for (; n < GENERAL_ENTRY_COUNT; n++)
				s_entries[n] = GENERAL_ENTRIES[n];

			for (int g = 0; g < DECORATION_GROUP_COUNT; g++, n++)
			{
				s_entries[n] = {
					"Removal",
					DECORATION_GROUPS[g].key,
					ConfigValueType::Boolean,
					DECORATION_GROUPS[g].defaultValue,
					DECORATION_GROUPS[g].description
				};
			}

			s_schema = { s_entries, n };
		}

		static ConfigEntry  s_entries[GENERAL_ENTRY_COUNT + DECORATION_GROUP_COUNT];
		static ConfigSchema s_schema;
		static IPluginSelf* s_self;
	};
}
