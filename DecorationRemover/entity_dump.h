#pragma once

#include <string>

// Debug-only world inspection. Walks every entity source this plugin can
// reach -- the Mass representation/visualisation system, Mass agent actors,
// the Chimera persistent-entity ID registry, and the biomes spawner system --
// and writes a pretty-printed JSON snapshot next to the plugin DLL.
//
// Read-only: nothing here modifies game state. Keypress-triggered only, since
// a full walk is an ObjectWalker sweep plus potentially hundreds of thousands
// of instance transforms.
namespace DecorationRemoverDebug
{
	// Writes one dump file. Returns the path written, or an empty string on
	// failure (the reason is logged).
	std::string DumpEntitiesToJson();
}

// Implemented in plugin.cpp -- lets the dump annotate each biomes species with
// the plugin's current removal decision without duplicating the filter logic.
bool DecorationRemover_SpeciesWouldBeRemoved(const std::string& speciesName);
