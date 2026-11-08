#pragma once
#include <string>

struct TikTokLiveSettings {
    bool allowGifts = true;
    bool allowComments = true;
    std::string contentDisclosure = "off";

    bool Valid() const {
        return contentDisclosure == "off" || contentDisclosure == "own_brand" ||
            contentDisclosure == "third_party" || contentDisclosure == "both";
    }
};
