#include "plugin.h"
#include "plugin_helpers.h"
#include "plugin_config.h"

#include "SDK/ErrantBiomesRuntime_classes.hpp"

#include <string>
#include <vector>
#include <set>
#include <algorithm>

// Global plugin self pointer — stable for the plugin's lifetime, retained from PluginInit
static IPluginSelf* g_self = nullptr;

IPluginSelf* GetSelf() { return g_self; }

// Plugin metadata
#ifndef MODLOADER_BUILD_TAG
#define MODLOADER_BUILD_TAG "dev"
#endif

static PluginInfo s_pluginInfo = {
	"DecorationRemover",
	MODLOADER_BUILD_TAG,
	"AlienX",
	"Selectively removes world decoration species spawned by the ErrantBiomes runtime",
	PLUGIN_INTERFACE_VERSION,
	PLUGIN_TARGET_CLIENT
};

static bool g_actorHookRegistered = false;
static bool g_worldHookRegistered = false;
static bool g_configHookRegistered = false;
static bool g_inspectKeybindRegistered = false;
static bool g_logSpecies = true;

// Active lowercase species-name substrings, rebuilt from config toggles +
// CustomFilters. Empty = discovery mode.
static std::vector<std::string> g_removeFilters;

// Just the CustomFilters substrings (subset of g_removeFilters), kept
// separately so the inspect keybind can tell a group match from a custom one.
static std::vector<std::string> g_customFilters;

// Species already logged, so discovery output appears once per species.
static std::set<std::string> g_loggedSpecies;

static std::string ToLower(std::string s)
{
	std::transform(s.begin(), s.end(), s.begin(),
		[](unsigned char c) { return (char)std::tolower(c); });
	return s;
}

static void AppendCsvFilters(const char* csv, std::vector<std::string>& out)
{
	std::string token;
	for (const char* p = csv;; p++)
	{
		if (*p == ',' || *p == '\0')
		{
			size_t a = token.find_first_not_of(" \t");
			size_t b = token.find_last_not_of(" \t");
			if (a != std::string::npos)
				out.push_back(ToLower(token.substr(a, b - a + 1)));
			token.clear();
			if (*p == '\0')
				break;
		}
		else
		{
			token += *p;
		}
	}
}

// Re-reads all config values. Returns true if the filter set changed.
// Called at init, on world begin play, and from the config-changed callback.
static bool RefreshConfig()
{
	g_logSpecies = DecorationRemoverConfig::Config::ShouldLogSpecies();

	std::vector<std::string> newFilters;
	std::swap(newFilters, g_removeFilters); // g_removeFilters now empty

	for (int g = 0; g < DecorationRemoverConfig::DECORATION_GROUP_COUNT; g++)
	{
		const auto& group = DecorationRemoverConfig::DECORATION_GROUPS[g];
		if (!DecorationRemoverConfig::Config::IsGroupEnabled(group))
			continue;
		for (int f = 0; f < group.filterCount; f++)
			g_removeFilters.push_back(group.filters[f]);
	}

	g_customFilters.clear();
	AppendCsvFilters(DecorationRemoverConfig::Config::GetCustomFilters(), g_customFilters);
	for (const auto& f : g_customFilters)
		g_removeFilters.push_back(f);

	if (newFilters == g_removeFilters)
		return false;

	LOG_INFO("Removal filters rebuilt — %d substring(s) active", (int)g_removeFilters.size());
	g_loggedSpecies.clear(); // re-log species with updated [REMOVING/keeping] states
	return true;
}

// Food-source plants that must never be removed, even via CustomFilters.
static const char* const PROTECTED_SPECIES[] = { "polifruit", "hydrobulb" };

static bool SpeciesMatchesFilter(const std::string& speciesLower)
{
	for (const auto* protectedName : PROTECTED_SPECIES)
		if (speciesLower.find(protectedName) != std::string::npos)
			return false;

	for (const auto& filter : g_removeFilters)
		if (speciesLower.find(filter) != std::string::npos)
			return true;
	return false;
}

