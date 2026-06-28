#pragma once

#include <string>

namespace LinkuraLocal::HttpMock {
    void* CreateSelfhostApiTask(const std::string& baseUrl, const std::string& apiPath, const std::string& requestBodyJson);
    std::string FetchSelfhostApiBody(const std::string& baseUrl, const std::string& apiPath, const std::string& requestBodyJson);
}
