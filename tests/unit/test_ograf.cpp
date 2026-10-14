// SPDX-License-Identifier: MIT
// OGraf (docs/ograf.md): manifest check, ZIP safety, library, layers with a fake renderer page,
// the Server API routes.
#include <doctest/doctest.h>

#include <deque>
#include <filesystem>
#include <fstream>
#include <map>

#include "miniz/miniz.h"
#include "ograf/files.hpp"
#include "ograf/layers.hpp"
#include "ograf/library.hpp"
#include "ograf/manifest.hpp"
#include "ograf/serverapi.hpp"
#include "ograf/zip.hpp"

using namespace mbs::ograf;
namespace fs = std::filesystem;

namespace
{
    std::string const kFixtures = MBS_SOURCE_DIR "/tests/unit/ograf";

    std::string manifest(std::string const& id, std::string const& extra = "")
    {
        return R"({"$schema":"https://ograf.ebu.io/v1/specification/json-schemas/graphics/schema.json","id":")" + id +
               R"(","name":"Name )" + id + R"(","main":"graphic.mjs","supportsRealTime":true,"supportsNonRealTime":false)" + extra + "}";
    }

    struct TempDir
    {
        fs::path path;
        TempDir()
        {
            path = fs::temp_directory_path() / ("ograf-test-" + randomId());
            fs::create_directories(path);
        }
        ~TempDir()
        {
            std::error_code ec;
            fs::remove_all(path, ec);
        }
    };

    struct Entry
    {
        std::string name;
        std::string data;
    };

    std::string makeZip(fs::path const& dir, std::vector<Entry> const& entries)
    {
        auto const path = (dir / ("z-" + randomId() + ".zip")).string();
        mz_zip_archive zip{};
        REQUIRE(mz_zip_writer_init_file(&zip, path.c_str(), 0));
        for (auto const& e : entries)
        {
            REQUIRE(mz_zip_writer_add_mem(&zip, e.name.c_str(), e.data.data(), e.data.size(), MZ_DEFAULT_COMPRESSION));
        }
        REQUIRE(mz_zip_writer_finalize_archive(&zip));
        REQUIRE(mz_zip_writer_end(&zip));
        return path;
    }

    // A fake renderer page: records calls, answers from a script.
    struct FakeRenderer : Renderer
    {
        std::vector<std::pair<int, std::string>> calls;
        std::vector<picojson::value> args;
        std::deque<CallResult> answers; // empty: {statusCode 200}
        CallResult call(int layer, std::string const& method, picojson::value const& a) override
        {
            calls.emplace_back(layer, method);
            args.push_back(a);
            if (!answers.empty())
            {
                auto r = answers.front();
                answers.pop_front();
                return r;
            }
            picojson::object ok;
            ok["statusCode"] = picojson::value(200.0);
            return {CallResult::Kind::Ok, picojson::value(ok), {}};
        }
    };

    CallResult okStep(int step)
    {
        picojson::object o;
        o["statusCode"] = picojson::value(200.0);
        o["currentStep"] = picojson::value(static_cast<double>(step));
        return {CallResult::Kind::Ok, picojson::value(o), {}};
    }

    picojson::value parse(std::string const& text)
    {
        picojson::value v;
        REQUIRE(parseJson(v, text).empty());
        return v;
    }

    std::string const kRuntime = "export default class extends HTMLElement {}";
}

TEST_CASE("ograf: the EBU examples pass the v1 schema")
{
    for (auto const& entry : fs::directory_iterator(kFixtures))
    {
        auto const text = readFile(entry.path().string());
        REQUIRE(text);
        auto const check = checkManifest(*text, [](std::string const&) { return true; }, RenderInfo{1920, 1080, 50, false});
        INFO(entry.path().filename().string());
        for (auto const& e : check.errors)
        {
            INFO(e);
        }
        CHECK(check.ok);
        CHECK(!check.id.empty());
    }
}