// Log a species once, with the mesh/asset names it spawns, so the user can
// spot anything not covered by the config toggles.
static void LogSpeciesDiscovery(SDK::UBiomesSpeciesInfo* species, const std::string& speciesName, bool willRemove)
{
	if (!g_logSpecies || !g_loggedSpecies.insert(speciesName).second)
		return;

	std::string assets;
	for (int i = 0; i < species->Assets.Num(); i++)
	{
		SDK::UObject* asset = species->Assets[i];
		if (!asset)
			continue;
		if (!assets.empty())
			assets += ", ";
		assets += asset->GetName();
	}

	LOG_INFO("Species discovered: %s [%s] assets: %s",
		speciesName.c_str(),
		willRemove ? "REMOVING" : "keeping",
		assets.empty() ? "<none>" : assets.c_str());
}

// Clears the instanced mesh instances of any spawner component on this
// container whose species matches the active filter set. Called once per
// container, right as it begins play — cheap, no full-object-list walk.
static void ProcessContainer(SDK::ABiomesRuntimeSpawnerContainer* container)
{
	int clearedComponents = 0;
	int clearedInstances = 0;

	for (int s = 0; s < container->SpawnerComponents.Num(); s++)
	{
		SDK::UBiomesRuntimeSpawnerComponent* spawner = container->SpawnerComponents[s];
		if (!spawner || !spawner->SpeciesInfo)
			continue;

		std::string speciesName = spawner->SpeciesInfo->GetName();
		bool remove = SpeciesMatchesFilter(ToLower(speciesName));

		LogSpeciesDiscovery(spawner->SpeciesInfo, speciesName, remove);

		if (!remove)
			continue;

		for (int p = 0; p < spawner->InstanceComponentPartitions.Num(); p++)
		{
			auto& partition = spawner->InstanceComponentPartitions[p];
			for (int i = 0; i < partition.RuntimeInstanceComponents.Num(); i++)
			{
				SDK::UInstancedStaticMeshComponent* ism = partition.RuntimeInstanceComponents[i];
				if (!ism)
					continue;

				int32_t count = ism->GetInstanceCount();
				if (count <= 0)
					continue;

				ism->ClearInstances();
				clearedComponents++;
				clearedInstances += count;
			}
		}
	}

	if (clearedInstances > 0)
		LOG_DEBUG("Cleared %d instance(s) across %d component(s)", clearedInstances, clearedComponents);
}

// Finds the first decoration group whose filter substrings match this
// species name, regardless of whether that group is currently enabled.
// Used by the inspect keybind to explain *why* a species is or isn't removed.
static const DecorationRemoverConfig::DecorationGroup* FindMatchingGroup(const std::string& speciesLower)
{
	for (int g = 0; g < DecorationRemoverConfig::DECORATION_GROUP_COUNT; g++)
	{
		const auto& group = DecorationRemoverConfig::DECORATION_GROUPS[g];
		for (int f = 0; f < group.filterCount; f++)
			if (speciesLower.find(group.filters[f]) != std::string::npos)
				return &group;
	}
	return nullptr;
}

static const char* FindMatchingCustomFilter(const std::string& speciesLower)
{
	for (const auto& filter : g_customFilters)
		if (speciesLower.find(filter) != std::string::npos)
			return filter.c_str();
	return nullptr;
}

