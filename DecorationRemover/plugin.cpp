#include "plugin.h"
#include "plugin_helpers.h"
#include "plugin_config.h"
#include "entity_dump.h"

#include "SDK/ErrantBiomesRuntime_classes.hpp"

#include <string>
#include <vector>
#include <set>
#include <map>
#include <algorithm>
#include <cmath>

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

// Exposed to entity_dump.cpp so the debug dump can annotate each species with
// the plugin's current removal decision without re-implementing the filters.
bool DecorationRemover_SpeciesWouldBeRemoved(const std::string& speciesName)
{
	return SpeciesMatchesFilter(ToLower(speciesName));
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
	int matchedSpecies = 0;
	int nullSpecies = 0;

	for (int s = 0; s < container->SpawnerComponents.Num(); s++)
	{
		SDK::UBiomesRuntimeSpawnerComponent* spawner = container->SpawnerComponents[s];
		if (!spawner || !spawner->SpeciesInfo)
		{
			nullSpecies++;
			continue;
		}

		std::string speciesName = spawner->SpeciesInfo->GetName();
		bool remove = SpeciesMatchesFilter(ToLower(speciesName));

		LogSpeciesDiscovery(spawner->SpeciesInfo, speciesName, remove);

		if (!remove)
			continue;

		matchedSpecies++;
		LOG_DEBUG("Container '%s': species '%s' matched — zeroing spawn distances (was %.0f sq)",
			container->GetName().c_str(), speciesName.c_str(), spawner->MaxSpawningDistanceSquared);

		// Instances spawn lazily as the player approaches (distance-gated by
		// the spawning subsystem), so clearing once at BeginPlay would miss
		// everything spawned afterwards. Zeroing the spawn distances instead
		// prevents this spawner's instances -- visuals and their collider
		// proxies -- from ever spawning.
		spawner->MaxSpawningDistanceSquared = 0.0;
		spawner->MaxSpawningDistance = 0.0f;
		for (int d = 0; d < spawner->AssetMaxSpawningDistanceSquared.Num(); d++)
			spawner->AssetMaxSpawningDistanceSquared[d] = 0.0;

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

	if (matchedSpecies > 0 || clearedInstances > 0)
		LOG_DEBUG("Container '%s': %d spawner component(s), %d matched filter, %d with null species, cleared %d instance(s) across %d component(s) — %d filter(s) active",
			container->GetName().c_str(), container->SpawnerComponents.Num(), matchedSpecies,
			nullSpecies, clearedInstances, clearedComponents, (int)g_removeFilters.size());
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
static void DescribeUnmanagedComponent(SDK::AActor* hitActor, SDK::UPrimitiveComponent* hitComponent, int32_t hitItem)
{
	using namespace SDK;

	std::string meshName = "<unresolved>";

	auto* hooks = GetHooks();
	if (hooks && hooks->ObjectProperties && hooks->ObjectProperties->IsReady())
	{
		PluginPropertyHandle prop = hooks->ObjectProperties->FindPropertyOnObject(hitComponent, "StaticMesh");
		void* meshObj = nullptr;
		if (prop && hooks->ObjectProperties->GetObjectProperty(hitComponent, prop, &meshObj) && meshObj)
			meshName = static_cast<UObject*>(meshObj)->GetFullName();
	}

	int32 instanceCount = -1;
	if (hitComponent->IsA(UInstancedStaticMeshComponent::StaticClass()))
		instanceCount = static_cast<UInstancedStaticMeshComponent*>(hitComponent)->GetInstanceCount();

	LOG_INFO("Inspect: hit actor '%s' component '%s' (class: %s, item: %d, instances: %d) mesh: %s -- not managed by the species/spawner system, plugin toggles do not apply to it",
		hitActor->GetFullName().c_str(),
		hitComponent->GetName().c_str(),
		hitComponent->Class ? hitComponent->Class->GetName().c_str() : "<null>",
		hitItem,
		instanceCount,
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

// Traces from the player's eyes along the view direction and resolves the hit
// primitive component and its owning actor. Returns false (with a log line
// prefixed by `context`) if there's no pawn, no hit, or no valid owner.
// outItem receives FHitResult::Item -- the instance index when the hit
// component is an instanced static mesh, -1 otherwise.
static bool TraceUnderCrosshair(const char* context, SDK::UPrimitiveComponent** outComponent, SDK::AActor** outActor, int32_t* outItem = nullptr)
{
	using namespace SDK;

	*outComponent = nullptr;
	*outActor = nullptr;
	if (outItem)
		*outItem = -1;

	UWorld* world = UWorld::GetWorld();
	if (!world)
		return false;

	APlayerController* pc = UGameplayStatics::GetPlayerController(world, 0);
	APawn* pawn = pc ? pc->Pawn : nullptr;
	if (!pawn)
	{
		LOG_INFO("%s: no local pawn", context);
		return false;
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
		LOG_INFO("%s: no hit within range", context);
		return false;
	}

	UPrimitiveComponent* hitComponent = hitResult.Component.Get();
	AActor* hitActor = hitComponent ? hitComponent->GetOwner() : nullptr;

	if (!hitActor || !UKismetSystemLibrary::IsValid(hitActor))
	{
		LOG_INFO("%s: hit an object with no valid owning actor", context);
		return false;
	}

	*outComponent = hitComponent;
	*outActor = hitActor;
	if (outItem)
		*outItem = hitResult.Item;
	return true;
}

// Resolves an ISM component's static mesh name via reflection (no generated
// accessor for UStaticMeshComponent::StaticMesh in this SDK dump).
static std::string GetComponentMeshName(SDK::UPrimitiveComponent* component)
{
	auto* hooks = GetHooks();
	if (hooks && hooks->ObjectProperties && hooks->ObjectProperties->IsReady())
	{
		PluginPropertyHandle prop = hooks->ObjectProperties->FindPropertyOnObject(component, "StaticMesh");
		void* meshObj = nullptr;
		if (prop && hooks->ObjectProperties->GetObjectProperty(component, prop, &meshObj) && meshObj)
			return static_cast<SDK::UObject*>(meshObj)->GetName();
	}
	return "<unresolved>";
}

// Diagnostic: logs every ISM/HISM instance within `radius` units of the given
// world location -- component, owner actor, mesh, and distance. This is how
// we find where a decoration's *visual* instance lives when the raycast can
// only ever hit its invisible collider proxy. Keypress-only cost.
static void SurveyNearbyInstances(const char* componentClassName, const SDK::FVector& location, double radius)
{
	using namespace SDK;

	auto* hooks = GetHooks();
	if (!hooks || !hooks->ObjectWalker || !hooks->ObjectWalker->IsReady())
		return;

	static PluginObjectInfo s_matches[2048];
	const int capacity = sizeof(s_matches) / sizeof(s_matches[0]);

	int total = hooks->ObjectWalker->FindObjectsByClassNameInto(
		componentClassName, PluginObjectLookup_InstanceOnly, s_matches, capacity);

	// Aggregate per mesh: prebuilt/merged cluster meshes have ONE instance
	// transform at the cluster origin, so per-instance lines with a tight
	// radius miss them entirely. Summarise instead: how many instances of
	// each mesh sit within radius, and how close the nearest one is.
	struct MeshSummary { int count; double minDist; std::string nearestComponent; std::string nearestOwner; };
	std::map<std::string, MeshSummary> byMesh;

	int found = total < capacity ? total : capacity;
	for (int c = 0; c < found; c++)
	{
		auto* ism = static_cast<UInstancedStaticMeshComponent*>(s_matches[c].object);
		if (!ism)
			continue;

		int32 count = ism->GetInstanceCount();
		std::string meshName; // resolved lazily, once per component with a match

		for (int32 i = 0; i < count; i++)
		{
			FTransform transform;
			if (!ism->GetInstanceTransform(i, &transform, true))
				continue;

			double dx = transform.Translation.X - location.X;
			double dy = transform.Translation.Y - location.Y;
			double dz = transform.Translation.Z - location.Z;
			double distSq = dx * dx + dy * dy + dz * dz;
			if (distSq > radius * radius)
				continue;

			if (meshName.empty())
				meshName = GetComponentMeshName(ism);

			auto& summary = byMesh[meshName];
			summary.count++;
			double dist = std::sqrt(distSq);
			if (summary.count == 1 || dist < summary.minDist)
			{
				summary.minDist = dist;
				summary.nearestComponent = ism->GetName();
				AActor* owner = ism->GetOwner();
				summary.nearestOwner = owner ? owner->GetName() : "<null>";
			}
		}
	}

	for (const auto& [meshName, summary] : byMesh)
		LOG_INFO("  nearby [%s]: mesh=%s x%d nearest=%.0f (component=%s owner=%s)",
			componentClassName, meshName.c_str(), summary.count, summary.minDist,
			summary.nearestComponent.c_str(), summary.nearestOwner.c_str());
}

// Fired by the inspect keybind. Traces from the player's eyes and identifies
// which spawner component (and therefore which species) tracks the specific
// mesh instance under the crosshair.
static void OnInspectKeybindPressed(EModKey /*key*/, EModKeyEvent /*event*/)
{
	using namespace SDK;

	UPrimitiveComponent* hitComponent = nullptr;
	AActor* hitActor = nullptr;
	int32_t hitItem = -1;
	if (!TraceUnderCrosshair("Inspect", &hitComponent, &hitActor, &hitItem))
		return;

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

	// Genuinely not tracked by any spawner component -- report what we can,
	// then survey what else sits near the hit instance (the visual mesh the
	// raycast can't hit directly, since it hits the collider proxy instead).
	DescribeUnmanagedComponent(hitActor, hitComponent, hitItem);

	if (hitItem >= 0 && hitComponent->IsA(UInstancedStaticMeshComponent::StaticClass()))
	{
		FTransform transform;
		if (static_cast<UInstancedStaticMeshComponent*>(hitComponent)->GetInstanceTransform(hitItem, &transform, true))
		{
			LOG_INFO("Inspect: surveying instances within 3000 units of the hit instance (per-mesh summary):");
			SurveyNearbyInstances("InstancedStaticMeshComponent", transform.Translation, 3000.0);
			SurveyNearbyInstances("HierarchicalInstancedStaticMeshComponent", transform.Translation, 3000.0);
		}
	}
}

// Removes any instance sitting at (approximately) the given world location
// from every live ISM/HISM component of the named class. The biomes system
// splits decorations into separate collision and visual instance components
// on different actors, so removing only the hit instance leaves its twin
// behind -- this sweep catches the pair. Keypress-only cost.
static int RemovePairedInstancesAt(const char* componentClassName, const SDK::FVector& location, SDK::UInstancedStaticMeshComponent* alreadyHandled)
{
	using namespace SDK;

	auto* hooks = GetHooks();
	if (!hooks || !hooks->ObjectWalker || !hooks->ObjectWalker->IsReady())
		return 0;

	static PluginObjectInfo s_matches[2048];
	const int capacity = sizeof(s_matches) / sizeof(s_matches[0]);

	int total = hooks->ObjectWalker->FindObjectsByClassNameInto(
		componentClassName, PluginObjectLookup_InstanceOnly, s_matches, capacity);

	const double tolerance = 1.0; // paired instances share the same world transform
	int removed = 0;

	int found = total < capacity ? total : capacity;
	for (int c = 0; c < found; c++)
	{
		auto* ism = static_cast<UInstancedStaticMeshComponent*>(s_matches[c].object);
		if (!ism || ism == alreadyHandled)
			continue;

		int32 count = ism->GetInstanceCount();
		for (int32 i = count - 1; i >= 0; i--)
		{
			FTransform transform;
			if (!ism->GetInstanceTransform(i, &transform, true))
				continue;

			double dx = transform.Translation.X - location.X;
			double dy = transform.Translation.Y - location.Y;
			double dz = transform.Translation.Z - location.Z;
			if (dx * dx + dy * dy + dz * dz > tolerance * tolerance)
				continue;

			if (ism->RemoveInstance(i))
				removed++;
		}
	}

	return removed;
}

// Fired by the delete keybind. If the crosshair is on an instanced static
// mesh, removes that specific instance (plus its paired collision/visual twin
// on other components); otherwise destroys the hit actor via Kismet.
// Dev tool -- aim carefully.
static void OnDeleteKeybindPressed(EModKey /*key*/, EModKeyEvent /*event*/)
{
	using namespace SDK;

	UPrimitiveComponent* hitComponent = nullptr;
	AActor* hitActor = nullptr;
	int32_t hitItem = -1;
	if (!TraceUnderCrosshair("Delete", &hitComponent, &hitActor, &hitItem))
		return;

	bool isInstanced = hitComponent->IsA(UInstancedStaticMeshComponent::StaticClass());
	if (isInstanced && hitItem >= 0)
	{
		auto* ism = static_cast<UInstancedStaticMeshComponent*>(hitComponent);

		// Capture the instance's world location BEFORE removing it, so the
		// paired-twin sweep below knows where to look.
		FTransform transform{};
		bool haveTransform = ism->GetInstanceTransform(hitItem, &transform, true);

		bool removedHit = ism->RemoveInstance(hitItem);

		int removedPaired = 0;
		if (haveTransform)
		{
			removedPaired += RemovePairedInstancesAt("InstancedStaticMeshComponent", transform.Translation, ism);
			removedPaired += RemovePairedInstancesAt("HierarchicalInstancedStaticMeshComponent", transform.Translation, ism);
		}

		LOG_INFO("Delete: removed instance %d from '%s' on '%s' (%s), plus %d paired instance(s) at the same location",
			hitItem,
			hitComponent->GetName().c_str(),
			hitActor->GetName().c_str(),
			removedHit ? "ok" : "remove failed",
			removedPaired);
		return;
	}

	LOG_INFO("Delete: destroying '%s' (class: %s)",
		hitActor->GetName().c_str(),
		hitActor->Class ? hitActor->Class->GetName().c_str() : "<null>");

	hitActor->K2_DestroyActor();
}

// Fired by the purge keybind. One-shot ObjectWalker pass over every live
// spawner (both shapes), clearing all matched species' instances. This is the
// fallback for the fact that the spawning subsystem snapshots spawn distances
// at registration — zeroing them at BeginPlay doesn't stop later spawning, so
// the user purges the already-spawned instances on demand instead.
static void ProcessStandaloneSpawner(SDK::ABiomesRuntimeSpawner* spawner);

static void OnPurgeKeybindPressed(EModKey /*key*/, EModKeyEvent /*event*/)
{
	auto* hooks = GetHooks();
	if (!hooks || !hooks->ObjectWalker || !hooks->ObjectWalker->IsReady())
	{
		LOG_INFO("Purge: ObjectWalker unavailable");
		return;
	}

	static PluginObjectInfo s_matches[2048];
	const int capacity = sizeof(s_matches) / sizeof(s_matches[0]);

	int totalContainers = hooks->ObjectWalker->FindObjectsByClassNameInto(
		"BiomesRuntimeSpawnerContainer", PluginObjectLookup_InstanceOnly, s_matches, capacity);
	int found = totalContainers < capacity ? totalContainers : capacity;
	for (int c = 0; c < found; c++)
		if (auto* container = static_cast<SDK::ABiomesRuntimeSpawnerContainer*>(s_matches[c].object))
			ProcessContainer(container);

	int totalStandalone = hooks->ObjectWalker->FindObjectsByClassNameInto(
		"BiomesRuntimeSpawner", PluginObjectLookup_InstanceOnly, s_matches, capacity);
	found = totalStandalone < capacity ? totalStandalone : capacity;
	for (int c = 0; c < found; c++)
		if (auto* spawner = static_cast<SDK::ABiomesRuntimeSpawner*>(s_matches[c].object))
			ProcessStandaloneSpawner(spawner);

	LOG_INFO("Purge: swept %d container(s) and %d standalone spawner(s) — %d filter(s) active",
		totalContainers, totalStandalone, (int)g_removeFilters.size());
}

// Fired by the entity-dump keybind. Read-only debug snapshot of everything the
// plugin can enumerate, written as JSON -- see entity_dump.cpp. Gated behind
// the [Debug] EntityDump toggle because a whole-world walk hitches the game.
static void OnEntityDumpKeybindPressed(EModKey /*key*/, EModKeyEvent /*event*/)
{
	if (!DecorationRemoverConfig::Config::IsEntityDumpEnabled())
	{
		LOG_INFO("Entity dump: disabled -- enable [Debug] EntityDump in the plugin config to use F9");
		return;
	}

	DecorationRemoverDebug::DumpEntitiesToJson();
}

// The standalone spawner shape: one actor per species with its own soft
// SpeciesInfo, spawn distance, and HISM list -- used alongside the
// container/component shape. Trees and other sparse decorations come
// through here.
static void ProcessStandaloneSpawner(SDK::ABiomesRuntimeSpawner* spawner)
{
	using namespace SDK;

	UBiomesSpeciesInfo* species = spawner->SpeciesInfo.Get();
	if (!species)
	{
		LOG_DEBUG("Standalone spawner '%s' has unresolved SpeciesInfo -- cannot filter it", spawner->GetName().c_str());
		return;
	}

	std::string speciesName = species->GetName();
	bool remove = SpeciesMatchesFilter(ToLower(speciesName));

	LogSpeciesDiscovery(species, speciesName, remove);

	LOG_DEBUG("Standalone spawner '%s': species '%s' — %s (%d filter(s) active)",
		spawner->GetName().c_str(), speciesName.c_str(),
		remove ? "MATCHED, removing" : "no filter match", (int)g_removeFilters.size());

	if (!remove)
		return;

	// Prevent future distance-gated spawning, then clear anything present.
	spawner->MaxSpawningDistance = 0.0f;

	int clearedInstances = 0;
	for (int i = 0; i < spawner->InstancedStaticMeshComponents.Num(); i++)
	{
		UHierarchicalInstancedStaticMeshComponent* ism = spawner->InstancedStaticMeshComponents[i];
		if (!ism)
			continue;

		int32 count = ism->GetInstanceCount();
		if (count <= 0)
			continue;

		ism->ClearInstances();
		clearedInstances += count;
	}

	if (clearedInstances > 0)
		LOG_DEBUG("Standalone spawner '%s': cleared %d instance(s)", speciesName.c_str(), clearedInstances);
}

// Debug: classes already reported by OnActorBeginPlay, so the biomes-actor
// discovery line appears once per class instead of once per actor.
static std::set<std::string> g_loggedBeginPlayClasses;

// Fired by the modloader for every AActor::BeginPlay, on the game thread.
// Filters down to the two biomes spawner shapes — the actor types that own
// the instanced-mesh decoration components — and processes only that actor.
static void OnActorBeginPlay(void* actorPtr)
{
	auto* actor = static_cast<SDK::AActor*>(actorPtr);
	if (!actor)
		return;

	bool isContainer = actor->IsA(SDK::ABiomesRuntimeSpawnerContainer::StaticClass());
	bool isStandalone = !isContainer && actor->IsA(SDK::ABiomesRuntimeSpawner::StaticClass());

	// Debug: surface every biomes-related actor class passing through BeginPlay
	// (once per class), including the ones our IsA gates DON'T match — if the
	// spawners never show up here, the decorations are fed by something else.
	std::string className = actor->Class ? actor->Class->GetName() : "<null>";
	if (className.find("Biomes") != std::string::npos &&
		g_loggedBeginPlayClasses.insert(className).second)
	{
		LOG_INFO("BeginPlay: biomes actor class '%s' (e.g. '%s') — container=%s standalone=%s",
			className.c_str(), actor->GetName().c_str(),
			isContainer ? "yes" : "no", isStandalone ? "yes" : "no");
	}

	if (isContainer)
		ProcessContainer(static_cast<SDK::ABiomesRuntimeSpawnerContainer*>(actor));
	else if (isStandalone)
		ProcessStandaloneSpawner(static_cast<SDK::ABiomesRuntimeSpawner*>(actor));
}

// Reset per-world state on a fresh world. Containers themselves are only
// ever processed as they pass through OnActorBeginPlay.
static void OnWorldBeginPlay(SDK::UWorld* /*world*/)
{
	LOG_DEBUG("World begin play — refreshing config");
	RefreshConfig();
	g_loggedSpecies.clear();
	g_loggedBeginPlayClasses.clear();
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
			hooks->UI->RegisterOnConfigChanged(self, OnConfigChanged);
			g_configHookRegistered = true;
		}

		if (hooks->Input)
		{
			hooks->Input->RegisterKeybindByName("F6", EModKeyEvent::Pressed, OnInspectKeybindPressed);
			hooks->Input->RegisterKeybindByName("F7", EModKeyEvent::Pressed, OnDeleteKeybindPressed);
			hooks->Input->RegisterKeybindByName("F8", EModKeyEvent::Pressed, OnPurgeKeybindPressed);
			hooks->Input->RegisterKeybindByName("F9", EModKeyEvent::Pressed, OnEntityDumpKeybindPressed);
			g_inspectKeybindRegistered = true;
		}

		LOG_INFO("Plugin initialized — %d removal filter(s) active. F6 = inspect decoration under crosshair, F7 = delete actor under crosshair, F8 = purge all filtered decoration now, F9 = dump entities to JSON ([Debug] EntityDump).", (int)g_removeFilters.size());

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
				hooks->UI->UnregisterOnConfigChanged(GetSelf(), OnConfigChanged);
			g_configHookRegistered = false;
		}

		if (g_inspectKeybindRegistered)
		{
			if (hooks && hooks->Input)
			{
				hooks->Input->UnregisterKeybindByName("F6", EModKeyEvent::Pressed, OnInspectKeybindPressed);
				hooks->Input->UnregisterKeybindByName("F7", EModKeyEvent::Pressed, OnDeleteKeybindPressed);
				hooks->Input->UnregisterKeybindByName("F8", EModKeyEvent::Pressed, OnPurgeKeybindPressed);
					hooks->Input->UnregisterKeybindByName("F9", EModKeyEvent::Pressed, OnEntityDumpKeybindPressed);
			}
			g_inspectKeybindRegistered = false;
		}

		g_self = nullptr;
	}

} // extern "C"
