#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "entity_dump.h"
#include "plugin_helpers.h"
#include "plugin_config.h"

#include "SDK/Engine_classes.hpp"
#include "SDK/MassRepresentation_classes.hpp"
#include "SDK/MassActors_classes.hpp"
#include "SDK/ChimeraMassCommon_classes.hpp"
#include "SDK/ErrantBiomesRuntime_classes.hpp"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <ostream>
#include <string>
#include <vector>

namespace DecorationRemoverDebug
{

// Safety valve: a whole-world dump with instances enabled can run into the
// millions of rows. Past this many instance lines the dump stops emitting
// them and flags itself as truncated, so a stray keypress can never fill the
// user's disk.
static const long long MAX_INSTANCE_ROWS = 500000;

// ---------------------------------------------------------------------------
// Minimal pretty-printing JSON writer
// ---------------------------------------------------------------------------

// Just enough for this dump: nested objects/arrays, scalars, two-space
// indent, and a Raw() escape hatch so the enormous instance arrays stay at one
// line per instance instead of one line per coordinate.
class JsonWriter
{
public:
	explicit JsonWriter(std::ostream& out) : m_out(out) {}

	void BeginObject(const char* key = nullptr) { Prefix(key); m_out << '{'; m_stack.push_back(true); }
	void EndObject()                            { Close(); m_out << '}'; }
	void BeginArray(const char* key = nullptr)  { Prefix(key); m_out << '['; m_stack.push_back(true); }
	void EndArray()                             { Close(); m_out << ']'; }

	void String(const char* key, const std::string& value) { Prefix(key); m_out << '"' << Escape(value) << '"'; }
	void Int(const char* key, long long value)             { Prefix(key); m_out << value; }
	void Bool(const char* key, bool value)                 { Prefix(key); m_out << (value ? "true" : "false"); }
	void Null(const char* key)                             { Prefix(key); m_out << "null"; }

	void Number(const char* key, double value)
	{
		Prefix(key);
		if (!std::isfinite(value)) // NaN/inf are not valid JSON
		{
			m_out << "null";
			return;
		}
		char buffer[64];
		snprintf(buffer, sizeof(buffer), "%.3f", value);
		m_out << buffer;
	}

	// Pre-formatted single-line JSON, written verbatim.
	void Raw(const char* key, const char* json) { Prefix(key); m_out << json; }

	// Trailing newline once the root object is closed.
	void Finish() { m_out << '\n'; }

private:
	void Indent(size_t depth)
	{
		for (size_t i = 0; i < depth; i++)
			m_out << "  ";
	}

	void Prefix(const char* key)
	{
		if (!m_stack.empty())
		{
			if (!m_stack.back())
				m_out << ',';
			m_stack.back() = false;
			m_out << '\n';
			Indent(m_stack.size());
		}
		if (key)
			m_out << '"' << key << "\": ";
	}

	// Closes the current container: an empty one stays on a single line.
	void Close()
	{
		bool empty = m_stack.empty() || m_stack.back();
		if (!m_stack.empty())
			m_stack.pop_back();
		if (!empty)
		{
			m_out << '\n';
			Indent(m_stack.size());
		}
	}

	static std::string Escape(const std::string& value)
	{
		std::string out;
		out.reserve(value.size() + 8);
		for (unsigned char c : value)
		{
			switch (c)
			{
			case '"':  out += "\\\""; break;
			case '\\': out += "\\\\"; break;
			case '\n': out += "\\n";  break;
			case '\r': out += "\\r";  break;
			case '\t': out += "\\t";  break;
			default:
				if (c < 0x20)
				{
					char buffer[8];
					snprintf(buffer, sizeof(buffer), "\\u%04x", c);
					out += buffer;
				}
				else
				{
					out += (char)c;
				}
			}
		}
		return out;
	}

	std::ostream&     m_out;
	std::vector<bool> m_stack; // one entry per open container: "still empty?"
};

// ---------------------------------------------------------------------------
// Shared dump state
// ---------------------------------------------------------------------------

struct DumpContext
{
	double        radius = 0.0;          // 0 = no distance filter
	bool          includeInstances = true;
	bool          havePlayer = false;
	SDK::FVector  playerLocation{};

	long long     instanceRows = 0;      // instance lines emitted so far
	bool          truncated = false;     // hit MAX_INSTANCE_ROWS

