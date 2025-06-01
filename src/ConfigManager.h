#pragma once
#include <optional>
#include <string>

namespace cfg
{
    std::string                        defaultDatabasePath();             
    std::optional<std::string>         loadDatabasePath();                
    void                               saveDatabasePath(std::string const&); 
} // namespace cfg