TEST_CASE("ograf: manifest rules")
{
    auto const yes = [](std::string const&) { return true; };
    RenderInfo const r{1920, 1080, 50, false};
    CHECK(checkManifest(manifest("a"), yes, r).ok);
    CHECK_FALSE(checkManifest("not json", yes, r).ok);
    CHECK_FALSE(checkManifest(manifest("a/b"), yes, r).ok);
    CHECK_FALSE(checkManifest(manifest("a"), [](std::string const&) { return false; }, r).ok); // main missing
    auto const nonRealtime = checkManifest(
        R"({"$schema":"https://ograf.ebu.io/v1/specification/json-schemas/graphics/schema.json","id":"x","name":"x","main":"g.mjs","supportsRealTime":false,"supportsNonRealTime":true})",
        yes, r);
    CHECK_FALSE(nonRealtime.ok);
    auto const noName = checkManifest(
        R"({"$schema":"https://ograf.ebu.io/v1/specification/json-schemas/graphics/schema.json","id":"x","main":"g.mjs","supportsRealTime":true,"supportsNonRealTime":false})", yes, r);
    CHECK_FALSE(noName.ok); // the schema's required fields
    // A newer field: a warning, not a refusal.
    auto const extra = checkManifest(manifest("a", R"(,"futureField":1)"), yes, r);
    CHECK(extra.ok);
    CHECK(extra.warnings.size() == 1);
    // A requirement this output does not meet: a warning.
    auto const fps = checkManifest(manifest("a", R"(,"renderRequirements":[{"frameRate":{"exact":25}}])"), yes, r);
    CHECK(fps.ok);
    REQUIRE(fps.warnings.size() == 1);
    CHECK(fps.warnings[0].find("renderRequirements") != std::string::npos);
    CHECK(checkManifest(manifest("a", R"(,"renderRequirements":[{"frameRate":{"exact":25}},{"frameRate":{"min":50}}])"), yes, r).warnings.empty());
    // A missing thumbnail is a warning.
    auto const thumb = checkManifest(manifest("a", R"(,"thumbnails":[{"file":"t.png"}])"), [](std::string const& p) { return p == "graphic.mjs"; }, r);
    CHECK(thumb.ok);
    CHECK(thumb.warnings.size() == 1);
}

TEST_CASE("ograf: package paths stay inside the package")
{
    CHECK(packagePath("", "graphic.mjs") == "graphic.mjs");
    CHECK(packagePath("a/b", "../lib/x.js") == "a/lib/x.js");
    CHECK(packagePath("a", "./x/./y.js") == "a/x/y.js");
    CHECK(packagePath("", "../x.js").empty());
    CHECK(packagePath("a", "../../x.js").empty());
    CHECK(packagePath("", "/etc/passwd").empty());
    CHECK(packagePath("", "https://example.com/x.js").empty());
    CHECK(packagePath("", "a\\b.js").empty());
}

TEST_CASE("ograf: ZIP safety")
{
    TempDir t;
    std::string error;
    auto const unpack = [&](std::vector<Entry> const& entries, ZipLimits limits = {}) {
        auto const dest = t.path / ("d-" + randomId());
        fs::create_directories(dest);
        error.clear();
        return unpackZip(makeZip(t.path, entries), dest.string(), limits, error);
    };
    auto const ok = unpack({{"pkg/a.ograf.json", "{}"}, {"pkg/lib/x.js", "x"}, {"__MACOSX/pkg/._a", "junk"}, {".DS_Store", "junk"}});
    REQUIRE(ok);
    CHECK(*ok == std::vector<std::string>{"pkg/a.ograf.json", "pkg/lib/x.js"});
    CHECK_FALSE(unpack({{"../evil.js", "x"}}));
    CHECK(error.find("unsafe") != std::string::npos);
    CHECK_FALSE(unpack({{"a/../../evil.js", "x"}}));
    CHECK_FALSE(unpack({{"a.js", "x"}, {"b.js", "y"}, {"c.js", "z"}}, ZipLimits{1 << 20, 2}));
    CHECK(error.find("too many") != std::string::npos);
    CHECK_FALSE(unpack({{"big.js", std::string(4096, 'a')}}, ZipLimits{1000, 10}));
    CHECK(error.find("unpacked size") != std::string::npos);
    std::string notZip = (t.path / "plain.zip").string();
    std::ofstream(notZip) << "hello";
    CHECK_FALSE(unpackZip(notZip, t.path.string(), {}, error));
}

TEST_CASE("ograf: the library takes a ZIP as a whole, replaces by id, keeps a package in use")
{
    TempDir t;
    Library lib((t.path / "lib").string(), RenderInfo{}, ZipLimits{});
    lib.load();
    auto const two = makeZip(t.path, {{"a/one.ograf.json", manifest("one")}, {"a/graphic.mjs", kRuntime}, {"two.ograf.json", R"({"$schema":"https://ograf.ebu.io/v1/specification/json-schemas/graphics/schema.json","id":"two","name":"Two","main":"a/graphic.mjs","supportsRealTime":true,"supportsNonRealTime":false})"}});
    auto const added = lib.addZip(two, {});
    INFO(added.body.serialize());
    REQUIRE(added.status == 200);
    REQUIRE(lib.list().size() == 2);
    auto const one = lib.find("one");
    REQUIRE(one);
    CHECK(one->mainPath() == "a/graphic.mjs");
    CHECK_FALSE(lib.filePath(one->package, "a/graphic.mjs").empty());
    CHECK(lib.filePath(one->package, "../index.json").empty());
    CHECK(lib.filePath("..", "index.json").empty());

    // One bad manifest refuses the whole ZIP.
    auto const bad = lib.addZip(makeZip(t.path, {{"x.ograf.json", manifest("x")}, {"graphic.mjs", kRuntime}, {"y.ograf.json", manifest("y/z")}}), {});
    CHECK(bad.status == 422);
    CHECK_FALSE(lib.find("x"));
    CHECK(lib.addZip(makeZip(t.path, {{"p/a.ograf.json", manifest("dup")}, {"q/b.ograf.json", manifest("dup")}, {"p/graphic.mjs", kRuntime}, {"q/graphic.mjs", kRuntime}}), {}).status == 422);
    CHECK(lib.addZip(makeZip(t.path, {{"readme.txt", "no manifest"}}), {}).status == 422);

    // Replace "one" while a layer uses the old package: both packages stay until collected.
    auto const oldPackage = one->package;
    auto const replaced = lib.addZip(makeZip(t.path, {{"one.ograf.json", manifest("one")}, {"graphic.mjs", kRuntime}}), {oldPackage});
    REQUIRE(replaced.status == 200);
    CHECK(replaced.body.get("graphics").get<picojson::array>()[0].get("replaced").get<bool>());
    CHECK(lib.find("one")->package != oldPackage);
    CHECK(lib.findInPackage(oldPackage, "one"));
    CHECK(fs::exists(fs::path(lib.packagesDir()) / oldPackage));
    lib.collect({});
    CHECK_FALSE(lib.findInPackage(oldPackage, "one"));
    CHECK(fs::exists(fs::path(lib.packagesDir()) / lib.find("two")->package)); // two still listed: package kept

    // The index survives a restart.
    Library again((t.path / "lib").string(), RenderInfo{}, ZipLimits{});
    again.load();
    CHECK(again.list().size() == 2);

    // Delete.
    CHECK(lib.remove("two", {}));
    CHECK_FALSE(lib.find("two"));
    CHECK_FALSE(lib.remove("two", {}));
}

TEST_CASE("ograf: layers keep the state, merge updates, ignore stale answers")
{
    TempDir t;
    Library lib((t.path / "lib").string(), RenderInfo{}, ZipLimits{});
    lib.load();
    REQUIRE(lib.addZip(makeZip(t.path, {{"g.ograf.json", manifest("g", R"(,"stepCount":2)")}, {"graphic.mjs", kRuntime}}), {}).status == 200);
    FakeRenderer page;
    auto const statePath = (t.path / "layers.json").string();
    LayerSet layers(page, lib, statePath, RenderInfo{});

    CHECK(layers.load(9, "g", {}).status == 404);
    CHECK(layers.load(1, "missing", {}).status == 404);
    auto const loaded = layers.load(3, "g", parse(R"({"name":"A","title":"T"})"));
    REQUIRE(loaded.status == 200);
    auto const instance = loaded.body.get("graphicInstanceId").to_str();
    CHECK(page.calls.back() == std::pair<int, std::string>{3, "load"});
    CHECK(page.args.back().get("module").to_str().find("https://ograf.local/packages/") == 0);
    CHECK(page.args.back().get("renderCharacteristics").get("frameRate").get<double>() == 50);
    CHECK(layers.snapshot()[2].state == "ready");

    CHECK(layers.action(3, "other-instance", "playAction", parse("{}")).status == 404);
    page.answers.push_back(okStep(0));
    CHECK(layers.action(3, instance, "playAction", parse("{}")).status == 200);
    CHECK(layers.snapshot()[2].state == "on");
    CHECK(layers.snapshot()[2].step == 0);

    CHECK(layers.action(3, instance, "updateAction", parse(R"({"data":{"title":"New"}})")).status == 200);
    CHECK(layers.snapshot()[2].data.serialize() == R"({"name":"A","title":"New"})");

    CHECK(layers.action(3, instance, "customAction", parse(R"({"payload":{"x":1}})"), "highlight").status == 200);
    CHECK(page.args.back().get("id").to_str() == "highlight");

    page.answers.push_back({CallResult::Kind::Threw, {}, "boom"});
    CHECK(layers.action(3, instance, "stopAction", parse("{}")).status == 550);
    CHECK(layers.snapshot()[2].error.find("boom") != std::string::npos);
    page.answers.push_back({CallResult::Kind::Timeout, {}, {}});
    CHECK(layers.action(3, instance, "stopAction", parse("{}")).status == 504);
    CHECK(layers.action(3, instance, "stopAction", parse("{}")).status == 200);
    CHECK(layers.snapshot()[2].state == "stopped");
    CHECK(layers.packagesInUse().size() == 1);

    // A process restart: the saved layer comes back loaded, not playing.
    {
        FakeRenderer page2;
        LayerSet after(page2, lib, statePath, RenderInfo{});
        after.loadSaved();
        after.restore(false);
        REQUIRE(page2.calls.size() == 1);
        CHECK(page2.calls[0] == std::pair<int, std::string>{3, "load"});
        CHECK(page2.args[0].get("data").serialize() == R"({"name":"A","title":"New"})");
        CHECK(after.snapshot()[2].state == "ready");
    }

    // A renderer reload: an on-air layer goes back to its step without animation.
    page.answers.push_back(okStep(1));
    REQUIRE(layers.action(3, instance, "playAction", parse(R"({"goto":1})")).status == 200);
    page.calls.clear();
    page.args.clear();
    layers.pageLost();
    layers.restore(true);
    REQUIRE(page.calls.size() == 2);
    CHECK(page.calls[1] == std::pair<int, std::string>{3, "playAction"});
    CHECK(page.args[1].serialize() == R"({"goto":1,"skipAnimation":true})");

    // Clear and filters.
    REQUIRE(layers.load(5, "g", {}).status == 200);
    auto const cleared = layers.clearWhere(std::nullopt, "g", "");
    CHECK(cleared.size() == 2);
    CHECK_FALSE(layers.anyLoaded());
}

TEST_CASE("ograf: Server API routes")
{
    TempDir t;
    Library lib((t.path / "lib").string(), RenderInfo{}, ZipLimits{});
    lib.load();
    REQUIRE(lib.addZip(makeZip(t.path, {{"g.ograf.json", manifest("my graphic", R"(,"thumbnails":[{"file":"t.png"}])")}, {"graphic.mjs", kRuntime}, {"t.png", "PNG"}, {"secret.txt", "s"}}), {}).status == 200);
    FakeRenderer page;
    LayerSet layers(page, lib, (t.path / "layers.json").string(), RenderInfo{});
    std::string problemText;
    ApiContext ctx{lib, layers, "Browser", "test", {"https://controller.example"}, [&] { return problemText; }};
    auto const call = [&](std::string const& method, std::string const& path, std::string const& body = "", std::string const& query = "", std::string const& origin = "") {
        mbs::ops::HttpRequest req;
        req.method = method;
        req.path = path;
        req.query = query;
        req.body = body;
        if (!origin.empty())
        {
            req.headers.emplace_back("Origin", origin);
        }
        mbs::ops::HttpResponse res;
        CHECK(isOgrafPath(path));
        handleOgraf(ctx, req, res);
        return res;
    };

    CHECK(call("GET", "/ograf/v1").status == 200);
    auto const list = parse(call("GET", "/ograf/v1/graphics").body);
    REQUIRE(list.get("graphics").get<picojson::array>().size() == 1);
    CHECK(call("GET", "/ograf/v1/graphics/my%20graphic").status == 200);
    CHECK(call("GET", "/ograf/v1/graphics/nope").status == 404);
    auto const thumb = call("GET", "/ograf/v1/graphics/my%20graphic/thumbnail", "", "file=t.png");
    CHECK(thumb.status == 200);
    CHECK(thumb.contentType == "image/png");
    CHECK(call("GET", "/ograf/v1/graphics/my%20graphic/thumbnail", "", "file=secret.txt").status == 404); // only listed thumbnails

    auto const renderer = parse(call("GET", "/ograf/v1/renderers/main").body);
    CHECK(renderer.get("renderer").get("renderTargets").get<picojson::array>().size() == 8);
    CHECK(call("GET", "/ograf/v1/renderers/other").status == 404);

    auto const loaded = call("POST", "/ograf/v1/renderers/main/target/graphicInstance/load", R"({"renderTarget":{"layer":2},"graphicId":"my graphic","params":{"data":{}}})");
    REQUIRE(loaded.status == 200);
    auto const instance = parse(loaded.body).get("graphicInstanceId").to_str();
    CHECK(call("POST", "/ograf/v1/renderers/main/target/graphicInstance/load", R"({"renderTarget":{"layer":9},"graphicId":"my graphic","params":{"data":{}}})").status == 404);
    auto const target = parse(call("GET", "/ograf/v1/renderers/main/target", "", "renderTarget=%7B%22layer%22%3A2%7D").body);
    CHECK(target.get("graphicInstances").get<picojson::array>().size() == 1);
    CHECK(call("POST", "/ograf/v1/renderers/main/target/graphicInstance/playAction", R"({"renderTarget":{"layer":2},"graphicInstanceId":")" + instance + R"(","params":{}})").status == 200);
    CHECK(call("POST", "/ograf/v1/renderers/main/target/graphicInstance/customActions/x", R"({"renderTarget":{"layer":2},"graphicInstanceId":")" + instance + R"(","params":{"payload":1}})").status == 200);
    CHECK(page.args.back().get("id").to_str() == "x");

    // The graphics list says where a graphic runs.
    auto const used = parse(call("GET", "/ograf/v1/graphics").body).get("graphics").get<picojson::array>()[0];
    CHECK(used.get("v_inUse").get<picojson::array>().size() == 1);

    // Without a renderer page, actions answer 409.
    problemText = "mode is page";
    CHECK(call("POST", "/ograf/v1/renderers/main/target/graphicInstance/stopAction", R"({"renderTarget":{"layer":2},"graphicInstanceId":")" + instance + R"(","params":{}})").status == 409);
    problemText.clear();

    auto const cleared = parse(call("PUT", "/ograf/v1/renderers/main/target/graphicInstance/clear", R"({"filters":[{"renderTarget":{"layer":2}}]})").body);
    CHECK(cleared.get("graphicInstances").get<picojson::array>().size() == 1);
    CHECK(parse(call("GET", "/api/v1/ograf/layers").body).get("layers").get<picojson::array>()[1].get("state").to_str() == "empty");

    // CORS only for listed origins.
    auto const pre = call("OPTIONS", "/ograf/v1/graphics", "", "", "https://controller.example");
    CHECK(pre.status == 204);
    bool allowed = false;
    for (auto const& [k, v] : pre.headers)
    {
        allowed = allowed || (k == "Access-Control-Allow-Origin" && v == "https://controller.example");
    }
    CHECK(allowed);
    for (auto const& [k, v] : call("GET", "/ograf/v1/graphics", "", "", "https://evil.example").headers)
    {
        CHECK(k != "Access-Control-Allow-Origin");
    }

    // Delete with force clears the layers that use it.
    REQUIRE(call("POST", "/ograf/v1/renderers/main/target/graphicInstance/load", R"({"renderTarget":{"layer":1},"graphicId":"my graphic","params":{"data":{}}})").status == 200);
    CHECK(call("DELETE", "/ograf/v1/graphics/my%20graphic", "", "force=true").status == 200);
    CHECK_FALSE(layers.anyLoaded());
    CHECK(call("GET", "/ograf/v1/graphics/my%20graphic").status == 404);
}

TEST_CASE("ograf: Server API shapes follow server-api.yaml")
{
    TempDir t;
    Library lib((t.path / "lib").string(), RenderInfo{}, ZipLimits{});
    lib.load();
    REQUIRE(lib.addZip(makeZip(t.path, {{"g.ograf.json", manifest("g")}, {"graphic.mjs", kRuntime}}), {}).status == 200);
    REQUIRE(lib.addZip(makeZip(t.path, {{"h.ograf.json", manifest("h")}, {"graphic.mjs", kRuntime}}), {}).status == 200);
    FakeRenderer page;
    LayerSet layers(page, lib, (t.path / "layers.json").string(), RenderInfo{});
    std::string problemText;
    std::string errorText;
    ApiContext ctx{lib, layers, "Browser", "test", {"https://controller.example"}, [&] { return problemText; }, [&] { return errorText; }};
    auto const call = [&](std::string const& method, std::string const& path, std::string const& body = "", std::string const& query = "") {
        mbs::ops::HttpRequest req;
        req.method = method;
        req.path = path;
        req.query = query;
        req.body = body;
        mbs::ops::HttpResponse res;
        handleOgraf(ctx, req, res);
        return res;
    };
    auto const isProblem = [](mbs::ops::HttpResponse const& res) {
        auto const v = parse(res.body);
        return v.get("status").is<double>() && static_cast<int>(v.get("status").get<double>()) == res.status && v.get("title").is<std::string>() &&
               v.get("detail").is<std::string>();
    };
    std::string const T = "/ograf/v1/renderers/main/target/graphicInstance/";
    auto const load = [&](int layer, std::string const& id) {
        auto const res = call("POST", T + "load", R"({"renderTarget":{"layer":)" + std::to_string(layer) + R"(},"graphicId":")" + id + R"(","params":{"data":{}}})");
        REQUIRE(res.status == 200);
        return parse(res.body);
    };

    // GET / : name (required), version; also with the trailing slash a client adds to the base URL.
    CHECK(parse(call("GET", "/ograf/v1/").body).get("name").to_str() == "Browser");

    // GraphicListInfo: id and name.
    auto const graphics = parse(call("GET", "/ograf/v1/graphics").body);
    for (auto const& g : graphics.get("graphics").get<picojson::array>())
    {
        CHECK(g.get("id").is<std::string>());
        CHECK(g.get("name").is<std::string>());
    }
    // GET /graphics/{id}: graphic (the manifest) and metadata.createdAt.
    auto const info = parse(call("GET", "/ograf/v1/graphics/g").body);
    CHECK(info.get("graphic").get("id").to_str() == "g");
    CHECK(info.get("metadata").get("createdAt").is<std::string>());
    CHECK(isProblem(call("GET", "/ograf/v1/graphics/nope")));
    CHECK(isProblem(call("GET", "/ograf/v1/graphics/g/thumbnail")));

    // Renderer list: id and name. RendererInfo: id, name, status, renderTargets, a shallow
    // renderTargetSchema with a default target, customActions.
    auto const list = parse(call("GET", "/ograf/v1/renderers").body).get("renderers").get<picojson::array>();
    REQUIRE(list.size() == 1);
    CHECK(list[0].get("id").to_str() == "main");
    CHECK(list[0].get("name").to_str() == "Browser");
    auto const r = parse(call("GET", "/ograf/v1/renderers/main").body).get("renderer");
    CHECK(r.get("status").get("status").to_str() == "OK");
    CHECK(r.get("customActions").get<picojson::array>().empty());
    auto const& schema = r.get("renderTargetSchema");
    CHECK(schema.get("type").to_str() == "object");
    CHECK(schema.get("properties").get("layer").get("type").to_str() == "integer");
    CHECK(schema.get("default").serialize() == R"({"layer":1})");
    auto const targets = r.get("renderTargets").get<picojson::array>();
    REQUIRE(targets.size() == 8);
    CHECK(targets[0].get("renderTarget").serialize() == R"({"layer":1})");
    CHECK(targets[0].get("name").to_str() == "Layer 1");
    CHECK(targets[0].get("graphicInstances").get<picojson::array>().empty());
    CHECK(isProblem(call("GET", "/ograf/v1/renderers/other")));

    // Status: WARNING while the renderer cannot take actions, ERROR when the page failed.
    problemText = "loading";
    CHECK(parse(call("GET", "/ograf/v1/renderers/main").body).get("renderer").get("status").get("status").to_str() == "WARNING");
    errorText = "crashed";
    auto const failed = parse(call("GET", "/ograf/v1/renderers/main").body).get("renderer").get("status");
    CHECK(failed.get("status").to_str() == "ERROR");
    CHECK(failed.get("message").to_str() == "crashed");
    problemText.clear();
    errorText.clear();

    // Load: graphicInstanceId and statusCode; the renderTarget is a JSON object, not a string.
    auto const a = load(1, "g");
    CHECK(a.get("graphicInstanceId").is<std::string>());
    CHECK(a.get("statusCode").get<double>() == 200);
    CHECK(call("POST", T + "load", R"({"renderTarget":"{\"layer\":1}","graphicId":"g","params":{"data":{}}})").status == 404);
    CHECK(isProblem(call("POST", T + "load", R"({"renderTarget":{"layer":1},"graphicId":"nope","params":{"data":{}}})")));
    CHECK(call("POST", T + "load", R"({"renderTarget":{"layer":1},"params":{"data":{}}})").status == 400);
    auto const instance = a.get("graphicInstanceId").to_str();

    // GET target: renderTarget is a JSON-stringified RenderTargetIdentifier in the query.
    auto const ti = call("GET", "/ograf/v1/renderers/main/target", "", "renderTarget=%7B%22layer%22%3A1%7D");
    REQUIRE(ti.status == 200);
    auto const gi = parse(ti.body).get("graphicInstances").get<picojson::array>();
    REQUIRE(gi.size() == 1);
    CHECK(gi[0].get("graphicInstanceId").to_str() == instance);
    CHECK(gi[0].get("graphic").get("id").to_str() == "g");
    CHECK(gi[0].get("graphic").get("name").is<std::string>());
    CHECK(call("GET", "/ograf/v1/renderers/main/target").status == 404);
    CHECK(call("GET", "/ograf/v1/renderers/main/target", "", "renderTarget=%7B%22layer%22%3A9%7D").status == 404);
    CHECK(call("GET", "/ograf/v1/renderers/main/target", "", "renderTarget=1").status == 404);

    // Actions: graphicInstanceId, statusCode, statusMessage and currentStep from the graphic.
    picojson::object played;
    played["statusCode"] = picojson::value(200.0);
    played["statusMessage"] = picojson::value("Playing");
    played["currentStep"] = picojson::value(0.0);
    page.answers.push_back({CallResult::Kind::Ok, picojson::value(played), {}});
    auto const play = parse(call("POST", T + "playAction", R"({"renderTarget":{"layer":1},"graphicInstanceId":")" + instance + R"(","params":{"delta":1}})").body);
    CHECK(play.get("graphicInstanceId").to_str() == instance);
    CHECK(play.get("statusCode").get<double>() == 200);
    CHECK(play.get("statusMessage").to_str() == "Playing");
    CHECK(play.get("currentStep").get<double>() == 0);
    CHECK(page.args.back().serialize() == R"({"delta":1})");
    auto const update = call("POST", T + "updateAction", R"({"renderTarget":{"layer":1},"graphicInstanceId":")" + instance + R"(","params":{"data":{"x":1},"skipAnimation":true}})");
    CHECK(update.status == 200);
    CHECK(page.args.back().get("skipAnimation").get<bool>());
    // A custom action gets {id, payload, skipAnimation} (the graphic's customAction() argument).
    CHECK(call("POST", T + "customActions/highlight", R"({"renderTarget":{"layer":1},"graphicInstanceId":")" + instance + R"(","params":{"payload":{"on":true}}})").status == 200);
    CHECK(page.args.back().get("id").to_str() == "highlight");
    CHECK(page.args.back().get("payload").serialize() == R"({"on":true})");
    // The wrong instance or an empty layer: 404; a missing instance id: 400.
    CHECK(isProblem(call("POST", T + "stopAction", R"({"renderTarget":{"layer":1},"graphicInstanceId":"other","params":{}})")));
    CHECK(call("POST", T + "stopAction", R"({"renderTarget":{"layer":2},"graphicInstanceId":")" + instance + R"(","params":{}})").status == 404);
    CHECK(call("POST", T + "stopAction", R"({"renderTarget":{"layer":1},"params":{}})").status == 400);
    // The graphic throws: 550, timeout: 504; both ErrorResponses.
    page.answers.push_back({CallResult::Kind::Threw, {}, "boom"});
    auto const threw = call("POST", T + "stopAction", R"({"renderTarget":{"layer":1},"graphicInstanceId":")" + instance + R"(","params":{}})");
    CHECK(threw.status == 550);
    CHECK(isProblem(threw));
    page.answers.push_back({CallResult::Kind::Timeout, {}, ""});
    CHECK(call("POST", T + "stopAction", R"({"renderTarget":{"layer":1},"graphicInstanceId":")" + instance + R"(","params":{}})").status == 504);
    // Renderer custom actions: there are none.
    CHECK(isProblem(call("POST", "/ograf/v1/renderers/main/customActions/x", R"({"payload":{}})")));

    // Clear: the filters (each matches with all its parts; any filter clears).
    load(2, "h");
    load(3, "h");
    auto const clear = [&](std::string const& body) { return parse(call("PUT", T + "clear", body).body).get("graphicInstances").get<picojson::array>(); };
    CHECK(clear(R"({"filters":[{"graphicId":7}]})").empty()); // a wrong type matches nothing
    CHECK(clear(R"({"filters":[{"renderTarget":{"layer":9}}]})").empty());
    CHECK(clear(R"({"filters":[{"renderTarget":{"layer":1},"graphicId":"h"}]})").empty());
    auto const c1 = clear(R"({"filters":[{"graphicInstanceId":")" + instance + R"("}]})");
    REQUIRE(c1.size() == 1);
    CHECK(c1[0].get("renderTarget").serialize() == R"({"layer":1})");
    CHECK(c1[0].get("graphicInstanceId").to_str() == instance);
    load(1, "g");
    CHECK(clear(R"({"filters":[{"graphicId":"h","renderTarget":{"layer":2}},{"graphicId":"g"}]})").size() == 2);
    load(1, "g");
    CHECK(clear(R"({"filters":[]})").size() == 2); // no filters: everything
    CHECK_FALSE(layers.anyLoaded());
    CHECK(call("POST", T + "clear", R"({"filters":[]})").status == 405);

    // Without the token: a 401 ErrorResponse that a listed browser origin can read.
    mbs::ops::HttpRequest req;
    req.method = "GET";
    req.path = "/ograf/v1/graphics";
    req.headers.emplace_back("Origin", "https://controller.example");
    mbs::ops::HttpResponse res;
    unauthorized(ctx, req, res);
    CHECK(res.status == 401);
    CHECK(isProblem(res));
    bool allowed = false;
    for (auto const& [k, v] : res.headers)
    {
        allowed = allowed || (k == "Access-Control-Allow-Origin" && v == "https://controller.example");
    }
    CHECK(allowed);
}

TEST_CASE("ograf: GET /graphics/{id} keeps the manifest's key order")
{
    TempDir t;
    Library lib((t.path / "lib").string(), RenderInfo{}, ZipLimits{});
    lib.load();
    auto const schema = R"(,"schema":{"type":"object","properties":{"zeta":{"type":"string"},"alpha":{"type":"number"}}})";
    REQUIRE(lib.addZip(makeZip(t.path, {{"g.ograf.json", manifest("g", schema)}, {"graphic.mjs", kRuntime}}), {}).status == 200);
    FakeRenderer page;
    LayerSet layers(page, lib, (t.path / "layers.json").string(), RenderInfo{});
    ApiContext ctx{lib, layers, "Browser", "test", {}, {}};
    mbs::ops::HttpRequest req;
    req.method = "GET";
    req.path = "/ograf/v1/graphics/g";
    mbs::ops::HttpResponse res;
    handleOgraf(ctx, req, res);
    REQUIRE(res.status == 200);
    auto const v = parse(res.body); // still valid JSON
    CHECK(v.get("graphic").get("id").to_str() == "g");
    CHECK(v.get("metadata").get("createdAt").is<std::string>());
    CHECK(v.get("v_info").get("id").to_str() == "g");
    auto const zeta = res.body.find("\"zeta\"");
    auto const alpha = res.body.find("\"alpha\"");
    REQUIRE(zeta != std::string::npos);
    REQUIRE(alpha != std::string::npos);
    CHECK(zeta < alpha); // the file's order, not picojson's sorted one
    CHECK(res.body.find("\"$schema\"") < res.body.find("\"id\""));
}