// Reports, via LOG_INFO, exactly why the species under the player's crosshair
// is or isn't being removed -- which toggle (if any) covers it, whether that
// toggle is currently on, or whether it's protected/uncovered.
static void ReportSpeciesFilterState(const std::string& speciesName)
{
	std::string speciesLower = ToLower(speciesName);

	for (const auto* protectedName : PROTECTED_SPECIES)
	{
		if (speciesLower.find(protectedName) != std::string::npos)
		{
			LOG_INFO("Inspect: '%s' matches protected substring '%s' -- NEVER removed, regardless of any toggle", speciesName.c_str(), protectedName);
			return;
		}
	}

	if (const auto* group = FindMatchingGroup(speciesLower))
	{
		bool enabled = DecorationRemoverConfig::Config::IsGroupEnabled(*group);
		LOG_INFO("Inspect: '%s' belongs to group '%s' (%s) -- toggle is currently %s",
			speciesName.c_str(), group->key, group->description, enabled ? "ON (being removed)" : "OFF (being kept)");
		return;
	}

	if (const char* custom = FindMatchingCustomFilter(speciesLower))
	{
		LOG_INFO("Inspect: '%s' matches CustomFilters entry '%s' -- being removed", speciesName.c_str(), custom);
		return;
	}

	LOG_INFO("Inspect: '%s' does not match any group toggle or CustomFilters entry -- being kept by default", speciesName.c_str());
}

// Best-effort identification for a hit component that isn't tracked by any
// UBiomesRuntimeSpawnerComponent (i.e. not part of the species/filter system
// this plugin drives). Reports the component name and, if resolvable via
// reflection, its static mesh asset -- so the user has *something* to go on,
// and knows this decoration's toggles won't do anything for it.
static void DescribeUnmanagedComponent(SDK::AActor* hitActor, SDK::UPrimitiveComponent* hitComponent)
{
	using namespace SDK;

	std::string meshName = "<unresolved>";

	auto* hooks = GetHooks();
	if (hooks && hooks->ObjectProperties && hooks->ObjectProperties->IsReady())
	{
		PluginPropertyHandle prop = hooks->ObjectProperties->FindPropertyOnObject(hitComponent, "StaticMesh");
		void* meshObj = nullptr;
		if (prop && hooks->ObjectProperties->GetObjectProperty(hitComponent, prop, &meshObj) && meshObj)
			meshName = static_cast<UObject*>(meshObj)->GetName();
	}

	LOG_INFO("Inspect: hit '%s' (class: %s) via component '%s' (mesh: %s) -- not managed by the species/spawner system, plugin toggles do not apply to it",
		hitActor->GetName().c_str(),
		hitActor->Class ? hitActor->Class->GetName().c_str() : "<null>",
		hitComponent->GetName().c_str(),
		meshName.c_str());
}

// Searches one container's spawner components for the partition that tracks
// this exact ISM component. Returns the owning spawner, or null.
static SDK::UBiomesRuntimeSpawnerComponent* FindSpawnerTrackingComponent(
	SDK::ABiomesRuntimeSpawnerContainer* container, SDK::UPrimitiveComponent* hitComponent)
{
	for (int s = 0; s < container->SpawnerComponents.Num(); s++)
	{
		SDK::UBiomesRuntimeSpawnerComponent* spawner = container->SpawnerComponents[s];
		if (!spawner || !spawner->SpeciesInfo)
			continue;

		for (int p = 0; p < spawner->InstanceComponentPartitions.Num(); p++)
		{
			auto& partition = spawner->InstanceComponentPartitions[p];
			for (int i = 0; i < partition.RuntimeInstanceComponents.Num(); i++)
				if (partition.RuntimeInstanceComponents[i] == hitComponent)
					return spawner;
		}
	}
	return nullptr;
}

// The runtime ISM components are NOT owned by the spawner container that
// manages them -- they're attached to a plain BiomesPartitionActor (the
// per-grid-cell render partition), so the hit actor's class tells us nothing.
// Walk the live containers and find whichever spawner tracks this component.
// ObjectWalker is expensive, but this only runs on an explicit keypress.
static SDK::UBiomesRuntimeSpawnerComponent* FindSpawnerForComponentGlobal(SDK::UPrimitiveComponent* hitComponent)
{
	auto* hooks = GetHooks();
	if (!hooks || !hooks->ObjectWalker || !hooks->ObjectWalker->IsReady())
		return nullptr;

	static PluginObjectInfo s_matches[512];
	const int capacity = sizeof(s_matches) / sizeof(s_matches[0]);

	int total = hooks->ObjectWalker->FindObjectsByClassNameInto(
		"BiomesRuntimeSpawnerContainer", PluginObjectLookup_InstanceOnly, s_matches, capacity);

	int found = total < capacity ? total : capacity;
	for (int c = 0; c < found; c++)
	{
		auto* container = static_cast<SDK::ABiomesRuntimeSpawnerContainer*>(s_matches[c].object);
		if (!container)
			continue;

		if (SDK::UBiomesRuntimeSpawnerComponent* spawner = FindSpawnerTrackingComponent(container, hitComponent))
			return spawner;
	}
	return nullptr;
}

