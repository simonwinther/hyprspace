#pragma once

#include "../src/SpaceModel.hpp"
#include "../src/SpaceStore.hpp"

static void testSpaces() {
    using namespace hyprspace::spaces;
    section("spaces: stable identities, validated names and independent local numbers");
    CSpaceModel model;
    const auto  initial = model.defaultSpace;
    CHECK(model.spaces.size() == 1 && model.active == initial);
    CHECK(validId(initial) && model.space(initial)->name == "Default");
    std::string error;
    CHECK(!model.create(" \t ", &error) && !error.empty());
    CHECK(!model.create("bad\nname"));
    CHECK(!model.create(std::string(257, 'a')));
    CHECK(!model.create(std::string("bad\xc0\xaf")));
    CHECK(model.create("læsehjørne"));
    const auto fun  = model.create(" fun ").value_or("");
    const auto work = model.create("work").value_or("");
    CHECK(validId(fun) && validId(work) && fun != work);
    CHECK(model.spaceNamed("fun") && !model.create("fun"));
    CHECK(model.rename(fun, "hobby") && model.spaceNamed("hobby")->id == fun);
    CHECK(!model.rename(fun, "work") && !model.rename("missing", "name"));
    const auto first = model.addWorkspace(fun, 1, "", false, 101, "native-one", "DP-1").value_or("");
    const auto other = model.addWorkspace(work, 1, "", false, 102, "native-two", "DP-2").value_or("");
    CHECK(validId(first) && validId(other));
    CHECK(model.local(fun, 1) && model.local(work, 1) && model.local(fun, 1)->id != model.local(work, 1)->id);
    CHECK(model.byNative(101) && model.byNative(101)->id == first && !model.byNative(0));
    CHECK(!model.addWorkspace(fun, 1, "duplicate", false));
    CHECK(!model.addWorkspace(fun, 3, "native duplicate", false, 102));
    CHECK(!model.addWorkspace(fun, 3, "name duplicate", false, 103, "native-two"));
    CHECK(!model.addWorkspace(fun, -1, "negative", false));
    CHECK(!model.addWorkspace(fun, 1, "special", true));
    CHECK(model.addWorkspace(fun, 7, "", false));
    CHECK(model.lowestNumber(fun) == 2);
    CHECK(model.select(fun, "DP-1", first));
    CHECK(model.selection(fun, "DP-1") && model.selection(fun, "DP-1")->id == first);
    CHECK(!model.select(work, "DP-1", first));
    CHECK(model.validate(&error));
    const auto stableName = managedWorkspaceName(first);
    CHECK(model.moveWorkspace(first, initial));
    CHECK(model.workspace(first) && model.workspace(first)->space == initial && model.workspace(first)->nativeId == 101);
    CHECK(!model.selection(fun, "DP-1") && managedWorkspaceName(first) == stableName);
    CHECK(model.forgetWorkspace(first) && !model.byNative(101) && !model.forgetWorkspace(first));

    section("spaces: reordering preserves identity, active space, output selections and saved order");
    CSpaceModel ordered;
    const auto  home            = ordered.defaultSpace;
    const auto  school          = ordered.create("School").value();
    const auto  play            = ordered.create("Play").value();
    const auto  study           = ordered.create("Study").value();
    const auto  schoolWorkspace = ordered.addWorkspace(school, 12, "", false, 812, "12", "DP-2").value();
    CHECK(ordered.select(school, "DP-2", schoolWorkspace));
    ordered.active              = school;
    const auto workspaceRecords = ordered.workspaces;
    const auto order            = [&] {
        std::vector<std::string> ids;
        for (const auto& item : ordered.spaces)
            ids.push_back(item.id);
        return ids;
    };
    CHECK(ordered.reorder(study, school, false));
    CHECK(order() == std::vector<std::string>({home, study, school, play}));
    CHECK(ordered.reorder(study, play, true));
    CHECK(order() == std::vector<std::string>({home, school, play, study}));
    CHECK(ordered.reorder(home, study, true));
    CHECK(order() == std::vector<std::string>({school, play, study, home}));
    CHECK(ordered.reorder(home, school, false));
    CHECK(order() == std::vector<std::string>({home, school, play, study}));
    // A filtered picker can move past hidden siblings without reordering them.
    CHECK(ordered.reorder(study, school, false));
    CHECK(order() == std::vector<std::string>({home, study, school, play}));
    CHECK(ordered.active == school && ordered.defaultSpace == home);
    CHECK(ordered.workspaces == workspaceRecords && ordered.selection(school, "DP-2")->id == schoolWorkspace);
    const auto reorderedState = serialize(ordered);
    CHECK(ordered.reorder(school, school, false));
    CHECK(!ordered.reorder("missing", school, true, &error));
    CHECK(!ordered.reorder(school, "missing", false, &error));
    CHECK(serialize(ordered) == reorderedState);
    const auto restoredOrder = deserialize(reorderedState);
    CHECK(restoredOrder && restoredOrder->spaces == ordered.spaces && restoredOrder->workspaces == workspaceRecords && restoredOrder->active == school);

    section("spaces: validated appearance updates are atomic and retain logical ownership");
    CSpaceModel appearance;
    CHECK(appearance.spaces.size() == 1 && appearance.spaces.front().name == "Default");
    CHECK(appearance.spaces.front().icon == DEFAULT_SPACE_ICON && appearance.spaces.front().color == DEFAULT_SPACE_COLOR);
    std::set<std::string_view> iconIds;
    for (const auto& icon : SPACE_ICON_CATALOG) {
        CHECK(iconIds.insert(icon.id).second && !icon.label.empty());
        CHECK(validSpaceIcon(icon.id) && spaceIcon(icon.id)->label == icon.label);
    }
    CHECK(iconIds.size() == 48 && !validSpaceIcon("../unknown.svg"));
    CHECK(normalizeSpaceColor("#AaBBcC") == "#aabbcc");
    for (const auto invalid : {"", "blue", "#abc", "#12345678", "#12x456", "112233", "#112233\n"})
        CHECK(!normalizeSpaceColor(invalid));
    const auto decorated = appearance.create("Study", &error, "graduation-cap", "#AABBCC").value_or("");
    CHECK(validId(decorated) && appearance.space(decorated)->icon == "graduation-cap" && appearance.space(decorated)->color == "#aabbcc");
    const auto decoratedWorkspace = appearance.addWorkspace(decorated, 1, "", false, 501, "appearance-native", "DP-1").value_or("");
    CHECK(appearance.select(decorated, "DP-1", decoratedWorkspace));
    auto beforeAppearance = serialize(appearance);
    CHECK(!appearance.create("Invalid appearance", &error, "unknown", "#112233"));
    CHECK(!appearance.create("Invalid appearance", &error, "heart", "#abcd"));
    CHECK(!appearance.update(decorated, "Must not rename", "unknown", "#112233", &error));
    CHECK(!appearance.update(decorated, "Must not rename", "heart", "#gg1122", &error));
    CHECK(!appearance.update(decorated, "Default", "heart", "#112233", &error));
    CHECK(serialize(appearance) == beforeAppearance);
    CHECK(appearance.update(decorated, std::nullopt, "briefcase", "#F38BA8", &error));
    CHECK(appearance.rename(decorated, "ATDL"));
    CHECK(appearance.space(decorated)->icon == "briefcase" && appearance.space(decorated)->color == "#f38ba8");
    CHECK(appearance.selection(decorated, "DP-1")->id == decoratedWorkspace && appearance.workspace(decoratedWorkspace)->nativeId == 501);
    const auto appearanceRestored = deserialize(serialize(appearance));
    CHECK(appearanceRestored && appearanceRestored->spaces == appearance.spaces && appearanceRestored->workspaces == appearance.workspaces);

    auto legacyAppearance = serialize(appearance);
    for (auto& owner : legacyAppearance["spaces"]) {
        owner.erase("icon");
        owner.erase("color");
    }
    const auto legacyRestored = deserialize(legacyAppearance);
    CHECK(legacyRestored && legacyRestored->workspaces == appearance.workspaces &&
          legacyRestored->space(decorated)->selections == appearance.space(decorated)->selections);
    CHECK(legacyRestored->space(decorated)->icon == DEFAULT_SPACE_ICON && legacyRestored->space(decorated)->color == DEFAULT_SPACE_COLOR);
    legacyAppearance["spaces"][1]["icon"]  = "future-icon";
    legacyAppearance["spaces"][1]["color"] = 17;
    const auto unknownAppearance           = deserialize(legacyAppearance);
    CHECK(unknownAppearance && unknownAppearance->workspaces == appearance.workspaces && unknownAppearance->active == appearance.active);
    CHECK(unknownAppearance->space(decorated)->icon == DEFAULT_SPACE_ICON && unknownAppearance->space(decorated)->color == DEFAULT_SPACE_COLOR);
    legacyAppearance["spaces"][1]["icon"]  = "heart";
    legacyAppearance["spaces"][1]["color"] = "#1122AA";
    const auto normalizedAppearance        = deserialize(legacyAppearance);
    CHECK(normalizedAppearance && normalizedAppearance->space(decorated)->icon == "heart" && normalizedAppearance->space(decorated)->color == "#1122aa");

    section("spaces: deletion preserves sparse numbers before allocating collisions");
    CSpaceModel transfers;
    const auto  destination = transfers.defaultSpace;
    const auto  source      = transfers.create("ATDL").value_or("");
    for (const auto number : {1, 3, 5})
        CHECK(transfers.addWorkspace(destination, number, "", false));
    std::map<int64_t, std::string> moved;
    for (const auto number : {1, 2, 5, 7})
        moved[number] = transfers.addWorkspace(source, number, "", false, 100 + number).value_or("");
    const auto namedSource = transfers.addWorkspace(source, 0, "notes", false).value_or("");
    CHECK(transfers.addWorkspace(destination, 0, "notes", false));
    const auto specialSource = transfers.addWorkspace(source, 0, "scratch", true).value_or("");
    CHECK(!specialSource.empty());
    CHECK(transfers.select(source, "special:DP-1", specialSource));
    CHECK(!transfers.select(source, "DP-1", specialSource));
    CHECK(transfers.addWorkspace(destination, 0, "scratch", true));
    CHECK(transfers.select(source, "DP-3", moved[7]));
    transfers.active = source;
    CHECK(transfers.erase(source, destination, &error));
    CHECK(!transfers.space(source) && transfers.active == destination);
    CHECK(transfers.workspace(moved[1])->number == 4 && transfers.workspace(moved[5])->number == 6);
    CHECK(transfers.workspace(moved[2])->number == 2 && transfers.workspace(moved[7])->number == 7);
    CHECK(transfers.workspace(moved[1])->name == "4" && transfers.workspace(moved[1])->nativeId == 101);
    CHECK(transfers.workspace(namedSource)->name == "notes (2)");
    CHECK(transfers.named(destination, "scratch (2)", true));
    CHECK(transfers.selection(destination, "special:DP-1") && transfers.selection(destination, "special:DP-1")->id == specialSource);
    CHECK(transfers.selection(destination, "DP-3") && transfers.selection(destination, "DP-3")->id == moved[7]);
    CHECK(transfers.validate(&error));
    CHECK(!transfers.erase(destination, destination));
    const auto replacement = transfers.create("replacement").value_or("");
    CHECK(transfers.erase(destination, replacement));
    CHECK(transfers.defaultSpace == replacement && transfers.active == replacement && transfers.validate());

    section("spaces: colliding maximum-length aliases retain valid UTF-8 names");
    for (const std::string glyph : {"a", "æ", "あ", "😀"}) {
        std::string base = "x";
        while (base.size() + glyph.size() <= 256)
            base += glyph;
        base.append(256 - base.size(), 'x');
        CHECK(validName(base) && base.size() == 256);
        for (const auto suffix : {2, 10, 100, 1000}) {
            const auto alias = suffixedWorkspaceName(base, suffix);
            CHECK(validName(alias) && alias.size() <= 256 && alias.ends_with(" (" + std::to_string(suffix) + ")"));
        }
        CSpaceModel aliases;
        const auto  owner    = aliases.defaultSpace;
        const auto  incoming = aliases.create("Incoming").value();
        CHECK(aliases.addWorkspace(owner, 0, base, false));
        CHECK(aliases.addWorkspace(owner, 0, suffixedWorkspaceName(base, 2), false));
        const auto moved = aliases.addWorkspace(incoming, 0, base, false).value();
        CHECK(aliases.moveWorkspace(moved, owner));
        CHECK(aliases.workspace(moved)->name == suffixedWorkspaceName(base, 3) && aliases.validate());
    }

    section("spaces: indexed validation checks large graphs without confusing space and workspace identities");
    CSpaceModel graph;
    graph.spaces.clear();
    for (size_t owner = 0; owner < 64; ++owner) {
        const auto identity = newId();
        graph.spaces.push_back({.id = identity, .name = "Space " + std::to_string(owner), .selections = {}});
        for (int64_t number = 1; number <= 32; ++number) {
            const auto workspace = newId();
            graph.workspaces.push_back({.id = workspace, .space = identity, .number = number, .name = std::to_string(number)});
            graph.spaces.back().selections["Output " + std::to_string(number)] = workspace;
        }
    }
    graph.active = graph.defaultSpace = graph.spaces.front().id;
    CHECK(graph.validate());
    const auto graphState = serialize(graph);
    CHECK(deserialize(graphState));
    auto badGraph                      = graphState;
    badGraph["workspaces"][0]["space"] = graph.workspaces.back().id;
    CHECK(!deserialize(badGraph));
    badGraph                        = graphState;
    badGraph["workspaces"][0]["id"] = graph.spaces.back().id;
    CHECK(!deserialize(badGraph));
    badGraph                                        = graphState;
    badGraph["spaces"][0]["selections"]["Output 1"] = graph.spaces.back().id;
    CHECK(!deserialize(badGraph));
    badGraph                                        = graphState;
    badGraph["spaces"][0]["selections"]["Output 1"] = graph.workspaces.back().id;
    CHECK(!deserialize(badGraph));

    section("spaces: persistence validates complete graphs and rejects unsupported state");
    CHECK(model.update(work, std::nullopt, "palette", "#F9E2AF"));
    model.runtimePins["instance-signature/0x1234"] = true;
    const auto state                               = serialize(model);
    const auto restored                            = deserialize(state, &error);
    CHECK(restored && restored->spaces == model.spaces && restored->workspaces == model.workspaces && restored->active == model.active &&
          restored->runtimePins == model.runtimePins);
    CHECK(!state.contains("history"));
    const auto legacyVisit = newId();
    const nlohmann::json legacyHistory{
        {"version", 1},
        {"cursor", legacyVisit},
        {"entries", {{{"id", legacyVisit},
                      {"space_id", model.active},
                      {"workspace_id", model.workspaces.front().id},
                      {"monitor", "DP-1"},
                      {"window_session", "previous-compositor"},
                      {"window_id", 42},
                      {"window_title", "Editor"},
                      {"time_ms", 12345}}}}};
    for (const auto& discarded : {legacyHistory, nlohmann::json(false), nlohmann::json("invalid"), nlohmann::json(nullptr),
                                 nlohmann::json{{"version", 99}, {"entries", nlohmann::json::array()}}}) {
        auto legacyState       = state;
        legacyState["history"] = discarded;
        const auto loaded      = deserialize(legacyState);
        CHECK(loaded && serialize(*loaded) == state);
    }
    auto broken       = state;
    broken["version"] = 2;
    CHECK(!deserialize(broken, &error));
    broken = state;
    broken.erase("pin_intents");
    CHECK(deserialize(broken) && deserialize(broken)->runtimePins.empty());
    broken["pin_intents"] = {{"instance-signature/0x1234", "invalid"}};
    CHECK(!deserialize(broken));
    broken                           = state;
    broken["workspaces"][0]["space"] = newId();
    CHECK(!deserialize(broken));
    broken = state;
    broken["workspaces"].push_back(broken["workspaces"][0]);
    CHECK(!deserialize(broken));
    broken                            = state;
    broken["workspaces"][0]["number"] = 1.5;
    CHECK(!deserialize(broken));
    broken                                    = state;
    broken["spaces"][0]["selections"]["DP-1"] = newId();
    CHECK(!deserialize(broken));
    broken           = state;
    broken["active"] = newId();
    CHECK(!deserialize(broken));
    CHECK(spaceStatePath("/state", "/home/user") == "/state/hyprspace/spaces.json");
    CHECK(spaceStatePath("relative", "/home/user") == "/home/user/.local/state/hyprspace/spaces.json");
    CHECK(spaceStatePath(nullptr, nullptr).empty());

    section("spaces: atomic backup recovery, immutable async snapshots and failure reporting");
    std::string temporary = (fs::temp_directory_path() / "hyprspace-spaces-XXXXXX").string();
    const auto* directory = mkdtemp(temporary.data());
    CHECK(directory);
    if (!directory)
        return;
    const auto file = fs::path(directory) / "state/spaces.json";
    CHECK(!load(file).model && load(file).error.empty());
    CHECK(save(file, model, &error));
    CHECK(fs::exists(file.string() + ".bak"));
    {
        auto legacyState       = serialize(model);
        legacyState["history"] = legacyHistory;
        std::ofstream(file) << legacyState.dump();
    }
    const auto legacyLoaded = load(file);
    CHECK(legacyLoaded.model && serialize(*legacyLoaded.model) == serialize(model));
    CHECK((fs::status(file).permissions() & fs::perms::group_all) == fs::perms::none);
    CHECK((fs::status(file).permissions() & fs::perms::others_all) == fs::perms::none);
    auto changed = model;
    CHECK(changed.rename(work, "new work"));
    CHECK(save(file, changed, &error));
    CHECK(load(file).model && load(file).model->spaceNamed("new work"));
    {
        std::ofstream(file) << "{ corrupt";
    }
    const auto recovered = load(file);
    CHECK(recovered.model && recovered.recovered && recovered.model->spaceNamed("work"));
    CHECK(save(file, changed));
    {
        std::ofstream(file) << "corrupt again";
    }
    CHECK(load(file).model && load(file).model->spaceNamed("work")); // corrupt primary never replaces good backup
    {
        CSpaceStore store(file);
        store.submit(model);
        CHECK(model.update(work, std::nullopt, "heart", "#112233"));
        CHECK(model.rename(work, "not submitted"));
        CHECK(store.flush() && store.error().empty());
        CHECK(load(file).model && load(file).model->spaceNamed("work"));
        CHECK(load(file).model->space(work)->icon == "palette" && load(file).model->space(work)->color == "#f9e2af");
        for (int i = 0; i < 20; ++i) {
            CHECK(model.rename(work, "work " + std::to_string(i)));
            store.submit(model);
        }
        CHECK(store.flush());
        CHECK(load(file).model && load(file).model->spaceNamed("work 19"));
        CHECK(model.rename(work, "saved on shutdown"));
        store.submit(model);
    }
    CHECK(load(file).model && load(file).model->spaceNamed("saved on shutdown"));
    {
        CSpaceStore bad("relative/spaces.json");
        bad.submit(model);
        CHECK(!bad.flush() && !bad.error().empty());
    }
    {
        std::ofstream(file) << "bad";
        std::ofstream(file.string() + ".bak") << "bad backup";
    }
    CHECK(!load(file).model && !load(file).error.empty());
    CHECK(!save("relative/spaces.json", model));
    fs::remove_all(directory);
}
