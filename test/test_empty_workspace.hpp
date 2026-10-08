#pragma once

#include "../src/EmptyWorkspace.hpp"

static void testEmptyWorkspaceAllocation() {
    section("empty workspace: local prepared and active empties precede allocation");
    std::vector<SEmptyWorkspaceCandidate> workspaces{
        {.id = 1, .monitor = 20, .empty = true, .active = true},
        {.id = 2, .monitor = 10},
        {.id = 5, .monitor = 10, .empty = true},
        {.id = 42, .monitor = 10, .empty = true, .active = true},
        {.id = 77, .monitor = 10, .empty = true, .prepared = true},
        {.id = -99, .monitor = 10, .empty = true, .special = true},
    };
    auto choice = chooseEmptyWorkspace(workspaces, 10, {});
    CHECK(choice && choice->existing && choice->id == 77);
    CHECK(chooseEmptyWorkspace(workspaces, 10, {})->id == 77); // repeats reuse the prepared tile
    workspaces[4].empty = false;
    choice              = chooseEmptyWorkspace(workspaces, 10, {});
    CHECK(choice && choice->existing && choice->id == 42);
    workspaces[3].empty = false;
    workspaces.push_back({.id = -1337, .monitor = 10, .empty = true});
    choice = chooseEmptyWorkspace(workspaces, 10, {});
    CHECK(choice && choice->existing && choice->id == -1337); // overview order includes named normals
    workspaces.back().allowed = false;
    choice                    = chooseEmptyWorkspace(workspaces, 10, {});
    CHECK(choice && choice->existing && choice->id == 5);
    workspaces[2].allowed = false;
    choice                = chooseEmptyWorkspace(workspaces, 10, {});
    CHECK(choice && !choice->existing && choice->id == 3);
    CHECK(workspaces[0].empty && workspaces[0].active && workspaces[0].monitor == 20);

    section("empty workspace: globally occupied IDs and remote bindings reserve numbers");
    std::vector<SEmptyWorkspaceCandidate> occupied{{.id = 1, .monitor = 20, .empty = true}, {.id = 4, .monitor = 10}};
    std::vector<SEmptyWorkspaceRule>      rules{
        {.selector = "2", .monitorBinding = true},
        {.selector = "3", .monitorBinding = true, .targetMonitor = true},
        {.selector = "r[5-9]", .monitorBinding = true},
    };
    choice = chooseEmptyWorkspace(occupied, 10, rules);
    CHECK(choice && choice->id == 3 && !choice->existing);
    occupied.push_back({.id = 3, .monitor = 20, .empty = true});
    choice = chooseEmptyWorkspace(occupied, 10, rules);
    CHECK(choice && choice->id == 10);
    rules[0].enabled = false;
    choice           = chooseEmptyWorkspace(occupied, 10, rules);
    CHECK(choice && choice->id == 2);
    rules[0].enabled = true;
    rules.push_back({.selector = "r[1-12]", .monitorBinding = true, .targetMonitor = true});
    CHECK(chooseEmptyWorkspace(occupied, 10, rules)->id == 10); // conflicting reservations stay safe

    section("empty workspace: ranges jump without scanning and overflow reports exhaustion");
    rules  = {{.selector = " r[1-500000000] ", .monitorBinding = true}};
    choice = chooseEmptyWorkspace({}, 10, rules);
    CHECK(choice && choice->id == 500000001);
    rules  = {{.selector = "r[1-2147483646]", .monitorBinding = true}};
    choice = chooseEmptyWorkspace({}, 10, rules);
    CHECK(choice && choice->id == MAX_EMPTY_WORKSPACE_ID);
    const std::vector<SEmptyWorkspaceCandidate> lastOccupied{{.id = MAX_EMPTY_WORKSPACE_ID, .monitor = 20}};
    CHECK(!chooseEmptyWorkspace(lastOccupied, 10, rules));
    rules = {{.selector = "r[1-9223372036854775807]", .monitorBinding = true}};
    CHECK(!chooseEmptyWorkspace({}, 10, rules));
    rules = {{.selector = "", .monitorBinding = true}};
    CHECK(!chooseEmptyWorkspace({}, 10, rules));

    section("empty workspace: static default names retain named monitor reservations");
    occupied = {{.id = 1, .monitor = 20}, {.id = 2, .monitor = 20}};
    rules    = {{.selector = "3", .defaultName = "reserved"}, {.selector = "name:reserved", .monitorBinding = true}};
    CHECK(chooseEmptyWorkspace(occupied, 10, rules)->id == 4);
    rules[0].selector = "r[3-8]";
    CHECK(chooseEmptyWorkspace(occupied, 10, rules)->id == 9);
    rules[0].selector = "name:3";
    CHECK(chooseEmptyWorkspace(occupied, 10, rules)->id == 4);
    rules[1].targetMonitor = true;
    CHECK(chooseEmptyWorkspace(occupied, 10, rules)->id == 3);
    rules = {{.selector = "name:3", .monitorBinding = true}, {.selector = "special:tools", .monitorBinding = true}};
    CHECK(chooseEmptyWorkspace(occupied, 10, rules)->id == 4);

    section("empty workspace: unsupported assignments fail before allocation but allow existing local empties");
    rules = {{.selector = "r[1-10] w[0]", .monitorBinding = true}};
    std::string error;
    CHECK(!chooseEmptyWorkspace({}, 10, rules, &error));
    CHECK(!error.empty());
    occupied.push_back({.id = 11, .monitor = 10, .empty = true});
    choice = chooseEmptyWorkspace(occupied, 10, rules, &error);
    CHECK(choice && choice->existing && choice->id == 11);
    CHECK(error.empty());
    rules[0].monitorBinding = false; // conditional styling remains native
    CHECK(chooseEmptyWorkspace({}, 10, rules)->id == 1);
    rules[0].monitorBinding = true;
    rules[0].enabled        = false;
    CHECK(chooseEmptyWorkspace({}, 10, rules)->id == 1);
    rules = {{.selector = "w[0]", .defaultName = "remote"}, {.selector = "name:remote", .monitorBinding = true}};
    CHECK(!chooseEmptyWorkspace({}, 10, rules));
}

static void testPreparedLaunchLifetime() {
    section("launch: consuming a context transfers prepared workspace ownership once");
    CLaunchContexts<std::shared_ptr<int>> contexts;
    const auto                            now      = decltype(contexts)::Clock::now();
    auto                                  resource = std::make_shared<int>(5);
    std::weak_ptr<int>                    observed = resource;
    CHECK(contexts.capture("prepared", resource, now));
    resource.reset();
    CHECK(!observed.expired());
    auto consumed = contexts.consume("prepared", now);
    CHECK(consumed && **consumed == 5);
    CHECK(!contexts.consume("prepared", now));
    consumed.reset();
    CHECK(observed.expired()); // consumed capture no longer retains its resource

    resource = std::make_shared<int>(6);
    observed = resource;
    CHECK(contexts.capture("expiring", resource, now));
    resource.reset();
    contexts.prune(now + std::chrono::minutes(3));
    CHECK(observed.expired());
}
