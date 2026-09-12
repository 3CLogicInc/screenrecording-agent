#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "recording/monitor_layout.hpp"
#include "recording/monitor_capture_settings.hpp"

using screenrec::fitCompositeToCanvas;
using screenrec::getCompositeBounds;
using screenrec::MAX_RECORDED_MONITORS;
using screenrec::MonitorBounds;
using screenrec::MonitorInfo;
using screenrec::normalizeEqualTiles;
using screenrec::normalizeMixedDpiLayout;
using screenrec::selectRecordedMonitors;

namespace {

MonitorInfo monitor(int index, bool isPrimary) {
    MonitorInfo m;
    m.index = index;
    m.name = "Display_" + std::to_string(index);
    m.x = index * 1920;
    m.y = 0;
    m.width = 1920;
    m.height = 1080;
    m.isPrimary = isPrimary;
    return m;
}

}

TEST_CASE("at or under the cap, all monitors are recorded") {
    CHECK(selectRecordedMonitors({}).empty());
    CHECK(selectRecordedMonitors({monitor(0, true)}).size() == 1);
    CHECK(selectRecordedMonitors({monitor(0, true), monitor(1, false)}).size() == 2);
    CHECK(selectRecordedMonitors(
              {monitor(0, true), monitor(1, false), monitor(2, false)})
              .size() == 3);
}

TEST_CASE("over the cap, primary plus first extended monitors are kept in order") {
    const auto selected = selectRecordedMonitors(
        {monitor(0, false), monitor(1, true), monitor(2, false), monitor(3, false)});
    REQUIRE(selected.size() == MAX_RECORDED_MONITORS);
    CHECK(selected[0].index == 0);
    CHECK(selected[1].index == 1);
    CHECK(selected[1].isPrimary);
    CHECK(selected[2].index == 2);
}

TEST_CASE("late primary is always kept even when extended slots fill first") {
    const auto selected = selectRecordedMonitors(
        {monitor(0, false), monitor(1, false), monitor(2, false), monitor(3, true)});
    REQUIRE(selected.size() == MAX_RECORDED_MONITORS);
    CHECK(selected[0].index == 0);
    CHECK(selected[1].index == 1);
    CHECK(selected[2].index == 3);
    CHECK(selected[2].isPrimary);
}

TEST_CASE("no primary flagged falls back to the first monitors in order") {
    const auto selected = selectRecordedMonitors(
        {monitor(0, false), monitor(1, false), monitor(2, false), monitor(3, false)});
    REQUIRE(selected.size() == MAX_RECORDED_MONITORS);
    CHECK(selected[0].index == 0);
    CHECK(selected[1].index == 1);
    CHECK(selected[2].index == 2);
}

TEST_CASE("maxCount of zero means no cap") {
    const auto selected = selectRecordedMonitors(
        {monitor(0, true), monitor(1, false), monitor(2, false), monitor(3, false)}, 0);
    CHECK(selected.size() == 4);
}

TEST_CASE("selection is deterministic across calls") {
    const std::vector<MonitorInfo> monitors = {
        monitor(0, false), monitor(1, true), monitor(2, false),
        monitor(3, false), monitor(4, false)};
    const auto first = selectRecordedMonitors(monitors);
    const auto second = selectRecordedMonitors(monitors);
    REQUIRE(first.size() == second.size());
    for (size_t i = 0; i < first.size(); ++i) {
        CHECK(first[i].index == second[i].index);
    }
}

namespace {

// Point-space display as macOS detectMonitors sees it before normalization.
MonitorInfo pointDisplay(int pointX, int pointY, int pixelW, int pixelH, float scale) {
    MonitorInfo m;
    m.index = 0;
    m.x = pointX;
    m.y = pointY;
    m.width = pixelW;
    m.height = pixelH;
    m.isPrimary = pointX == 0 && pointY == 0;
    m.dpiScaleX = scale;
    m.dpiScaleY = scale;
    return m;
}

}

TEST_CASE("uniform-scale displays normalize with no gaps and renderScale 1") {
    // Two 2x Retina displays adjacent in point space (1280pt + 1280pt wide).
    std::vector<MonitorInfo> monitors = {
        pointDisplay(0, 0, 2560, 1664, 2.0f),
        pointDisplay(1280, 0, 2560, 1440, 2.0f),
    };
    normalizeMixedDpiLayout(monitors);

    CHECK(monitors[0].x == 0);
    CHECK(monitors[0].renderScale == doctest::Approx(1.0f));
    // Second display starts exactly where the first ends: 1280pt * 2 = 2560px.
    CHECK(monitors[1].x == 2560);
    CHECK(monitors[1].renderScale == doctest::Approx(1.0f));
}

