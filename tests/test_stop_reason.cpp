#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include <string>

#include "recording/stop_reason.hpp"

namespace sr = screenrec::stop_reason;
namespace journal = screenrec::journal;

TEST_CASE("success carries no error") {
    const auto c = sr::classify(sr::CODE_SUCCESS);
    CHECK(c.endReason == std::string(journal::END_CLIENT_STOP));
    CHECK(std::string(c.errorCode).empty());
    CHECK(c.message.empty());
}

TEST_CASE("no-space maps to disk_full") {
    const auto c = sr::classify(sr::CODE_NO_SPACE);
    CHECK(c.endReason == std::string(journal::END_DISK_FULL));
    CHECK(c.errorCode == std::string("obs_no_space"));
    CHECK_FALSE(c.message.empty());
}

TEST_CASE("encoder failure maps to error with dedicated code") {
    const auto c = sr::classify(sr::CODE_ENCODE_ERROR);
    CHECK(c.endReason == std::string(journal::END_ERROR));
    CHECK(c.errorCode == std::string("obs_encode_error"));
}

TEST_CASE("every non-success code classifies as a failure with a message") {
    for (const int code : {sr::CODE_BAD_PATH, sr::CODE_CONNECT_FAILED, sr::CODE_INVALID_STREAM,
             sr::CODE_ERROR, sr::CODE_DISCONNECTED, sr::CODE_UNSUPPORTED,
             sr::CODE_NO_SPACE, sr::CODE_ENCODE_ERROR, -99}) {
        const auto c = sr::classify(code);
        CHECK(c.endReason != std::string(journal::END_CLIENT_STOP));
        CHECK_FALSE(std::string(c.errorCode).empty());
        CHECK_FALSE(c.message.empty());
    }
}

TEST_CASE("unknown codes include the raw code in the message") {
    const auto c = sr::classify(-42);
    CHECK(c.message.find("-42") != std::string::npos);
}
