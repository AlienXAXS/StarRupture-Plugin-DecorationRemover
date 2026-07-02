#include "plugin_config.h"

namespace DecorationRemoverConfig
{
	IPluginSelf* Config::s_self = nullptr;
	ConfigEntry  Config::s_entries[GENERAL_ENTRY_COUNT + DECORATION_GROUP_COUNT] = {};
	ConfigSchema Config::s_schema = {};
}