	// Running totals for the summary block.
	int       visualizationComponents = 0;
	int       visualizationMeshInfos = 0;
	int       visualizationIsmComponents = 0;
	long long visualizationInstances = 0;
	int       agentActors = 0;
	int       persistentEntities = 0;
	int       biomesContainers = 0;
	int       biomesSpawnerComponents = 0;
	int       biomesStandaloneSpawners = 0;
};

static bool InRange(const DumpContext& ctx, const SDK::FVector& location)
{
	if (ctx.radius <= 0.0 || !ctx.havePlayer)
		return true;

	double dx = location.X - ctx.playerLocation.X;
	double dy = location.Y - ctx.playerLocation.Y;
	double dz = location.Z - ctx.playerLocation.Z;
	return dx * dx + dy * dy + dz * dz <= ctx.radius * ctx.radius;
}

static void WriteVector(JsonWriter& json, const char* key, const SDK::FVector& v)
{
	char buffer[128];
	snprintf(buffer, sizeof(buffer), "{ \"x\": %.1f, \"y\": %.1f, \"z\": %.1f }", v.X, v.Y, v.Z);
	json.Raw(key, buffer);
}

static std::string ObjectName(SDK::UObject* object)
{
	return object ? object->GetName() : std::string("<null>");
}

static std::string ClassName(SDK::UObject* object)
{
	return (object && object->Class) ? object->Class->GetName() : std::string("<null>");
}

// Resolves a mesh component's static mesh via reflection -- the SDK dump has no
// generated accessor for UStaticMeshComponent::StaticMesh.
static std::string MeshNameOf(SDK::UPrimitiveComponent* component)
{
	auto* hooks = GetHooks();
	if (hooks && hooks->ObjectProperties && hooks->ObjectProperties->IsReady())
	{
		PluginPropertyHandle prop = hooks->ObjectProperties->FindPropertyOnObject(component, "StaticMesh");
		void* meshObj = nullptr;
		if (prop && hooks->ObjectProperties->GetObjectProperty(component, prop, &meshObj) && meshObj)
			return static_cast<SDK::UObject*>(meshObj)->GetFullName();
	}
	return "<unresolved>";
}

// Walks GObjects for className, growing the buffer until the walker reports no
// truncation (it returns the total match count, which may exceed capacity).
static std::vector<PluginObjectInfo> FindObjects(const char* className)
{
	std::vector<PluginObjectInfo> results;

	auto* hooks = GetHooks();
	if (!hooks || !hooks->ObjectWalker || !hooks->ObjectWalker->IsReady())
		return results;

	int capacity = 512;
	for (int attempt = 0; ; attempt++)
	{
		results.assign(capacity, PluginObjectInfo{});
		int total = hooks->ObjectWalker->FindObjectsByClassNameInto(
			className, PluginObjectLookup_InstanceOnly, results.data(), capacity);

		if (total <= capacity || attempt >= 3)
		{
			int filled = total < 0 ? 0 : (total < capacity ? total : capacity);
			results.resize(filled);
			return results;
		}
		capacity = total + 64; // re-walk with an exact-fit buffer
	}
}

// Emits the instance array for one ISM/HISM component, honouring the radius
// filter and the global row cap. Returns how many instances were in range.
static long long WriteInstances(JsonWriter& json, DumpContext& ctx, SDK::UInstancedStaticMeshComponent* ism, int32_t count)
{
	long long inRange = 0;

	json.BeginArray("instances");
	for (int32_t i = 0; i < count; i++)
	{
		SDK::FTransform transform;
		if (!ism->GetInstanceTransform(i, &transform, true))
			continue;

		if (!InRange(ctx, transform.Translation))
			continue;

		inRange++;

		if (ctx.instanceRows >= MAX_INSTANCE_ROWS)
		{
			ctx.truncated = true;
			continue; // keep counting, stop writing
		}

		char buffer[256];
		snprintf(buffer, sizeof(buffer),
			"{ \"index\": %d, \"x\": %.1f, \"y\": %.1f, \"z\": %.1f, \"scale\": [%.3f, %.3f, %.3f] }",
			i,
			transform.Translation.X, transform.Translation.Y, transform.Translation.Z,
			transform.Scale3D.X, transform.Scale3D.Y, transform.Scale3D.Z);
		json.Raw(nullptr, buffer);
		ctx.instanceRows++;
	}
	json.EndArray();

	return inRange;
}

// ---------------------------------------------------------------------------
// Section: Mass representation / visualisation
// ---------------------------------------------------------------------------

// UMassEntitySubsystem's FMassEntityManager (the archetype/chunk storage that
// actually owns every entity's fragments) is opaque in the SDK dump -- there
// is no reflection path to per-entity FTransformFragments. What *is* reachable
// is the other end of the pipeline: every Mass entity that renders goes
// through a UMassVisualizationComponent, whose ISM components hold one
// instance transform per visible entity. So this is the closest thing to a
// positional census of the Mass world that a plugin can take.
static void WriteMassVisualization(JsonWriter& json, DumpContext& ctx)
{
	using namespace SDK;

	json.BeginArray("massVisualization");

	for (const auto& match : FindObjects("MassVisualizationComponent"))
	{
		auto* visComponent = static_cast<UMassVisualizationComponent*>(match.object);
		if (!visComponent)
			continue;

		ctx.visualizationComponents++;

		json.BeginObject();
		json.String("component", ObjectName(visComponent));

		AActor* owner = visComponent->GetOwner();
		json.String("ownerActor", owner ? owner->GetName() : "<null>");
		json.String("ownerClass", ClassName(owner));
		if (owner)
			WriteVector(json, "ownerLocation", owner->K2_GetActorLocation());
		else
			json.Null("ownerLocation");

		json.BeginArray("meshInfos");
		for (int m = 0; m < visComponent->InstancedStaticMeshInfos.Num(); m++)
		{
			auto& info = visComponent->InstancedStaticMeshInfos[m];
			ctx.visualizationMeshInfos++;

			json.BeginObject();
			json.Int("index", m);

			// Desc: what this visualisation *should* render -- the authored
			// mesh list, straight out of the entity's visualisation trait.
			json.BeginArray("meshes");
			for (int d = 0; d < info.Desc.Meshes.Num(); d++)
			{
				auto& meshDesc = info.Desc.Meshes[d];

				json.BeginObject();
				json.String("mesh", meshDesc.Mesh ? meshDesc.Mesh->GetFullName() : "<null>");
				json.String("ismComponentClass", meshDesc.ISMComponentClass ? meshDesc.ISMComponentClass->GetName() : "<null>");
				json.Number("minLODSignificance", meshDesc.MinLODSignificance);
				json.Number("maxLODSignificance", meshDesc.MaxLODSignificance);
				json.Bool("castShadows", meshDesc.bCastShadows);
				json.Bool("createPhysics", meshDesc.bCreatePhysics);
				json.Int("materialOverrides", meshDesc.MaterialOverrides.Num());
				json.EndObject();
			}
			json.EndArray();

			// Components: what is actually instanced right now.
			json.BeginArray("components");
			for (int c = 0; c < info.InstancedStaticMeshComponents.Num(); c++)
			{
				UInstancedStaticMeshComponent* ism = info.InstancedStaticMeshComponents[c];
				if (!ism)
					continue;

				ctx.visualizationIsmComponents++;
				int32 count = ism->GetInstanceCount();
				ctx.visualizationInstances += count;

				json.BeginObject();
				json.String("name", ObjectName(ism));
				json.String("class", ClassName(ism));
				json.String("mesh", MeshNameOf(ism));
				AActor* ismOwner = ism->GetOwner();
				json.String("ownerActor", ismOwner ? ismOwner->GetName() : "<null>");
				json.Int("instanceCount", count);

				if (ctx.includeInstances)
					json.Int("instancesInRange", WriteInstances(json, ctx, ism, count));

				json.EndObject();
			}
			json.EndArray();

			json.EndObject();
		}
		json.EndArray();

		json.EndObject();
	}

	json.EndArray();
}

// ---------------------------------------------------------------------------
// Section: actor-backed Mass entities
// ---------------------------------------------------------------------------

// Entities with an actor representation carry a UMassAgentComponent, so their
// owning actor gives us a name, a class and a real world location -- the one
// place where a Mass entity is directly addressable as a UObject.
static void WriteMassAgentActors(JsonWriter& json, DumpContext& ctx)
{
	using namespace SDK;

	json.BeginArray("massAgentActors");

	for (const auto& match : FindObjects("MassAgentComponent"))
	{
		auto* agent = static_cast<UMassAgentComponent*>(match.object);
		if (!agent)
			continue;

		AActor* owner = agent->GetOwner();
		if (!owner)
			continue;

		FVector location = owner->K2_GetActorLocation();
		if (!InRange(ctx, location))
			continue;

		ctx.agentActors++;

		json.BeginObject();
		json.String("actor", owner->GetName());
		json.String("class", ClassName(owner));
		json.String("component", ObjectName(agent));
		WriteVector(json, "location", location);
		json.EndObject();
	}

	json.EndArray();
}

// ---------------------------------------------------------------------------
// Section: persistent Mass entity registry
// ---------------------------------------------------------------------------

// UCrMassPersistentIDSubsystem maps every save-persistent Mass entity handle to
// its stable ID. This is the only complete *roster* of Mass entities available
// -- handles only, no transforms, since the fragments live in the opaque
// entity manager. Useful as a census: how many entities exist, and which
// handles are live.
static void WritePersistentEntities(JsonWriter& json, DumpContext& ctx)
{
	using namespace SDK;

	json.BeginArray("massPersistentEntities");

	for (const auto& match : FindObjects("CrMassPersistentIDSubsystem"))
	{
		auto* subsystem = static_cast<UCrMassPersistentIDSubsystem*>(match.object);
		if (!subsystem)
			continue;

		json.BeginObject();
		json.String("subsystem", ObjectName(subsystem));
		json.Int("maxId", subsystem->MaxID);
		json.Int("count", subsystem->HandleIDMap.Num());

		json.BeginArray("entities");
		for (int32 i = 0; i < subsystem->HandleIDMap.NumAllocated(); i++)
		{
			if (!subsystem->HandleIDMap.IsValidIndex(i))
				continue;

			auto& pair = subsystem->HandleIDMap[i];
			ctx.persistentEntities++;

			char buffer[160];
			snprintf(buffer, sizeof(buffer),
				"{ \"entityIndex\": %d, \"serialNumber\": %d, \"persistentId\": %u }",
				pair.Key().Index, pair.Key().SerialNumber, pair.Value().ID);
			json.Raw(nullptr, buffer);
		}
		json.EndArray();

		json.EndObject();
	}

	json.EndArray();
}

// ---------------------------------------------------------------------------
// Section: biomes decoration spawners
// ---------------------------------------------------------------------------

static void WriteSpeciesAssets(JsonWriter& json, SDK::UBiomesSpeciesInfo* species)
{
	json.BeginArray("assets");
	for (int i = 0; i < species->Assets.Num(); i++)
	{
		SDK::UObject* asset = species->Assets[i];
		if (asset)
			json.String(nullptr, asset->GetName());
	}
	json.EndArray();
}

// The decoration system this plugin drives. Not Mass -- but it is the other
// half of "what is in the world", and the dump is far more useful with both
// sides in one file.
static void WriteBiomesSpawners(JsonWriter& json, DumpContext& ctx)
{
	using namespace SDK;

	json.BeginArray("biomesContainers");
	for (const auto& match : FindObjects("BiomesRuntimeSpawnerContainer"))
	{
		auto* container = static_cast<ABiomesRuntimeSpawnerContainer*>(match.object);
		if (!container)
			continue;

		FVector containerLocation = container->K2_GetActorLocation();
		if (!InRange(ctx, containerLocation))
			continue;

		ctx.biomesContainers++;

		json.BeginObject();
		json.String("actor", container->GetName());
		json.String("class", ClassName(container));
		WriteVector(json, "location", containerLocation);

		json.BeginArray("spawnerComponents");
		for (int s = 0; s < container->SpawnerComponents.Num(); s++)
		{
			UBiomesRuntimeSpawnerComponent* spawner = container->SpawnerComponents[s];
			if (!spawner)
				continue;

			ctx.biomesSpawnerComponents++;

			json.BeginObject();
			json.String("component", spawner->GetName());

			if (spawner->SpeciesInfo)
			{
				std::string speciesName = spawner->SpeciesInfo->GetName();
				json.String("species", speciesName);
				json.Bool("wouldBeRemoved", DecorationRemover_SpeciesWouldBeRemoved(speciesName));
				WriteSpeciesAssets(json, spawner->SpeciesInfo);
			}
			else
			{
				json.Null("species");
			}

			json.Number("maxSpawningDistance", spawner->MaxSpawningDistance);
			json.Number("maxSpawningDistanceSquared", spawner->MaxSpawningDistanceSquared);

			json.BeginArray("instanceComponents");
			for (int p = 0; p < spawner->InstanceComponentPartitions.Num(); p++)
			{
				auto& partition = spawner->InstanceComponentPartitions[p];
				for (int i = 0; i < partition.RuntimeInstanceComponents.Num(); i++)
				{
					UInstancedStaticMeshComponent* ism = partition.RuntimeInstanceComponents[i];
					if (!ism)
						continue;

					int32 count = ism->GetInstanceCount();

					json.BeginObject();
					json.Int("partition", p);
					json.String("name", ObjectName(ism));
					json.String("class", ClassName(ism));
					json.String("mesh", MeshNameOf(ism));
					json.Int("instanceCount", count);
					if (ctx.includeInstances)
						json.Int("instancesInRange", WriteInstances(json, ctx, ism, count));
					json.EndObject();
				}
			}
			json.EndArray();

			json.EndObject();
		}
		json.EndArray();

		json.EndObject();
	}
	json.EndArray();

	json.BeginArray("biomesStandaloneSpawners");
	for (const auto& match : FindObjects("BiomesRuntimeSpawner"))
	{
		auto* spawner = static_cast<ABiomesRuntimeSpawner*>(match.object);
		if (!spawner)
			continue;

		FVector spawnerLocation = spawner->K2_GetActorLocation();
		if (!InRange(ctx, spawnerLocation))
			continue;

		ctx.biomesStandaloneSpawners++;

		json.BeginObject();
		json.String("actor", spawner->GetName());
		json.String("class", ClassName(spawner));
		WriteVector(json, "location", spawnerLocation);
		json.Number("maxSpawningDistance", spawner->MaxSpawningDistance);

		// Soft pointer: null until the species asset has actually streamed in.
		UBiomesSpeciesInfo* species = spawner->SpeciesInfo.Get();
		if (species)
		{
			std::string speciesName = species->GetName();
			json.String("species", speciesName);
			json.Bool("wouldBeRemoved", DecorationRemover_SpeciesWouldBeRemoved(speciesName));
			WriteSpeciesAssets(json, species);
		}
		else
		{
			json.Null("species");
		}

		json.BeginArray("instanceComponents");
		for (int i = 0; i < spawner->InstancedStaticMeshComponents.Num(); i++)
		{
			UHierarchicalInstancedStaticMeshComponent* ism = spawner->InstancedStaticMeshComponents[i];
			if (!ism)
				continue;

			int32 count = ism->GetInstanceCount();

			json.BeginObject();
			json.String("name", ObjectName(ism));
			json.String("class", ClassName(ism));
			json.String("mesh", MeshNameOf(ism));
			json.Int("instanceCount", count);
			if (ctx.includeInstances)
				json.Int("instancesInRange", WriteInstances(json, ctx, ism, count));
			json.EndObject();
		}
		json.EndArray();

		json.EndObject();
	}
	json.EndArray();
}

// ---------------------------------------------------------------------------
// Output file
// ---------------------------------------------------------------------------

// Dumps land in an EntityDumps folder beside the plugin DLL, so they stay with
// the mod rather than in whatever the game's working directory happens to be.
static std::string GetDumpDirectory()
{
	HMODULE module = nullptr;
	if (!GetModuleHandleExW(
		GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		reinterpret_cast<LPCWSTR>(&GetDumpDirectory), &module))
		return "";

	wchar_t widePath[MAX_PATH] = {};
	DWORD length = GetModuleFileNameW(module, widePath, MAX_PATH);
	if (length == 0 || length >= MAX_PATH)
		return "";

	char path[MAX_PATH * 2] = {};
	if (WideCharToMultiByte(CP_UTF8, 0, widePath, -1, path, sizeof(path), nullptr, nullptr) == 0)
		return "";

	std::string directory(path);
	size_t slash = directory.find_last_of("\\/");
	if (slash == std::string::npos)
		return "";

	directory.resize(slash + 1);
	directory += "EntityDumps";

	if (!CreateDirectoryA(directory.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
		return "";

	return directory + "\\";
}

static std::string Timestamp(const SYSTEMTIME& time, bool forFilename)
{
	char buffer[64];
	snprintf(buffer, sizeof(buffer),
		forFilename ? "%04d%02d%02d_%02d%02d%02d" : "%04d-%02d-%02d %02d:%02d:%02d",
		time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond);
	return buffer;
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

std::string DumpEntitiesToJson()
{
	using namespace SDK;

	auto* hooks = GetHooks();
	if (!hooks || !hooks->ObjectWalker || !hooks->ObjectWalker->IsReady())
	{
		LOG_WARN("Entity dump: ObjectWalker unavailable");
		return "";
	}

	UWorld* world = UWorld::GetWorld();
	if (!world)
	{
		LOG_WARN("Entity dump: no world");
		return "";
	}

	DumpContext ctx;
	ctx.radius = (double)DecorationRemoverConfig::Config::GetEntityDumpRadius();
	ctx.includeInstances = DecorationRemoverConfig::Config::ShouldDumpInstances();

	APlayerController* pc = UGameplayStatics::GetPlayerController(world, 0);
	if (APawn* pawn = pc ? pc->Pawn : nullptr)
	{
		ctx.playerLocation = pawn->K2_GetActorLocation();
		ctx.havePlayer = true;
	}
	else if (ctx.radius > 0.0)
	{
		LOG_WARN("Entity dump: no local pawn -- radius filter ignored, dumping the whole world");
	}

	std::string directory = GetDumpDirectory();
	if (directory.empty())
	{
		LOG_ERROR("Entity dump: could not resolve or create the EntityDumps directory");
		return "";
	}

	SYSTEMTIME now{};
	GetLocalTime(&now);

	std::string path = directory + "EntityDump_" + Timestamp(now, true) + ".json";

	std::ofstream file(path, std::ios::binary | std::ios::trunc);
	if (!file)
	{
		LOG_ERROR("Entity dump: could not open '%s' for writing", path.c_str());
		return "";
	}

	LOG_INFO("Entity dump: walking the world (radius: %s, instances: %s) -- this will hitch",
		ctx.radius > 0.0 ? "limited" : "unlimited",
		ctx.includeInstances ? "included" : "counts only");

	JsonWriter json(file);
	json.BeginObject();

	json.BeginObject("meta");
	json.String("generatedAt", Timestamp(now, false));
	json.String("world", world->GetName());
	json.Number("radius", ctx.radius);
	json.Bool("includeInstances", ctx.includeInstances);
	if (ctx.havePlayer)
		WriteVector(json, "playerLocation", ctx.playerLocation);
	else
		json.Null("playerLocation");
	json.String("note", "Mass fragment storage (FMassEntityManager) is not reachable from a plugin, "
		"so entity transforms come from the visualisation ISM instances rather than from "
		"per-entity FTransformFragments.");
	json.EndObject();

	WriteMassVisualization(json, ctx);
	WriteMassAgentActors(json, ctx);
	WritePersistentEntities(json, ctx);
	WriteBiomesSpawners(json, ctx);

	json.BeginObject("summary");
	json.Int("massVisualizationComponents", ctx.visualizationComponents);
	json.Int("massVisualizationMeshInfos", ctx.visualizationMeshInfos);
	json.Int("massVisualizationIsmComponents", ctx.visualizationIsmComponents);
	json.Int("massVisualizationInstances", ctx.visualizationInstances);
	json.Int("massAgentActors", ctx.agentActors);
	json.Int("massPersistentEntities", ctx.persistentEntities);
	json.Int("biomesContainers", ctx.biomesContainers);
	json.Int("biomesSpawnerComponents", ctx.biomesSpawnerComponents);
	json.Int("biomesStandaloneSpawners", ctx.biomesStandaloneSpawners);
	json.Int("instanceRowsWritten", ctx.instanceRows);
	json.Bool("instanceRowsTruncated", ctx.truncated);
	json.EndObject();

	json.EndObject();
	json.Finish();

	file.close();

	if (ctx.truncated)
		LOG_WARN("Entity dump: instance rows capped at %lld -- narrow [Debug] EntityDumpRadius for a complete slice", MAX_INSTANCE_ROWS);

	LOG_INFO("Entity dump written: %s", path.c_str());
	LOG_INFO("Entity dump: %d visualisation component(s), %d ISM component(s), %lld instance(s), %d agent actor(s), %d persistent entity handle(s), %d biomes container(s), %d standalone spawner(s)",
		ctx.visualizationComponents, ctx.visualizationIsmComponents, ctx.visualizationInstances,
		ctx.agentActors, ctx.persistentEntities, ctx.biomesContainers, ctx.biomesStandaloneSpawners);

	return path;
}

} // namespace DecorationRemoverDebug
