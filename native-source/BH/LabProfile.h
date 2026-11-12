#pragma once

#include <string>

namespace LabProfile {
    bool LoadAndVerify(std::string& error);
    bool VerifyClient(std::string& error);
    bool VerifyCommon(std::string& error);
    bool VerifyServer(std::string& error);
    bool BackendFileIdentity(std::string& digest);
    bool ModFileIdentity(std::string& digest);
    const std::string& Token();
    int Port();
}