// Fired by the inspect keybind. Traces from the player's eyes and identifies
// which spawner component (and therefore which species) tracks the specific
// mesh instance under the crosshair.
static void OnInspectKeybindPressed(EModKey /*key*/, EModKeyEvent /*event*/)
{
	using namespace SDK;

	UWorld* world = UWorld::GetWorld();
	if (!world)
		return;

	APlayerController* pc = UGameplayStatics::GetPlayerController(world, 0);
	APawn* pawn = pc ? pc->Pawn : nullptr;
	if (!pawn)
	{
		LOG_INFO("Inspect: no local pawn");
		return;
	}

	FVector  eyeLocation;
	FRotator eyeRotation;
	pawn->GetActorEyesViewPoint(&eyeLocation, &eyeRotation);

	FVector direction     = UKismetMathLibrary::GetForwardVector(eyeRotation);
	FVector startLocation = eyeLocation + direction * 80.0;
	FVector endLocation   = eyeLocation + direction * 5000.0;

	TArray<AActor*> ignoreActors;
	ignoreActors.Add(pawn);

	FHitResult   hitResult;
	FLinearColor noColor{ 0.f, 0.f, 0.f, 0.f };

	bool bHit = UKismetSystemLibrary::LineTraceSingle(
		world, startLocation, endLocation,
		ETraceTypeQuery::TraceTypeQuery1, false,
		ignoreActors, EDrawDebugTrace::None,
		&hitResult, true, noColor, noColor, 0.f);

	if (!bHit)
	{
		LOG_INFO("Inspect: no hit within range");
		return;
	}

	UPrimitiveComponent* hitComponent = hitResult.Component.Get();
	AActor* hitActor = hitComponent ? hitComponent->GetOwner() : nullptr;

	if (!hitActor || !UKismetSystemLibrary::IsValid(hitActor))
	{
		LOG_INFO("Inspect: hit an object with no valid owning actor");
		return;
	}

	// Fast path: if the owner happens to be a container, search it directly.
	if (hitActor->IsA(ABiomesRuntimeSpawnerContainer::StaticClass()))
	{
		auto* container = static_cast<ABiomesRuntimeSpawnerContainer*>(hitActor);
		if (UBiomesRuntimeSpawnerComponent* spawner = FindSpawnerTrackingComponent(container, hitComponent))
		{
			ReportSpeciesFilterState(spawner->SpeciesInfo->GetName());
			return;
		}
	}

	// Normal case: the ISM's owner is a BiomesPartitionActor render cell, not
	// the container that manages it. Search all live containers for whichever
	// spawner tracks this exact component.
	if (UBiomesRuntimeSpawnerComponent* spawner = FindSpawnerForComponentGlobal(hitComponent))
	{
		ReportSpeciesFilterState(spawner->SpeciesInfo->GetName());
		return;
	}

	// Genuinely not tracked by any spawner component -- report what we can.
	DescribeUnmanagedComponent(hitActor, hitComponent);
}

// Fired by the modloader for every AActor::BeginPlay, on the game thread.
// Filters down to spawner containers — the actor type that owns the
// instanced-mesh decoration components — and processes only that one actor.
static void OnActorBeginPlay(void* actorPtr)
{
	auto* actor = static_cast<SDK::AActor*>(actorPtr);

	if (!actor || !actor->IsA(SDK::ABiomesRuntimeSpawnerContainer::StaticClass()))
		return;

	ProcessContainer(static_cast<SDK::ABiomesRuntimeSpawnerContainer*>(actor));
}

