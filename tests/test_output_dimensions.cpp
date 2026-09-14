#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "recording/output_dimensions.hpp"

using screenrec::computeOutputDimensions;
using screenrec::MAX_ENCODE_WIDTH;
using screenrec::OutputDimensions;
using screenrec::roundDownEven;

namespace {

double aspect(const OutputDimensions& d) {
    return static_cast<double>(d.width) / static_cast<double>(d.height);
}

void expectEven(const OutputDimensions& d) {
    CHECK(d.width % 2 == 0);
    CHECK(d.height % 2 == 0);
}

}

TEST_CASE("roundDownEven floors odd values and clamps to a minimum of 2") {
    CHECK(roundDownEven(0) == 2);
    CHECK(roundDownEven(1) == 2);
    CHECK(roundDownEven(2) == 2);
    CHECK(roundDownEven(3) == 2);
    CHECK(roundDownEven(4) == 4);
    CHECK(roundDownEven(641) == 640);
    CHECK(roundDownEven(1920) == 1920);
}

TEST_CASE("single 16:9 monitor lands exactly on the preset resolution") {
    const auto out = computeOutputDimensions(1920, 1080, 720);
    CHECK(out.width == 1280);
    CHECK(out.height == 720);
    expectEven(out);
}

TEST_CASE("dual-monitor canvas keeps per-monitor legibility at the height cap") {
    // Old WxH-box policy collapsed this to 1280x360.
    const auto out = computeOutputDimensions(3840, 1080, 720);
    CHECK(out.width == 2560);
    CHECK(out.height == 720);
    expectEven(out);
    CHECK(aspect(out) == doctest::Approx(3840.0 / 1080.0).epsilon(0.01));
}

TEST_CASE("non-16:9 Retina panel is height-capped, width follows aspect") {
    // MacBook Air 13.6" (2560x1664): 2560 * 720/1664 = 1107.7 -> even 1106.
    const auto out = computeOutputDimensions(2560, 1664, 720);
    CHECK(out.width == 1106);
    CHECK(out.height == 720);
    expectEven(out);
    CHECK(aspect(out) == doctest::Approx(2560.0 / 1664.0).epsilon(0.01));
}

TEST_CASE("ultrawide dual-monitor canvas keeps full height at the 1080p preset") {
    const auto out = computeOutputDimensions(7036, 2352, 1080);
    CHECK(out.width == 3230);
    CHECK(out.height == 1080);
    CHECK(out.width <= MAX_ENCODE_WIDTH);
    expectEven(out);
    CHECK(aspect(out) == doctest::Approx(7036.0 / 2352.0).epsilon(0.01));
}

TEST_CASE("encoder width ceiling fires only on unsupported many-monitor canvases") {
    // Five 1080p monitors: 9600 wide would exceed what decoders accept.
    const auto out = computeOutputDimensions(9600, 1080, 1080);
    CHECK(out.width == MAX_ENCODE_WIDTH);
    CHECK(out.height == 460);
    expectEven(out);
    CHECK(aspect(out) == doctest::Approx(9600.0 / 1080.0).epsilon(0.01));

    // Three monitors (the PRD maximum) stay under the ceiling.
    const auto threeMon = computeOutputDimensions(5760, 1080, 720);
    CHECK(threeMon.width == 3840);
    CHECK(threeMon.height == 720);
}

TEST_CASE("4:3 canvas taller than the cap binds on height") {
    const auto out = computeOutputDimensions(1600, 1200, 1080);
    CHECK(out.width == 1440);
    CHECK(out.height == 1080);
    expectEven(out);
    CHECK(aspect(out) == doctest::Approx(4.0 / 3.0).epsilon(0.01));
}

TEST_CASE("canvas already at the height cap is passed through unchanged") {
    const auto out = computeOutputDimensions(1920, 1080, 1080);
    CHECK(out.width == 1920);
    CHECK(out.height == 1080);
}

TEST_CASE("small canvas is never upscaled to a larger height cap") {
    const auto out = computeOutputDimensions(1280, 720, 1080);
    CHECK(out.width == 1280);
    CHECK(out.height == 720);
}

TEST_CASE("odd-sized native canvas is clamped to even when not upscaling") {
    const auto out = computeOutputDimensions(1365, 767, 1080);
    CHECK(out.width == 1364);
    CHECK(out.height == 766);
    expectEven(out);
}

TEST_CASE("degenerate inputs fall back to the even-clamped canvas") {
    CHECK(computeOutputDimensions(1920, 1080, 0).width == 1920);
    CHECK(computeOutputDimensions(1920, 1080, 0).height == 1080);

    const auto out = computeOutputDimensions(0, 0, 720);
    CHECK(out.width == 2);
    CHECK(out.height == 2);
}

TEST_CASE("extreme aspect canvas never collapses a dimension to zero") {
    const auto out = computeOutputDimensions(10000, 10, 1080);
    expectEven(out);
    CHECK(out.height >= 2);
    CHECK(out.width <= MAX_ENCODE_WIDTH);
}