TEST_CASE("mixed-DPI displays tile seamlessly via renderScale") {
    // 2x Retina laptop + 1x external: per-display scaling would overlap
    // (external at x=1920, laptop ends at 2560); canonical 2x scale fixes both.
    std::vector<MonitorInfo> monitors = {
        pointDisplay(0, 0, 2560, 1664, 2.0f),
        pointDisplay(1280, 0, 1920, 1080, 1.0f),
    };
    normalizeMixedDpiLayout(monitors);

    CHECK(monitors[0].renderScale == doctest::Approx(1.0f));
    CHECK(monitors[1].x == 2560);                            // no overlap, no gap
    CHECK(monitors[1].renderScale == doctest::Approx(2.0f)); // 1920px drawn as 3840

    MonitorBounds bounds;
    REQUIRE(getCompositeBounds(monitors, bounds));
    CHECK(bounds.minX == 0);
    CHECK(bounds.maxX == 2560 + 3840); // laptop + scaled external
    CHECK(bounds.maxY == 2160);        // external: 1080px * 2
}

TEST_CASE("scaled external display (looks-like mode) keeps its point footprint") {
    // MacBook Air 2x + 4K external running "looks like 2048x1152"
    // (4096x2304 pixels / 2048x1152 points = 2x) to its right.
    std::vector<MonitorInfo> monitors = {
        pointDisplay(0, 0, 2560, 1664, 2.0f),
        pointDisplay(1280, 0, 4096, 2304, 2.0f),
    };
    normalizeMixedDpiLayout(monitors);

    MonitorBounds bounds;
    REQUIRE(getCompositeBounds(monitors, bounds));
    CHECK(monitors[1].x == 2560);
    CHECK(bounds.maxX == 2560 + 4096);
    CHECK(bounds.maxY == 2304);
}

TEST_CASE("composite bounds honor negative origins (display left of primary)") {
    std::vector<MonitorInfo> monitors = {
        pointDisplay(-1920, 0, 1920, 1080, 1.0f),
        pointDisplay(0, 0, 1920, 1080, 1.0f),
    };
    normalizeMixedDpiLayout(monitors);

    MonitorBounds bounds;
    REQUIRE(getCompositeBounds(monitors, bounds));
    CHECK(bounds.minX == -1920);
    CHECK(bounds.maxX == 1920);
    CHECK(bounds.maxX - bounds.minX == 3840);
}

TEST_CASE("getCompositeBounds is false for an empty monitor list") {
    MonitorBounds bounds;
    CHECK_FALSE(getCompositeBounds({}, bounds));
}

TEST_CASE("normalize handles degenerate zero scales without dividing by zero") {
    std::vector<MonitorInfo> monitors = {pointDisplay(0, 0, 1920, 1080, 0.0f)};
    normalizeMixedDpiLayout(monitors);
    CHECK(monitors[0].renderScale == doctest::Approx(1.0f));
    CHECK(monitors[0].x == 0);
}

TEST_CASE("equal tiles: 1080p laptop beside a 4K TV gets the same tile height") {
    std::vector<MonitorInfo> monitors = {
        pointDisplay(0, 0, 1920, 1080, 1.0f),
        pointDisplay(1920, 0, 3840, 2160, 1.0f),
    };
    normalizeEqualTiles(monitors);

    CHECK(monitors[0].renderScale == doctest::Approx(2.0f));
    CHECK(monitors[1].renderScale == doctest::Approx(1.0f));
    CHECK(monitors[0].x == 0);
    CHECK(monitors[0].y == 0);
    CHECK(monitors[1].x == 3840);
    CHECK(monitors[1].y == 0);

    MonitorBounds bounds;
    REQUIRE(getCompositeBounds(monitors, bounds));
    CHECK(bounds.maxX - bounds.minX == 7680);
    CHECK(bounds.maxY - bounds.minY == 2160);
}

TEST_CASE("equal tiles: re-pack keeps left-to-right desktop order") {
    std::vector<MonitorInfo> monitors = {
        pointDisplay(0, 0, 1920, 1080, 1.0f),
        pointDisplay(-3840, 0, 3840, 2160, 1.0f),
    };
    normalizeEqualTiles(monitors);

    CHECK(monitors[1].x == 0);
    CHECK(monitors[0].x == 3840);
}

TEST_CASE("equal tiles: same-size monitors keep scale 1 and repack from origin") {
    std::vector<MonitorInfo> monitors = {monitor(0, true), monitor(1, false)};
    normalizeEqualTiles(monitors);

    CHECK(monitors[0].renderScale == doctest::Approx(1.0f));
    CHECK(monitors[1].renderScale == doctest::Approx(1.0f));
    CHECK(monitors[0].x == 0);
    CHECK(monitors[1].x == 1920);
}