// Reset per-world state on a fresh world. Containers themselves are only
// ever processed as they pass through OnActorBeginPlay.
static void OnWorldBeginPlay(SDK::UWorld* /*world*/)
{
	LOG_DEBUG("World begin play — refreshing config");
	RefreshConfig();
	g_loggedSpecies.clear();
}

// Fired when the user edits values in the plugin config UI. Rebuild the
// filter set and re-log species states. Note: instances already cleared only
// come back once their partition re-streams (move away and back, reload the
// save, or restart) — toggling a group back on does not restore them.
static void OnConfigChanged(const char* section, const char* key, const char* /*newValue*/)
{
	// The callback broadcasts every plugin's config edits — only react to ours.
	if (!section || _strnicmp(section, s_pluginInfo.name, strlen(s_pluginInfo.name)) != 0)
		return;

	LOG_INFO("Config changed (%s/%s) — refreshing", section, key);
	RefreshConfig();
}

extern "C" {

	__declspec(dllexport) PluginInfo* GetPluginInfo()
	{
		return &s_pluginInfo;
	}

	__declspec(dllexport) bool PluginInit(IPluginSelf* self)
	{
		// Store the plugin self pointer — valid for the plugin's entire lifetime
		g_self = self;

		LOG_INFO("Plugin initializing...");

		DecorationRemoverConfig::Config::Initialize(self);

		if (!DecorationRemoverConfig::Config::IsEnabled())
		{
			LOG_WARN("Plugin is disabled in config file");
			return true; // Return true so plugin loads but doesn't activate
		}

		RefreshConfig();

		auto* hooks = GetHooks();
		if (!hooks || !hooks->Actors || !hooks->World)
		{
			LOG_ERROR("Required hook interfaces unavailable");
			return false;
		}

		hooks->Actors->RegisterOnActorBeginPlay(OnActorBeginPlay);
		g_actorHookRegistered = true;

		hooks->World->RegisterOnWorldBeginPlay(OnWorldBeginPlay);
		g_worldHookRegistered = true;

		if (hooks->UI)
		{
			hooks->UI->RegisterOnConfigChanged(OnConfigChanged);
			g_configHookRegistered = true;
		}

		if (hooks->Input)
		{
			hooks->Input->RegisterKeybindByName("F6", EModKeyEvent::Pressed, OnInspectKeybindPressed);
			g_inspectKeybindRegistered = true;
		}

		LOG_INFO("Plugin initialized — %d removal filter(s) active. Press F6 while looking at a decoration to inspect its species/filter state.", (int)g_removeFilters.size());

		return true;
	}

	__declspec(dllexport) void PluginShutdown()
	{
		LOG_INFO("Plugin shutting down...");

		auto* hooks = GetHooks();

		if (g_actorHookRegistered)
		{
			if (hooks && hooks->Actors)
				hooks->Actors->UnregisterOnActorBeginPlay(OnActorBeginPlay);
			g_actorHookRegistered = false;
		}

		if (g_worldHookRegistered)
		{
			if (hooks && hooks->World)
				hooks->World->UnregisterOnWorldBeginPlay(OnWorldBeginPlay);
			g_worldHookRegistered = false;
		}

		if (g_configHookRegistered)
		{
			if (hooks && hooks->UI)
				hooks->UI->UnregisterOnConfigChanged(OnConfigChanged);
			g_configHookRegistered = false;
		}

		if (g_inspectKeybindRegistered)
		{
			if (hooks && hooks->Input)
				hooks->Input->UnregisterKeybindByName("F6", EModKeyEvent::Pressed, OnInspectKeybindPressed);
			g_inspectKeybindRegistered = false;
		}

		g_self = nullptr;
	}

} // extern "C"