TEST_CASE("equal tiles: vertically stacked displays re-pack side-by-side") {
    std::vector<MonitorInfo> monitors = {
        pointDisplay(0, 0, 1920, 1080, 1.0f),
        pointDisplay(0, 1080, 1920, 1080, 1.0f),
    };
    normalizeEqualTiles(monitors);

    MonitorBounds bounds;
    REQUIRE(getCompositeBounds(monitors, bounds));
    CHECK(bounds.maxX - bounds.minX == 3840);
    CHECK(bounds.maxY - bounds.minY == 1080);
}

TEST_CASE("equal tiles: composes with macOS mixed-DPI normalization, canvas stays minimal") {
    std::vector<MonitorInfo> monitors = {
        pointDisplay(0, 0, 2560, 1664, 2.0f),
        pointDisplay(1280, 0, 3840, 2160, 1.0f),
    };
    normalizeMixedDpiLayout(monitors);
    normalizeEqualTiles(monitors);

    CHECK(monitors[1].renderScale == doctest::Approx(1.0f));
    CHECK(monitors[0].renderScale == doctest::Approx(2160.0f / 1664.0f));

    MonitorBounds bounds;
    REQUIRE(getCompositeBounds(monitors, bounds));
    CHECK(bounds.minY == 0);
    CHECK(bounds.maxY == 2160);
}

TEST_CASE("equal tiles: empty and zero-height inputs are safe") {
    std::vector<MonitorInfo> empty;
    normalizeEqualTiles(empty);
    CHECK(empty.empty());

    std::vector<MonitorInfo> zero = {pointDisplay(0, 0, 1920, 0, 1.0f)};
    normalizeEqualTiles(zero);
    CHECK(zero[0].renderScale == doctest::Approx(1.0f));
}

TEST_CASE("fitCompositeToCanvas: identical geometry is identity") {
    const auto fit = fitCompositeToCanvas(3840, 1080, 3840, 1080);
    CHECK(fit.scale == doctest::Approx(1.0f));
    CHECK(fit.offsetX == doctest::Approx(0.0f));
    CHECK(fit.offsetY == doctest::Approx(0.0f));
}

TEST_CASE("fitCompositeToCanvas: hot-added display shrinks composite to fit") {
    // Canvas sized for one 1920x1080 display; a second joins side-by-side.
    const auto fit = fitCompositeToCanvas(1920, 1080, 3840, 1080);
    CHECK(fit.scale == doctest::Approx(0.5f));
    CHECK(fit.offsetX == doctest::Approx(0.0f));
    CHECK(fit.offsetY == doctest::Approx(270.0f)); // letterbox, centered
}

TEST_CASE("fitCompositeToCanvas: unplug expands the survivor (pillarboxed)") {
    // Canvas sized for two displays; one survives. Same height so scale
    // stays 1 and the survivor centers horizontally.
    const auto fit = fitCompositeToCanvas(3840, 1080, 1920, 1080);
    CHECK(fit.scale == doctest::Approx(1.0f));
    CHECK(fit.offsetX == doctest::Approx(960.0f));
    CHECK(fit.offsetY == doctest::Approx(0.0f));
}

TEST_CASE("fitCompositeToCanvas: smaller survivor upscales to fill") {
    // Canvas from a 4K display; the surviving display is 1080p — content
    // grows to fill rather than sitting tiny in a corner.
    const auto fit = fitCompositeToCanvas(3840, 2160, 1920, 1080);
    CHECK(fit.scale == doctest::Approx(2.0f));
    CHECK(fit.offsetX == doctest::Approx(0.0f));
    CHECK(fit.offsetY == doctest::Approx(0.0f));
}

TEST_CASE("fitCompositeToCanvas: degenerate inputs fall back to identity") {
    CHECK(fitCompositeToCanvas(0, 1080, 1920, 1080).scale == doctest::Approx(1.0f));
    CHECK(fitCompositeToCanvas(1920, 1080, 0, 1080).scale == doctest::Approx(1.0f));
    CHECK(fitCompositeToCanvas(1920, 1080, -5, 1080).offsetX == doctest::Approx(0.0f));
}

TEST_CASE("monitor capture method is Automatic: hot-plugged and cross-adapter displays must fall back to WGC instead of recording black") {
    const auto cfg = screenrec::capture::monitorCaptureSettings(monitor(1, false));
    CHECK(cfg.method == screenrec::capture::MONITOR_CAPTURE_METHOD_AUTO);
    CHECK(cfg.method != screenrec::capture::MONITOR_CAPTURE_METHOD_DXGI);
}

TEST_CASE("monitor capture settings pass the display id through and keep the cursor") {
    MonitorInfo tv = monitor(1, false);
    tv.name = "\\\\.\\DISPLAY2";
    const auto cfg = screenrec::capture::monitorCaptureSettings(tv);
    CHECK(cfg.monitorId == "\\\\.\\DISPLAY2");
    CHECK(cfg.captureCursor);
}
